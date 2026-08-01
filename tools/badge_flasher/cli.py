"""Operator-facing badge factory state machine."""

from __future__ import annotations

import argparse
import secrets
import sys
import tempfile
import time
import urllib.error
from collections.abc import Callable
from pathlib import Path
from typing import TypeVar

from .bundles import (
    FactoryBundle,
    fetch_github_bundles,
    is_strictly_newer,
    load_bundle,
    select_bundle,
)
from .devices import DeviceBackend, DeviceError, usb_jtag_app_reset
from .flash import FlashEngine
from .models import (
    BatchResult,
    FlashEvidence,
    PassedFactoryRecord,
    TopologyAssignment,
    UsbDevice,
)
from .probe import discover_topology, rebind_probe_ports
from .public_output import (
    capture_user_visible_output,
    print_live_user_visible as _print_live_user_visible,
    print_user_visible as _print_user_visible,
    scrub_factory_transcript,
    scrub_user_visible_text,
)
from .records import ManufacturingLedger
from .verify import (
    GAME_SEEDS,
    VerificationError,
    provision_game_seed,
    runtime_evidence,
    wait_for_preseed_runtime,
    wait_for_runtime,
)


REPOSITORY = "lnxgod/friendorfoe"
RESOURCE = Path(__file__).with_name("resources") / "badge-factory-flasher-embedded.zip"

RESET = "\x1b[0m"
BOLD = "\x1b[1m"
CYAN = "\x1b[38;5;51m"
BLUE = "\x1b[38;5;33m"
PURPLE = "\x1b[38;5;135m"
GOLD = "\x1b[38;5;220m"
GREEN = "\x1b[38;5;46m"
RED = "\x1b[38;5;196m"
DIM = "\x1b[38;5;244m"
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
T = TypeVar("T")


ART = r"""
          .        *           .                 .
     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
       \\\\  ||  ////       GAMECHANGERS AI
        \\\  ||  ///       BADGE FORGE // DC34
         \\  ||  //
          \  ||  /              /\
       ____\_||_/____           /__\
      /      ||      \         /\  /\
 ~~~ /_______||_______\ ~~~~~ /__\/__\ ~~~~~~~~~~~~~~
           __||__               ||
          /  ||  \              ||
         /___||___\          ___||___
             ||              \  ||  /
             ||               \ || /
             ||                \||/
             \/                 \/
       THREE BOARDS // ONE PROVEN GRAPH // ZERO GUESSES
"""


def print_user_visible(
    value: object = "",
    *,
    file=None,
    end: str = "\n",
    flush: bool = False,
) -> None:
    _print_user_visible(
        scrub_factory_transcript(value),
        file=file,
        end=end,
        flush=flush,
    )


class _CapturedFactoryOperationError(RuntimeError):
    def __init__(self, error_type: str, error: str) -> None:
        super().__init__(error)
        self.error_type = error_type


class _RoleOnlyCancelled(RuntimeError):
    """An attended operator declined a safe role-only reassignment."""


def _run_factory_operation(operation: Callable[[], T]) -> T:
    captured = capture_user_visible_output(
        operation,
        scrubber=scrub_factory_transcript,
    )
    if captured.stdout:
        print_user_visible(captured.stdout, end="")
    if captured.stderr:
        print_user_visible(captured.stderr, file=sys.stderr, end="")
    if captured.error_type is not None:
        if captured.error_type == "KeyboardInterrupt":
            raise KeyboardInterrupt
        if captured.error_type == "_RoleOnlyCancelled":
            raise _RoleOnlyCancelled(captured.error or "role reassignment cancelled")
        raise _CapturedFactoryOperationError(
            captured.error_type,
            captured.error or captured.error_type,
        )
    return captured.result  # type: ignore[return-value]


def _public_exception(exc: BaseException) -> str:
    if isinstance(exc, _CapturedFactoryOperationError):
        return f"{exc.error_type}: {exc}"
    return f"{type(exc).__name__}: {scrub_factory_transcript(exc)}"


