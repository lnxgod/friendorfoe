#!/usr/bin/env python3
"""Gate canary scanner flash harness.

This is intentionally narrow: it proves the gate node before any fleet work.
It uses the backend scanner rollout path first, then optionally falls back to
the older uplink relay on the selected gate slot only.
"""

from __future__ import annotations

import argparse
import builtins
import json
import sys
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

import fof_flash


DEFAULT_NODE = "gate"
DEFAULT_BACKEND = "http://127.0.0.1:8000"


def print(*args, **kwargs):  # noqa: A001 - keep script output unbuffered in non-TTY runs.
    kwargs.setdefault("flush", True)
    return builtins.print(*args, **kwargs)


def _json_loads(raw: bytes):
    text = raw.decode("utf-8", "replace")
    try:
        return json.loads(text)
    except ValueError:
        return {"raw": text}


def request_json(method: str, url: str, *, timeout: int = 30) -> tuple[int, dict]:
    req = Request(url, data=(b"" if method == "POST" else None), method=method)
    try:
        with urlopen(req, timeout=timeout) as resp:
            payload = _json_loads(resp.read())
            return resp.status, payload if isinstance(payload, dict) else {"body": payload}
    except HTTPError as exc:
        payload = _json_loads(exc.read())
        return exc.code, payload if isinstance(payload, dict) else {"body": payload}


def get_json(url: str, *, timeout: int = 30) -> dict:
    status, payload = request_json("GET", url, timeout=timeout)
    if status != 200:
        raise RuntimeError(f"GET {url} failed status={status} body={payload}")
    return payload


def post_json(url: str, *, timeout: int = 30) -> tuple[int, dict]:
    return request_json("POST", url, timeout=timeout)


def scanner_slots(uart: str) -> list[str]:
    if uart == "both":
        return ["ble", "wifi"]
    return [uart]


def find_node(backend: str, device_id: str) -> dict:
    data = get_json(f"{backend}/detections/nodes/status", timeout=10)
    for node in data.get("nodes", []):
        if node.get("device_id") == device_id:
            return node
    raise RuntimeError(f"{device_id} is not reporting in /detections/nodes/status")


def find_scanner(node: dict, uart: str) -> dict:
    for scanner in node.get("scanners") or []:
        if scanner.get("uart") == uart:
            return scanner
    raise RuntimeError(f"{node.get('device_id')} has no {uart} scanner slot in heartbeat")


def scanner_version(scanner: dict) -> str:
    return str(scanner.get("ver") or scanner.get("firmware_version") or scanner.get("version") or "")


def readiness(backend: str, device_id: str, uart: str, firmware_name: str) -> dict:
    query = urlencode({
        "device_id": device_id,
        "uart": uart,
        "firmware_name": firmware_name,
    })
    return get_json(f"{backend}/nodes/firmware/scanner/readiness?{query}", timeout=20)


def stage_target(backend: str, device_id: str, firmware_name: str) -> dict:
    query = urlencode({
        "device_id": device_id,
        "firmware_name": firmware_name,
    })
    status, payload = post_json(
        f"{backend}/nodes/firmware/scanner/stage-fleet?{query}",
        timeout=300,
    )
    if status != 200:
        raise RuntimeError(f"stage-fleet failed status={status} body={payload}")
    return payload


def run_safe_canary(backend: str, device_id: str, uart: str, firmware_name: str) -> dict:
    query = urlencode({
        "mode": "canary",
        "canary_device_id": device_id,
        "canary_uart": uart,
        "firmware_name": firmware_name,
    })
    status, payload = post_json(f"{backend}/nodes/firmware/scanner/rollout?{query}", timeout=30)
    if status != 200:
        return {
            "ok": False,
            "status": "request_failed",
            "status_code": status,
            "error": payload.get("detail") or payload.get("error") or payload,
            "payload": payload,
        }

    rollout_id = payload.get("rollout_id")
    deadline = time.time() + 270
    while rollout_id and time.time() < deadline:
        status, rollout = request_json(
            "GET",
            f"{backend}/nodes/firmware/rollouts/{rollout_id}",
            timeout=20,
        )
        if status != 200:
            return {
                "ok": False,
                "status": "poll_failed",
                "status_code": status,
                "error": rollout.get("detail") or rollout.get("error") or rollout,
            }
        if rollout.get("task_done"):
            return rollout
        time.sleep(2)

    return {
        "ok": False,
        "status": "timeout",
        "rollout_id": rollout_id,
        "error": "rollout_poll_timeout",
    }


