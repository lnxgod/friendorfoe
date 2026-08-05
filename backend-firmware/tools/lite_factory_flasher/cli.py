"""Operator-facing Backend Badge Lite factory state machine."""

from __future__ import annotations

import argparse
import re
import secrets
import sys
import tempfile
import time
import urllib.error
from collections.abc import Callable
from dataclasses import replace
from pathlib import Path
from typing import TypeVar

from tools.badge_flasher.devices import DeviceBackend, DeviceError, usb_jtag_app_reset
from tools.badge_flasher.flash import FlashEngine
from tools.badge_flasher.models import (
    FlashEvidence,
    TopologyAssignment,
    UsbDevice,
)
from tools.badge_flasher.probe import discover_topology, rebind_probe_ports
from tools.badge_flasher.public_output import (
    capture_user_visible_output,
    print_live_user_visible as _print_live_user_visible,
)

from .bundles import (
    BundleError,
    LiteFactoryBundle,
    fetch_github_bundles,
    is_trusted_release_bundle,
    is_strictly_newer,
    load_bundle,
    select_bundle,
)
from .models import LiteBatchResult, PassedLiteFactoryRecord
from .public_output import scrub_lite_transcript
from .records import LiteManufacturingLedger
from .verify import (
    VerificationError,
    runtime_evidence,
    verify_reboot_transition,
    wait_for_stable_runtime,
)


REPOSITORY = "lnxgod/friendorfoe"
RESOURCE = (
    Path(__file__).with_name("resources")
    / "lite-factory-flasher-embedded.zip"
)

RESET = "\x1b[0m"
BOLD = "\x1b[1m"
CYAN = "\x1b[38;5;51m"
BLUE = "\x1b[38;5;33m"
GOLD = "\x1b[38;5;220m"
GREEN = "\x1b[38;5;46m"
RED = "\x1b[38;5;196m"
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
T = TypeVar("T")

ART = r"""
       +--------------------------------------------------+
       | GAMECHANGERS AI // BACKEND BADGE LITE FACTORY  |
       +--------------------------------------------------+
                 [ BLE SCANNER ]
                         \
                    [ LITE UPLINK ]
                         /
                 [ WIFI SCANNER ]

          THREE BOARDS // PROVEN GRAPH // READBACK
"""


def print_user_visible(
    value: object = "",
    *,
    file=None,
    end: str = "\n",
    flush: bool = False,
) -> None:
    print(
        scrub_lite_transcript(value),
        file=file,
        end=end,
        flush=flush,
    )


class _CapturedOperationError(RuntimeError):
    def __init__(self, error_type: str, error: str) -> None:
        super().__init__(error)
        self.error_type = error_type


def _run_factory_operation(operation: Callable[[], T]) -> T:
    captured = capture_user_visible_output(
        operation,
        scrubber=scrub_lite_transcript,
    )
    if captured.stdout:
        print_user_visible(captured.stdout, end="")
    if captured.stderr:
        print_user_visible(captured.stderr, file=sys.stderr, end="")
    if captured.error_type is not None:
        if captured.error_type == "KeyboardInterrupt":
            raise KeyboardInterrupt
        raise _CapturedOperationError(
            captured.error_type,
            captured.error or captured.error_type,
        )
    return captured.result  # type: ignore[return-value]


def _public_exception(exc: BaseException) -> str:
    if isinstance(exc, _CapturedOperationError):
        return f"{exc.error_type}: {exc}"
    return f"{type(exc).__name__}: {scrub_lite_transcript(exc)}"


def _prompt_operator(message: str) -> str:
    _print_live_user_visible(
        scrub_lite_transcript(message),
        end="",
        flush=True,
    )
    return input()


def paint(text: str, color: str, plain: bool) -> str:
    return text if plain else f"{color}{text}{RESET}"


def banner(plain: bool) -> None:
    if not plain:
        print_user_visible("\x1b[2J\x1b[H", end="")
    palette = (BLUE, CYAN, GOLD)
    for index, line in enumerate(ART.splitlines()):
        print_user_visible(paint(line, palette[index % len(palette)], plain))
    print_user_visible()