def _prompt_operator(message: str) -> str:
    _print_live_user_visible(
        scrub_factory_transcript(message),
        end="",
        flush=True,
    )
    return input()


ROLE_MENU = {
    "1": ("HUMAN", "normal", DIM),
    "2": ("INFECTED", "infected", GREEN),
    "3": ("HEALER", "immune", PURPLE),
}


def prompt_game_role(plain: bool) -> str:
    while True:
        print_user_visible(paint(
            "+==================================================+\n"
            "| GAMECHANGERS AI // SELECT NEXT BADGE             |\n"
            "+==================================================+\n"
            "| [1] HUMAN       // BLACK                         |\n"
            "| [2] INFECTED    // GREEN                         |\n"
            "| [3] HEALER      // HOT PINK                      |\n"
            "+==================================================+",
            CYAN + BOLD,
            plain,
        ))
        selected = _prompt_operator(
            paint("SELECT [1-3] > ", GOLD + BOLD, plain)
        ).strip().lower()
        if selected == "q":
            raise KeyboardInterrupt
        choice = ROLE_MENU.get(selected)
        if choice is None:
            phase("ROLE", "choose 1, 2, 3, or Q", plain)
            continue
        label, seed, color = choice
        while True:
            confirm = _prompt_operator(
                paint(
                    f"ARM NEXT BADGE AS {label}? [Y/N] > ",
                    color + BOLD,
                    plain,
                )
            ).strip().lower()
            if confirm == "y":
                return seed
            if confirm == "n":
                break
            phase("ROLE", "answer Y or N", plain)


def paint(text: str, color: str, plain: bool) -> str:
    return text if plain else f"{color}{text}{RESET}"


def banner(plain: bool) -> None:
    if not plain:
        print_user_visible("\x1b[2J\x1b[H", end="")
    lines = ART.splitlines()
    palette = (BLUE, CYAN, PURPLE, GOLD)
    for index, line in enumerate(lines):
        print_user_visible(
            paint(line, palette[index % len(palette)], plain)
        )
    print_user_visible()


def phase(label: str, detail: str, plain: bool) -> None:
    _print_live_user_visible(
        scrub_factory_transcript(
            f"{paint('[' + label + ']', CYAN + BOLD, plain)} {detail}"
        ),
        flush=True,
    )


def _handoff_factory_graph(
    engine: FlashEngine,
    devices: dict[str, UsbDevice],
    assignment: TopologyAssignment,
    plain: bool,
) -> None:
    for mac, label in (
        (assignment.ble_leaf_mac, "BLE-SCANNER"),
        (assignment.wifi_leaf_mac, "WIFI-SCANNER"),
        (assignment.uplink_mac, "UPLINK"),
    ):
        phase("BOOT", f"{label} force-clear/watchdog handoff", plain)
        engine.handoff_to_application(devices[mac])


def generate_receipt() -> str:
    return "rcpt_" + "".join(secrets.choice(CROCKFORD) for _ in range(8))


def _assignment_macs(assignment: TopologyAssignment) -> set[str]:
    return {
        assignment.uplink_mac,
        assignment.ble_leaf_mac,
        assignment.wifi_leaf_mac,
    }


def _resolve_prior_pass(
    device_macs: set[str],
    records: tuple[PassedFactoryRecord, ...],
    *,
    version: str,
    bundle_sha256: str,
) -> PassedFactoryRecord | None:
    intersecting = [
        record
        for record in records
        if device_macs & _assignment_macs(record.assignment)
    ]
    if not intersecting:
        return None
    exact = [
        record
        for record in intersecting
        if _assignment_macs(record.assignment) == device_macs
        and record.version == version
        and record.bundle_sha256 == bundle_sha256
    ]
    if exact:
        return exact[-1]
    raise DeviceError(
        "connected BADGE does not have one complete current prior PASS"
    )


