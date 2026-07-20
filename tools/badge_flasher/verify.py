"""Production USB identity and runtime health gates."""

from __future__ import annotations

import json
import time
from collections.abc import Callable
from typing import Any

from .models import TopologyAssignment, UsbDevice
from .topology import normalize_mac


class VerificationError(RuntimeError):
    pass


def verify_status(
    status: dict[str, Any], assignment: TopologyAssignment, version: str
) -> dict[str, Any]:
    expected_uplink = {
        "version": version,
        "firmware_name": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
    }
    for field, wanted in expected_uplink.items():
        got = status.get(field)
        if str(got).lstrip("v") != str(wanted).lstrip("v"):
            raise VerificationError(f"uplink {field} mismatch: got {got!r}, wanted {wanted!r}")
    if status.get("safe_mode") is not False or status.get("recovery_mode") != "normal":
        raise VerificationError("uplink is in safe/recovery mode")
    for field in ("usb_control_alive", "scanner_uart_alive"):
        if status.get(field) is not True:
            raise VerificationError(f"uplink runtime health failed: {field}")
    for field in ("display_alive", "power_converged", "scanner_power_converged"):
        if status.get(field) is not True:
            raise VerificationError(f"uplink runtime convergence failed: {field}")
    if int(status.get("psram_total", 0)) < 8 * 1024 * 1024:
        raise VerificationError("uplink PSRAM health proof is missing")

    expected = {"ble": assignment.ble_leaf_mac, "wifi": assignment.wifi_leaf_mac}
    by_uart = {item.get("uart"): item for item in status.get("scanners", []) if isinstance(item, dict)}
    for slot, wanted_mac in expected.items():
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            raise VerificationError(f"{slot} scanner is not connected")
        fields = {
            "firmware_name": "scanner-s3-combo-fof_badge",
            "app_project": "fof_badge_scanner",
            "hardware_type": "seeed_xiao_esp32s3",
        }
        for field, wanted in fields.items():
            if info.get(field) != wanted:
                raise VerificationError(f"{slot} scanner {field} mismatch")
        if str(info.get("ver") or info.get("version")).lstrip("v") != version.lstrip("v"):
            raise VerificationError(f"{slot} scanner version mismatch")
        if normalize_mac(str(info.get("hardware_id"))) != normalize_mac(wanted_mac):
            raise VerificationError(f"{slot} scanner MAC mismatch")
        if info.get("rollback_pending") is not False or info.get("recovery_mode") != "normal":
            raise VerificationError(f"{slot} scanner rollback/recovery is active")
        if info.get("role_acked") is not True:
            raise VerificationError(f"{slot} scanner role is not acknowledged")
        if info.get("health") != "ok":
            raise VerificationError(f"{slot} scanner health is not ok")
        expected_profile = "ble_primary" if slot == "ble" else "wifi_primary"
        if info.get("scan_profile") != expected_profile:
            raise VerificationError(f"{slot} scanner profile mismatch")
        if slot == "ble" and info.get("ble_scanning") is not True:
            raise VerificationError("BLE scanner radio is not active")
        if slot == "wifi" and info.get("wifi_active") is not True:
            raise VerificationError("Wi-Fi scanner radio is not active")
    return status


def runtime_evidence(status: dict[str, Any]) -> dict[str, Any]:
    """Retain factory proof without logging nearby RF/device observations."""
    uplink_fields = (
        "version", "firmware_name", "app_project", "hardware_type",
        "safe_mode", "recovery_mode", "crash_count", "display_alive",
        "usb_control_alive", "scanner_uart_alive", "power_converged",
        "scanner_power_converged", "psram_total", "psram_free",
    )
    result: dict[str, Any] = {
        field: status.get(field) for field in uplink_fields
    }
    scanner_fields = (
        "uart", "connected", "hardware_id", "firmware_name", "app_project",
        "hardware_type", "ver", "health", "recovery_mode", "rollback_pending",
        "role_acked", "slot_role", "scan_profile", "radios_ready",
        "ble_initialized", "ble_scanning", "ble_host_active", "ble_host_synced",
        "wifi_initialized", "wifi_init_rc", "wifi_active", "wifi_paused",
    )
    result["scanners"] = [
        {field: item.get(field) for field in scanner_fields}
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    ]
    return result


def wait_for_runtime(
    uplink: UsbDevice, assignment: TopologyAssignment, version: str,
    *, timeout_s: float = 60, serial_factory: Callable[[str], Any] | None = None,
) -> dict[str, Any]:
    if serial_factory is None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise VerificationError("pyserial is required for runtime verification") from exc
        serial_factory = lambda port: serial.Serial(
            port, 115200, timeout=0.1, write_timeout=1
        )
    handle = serial_factory(uplink.port)
    handle.dtr = False
    handle.rts = False
    # Opening native USB-Serial/JTAG resets the ESP32-S3. The badge needs
    # roughly four seconds to initialize PSRAM, LCD, UART tasks, and scanner
    # roles. Discard boot logs before issuing machine-control commands.
    time.sleep(5)
    handle.reset_input_buffer()
    deadline = time.monotonic() + timeout_s
    last_error = "no status received"
    buffer = bytearray()
    try:
        while time.monotonic() < deadline:
            handle.write(b"FOF_PING\nFOF_STATUS\n")
            poll_deadline = min(deadline, time.monotonic() + 5)
            while time.monotonic() < poll_deadline:
                chunk = handle.read(1024)
                if chunk:
                    buffer.extend(chunk)
                while b"\n" in buffer:
                    raw, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    line = raw.decode("utf-8", "replace").strip()
                    if not line.startswith("FOF_STATUS:"):
                        continue
                    try:
                        status = json.loads(line[len("FOF_STATUS:"):])
                        return verify_status(status, assignment, version)
                    except (json.JSONDecodeError, VerificationError, ValueError) as exc:
                        last_error = str(exc)
                if len(buffer) > 256 * 1024:
                    buffer.clear()
                time.sleep(0.02)
            time.sleep(1)
        raise VerificationError(f"runtime gate timed out: {last_error}")
    finally:
        handle.close()