def phase(label: str, detail: str, plain: bool) -> None:
    _print_live_user_visible(
        scrub_lite_transcript(
            f"{paint('[' + label + ']', CYAN + BOLD, plain)} {detail}"
        ),
        flush=True,
    )


def prompt_lite_confirmation(plain: bool) -> None:
    print_user_visible(paint(
        "FACTORY FLASH ERASES ALL THREE BOARDS.\n"
        "Use this only for a HEADLESS BACKEND BADGE LITE assembly.\n"
        "Do not connect a native/full badge or a configured field unit.",
        GOLD + BOLD,
        plain,
    ))
    answer = _prompt_operator(
        paint("TYPE LITE TO ARM THIS UNIT > ", GOLD + BOLD, plain)
    ).strip()
    if answer != "LITE":
        raise KeyboardInterrupt


def generate_receipt() -> str:
    return "lite_" + "".join(
        secrets.choice(CROCKFORD) for _ in range(8)
    )


def _assignment_macs(assignment: TopologyAssignment) -> set[str]:
    return {
        assignment.uplink_mac,
        assignment.ble_leaf_mac,
        assignment.wifi_leaf_mac,
    }


def _prior_pass(
    device_macs: set[str],
    records: tuple[PassedLiteFactoryRecord, ...],
    *,
    version: str,
    bundle_sha256: str,
) -> PassedLiteFactoryRecord | None:
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
        "connected Lite hardware intersects a different prior PASS graph"
    )


def _require_prior_graph(
    prior: PassedLiteFactoryRecord | None,
    assignment: TopologyAssignment,
) -> None:
    if prior is not None and assignment != prior.assignment:
        raise DeviceError(
            "rework topology differs from the authoritative prior PASS graph"
        )


def choose_bundle(
    args: argparse.Namespace,
    plain: bool,
) -> LiteFactoryBundle:
    embedded = load_bundle(RESOURCE, source="embedded")
    if not is_trusted_release_bundle(embedded):
        raise BundleError("embedded Lite factory release is not trusted")
    if args.bundle:
        operator = load_bundle(args.bundle, source="operator")
        candidate_digest = getattr(args, "accept_candidate_sha256", None)
        if (
            candidate_digest is not None
            and candidate_digest != operator.bundle_sha256
        ):
            raise BundleError(
                "--accept-candidate-sha256 does not match the selected "
                "Lite bundle"
            )
        if (
            not is_trusted_release_bundle(operator)
            and candidate_digest is None
        ):
            raise BundleError(
                "operator Lite bundle is not a trusted release; pass its exact "
                "digest with --accept-candidate-sha256 for a candidate run"
            )
        if (
            is_strictly_newer(embedded.version, operator.version)
            and not args.allow_downgrade
        ):
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
        with tempfile.TemporaryDirectory(prefix="fof-lite-factory-release-") as temp:
            releases = [
                release
                for release in fetch_github_bundles(REPOSITORY, Path(temp))
                if is_trusted_release_bundle(release)
            ]
            selected = select_bundle(embedded, releases)
            phase(
                "BUNDLE",
                f"validated {selected.source} release {selected.version}",
                plain,
            )
            return selected
    except (OSError, urllib.error.URLError, TimeoutError, ValueError) as exc:
        phase(
            "BUNDLE",
            f"GitHub unavailable ({exc}); using embedded {embedded.version}",
            plain,
        )
        return embedded


def _handoff_factory_graph(
    engine: FlashEngine,
    devices: dict[str, UsbDevice],
    assignment: TopologyAssignment,
    plain: bool,
) -> None:
    for mac, label in (
        (assignment.ble_leaf_mac, "BLE-SCANNER"),
        (assignment.wifi_leaf_mac, "WIFI-SCANNER"),
        (assignment.uplink_mac, "LITE-UPLINK"),
    ):
        phase("BOOT", f"{label} force-clear/watchdog handoff", plain)
        engine.handoff_to_application(devices[mac])