def _confirm_role_only_reassignment(plain: bool) -> None:
    while True:
        answer = _prompt_operator(paint(
            "ALREADY PASSED // REASSIGN ROLE ONLY? [Y/N] > ",
            GOLD + BOLD,
            plain,
        )).strip().lower()
        if answer == "y":
            return
        if answer == "n":
            raise _RoleOnlyCancelled("role-only reassignment declined")
        phase("ROLE", "answer Y or N", plain)


def choose_bundle(args: argparse.Namespace, plain: bool) -> FactoryBundle:
    embedded = load_bundle(RESOURCE, source="embedded")
    if args.bundle:
        operator = load_bundle(args.bundle, source="operator")
        if is_strictly_newer(embedded.version, operator.version) and not args.allow_downgrade:
            raise ValueError(
                f"operator bundle {operator.version} is older than embedded "
                f"{embedded.version}; --allow-downgrade is required"
            )
        phase("BUNDLE", f"validated operator release {operator.version}", plain)
        return operator
    if args.offline:
        phase("BUNDLE", f"validated embedded release {embedded.version}", plain)
        return embedded
    try:
        with tempfile.TemporaryDirectory(prefix="fof-factory-releases-") as temp:
            releases = fetch_github_bundles(REPOSITORY, Path(temp))
            chosen = select_bundle(embedded, releases)
            # GitHub bundles are extracted into independent temp roots by load_bundle.
            phase(
                "BUNDLE",
                f"validated {chosen.source} release {chosen.version}",
                plain,
            )
            return chosen
    except (OSError, urllib.error.URLError, TimeoutError, ValueError) as exc:
        phase("BUNDLE", f"GitHub unavailable ({exc}); using validated embedded {embedded.version}", plain)
        return embedded


