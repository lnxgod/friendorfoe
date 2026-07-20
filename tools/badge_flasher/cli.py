"""Operator-facing badge factory state machine."""

from __future__ import annotations

import argparse
import tempfile
import time
import urllib.error
from pathlib import Path

from .bundles import (
    FactoryBundle,
    fetch_github_bundles,
    is_strictly_newer,
    load_bundle,
    select_bundle,
)
from .devices import DeviceBackend, DeviceError, usb_jtag_app_reset
from .flash import FlashEngine
from .models import BatchResult, FlashEvidence, TopologyAssignment
from .probe import discover_topology, rebind_probe_ports
from .records import ManufacturingLedger
from .verify import runtime_evidence, wait_for_runtime


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


def paint(text: str, color: str, plain: bool) -> str:
    return text if plain else f"{color}{text}{RESET}"


def banner(plain: bool) -> None:
    if not plain:
        print("\x1b[2J\x1b[H", end="")
    lines = ART.splitlines()
    palette = (BLUE, CYAN, PURPLE, GOLD)
    for index, line in enumerate(lines):
        print(paint(line, palette[index % len(palette)], plain))
    print()


def phase(label: str, detail: str, plain: bool) -> None:
    print(f"{paint('[' + label + ']', CYAN + BOLD, plain)} {detail}", flush=True)


def choose_bundle(args: argparse.Namespace, plain: bool) -> FactoryBundle:
    embedded = load_bundle(RESOURCE, source="embedded")
    if args.bundle:
        operator = load_bundle(args.bundle, source="operator")
        if is_strictly_newer(embedded.version, operator.version) and not args.allow_downgrade:
            raise ValueError(
                f"operator bundle {operator.version} is older than embedded "
                f"{embedded.version}; --allow-downgrade is required"
            )
        phase("BUNDLE", f"operator bundle {operator.version} {operator.bundle_sha256[:12]}", plain)
        return operator
    if args.offline:
        phase("BUNDLE", f"offline embedded {embedded.version} {embedded.bundle_sha256[:12]}", plain)
        return embedded
    try:
        with tempfile.TemporaryDirectory(prefix="fof-factory-releases-") as temp:
            releases = fetch_github_bundles(REPOSITORY, Path(temp))
            chosen = select_bundle(embedded, releases)
            # GitHub bundles are extracted into independent temp roots by load_bundle.
            phase("BUNDLE", f"{chosen.source} {chosen.version} {chosen.bundle_sha256[:12]}", plain)
            return chosen
    except (OSError, urllib.error.URLError, TimeoutError, ValueError) as exc:
        phase("BUNDLE", f"GitHub unavailable ({exc}); using validated embedded {embedded.version}", plain)
        return embedded