def run_one(
    args: argparse.Namespace,
    plain: bool,
    bundle: LiteFactoryBundle | None = None,
    *,
    forbidden_macs: set[str] | None = None,
    passed_records: tuple[PassedLiteFactoryRecord, ...] = (),
) -> LiteBatchResult:
    bundle = bundle or choose_bundle(args, plain)
    backend = DeviceBackend()
    ports = backend.list_candidate_ports()
    phase("USB", f"found {len(ports)} candidate ports", plain)
    devices = backend.scan()
    if len(devices) != 3:
        raise DeviceError(
            f"exactly three ESP32-S3 boards required; found {len(devices)}"
        )
    repeated = set(devices) & set(forbidden_macs or ())
    if repeated:
        raise DeviceError(
            "previous Lite unit is still connected; unplug all three boards"
        )
    prior = _prior_pass(
        set(devices),
        passed_records,
        version=bundle.version,
        bundle_sha256=bundle.bundle_sha256,
    )
    if prior is not None and not getattr(args, "allow_rework", False):
        raise DeviceError(
            "previously passed Lite hardware requires explicit --allow-rework"
        )
    if prior is not None:
        phase("WARN", "previously passed Lite graph: intentional rework", plain)

    ordered = sorted(devices.values(), key=lambda item: item.mac)
    aliases = {
        device.mac: f"LITE board {index}"
        for index, device in enumerate(ordered, start=1)
    }
    for device in ordered:
        if device.flash_size != "8MB" or device.psram_size != "8MB":
            raise DeviceError(
                f"{aliases[device.mac]} expected 8MB flash + 8MB PSRAM; "
                f"got {device.flash_size} + {device.psram_size}"
            )
        phase(
            "IDENT",
            f"{aliases[device.mac]} S3 {device.revision} 8MB+8MB",
            plain,
        )

    engine = FlashEngine()
    phase("PROBE", "loading disposable topology firmware", plain)
    for device in ordered:
        engine.flash_and_verify(device, bundle, "probe")
        phase(
            "VERIFY",
            f"{aliases[device.mac]} probe write/readback PASS",
            plain,
        )

    time.sleep(1)
    probe_rom = backend.rebind(set(devices), timeout_s=30)
    for device in probe_rom.values():
        usb_jtag_app_reset(device.port)
    time.sleep(1)
    probe_devices = rebind_probe_ports(
        [device.port for device in probe_rom.values()],
        set(devices),
        timeout_s=10,
    )
    assignment = discover_topology(probe_devices.values(), timeout_s=10)
    _require_prior_graph(prior, assignment)
    phase("GRAPH", "LITE-UPLINK identified", plain)
    phase("GRAPH", "BLE-SCANNER identified", plain)
    phase("GRAPH", "WIFI-SCANNER identified", plain)

    evidence: list[FlashEvidence] = []
    production_order = (
        ("scanner", assignment.ble_leaf_mac, "BLE-SCANNER"),
        ("scanner", assignment.wifi_leaf_mac, "WIFI-SCANNER"),
        ("uplink", assignment.uplink_mac, "LITE-UPLINK"),
    )
    for role, mac, label in production_order:
        current = backend.rebind(set(devices), timeout_s=30)
        phase("FLASH", f"{label} erase/write/readback", plain)
        result = engine.flash_and_verify(current[mac], bundle, role)
        if role == "scanner":
            # The shared native-badge writer assumes one version for every
            # production role. Lite intentionally combines the accepted
            # production scanner release with a newer backend uplink, so keep
            # the manufacturing evidence truthful without changing that
            # proven low-level writer.
            result = replace(result, version=bundle.scanner_version)
        evidence.append(result)
        phase("VERIFY", f"{label} write + explicit readback PASS", plain)

    phase("REBIND", "proving post-flash USB identities by eFuse MAC", plain)
    rebound = backend.rebind(set(devices), timeout_s=30)
    _handoff_factory_graph(engine, rebound, assignment, plain)
    time.sleep(3)
    phase("HEALTH", "proving blank config and stable three-board runtime", plain)
    first = wait_for_stable_runtime(
        rebound[assignment.uplink_mac],
        assignment,
        bundle,
        timeout_s=75,
        candidate_ports=backend.list_candidate_ports,
    )

    phase("REBOOT", "resetting all three boards without reflashing", plain)
    reboot_rom = backend.rebind(set(devices), timeout_s=30)
    _handoff_factory_graph(engine, reboot_rom, assignment, plain)
    time.sleep(3)
    second = wait_for_stable_runtime(
        reboot_rom[assignment.uplink_mac],
        assignment,
        bundle,
        timeout_s=75,
        candidate_ports=backend.list_candidate_ports,
    )
    verify_reboot_transition(first, second)
    phase("DURABLE", "all boot IDs changed; config and roles remained exact", plain)

    return LiteBatchResult(
        unit_id=assignment.uplink_mac.replace(":", "")[-6:],
        version=bundle.version,
        scanner_version=bundle.scanner_version,
        bundle_sha256=bundle.bundle_sha256,
        passed=True,
        phase="complete",
        assignment=assignment,
        devices=tuple(evidence),
        runtime=runtime_evidence(first, second),
        receipt=generate_receipt(),
    )