def run_one(
    args: argparse.Namespace, plain: bool, bundle: FactoryBundle | None = None,
    *, game_role: str,
    forbidden_macs: set[str] | None = None,
    known_passed_macs: set[str] | None = None,
    passed_records: tuple[PassedFactoryRecord, ...] = (),
) -> BatchResult:
    if game_role not in GAME_SEEDS:
        raise ValueError("factory game role is invalid")
    bundle = bundle or choose_bundle(args, plain)
    backend = DeviceBackend()
    ports = backend.list_candidate_ports()
    phase("USB", f"found {len(ports)} candidate ports", plain)
    devices = backend.scan()
    if len(devices) != 3:
        raise DeviceError(f"exactly three ESP32-S3 boards required; found {len(devices)}")
    repeated = set(devices) & set(forbidden_macs or ())
    if repeated:
        raise DeviceError(
            "previous BADGE is still connected; unplug all three prior boards"
        )
    prior = _resolve_prior_pass(
        set(devices),
        passed_records,
        version=bundle.version,
        bundle_sha256=bundle.bundle_sha256,
    )
    historical = set(devices) & set(known_passed_macs or ())
    if prior is not None or historical:
        if getattr(args, "allow_rework", False):
            phase(
                "WARN",
                "previously passed BADGE detected (intentional rework only)",
                plain,
            )
        elif prior is None or getattr(args, "yes", False):
            raise DeviceError(
                "previously passed BADGE hardware requires explicit "
                "--allow-rework confirmation"
            )
    ordered_devices = sorted(devices.values(), key=lambda item: item.mac)
    board_aliases = {
        device.mac: f"BADGE board {index}"
        for index, device in enumerate(ordered_devices, start=1)
    }
    for device in ordered_devices:
        if device.flash_size != "8MB" or device.psram_size != "8MB":
            raise DeviceError(
                f"{board_aliases[device.mac]}: expected 8MB flash + "
                f"8MB PSRAM, got "
                f"{device.flash_size} flash + {device.psram_size} PSRAM"
            )
        phase(
            "IDENT",
            f"{board_aliases[device.mac]} S3 {device.revision} 8MB+8MB",
            plain,
        )

    engine = FlashEngine()
    if prior is not None and not getattr(args, "allow_rework", False):
        _confirm_role_only_reassignment(plain)
        assignment = prior.assignment
        _handoff_factory_graph(engine, devices, assignment, plain)
        time.sleep(3)
        expected_uplink_target = str(
            bundle.layout("uplink")["identity"]["target"]
        )
        phase(
            "HEALTH",
            "proving prior factory graph before role mutation",
            plain,
        )
        wait_for_preseed_runtime(
            devices[assignment.uplink_mac],
            assignment,
            bundle.version,
            timeout_s=75,
            candidate_ports=backend.list_candidate_ports,
            expected_target=expected_uplink_target,
        )
        phase(
            "SEED",
            f"programming GAME ROLE {game_role} and proving reboot",
            plain,
        )
        reboot_proof = provision_game_seed(
            devices[assignment.uplink_mac],
            game_role,
            bundle.version,
            timeout_s=60,
            candidate_ports=backend.list_candidate_ports,
            expected_target=expected_uplink_target,
        )
        phase(
            "HEALTH",
            "waiting for fresh uplink USB generation and both scanner radios",
            plain,
        )
        runtime = wait_for_runtime(
            devices[assignment.uplink_mac],
            assignment,
            bundle.version,
            game_role,
            reboot_proof=reboot_proof,
            timeout_s=75,
            candidate_ports=backend.list_candidate_ports,
            expected_target=expected_uplink_target,
        )
        return BatchResult(
            badge_id=assignment.uplink_mac.replace(":", "")[-6:],
            version=bundle.version,
            bundle_sha256=bundle.bundle_sha256,
            passed=True,
            phase="reassign",
            assignment=assignment,
            devices=(),
            runtime=runtime_evidence(runtime),
            game_seed=game_role,
            receipt=generate_receipt(),
        )

    phase("PROBE", "loading disposable topology firmware", plain)
    for device in ordered_devices:
        engine.flash_and_verify(device, bundle, "probe")
        phase(
            "VERIFY",
            f"{board_aliases[device.mac]} probe write/readback PASS",
            plain,
        )

    time.sleep(1)
    # Identify exactly the three proven MACs in ROM, boot their probe apps,
    # then speak the factory protocol only to those three rebound ports.
    probe_rom = backend.rebind(set(devices), timeout_s=30)
    for device in probe_rom.values():
        usb_jtag_app_reset(device.port)
    time.sleep(1)
    probe_devices = rebind_probe_ports(
        [device.port for device in probe_rom.values()], set(devices), timeout_s=10
    )
    assignment = discover_topology(probe_devices.values(), timeout_s=10)
    phase("GRAPH", "UPLINK identified", plain)
    phase("GRAPH", "BLE-SCANNER identified", plain)
    phase("GRAPH", "WIFI-SCANNER identified", plain)

    evidence: list[FlashEvidence] = []
    production_order = (
        ("scanner", assignment.ble_leaf_mac, "BLE-SCANNER"),
        ("scanner", assignment.wifi_leaf_mac, "WIFI-SCANNER"),
        ("uplink", assignment.uplink_mac, "UPLINK"),
    )
    for role, mac, label in production_order:
        # USB paths are not identities. Every reset can renumber macOS ports,
        # so re-enter ROM and bind all three immutable MACs before each erase.
        current = backend.rebind(set(devices), timeout_s=30)
        phase("FLASH", f"{label} erase/write/readback", plain)
        result = engine.flash_and_verify(current[mac], bundle, role)
        evidence.append(result)
        phase("VERIFY", f"{label} write + explicit readback PASS", plain)

    phase("REBIND", "proving all post-flash USB identities by ROM MAC", plain)
    rebound = backend.rebind(set(devices), timeout_s=30)
    _handoff_factory_graph(engine, rebound, assignment, plain)
    time.sleep(3)
    expected_uplink_target = str(
        bundle.layout("uplink")["identity"]["target"]
    )
    phase(
        "SEED",
        f"programming GAME ROLE {game_role} and proving reboot",
        plain,
    )

    def provision():
        return provision_game_seed(
            rebound[assignment.uplink_mac],
            game_role,
            bundle.version,
            timeout_s=60,
            candidate_ports=backend.list_candidate_ports,
            expected_target=expected_uplink_target,
        )

    try:
        reboot_proof = provision()
    except VerificationError as first:
        if not str(first).startswith("game seed provisioning timed out:"):
            raise
        phase("RETRY", "UPLINK non-writing ROM/application handoff", plain)
        retry_rom = backend.rebind(set(devices), timeout_s=30)
        _handoff_factory_graph(engine, retry_rom, assignment, plain)
        reboot_proof = provision()
    phase(
        "HEALTH",
        "waiting for fresh uplink USB generation and both scanner radios",
        plain,
    )
    runtime = wait_for_runtime(
        rebound[assignment.uplink_mac],
        assignment,
        bundle.version,
        game_role,
        reboot_proof=reboot_proof,
        timeout_s=75,
        candidate_ports=backend.list_candidate_ports,
        expected_target=expected_uplink_target,
    )
    receipt = generate_receipt()
    badge_id = assignment.uplink_mac.replace(":", "")[-6:]
    return BatchResult(
        badge_id=badge_id,
        version=bundle.version,
        bundle_sha256=bundle.bundle_sha256,
        passed=True,
        phase="complete",
        assignment=assignment,
        devices=tuple(evidence),
        runtime=runtime_evidence(runtime),
        game_seed=game_role,
        receipt=receipt,
    )