def run_one(
    args: argparse.Namespace, plain: bool, bundle: FactoryBundle | None = None,
    *, forbidden_macs: set[str] | None = None,
    known_passed_macs: set[str] | None = None,
) -> BatchResult:
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
            "previous badge is still connected; unplug all three prior boards: "
            + ", ".join(sorted(repeated))
        )
    historical = set(devices) & set(known_passed_macs or ())
    if historical:
        if not args.allow_rework:
            raise DeviceError(
                "previously passed MACs require explicit --allow-rework confirmation: "
                + ", ".join(sorted(historical))
            )
        phase(
            "WARN",
            "previously passed MACs detected (intentional rework only): "
            + ", ".join(sorted(historical)),
            plain,
        )
    for device in devices.values():
        if device.flash_size != "8MB" or device.psram_size != "8MB":
            raise DeviceError(
                f"{device.mac}: expected 8MB flash + 8MB PSRAM, got "
                f"{device.flash_size} flash + {device.psram_size} PSRAM"
            )
        phase("IDENT", f"{device.mac}  {device.port}  S3 {device.revision}  8MB+8MB", plain)

    engine = FlashEngine()
    phase("PROBE", "loading disposable topology firmware", plain)
    for device in sorted(devices.values(), key=lambda item: item.mac):
        engine.flash_and_verify(device, bundle, "probe")
        phase("VERIFY", f"probe write/readback PASS {device.mac}", plain)

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
    phase("GRAPH", f"UPLINK {assignment.uplink_mac}", plain)
    phase("GRAPH", f"BLE    {assignment.ble_leaf_mac}", plain)
    phase("GRAPH", f"WIFI   {assignment.wifi_leaf_mac}", plain)

    evidence: list[FlashEvidence] = []
    production_order = (
        ("scanner", assignment.ble_leaf_mac, "BLE scanner"),
        ("scanner", assignment.wifi_leaf_mac, "Wi-Fi scanner"),
        ("uplink", assignment.uplink_mac, "uplink/display"),
    )
    for role, mac, label in production_order:
        # USB paths are not identities. Every reset can renumber macOS ports,
        # so re-enter ROM and bind all three immutable MACs before each erase.
        current = backend.rebind(set(devices), timeout_s=30)
        phase("FLASH", f"{label} {mac} -> {bundle.version}", plain)
        result = engine.flash_and_verify(current[mac], bundle, role)
        evidence.append(result)
        phase("VERIFY", f"write + explicit readback PASS {mac}", plain)

    phase("REBIND", "proving all post-flash USB identities by ROM MAC", plain)
    rebound = backend.rebind(set(devices), timeout_s=30)
    # ROM probing leaves every board in its bootloader. Start both scanner
    # leaves first so their UART listeners are waiting before the uplink sends
    # its one-time abort, profile, and ready commands.
    usb_jtag_app_reset(rebound[assignment.ble_leaf_mac].port)
    usb_jtag_app_reset(rebound[assignment.wifi_leaf_mac].port)
    time.sleep(2)
    # wait_for_runtime opens the uplink native console, which performs the
    # same USB_UART_CHIP_RESET, then waits for the full badge boot window.
    phase("HEALTH", "waiting for uplink USB status and both scanner radios", plain)
    runtime = wait_for_runtime(
        rebound[assignment.uplink_mac], assignment, bundle.version, timeout_s=75
    )
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
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="FoF three-board badge factory flasher")
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
    result.add_argument("--records", type=Path, default=Path.home() / "Documents/FoF Badge Factory")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    plain = args.plain
    banner(plain)
    ledger = ManufacturingLedger(args.records)
    known_passed_macs = ledger.passed_macs()
    last_badge_macs: set[str] = set()
    try:
        bundle = choose_bundle(args, plain)
    except Exception as exc:
        print(paint(f"  FAIL // bundle selection: {exc}  ", RED + BOLD, plain))
        return 1
    while True:
        if not args.yes:
            input(paint("Plug one complete three-board badge into USB, then press ENTER ", GOLD + BOLD, plain))
        try:
            result = run_one(
                args,
                plain,
                bundle,
                forbidden_macs=last_badge_macs,
                known_passed_macs=known_passed_macs,
            )
            ledger.record(result)
            last_badge_macs = {
                result.assignment.uplink_mac,
                result.assignment.ble_leaf_mac,
                result.assignment.wifi_leaf_mac,
            }
            known_passed_macs.update(last_badge_macs)
            print()
            print(paint(f"  PASS // BADGE {result.badge_id} // {result.version}  ", GREEN + BOLD, plain))
            print(paint("  Remove the badge. Factory evidence has been fsync'd.  ", GREEN, plain))
        except KeyboardInterrupt:
            print("\nInterrupted; no PASS record written.")
            return 130
        except Exception as exc:
            try:
                ledger.record_failure(
                    version=bundle.version,
                    bundle_sha256=bundle.bundle_sha256,
                    phase="factory",
                    error=f"{type(exc).__name__}: {exc}",
                )
            except OSError as record_exc:
                print(paint(f"  RECORD FAILURE // {record_exc}  ", RED + BOLD, plain))
            print()
            print(paint(f"  FAIL // {type(exc).__name__}: {exc}  ", RED + BOLD, plain))
            print(paint("  Badge is NOT approved. Leave it in the rework bin.  ", RED, plain))
            if args.once:
                return 1
        if args.once:
            return 0
        input(paint("Press ENTER when the next badge is connected ", GOLD, plain))