class _FactoryArgumentError(ValueError):
    pass


class _FactoryArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise _FactoryArgumentError(message)


def _sha256_argument(value: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise argparse.ArgumentTypeError(
            "expected 64 lowercase SHA-256 hex characters"
        )
    return value


def parser() -> argparse.ArgumentParser:
    result = _FactoryArgumentParser(
        prog="lite_badge_factory.py",
        description="FoF three-board Backend Badge Lite factory flasher",
    )
    result.add_argument(
        "--bundle",
        type=Path,
        help="validated local Lite bundle ZIP",
    )
    result.add_argument(
        "--accept-candidate-sha256",
        type=_sha256_argument,
        help="exact digest required for an untrusted local candidate bundle",
    )
    release_mode = result.add_mutually_exclusive_group()
    release_mode.add_argument(
        "--offline",
        dest="offline",
        action="store_true",
        help="use the embedded release (default)",
    )
    release_mode.add_argument(
        "--online",
        dest="offline",
        action="store_false",
        help="explicitly check final GitHub backend releases",
    )
    result.set_defaults(offline=True)
    result.add_argument("--plain", action="store_true", help="disable ANSI colors")
    result.add_argument("--once", action="store_true", help="flash one Lite unit and exit")
    result.add_argument("--yes", action="store_true", help="do not wait for operator input")
    result.add_argument(
        "--confirm-product",
        choices=("badge_lite",),
        help="required with --yes to acknowledge the destructive Lite target",
    )
    result.add_argument(
        "--allow-rework",
        action="store_true",
        help="permit erase/reflash of an exact previously passed Lite graph",
    )
    result.add_argument(
        "--allow-downgrade",
        action="store_true",
        help="permit a local bundle older than the embedded release",
    )
    result.add_argument(
        "--records",
        type=Path,
        default=Path.home() / "Documents/FoF Backend Badge Lite Factory",
    )
    return result


def _run_locked_factory(
    args: argparse.Namespace,
    plain: bool,
    ledger: LiteManufacturingLedger,
) -> int:
    try:
        passed_records = _run_factory_operation(ledger.passed_records)
        bundle = _run_factory_operation(lambda: choose_bundle(args, plain))
    except Exception as exc:
        print_user_visible(
            paint(f"  FAIL // startup: {_public_exception(exc)}  ", RED + BOLD, plain)
        )
        return 1

    last_unit_macs: set[str] = set()
    while True:
        try:
            if not args.yes:
                prompt_lite_confirmation(plain)
                _prompt_operator(paint(
                    "Plug one complete LITE assembly into USB, then press ENTER ",
                    GOLD + BOLD,
                    plain,
                ))
        except (EOFError, KeyboardInterrupt):
            print_user_visible("\nInterrupted; no PASS record written.")
            return 130

        pass_recorded = False
        try:
            result = _run_factory_operation(lambda: run_one(
                args,
                plain,
                bundle,
                forbidden_macs=last_unit_macs,
                passed_records=passed_records,
            ))
            csv_recorded = _run_factory_operation(lambda: ledger.record(result))
            pass_recorded = True
            last_unit_macs = _assignment_macs(result.assignment)
            passed_records = (
                *passed_records,
                PassedLiteFactoryRecord(
                    version=result.version,
                    bundle_sha256=result.bundle_sha256,
                    assignment=result.assignment,
                ),
            )
            print_user_visible()
            print_user_visible(paint(
                f"  PASS // BACKEND BADGE LITE // RECEIPT {result.receipt}  ",
                GREEN + BOLD,
                plain,
            ))
            print_user_visible(paint(
                "  Remove all three boards. Factory evidence is fsync'd.  ",
                GREEN,
                plain,
            ))
            if not csv_recorded:
                print_user_visible(paint(
                    "  WARN // authoritative JSONL PASS committed; "
                    "CSV projection unavailable  ",
                    GOLD,
                    plain,
                ))
        except KeyboardInterrupt:
            if pass_recorded:
                print_user_visible("\nInterrupted after PASS; PASS already recorded.")
                return 130
            failure = "KeyboardInterrupt: active Lite factory operation interrupted"
            try:
                _run_factory_operation(lambda: ledger.record_failure(
                    version=bundle.version,
                    scanner_version=bundle.scanner_version,
                    bundle_sha256=bundle.bundle_sha256,
                    phase="factory",
                    error=failure,
                ))
            except Exception as record_exc:
                print_user_visible(
                    paint(
                        f"  RECORD FAILURE // {_public_exception(record_exc)}  ",
                        RED + BOLD,
                        plain,
                    )
                )
            print_user_visible(paint(f"  FAIL // {failure}  ", RED + BOLD, plain))
            return 130
        except Exception as exc:
            if pass_recorded:
                # The fsync-backed JSONL PASS is authoritative.  Do not append
                # a contradictory failure when only post-commit bookkeeping or
                # terminal output failed; stop the batch with success instead.
                return 0
            failure = _public_exception(exc)
            try:
                _run_factory_operation(lambda: ledger.record_failure(
                    version=bundle.version,
                    scanner_version=bundle.scanner_version,
                    bundle_sha256=bundle.bundle_sha256,
                    phase="factory",
                    error=failure,
                ))
            except Exception as record_exc:
                print_user_visible(
                    paint(
                        f"  RECORD FAILURE // {_public_exception(record_exc)}  ",
                        RED + BOLD,
                        plain,
                    )
                )
            print_user_visible()
            print_user_visible(paint(f"  FAIL // {failure}  ", RED + BOLD, plain))
            print_user_visible(paint(
                "  Unit is NOT approved. Place all three boards in rework.  ",
                RED,
                plain,
            ))
            if args.once:
                return 1
        if args.once:
            return 0
        try:
            _prompt_operator(paint(
                "Press ENTER after all three LITE boards are removed ",
                GOLD,
                plain,
            ))
        except (EOFError, KeyboardInterrupt):
            print_user_visible("\nInterrupted after PASS; PASS already recorded.")
            return 130


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
    except _FactoryArgumentError as exc:
        print_user_visible(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if args.yes and (
        not args.once or args.confirm_product != "badge_lite"
    ):
        print_user_visible(
            "ERROR: --yes requires --once --confirm-product badge_lite",
            file=sys.stderr,
        )
        return 2
    if args.accept_candidate_sha256 is not None and (
        args.bundle is None or not args.once
    ):
        print_user_visible(
            "ERROR: --accept-candidate-sha256 requires --bundle and --once",
            file=sys.stderr,
        )
        return 2

    plain = args.plain
    banner(plain)
    ledger = LiteManufacturingLedger(args.records)
    try:
        with ledger.exclusive_session():
            return _run_locked_factory(args, plain, ledger)
    except Exception as exc:
        print_user_visible(
            paint(
                f"  FAIL // startup: {_public_exception(exc)}  ",
                RED + BOLD,
                plain,
            )
        )
        return 1