def rollout_slot_result(rollout: dict, device_id: str, uart: str) -> dict:
    return (rollout.get("targets") or {}).get(f"{device_id}/{uart}") or {}


def run_legacy_relay(
    backend: str,
    device_id: str,
    ip: str,
    uart: str,
    bin_path: Path,
    target_version: str,
    *,
    allow_direct_legacy: bool,
) -> bool:
    modes = ["staged", "staged-legacy"]
    if allow_direct_legacy:
        modes.append("direct-legacy")
    for mode in modes:
        try:
            ok, body = fof_flash._flash_once(ip, uart, mode, bin_path)
        except SystemExit as exc:
            print(f"[flash] {mode} aborted: {exc}")
            ok = False
            body = {}
        if not ok:
            continue
        if fof_flash.wait_for_scanner_version(backend, device_id, ip, uart, target_version):
            return True
        print(f"[flash] {mode} transfer was not version-verified; treating as failed.")
    print(f"[flash] legacy relay modes failed; direct-legacy attempted={allow_direct_legacy}")
    return False


def wait_for_slot_telemetry(
    backend: str,
    device_id: str,
    uart: str,
    firmware_name: str,
    target_version: str,
    *,
    timeout_s: int = 180,
) -> tuple[bool, dict, dict]:
    deadline = time.time() + timeout_s
    last_scanner: dict = {}
    last_ready: dict = {}
    target_norm = fof_flash._norm_version(target_version)
    while time.time() < deadline:
        node = find_node(backend, device_id)
        scanner = find_scanner(node, uart)
        ready = readiness(backend, device_id, uart, firmware_name)
        last_scanner = scanner
        last_ready = ready
        fw_checks = int(scanner.get("fw_check_count") or 0)
        blockers = []
        if ready.get("scanners"):
            blockers = ready["scanners"][0].get("blockers") or []
        if (
            fof_flash._norm_version(scanner_version(scanner)) == target_norm
            and fw_checks > 0
            and "scanner_command_ingress_unreachable" not in blockers
        ):
            return True, scanner, ready
        time.sleep(3)
    return False, last_scanner, last_ready