class _FactoryArgumentError(ValueError):
    """Private argparse failure rendered only by the scrubbed CLI boundary."""


class _FactoryArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise _FactoryArgumentError(message)


def parser() -> argparse.ArgumentParser:
    result = _FactoryArgumentParser(
        prog="fof_badge_factory.py",
        description="FoF three-board badge factory flasher",
    )
    result.add_argument("--bundle", type=Path, help="validated local factory bundle")
    result.add_argument("--offline", action="store_true", help="skip the GitHub release check")
    result.add_argument("--plain", action="store_true", help="disable ANSI colors")
    result.add_argument("--once", action="store_true", help="flash one badge and exit")
    result.add_argument("--yes", action="store_true", help="do not wait for Enter")
    result.add_argument(
        "--allow-rework",
        action="store_true",
        help="explicitly permit flashing MACs already recorded as PASS",
    )
    result.add_argument(
        "--allow-downgrade",
        action="store_true",
        help="explicitly permit a local bundle older than the embedded release",
    )
    result.add_argument(
        "--game-role",
        choices=GAME_SEEDS,
        default=None,
        help="factory CON CRUD seed role",
    )
    result.add_argument("--records", type=Path, default=Path.home() / "Documents/FoF Badge Factory")
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
    except _FactoryArgumentError as exc:
        print_user_visible(f"ERROR: {exc}", file=sys.stderr)
        return 2
    plain = args.plain
    if args.yes and (not args.once or args.game_role is None):
        print_user_visible(
            "ERROR: --yes requires --once and an explicit --game-role",
            file=sys.stderr,
        )
        return 2
    banner(plain)
    ledger = ManufacturingLedger(args.records)
    try:
        passed_records = _run_factory_operation(ledger.passed_records)
    except Exception as exc:
        print_user_visible(
            paint(
                f"  FAIL // prior PASS ledger: {_public_exception(exc)}  ",
                RED + BOLD,
                plain,
            )
        )
        return 1
    known_passed_macs = {
        mac
        for record in passed_records
        for mac in _assignment_macs(record.assignment)
    }
    last_badge_macs: set[str] = set()
    try:
        bundle = _run_factory_operation(
            lambda: choose_bundle(args, plain)
        )
    except Exception as exc:
        print_user_visible(
            paint(
                f"  FAIL // bundle selection: {_public_exception(exc)}  ",
                RED + BOLD,
                plain,
            )
        )
        return 1
    while True:
        try:
            current_role = args.game_role or prompt_game_role(plain)
            phase("ROLE", f"GAME ROLE {current_role}", plain)
            if not args.yes:
                _prompt_operator(
                    paint(
                        "Plug one complete BADGE into USB, then press ENTER ",
                        GOLD + BOLD,
                        plain,
                    )
                )
        except (EOFError, KeyboardInterrupt):
            print_user_visible("\nInterrupted; no PASS record written.")
            return 130
        pass_recorded = False
        try:
            result = _run_factory_operation(
                lambda: run_one(
                    args,
                    plain,
                    bundle,
                    game_role=current_role,
                    forbidden_macs=last_badge_macs,
                    known_passed_macs=known_passed_macs,
                    passed_records=passed_records,
                )
            )
            _run_factory_operation(lambda: ledger.record(result))
            pass_recorded = True
            last_badge_macs = {
                result.assignment.uplink_mac,
                result.assignment.ble_leaf_mac,
                result.assignment.wifi_leaf_mac,
            }
            known_passed_macs.update(last_badge_macs)
            passed_records = (
                *passed_records,
                PassedFactoryRecord(
                    version=result.version,
                    bundle_sha256=result.bundle_sha256,
                    assignment=result.assignment,
                    game_seed=result.game_seed,
                ),
            )
            print_user_visible()
            outcome = (
                "REASSIGNED"
                if result.phase == "reassign"
                else "PASS"
            )
            print_user_visible(
                paint(
                    f"  {outcome} // GAME ROLE {result.game_seed} // "
                    f"RECEIPT {result.receipt}  ",
                    GREEN + BOLD,
                    plain,
                )
            )
            print_user_visible(
                paint(
                    "  Remove the badge. Factory evidence has been "
                    "fsync'd.  ",
                    GREEN,
                    plain,
                )
            )
        except _RoleOnlyCancelled:
            print_user_visible()
            print_user_visible(
                paint(
                    "  CANCELLED // badge unchanged; no record written  ",
                    GOLD + BOLD,
                    plain,
                )
            )
            if args.once:
                return 0
        except KeyboardInterrupt:
            if pass_recorded:
                print_user_visible(
                    "\nInterrupted after PASS; PASS already recorded."
                )
                return 130
            failure_text = "KeyboardInterrupt: active badge operation interrupted"
            try:
                _run_factory_operation(
                    lambda: ledger.record_failure(
                        version=bundle.version,
                        bundle_sha256=bundle.bundle_sha256,
                        phase="factory",
                        error=failure_text,
                        game_seed=current_role,
                    )
                )
            except Exception as record_exc:
                print_user_visible(
                    paint(
                        f"  RECORD FAILURE // "
                        f"{_public_exception(record_exc)}  ",
                        RED + BOLD,
                        plain,
                    )
                )
            print_user_visible()
            print_user_visible(
                paint(
                    f"  FAIL // {failure_text}  ",
                    RED + BOLD,
                    plain,
                )
            )
            print_user_visible(
                paint(
                    "  Badge is NOT approved. Leave it in the rework bin.  ",
                    RED,
                    plain,
                )
            )
            return 130
        except Exception as exc:
            failure_text = _public_exception(exc)
            try:
                _run_factory_operation(
                    lambda: ledger.record_failure(
                        version=bundle.version,
                        bundle_sha256=bundle.bundle_sha256,
                        phase="factory",
                        error=failure_text,
                        game_seed=current_role,
                    )
                )
            except Exception as record_exc:
                print_user_visible(
                    paint(
                        f"  RECORD FAILURE // "
                        f"{_public_exception(record_exc)}  ",
                        RED + BOLD,
                        plain,
                    )
                )
            print_user_visible()
            print_user_visible(
                paint(
                    f"  FAIL // {failure_text}  ",
                    RED + BOLD,
                    plain,
                )
            )
            print_user_visible(
                paint(
                    "  Badge is NOT approved. Leave it in the rework bin.  ",
                    RED,
                    plain,
                )
            )
            if args.once:
                return 1
        if args.once:
            return 0
        try:
            _prompt_operator(
                paint(
                    "Press ENTER after the BADGE is removed ",
                    GOLD,
                    plain,
                )
            )
        except (EOFError, KeyboardInterrupt):
            print_user_visible("\nInterrupted after PASS; PASS already recorded.")
            return 130