def print_gate_preflight(
    backend: str,
    node_alias: str,
    scanner_key: str,
    slots: list[str],
) -> tuple[str, str, str, Path]:
    health = get_json(f"{backend}/health", timeout=10)
    print(f"[health] {health}")

    device_id, ip = fof_flash.resolve_node(backend, node_alias)
    firmware_name = fof_flash.FW_NAMES[scanner_key]
    bin_path = fof_flash.FW_PATHS[scanner_key]
    target_version = fof_flash.current_repo_firmware_version()
    print(f"[resolve] {node_alias} -> {device_id} / {ip}")
    print(f"[target] {firmware_name} {target_version} bin={bin_path}")
    if not bin_path.exists():
        raise RuntimeError(f"firmware bin missing: {bin_path}")

    fw_info = get_json(f"http://{ip}/api/fw/info", timeout=10)
    print(f"[uplink fw] {fw_info}")

    node = find_node(backend, device_id)
    print(
        f"[heartbeat] age={node.get('age_s')}s fw={node.get('firmware_version')} "
        f"name={node.get('name')} ip={node.get('ip')}"
    )
    for slot in slots:
        sc = find_scanner(node, slot)
        print(
            f"[scanner {slot}] ver={scanner_version(sc)} board={sc.get('board')} "
            f"cmd_rx={sc.get('cmd_rx', 0)} fw_checks={sc.get('fw_check_count', 0)} "
            f"fw_state={sc.get('fw_state', '')}"
        )
    ready = readiness(backend, device_id, "both" if len(slots) > 1 else slots[0], firmware_name)
    print(f"[readiness] {json.dumps(ready, sort_keys=True)}")
    return device_id, ip, target_version, bin_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", default=DEFAULT_BACKEND)
    parser.add_argument("--node", default=DEFAULT_NODE)
    parser.add_argument("--scanner", choices=sorted(fof_flash.FW_PATHS), default="s3-combo-seed")
    parser.add_argument("--uart", choices=("ble", "wifi", "both"), default="both")
    parser.add_argument("--execute", action="store_true", help="Actually stage/trigger/flash the gate canary")
    parser.add_argument("--allow-legacy", action="store_true", help="Allow the older relay fallback on gate only")
    parser.add_argument(
        "--allow-direct-legacy",
        action="store_true",
        help="Also try the raw /api/ota/relay stream; this can hang old uplinks and is off by default",
    )
    args = parser.parse_args()

    slots = scanner_slots(args.uart)
    firmware_name = fof_flash.FW_NAMES[args.scanner]
    try:
        device_id, ip, target_version, bin_path = print_gate_preflight(
            args.backend,
            args.node,
            args.scanner,
            slots,
        )
    except (RuntimeError, URLError) as exc:
        print(f"[preflight] FAILED: {exc}")
        return 2

    if not args.execute:
        print("[dry-run] preflight complete; re-run with --execute to touch hardware.")
        return 0

    print("[stage] staging target firmware on gate uplink")
    try:
        stage = stage_target(args.backend, device_id, firmware_name)
    except (RuntimeError, URLError) as exc:
        print(f"[stage] FAILED: {exc}")
        return 3
    print(f"[stage] {json.dumps(stage, sort_keys=True)}")

    for slot in slots:
        print(f"[canary {slot}] trying safe scanner self-update path")
        rollout = run_safe_canary(args.backend, device_id, slot, firmware_name)
        result = rollout_slot_result(rollout, device_id, slot)
        print(f"[canary {slot}] rollout={json.dumps(rollout, sort_keys=True)}")
        if result.get("state") == "verified":
            ok, scanner, ready = wait_for_slot_telemetry(
                args.backend,
                device_id,
                slot,
                firmware_name,
                target_version,
            )
            if ok:
                print(f"[canary {slot}] verified by heartbeat: {scanner}")
                continue
            print(f"[canary {slot}] version matched but telemetry proof is missing: {scanner} {ready}")
            return 4

        error = str(result.get("error") or rollout.get("error") or rollout.get("status") or "")
        if not args.allow_legacy:
            print(f"[canary {slot}] blocked without legacy fallback: {error}")
            return 5

        print(f"[canary {slot}] safe path blocked/failed ({error}); trying gate-only legacy relay")
        legacy_ok = run_legacy_relay(
            args.backend,
            device_id,
            ip,
            slot,
            bin_path,
            target_version,
            allow_direct_legacy=args.allow_direct_legacy,
        )
        if not legacy_ok:
            print(f"[canary {slot}] legacy relay failed or lacked version proof")
            return 6

        ok, scanner, ready = wait_for_slot_telemetry(
            args.backend,
            device_id,
            slot,
            firmware_name,
            target_version,
        )
        if not ok:
            print(f"[canary {slot}] updated version did not prove robust updater telemetry: {scanner} {ready}")
            return 7
        print(f"[canary {slot}] legacy recovery succeeded and robust updater telemetry is alive: {scanner}")

    status, trigger = post_json(
        f"{args.backend}/nodes/firmware/scanner/trigger-check?"
        f"{urlencode({'device_id': device_id, 'uart': 'both'})}",
        timeout=30,
    )
    print(f"[post-check] trigger status={status} body={trigger}")
    final_ready = readiness(args.backend, device_id, "both", firmware_name)
    print(f"[final readiness] {json.dumps(final_ready, sort_keys=True)}")
    if final_ready.get("needs_usb_recovery_count"):
        return 8
    print("[done] gate canary passed; fleet can be split into remote-ready and USB-recovery buckets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
