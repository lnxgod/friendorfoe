"""Fresh USB, blank-config, graph-health, and reboot PASS gates."""

from __future__ import annotations

import json
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from tools.badge_flasher.models import TopologyAssignment, UsbDevice
from tools.badge_flasher.topology import normalize_mac
from tools.backend_lite_usb_fixture import (
    LITE_IDENTITY,
    ProtocolError,
    RedactedConfig,
    STATUS_MAX_BYTES,
    parse_config,
    parse_status,
)

from .bundles import LiteFactoryBundle
from .models import LiteRuntimeSnapshot


class VerificationError(RuntimeError):
    """A freshly flashed Lite assembly did not prove its factory contract."""


class _RetryablePort(VerificationError):
    pass


APPLICATION_ATTEMPT_TIMEOUT_S = 3.0
FACTORY_BACKEND_URL = "http://192.168.4.2:8000"
REQUIRED_CAPABILITIES = frozenset({
    "display_none",
    "usb_live",
    "usb_live_ack",
    "usb_buffered",
    "usb_config",
    "http_uplink",
    "config_ap",
    "ap_dashboard",
    "remote_ota",
    "production_scanner_uart",
})
SCANNER_TARGET = "scanner-s3-combo-fof_badge"
SCANNER_PROJECT = "fof_badge_scanner"
SCANNER_HARDWARE = "seeed_xiao_esp32s3"


def _no_duplicate_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError(f"duplicate runtime JSON member: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise VerificationError(f"invalid runtime JSON number: {value}")


def _parse_record(line: str, prefix: str) -> dict[str, Any]:
    if not line.startswith(prefix + " "):
        raise VerificationError(f"expected {prefix}")
    try:
        value = json.loads(
            line[len(prefix) + 1:],
            object_pairs_hook=_no_duplicate_object,
            parse_constant=_reject_constant,
        )
    except VerificationError:
        raise
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise VerificationError(f"{prefix} is malformed") from exc
    if not isinstance(value, dict):
        raise VerificationError(f"{prefix} is not an object")
    return value


class _LineReader:
    def __init__(self, handle: Any) -> None:
        self.handle = handle
        self.buffer = bytearray()

    def line(self, deadline: float) -> str | None:
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                raw, _, remainder = self.buffer.partition(b"\n")
                self.buffer[:] = remainder
                if len(raw) > STATUS_MAX_BYTES:
                    raise VerificationError("runtime USB line is oversized")
                try:
                    return raw.removesuffix(b"\r").decode("utf-8", "strict")
                except UnicodeDecodeError as exc:
                    raise VerificationError("runtime USB line is not UTF-8") from exc
            chunk = self.handle.read(2048)
            if chunk:
                self.buffer.extend(chunk)
                if len(self.buffer) > STATUS_MAX_BYTES * 2:
                    raise VerificationError("runtime USB transcript is oversized")
                continue
            time.sleep(0.01)
        return None


def _write_exact(handle: Any, value: bytes) -> None:
    written = handle.write(value)
    if written is not None and written != len(value):
        raise VerificationError("runtime USB command was partially written")


def _request_status(
    handle: Any,
    *,
    expected_version: str,
    deadline: float,
) -> dict[str, Any]:
    handle.reset_input_buffer()
    _write_exact(handle, b"FOF_PING\nFOF_STATUS\n")
    reader = _LineReader(handle)
    exact_pong = f"FOF_PONG:{expected_version}"
    pong_seen = False
    while time.monotonic() < deadline:
        line = reader.line(deadline)
        if line is None:
            break
        if line == exact_pong:
            pong_seen = True
            continue
        if line.startswith("FOF_PONG:"):
            raise VerificationError("Lite PONG version mismatch")
        if line == "FOF_ERROR:booting":
            raise _RetryablePort("Lite firmware is still booting")
        if line.startswith("FOF_ERROR:"):
            raise VerificationError(f"Lite status rejected: {line}")
        if not line.startswith("FOF_STATUS:"):
            continue
        if not pong_seen:
            raise _RetryablePort("Lite status arrived before the fresh PONG")
        try:
            return parse_status(line)
        except ProtocolError as exc:
            raise VerificationError(f"Lite status contract failed: {exc}") from exc
    raise _RetryablePort("timed out waiting for fresh Lite status")


def _request_config(handle: Any, *, deadline: float) -> RedactedConfig:
    handle.reset_input_buffer()
    _write_exact(handle, b"FOF_CONFIG_GET\n")
    reader = _LineReader(handle)
    while time.monotonic() < deadline:
        line = reader.line(deadline)
        if line is None:
            break
        if line.startswith("FOF_ERROR:"):
            raise VerificationError(f"Lite config query rejected: {line}")
        if not line.startswith("FOF_CONFIG:"):
            continue
        try:
            return parse_config(line)
        except ProtocolError as exc:
            raise VerificationError(f"Lite config contract failed: {exc}") from exc
    raise _RetryablePort("timed out waiting for redacted Lite config")


def _request_boot_health(
    handle: Any,
    *,
    deadline: float,
) -> tuple[dict[str, Any], dict[str, Any]]:
    handle.reset_input_buffer()
    _write_exact(handle, b"FOF_BACKEND_STATUS\n")
    reader = _LineReader(handle)
    boot: dict[str, Any] | None = None
    health: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        line = reader.line(deadline)
        if line is None:
            break
        if line.startswith("FOF_BACKEND_BOOT "):
            if boot is not None:
                raise VerificationError("duplicate Lite boot evidence")
            boot = _parse_record(line, "FOF_BACKEND_BOOT")
        elif line.startswith("FOF_BACKEND_HEALTH "):
            if health is not None:
                raise VerificationError("duplicate Lite health evidence")
            health = _parse_record(line, "FOF_BACKEND_HEALTH")
        elif line.startswith("FOF_ERROR:"):
            raise VerificationError(f"Lite health query rejected: {line}")
        if boot is not None and health is not None:
            return boot, health
    raise _RetryablePort("timed out waiting for Lite boot and health evidence")


def _canonical_runtime_mac(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise VerificationError(f"{label} is missing")
    try:
        return normalize_mac(value)
    except ValueError as exc:
        raise VerificationError(f"{label} is invalid") from exc


def _require_exact_identity(
    value: Mapping[str, Any],
    expected: Mapping[str, str],
    *,
    label: str,
) -> None:
    for key, wanted in expected.items():
        if value.get(key) != wanted:
            raise VerificationError(f"{label} {key} mismatch")


def _require_uint(value: object, label: str, *, nonzero: bool = False) -> int:
    if (
        type(value) is not int
        or value < 0
        or value > 0xFFFFFFFFFFFFFFFF
        or (nonzero and value == 0)
    ):
        raise VerificationError(f"{label} is invalid")
    return value


def _verify_blank_config(config: RedactedConfig, uplink_mac: str) -> dict[str, Any]:
    expected_device = "uplink_" + uplink_mac.replace(":", "")[-6:]
    if config.schema_version != 1 or config.generation != 1:
        raise VerificationError("Lite factory config schema/generation mismatch")
    if config.networks:
        raise VerificationError("Lite factory config contains a Wi-Fi network")
    if config.backend_url != FACTORY_BACKEND_URL:
        raise VerificationError("Lite factory backend URL is not the default")
    if config.device_id != expected_device or config.display_name != expected_device:
        raise VerificationError("Lite factory device/display identity mismatch")
    if not config.ap_password_set:
        raise VerificationError("Lite factory AP password is not provisioned")
    if config.auto_update_enabled:
        raise VerificationError("Lite factory auto-update must start disabled")
    if (
        config.has_location
        or config.latitude is not None
        or config.longitude is not None
        or config.altitude_m is not None
    ):
        raise VerificationError("Lite factory location must start unset")
    return asdict(config)


def _verify_scanners(
    status: Mapping[str, Any],
    assignment: TopologyAssignment,
    scanner_version: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    compact = status.get("scanner")
    summaries = status.get("scanner_summaries")
    if not isinstance(compact, list) or len(compact) != 2:
        raise VerificationError("Lite compact scanner inventory is incomplete")
    if not isinstance(summaries, list) or len(summaries) != 2:
        raise VerificationError("Lite scanner summaries are incomplete")
    expected_macs = (assignment.ble_leaf_mac, assignment.wifi_leaf_mac)
    boots: set[int] = set()
    result: list[dict[str, Any]] = []
    for slot, expected_mac in enumerate(expected_macs):
        compact_item = compact[slot]
        summary = summaries[slot]
        if not isinstance(compact_item, dict) or not isinstance(summary, dict):
            raise VerificationError(f"scanner slot {slot} is malformed")
        for source in (compact_item, summary):
            if source.get("slot") != slot:
                raise VerificationError(f"scanner slot {slot} is out of order")
            if source.get("connected") is not True:
                raise VerificationError(f"scanner slot {slot} is disconnected")
            if source.get("identity_valid") is not True:
                raise VerificationError(f"scanner slot {slot} identity is invalid")
        if summary.get("status_available") is not True:
            raise VerificationError(f"scanner slot {slot} status is unavailable")
        if summary.get("protocol") != "production_uart":
            raise VerificationError(f"scanner slot {slot} is not production UART")
        identity = summary.get("identity")
        if not isinstance(identity, dict):
            raise VerificationError(f"scanner slot {slot} identity is missing")
        _require_exact_identity(
            identity,
            {
                "target": SCANNER_TARGET,
                "project": SCANNER_PROJECT,
                "hardware": SCANNER_HARDWARE,
                "version": scanner_version,
            },
            label=f"scanner slot {slot}",
        )
        actual_mac = _canonical_runtime_mac(
            identity.get("hardware_id"),
            f"scanner slot {slot} hardware ID",
        )
        if actual_mac != normalize_mac(expected_mac):
            raise VerificationError(f"scanner slot {slot} MAC/topology mismatch")
        boot_id = _require_uint(
            identity.get("boot_id"),
            f"scanner slot {slot} boot ID",
            nonzero=True,
        )
        if boot_id in boots:
            raise VerificationError("scanner boot IDs are not unique")
        boots.add(boot_id)
        if summary.get("profile") != slot + 1:
            raise VerificationError(f"scanner slot {slot} profile mismatch")
        health = summary.get("health")
        if not isinstance(health, dict) or any(
            health.get(key) is not True
            for key in ("command", "radio", "role_acked")
        ):
            raise VerificationError(f"scanner slot {slot} health did not converge")
        errors = summary.get("errors")
        if not isinstance(errors, dict) or any(
            errors.get(key) != 0 for key in ("rx", "tx_drops")
        ):
            raise VerificationError(f"scanner slot {slot} transport is not clean")
        _require_uint(
            summary.get("uptime_ms"),
            f"scanner slot {slot} uptime",
            nonzero=True,
        )
        result.append({
            "slot": slot,
            "mac": actual_mac,
            "boot_id": boot_id,
            "profile": summary["profile"],
            "identity": {
                key: identity[key]
                for key in ("target", "project", "hardware", "version")
            },
            "health": dict(health),
            "errors": dict(errors),
            "uptime_ms": summary["uptime_ms"],
        })
    return result[0], result[1]


def _record_identity(version: str) -> dict[str, str]:
    return {
        "product_family": "badge_lite",
        "firmware_line": "backend",
        "component": "uplink",
        "target": "uplink-s3-backend",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "version": version,
    }


def verify_runtime_snapshot(
    *,
    status: dict[str, Any],
    config: RedactedConfig,
    boot: dict[str, Any],
    health: dict[str, Any],
    assignment: TopologyAssignment,
    bundle: LiteFactoryBundle,
) -> LiteRuntimeSnapshot:
    expected_uplink = {
        "product_family": LITE_IDENTITY[0],
        "target": LITE_IDENTITY[1],
        "project": LITE_IDENTITY[2],
        "hardware": LITE_IDENTITY[3],
        "version": bundle.version,
    }
    _require_exact_identity(status, expected_uplink, label="Lite uplink")
    if status.get("firmware_name") != LITE_IDENTITY[1]:
        raise VerificationError("Lite uplink firmware alias mismatch")
    if status.get("app_project") != LITE_IDENTITY[2]:
        raise VerificationError("Lite uplink project alias mismatch")
    if status.get("hardware_type") != LITE_IDENTITY[3]:
        raise VerificationError("Lite uplink hardware alias mismatch")
    expected_mac = normalize_mac(assignment.uplink_mac)
    for field in ("mac", "hardware_id"):
        if _canonical_runtime_mac(status.get(field), f"Lite uplink {field}") != expected_mac:
            raise VerificationError(f"Lite uplink {field}/topology mismatch")
    boot_id = _require_uint(status.get("boot_id"), "Lite uplink boot ID", nonzero=True)
    if status.get("mode") != "headless":
        raise VerificationError("Lite uplink is not headless")
    capabilities = status.get("capabilities")
    if not isinstance(capabilities, list) or not REQUIRED_CAPABILITIES <= frozenset(capabilities):
        raise VerificationError("Lite uplink capabilities are incomplete")
    if status.get("config_generation") != 1:
        raise VerificationError("Lite status is not factory config generation 1")
    wifi = status.get("wifi")
    recovery = status.get("recovery")
    if not isinstance(wifi, dict) or wifi != {
        "configured": False,
        "connected": False,
        "full_pass_failed": False,
    }:
        raise VerificationError("Lite factory Wi-Fi state is not blank")
    if not isinstance(recovery, dict) or recovery != {
        "reason": "wifi_unconfigured",
        "ap_running": True,
    }:
        raise VerificationError("Lite recovery AP did not converge")
    if status.get("ota_ready") is not True:
        raise VerificationError("Lite OTA subsystem is not ready")
    usb = status.get("usb")
    if (
        not isinstance(usb, dict)
        or usb.get("available") is not True
        or usb.get("host_connected") is not True
        or usb.get("output_poisoned") is not False
        or usb.get("required_failures") != 0
    ):
        raise VerificationError("Lite USB transport is not healthy")

    config_evidence = _verify_blank_config(config, expected_mac)
    scanners = _verify_scanners(status, assignment, bundle.scanner_version)
    record_identity = _record_identity(bundle.version)
    for label, record in (("boot", boot), ("health", health)):
        _require_exact_identity(record, record_identity, label=f"Lite {label}")
        if _canonical_runtime_mac(record.get("mac"), f"Lite {label} MAC") != expected_mac:
            raise VerificationError(f"Lite {label} MAC mismatch")
        if record.get("boot_id") != boot_id:
            raise VerificationError(f"Lite {label} boot ID mismatch")
        if record.get("device_id") != config.device_id:
            raise VerificationError(f"Lite {label} device ID mismatch")
        if record.get("config_state") != "loaded" or record.get("config_generation") != 1:
            raise VerificationError(f"Lite {label} config state mismatch")
        if record.get("auto_update_enabled") is not False:
            raise VerificationError(f"Lite {label} auto-update state mismatch")
        if record.get("uart0_started") is not True or record.get("uart1_started") is not True:
            raise VerificationError(f"Lite {label} UART workers are not running")
        if record.get("network_state") != "ap":
            raise VerificationError(f"Lite {label} network state is not recovery AP")
    if boot.get("nvs_erased") is not False:
        raise VerificationError("Lite boot reports erased NVS")
    if boot.get("ota_state") not in {"valid", "pending_verify"}:
        raise VerificationError("Lite boot OTA state is unsafe")
    if (
        health.get("nvs_loaded") is not True
        or health.get("nvs_erased") is not False
        or health.get("coordinator_started") is not True
        or health.get("rollback_clear") is not True
    ):
        raise VerificationError("Lite final health/rollback proof failed")

    sanitized_status = {
        "identity": expected_uplink,
        "mac": expected_mac,
        "boot_id": boot_id,
        "mode": status["mode"],
        "config_generation": status["config_generation"],
        "capabilities": sorted(frozenset(capabilities)),
        "wifi": dict(wifi),
        "recovery": dict(recovery),
        "ota_ready": status["ota_ready"],
        "usb": {
            key: usb.get(key)
            for key in (
                "available", "host_connected", "required_failures",
                "output_poisoned", "bytes_transmitted", "bytes_received",
            )
        },
        "scanners": list(scanners),
    }
    return LiteRuntimeSnapshot(
        status=sanitized_status,
        config=config_evidence,
        boot=dict(boot),
        health=dict(health),
    )


def _default_candidate_ports() -> list[str]:
    import glob

    from tools.badge_flasher.devices import PORT_PATTERNS

    return sorted({
        port
        for pattern in PORT_PATTERNS
        for port in glob.glob(pattern)
    })


def _descriptor_bound_serial_factory(hardware_id: str) -> Callable[[str], Any]:
    try:
        from scripts import usb_descriptor_binding
    except ImportError as exc:
        raise VerificationError("descriptor-bound USB support is unavailable") from exc
    try:
        trusted_serial = usb_descriptor_binding.canonical_usb_serial(hardware_id)
    except usb_descriptor_binding.UsbDescriptorBindingError as exc:
        raise VerificationError("Lite uplink descriptor identity is invalid") from exc

    def open_candidate(port: str) -> Any:
        try:
            matches = [
                record
                for record in usb_descriptor_binding.take_usb_descriptor_census()
                if record.device == port and record.serial_number == trusted_serial
            ]
            if len(matches) != 1:
                raise _RetryablePort("candidate is not the descriptor-bound Lite uplink")
            return usb_descriptor_binding.open_bound_application_serial(
                matches[0],
                expected_uplink_serial=trusted_serial,
                baudrate=921600,
                timeout=0.1,
                write_timeout=1,
            )
        except usb_descriptor_binding.UsbDescriptorBindingError as exc:
            raise _RetryablePort("descriptor-bound Lite uplink open failed") from exc

    return open_candidate


def _safe_close(handle: Any | None) -> None:
    if handle is None:
        return
    try:
        handle.close()
    except OSError:
        pass


def _query_once(
    handle: Any,
    *,
    assignment: TopologyAssignment,
    bundle: LiteFactoryBundle,
    deadline: float,
) -> LiteRuntimeSnapshot:
    status = _request_status(
        handle,
        expected_version=bundle.version,
        deadline=deadline,
    )
    config = _request_config(handle, deadline=deadline)
    boot, health = _request_boot_health(handle, deadline=deadline)
    return verify_runtime_snapshot(
        status=status,
        config=config,
        boot=boot,
        health=health,
        assignment=assignment,
        bundle=bundle,
    )


def _scanner_boots(snapshot: LiteRuntimeSnapshot) -> tuple[int, int]:
    scanners = snapshot.status["scanners"]
    return int(scanners[0]["boot_id"]), int(scanners[1]["boot_id"])


def _verify_stable_samples(
    first: LiteRuntimeSnapshot,
    second: LiteRuntimeSnapshot,
) -> None:
    if first.status["boot_id"] != second.status["boot_id"]:
        raise _RetryablePort("Lite uplink rebooted during stable proof")
    if _scanner_boots(first) != _scanner_boots(second):
        raise _RetryablePort("Lite scanner rebooted during stable proof")
    if first.config != second.config or first.boot != second.boot or first.health != second.health:
        raise _RetryablePort("Lite boot/config evidence changed during stable proof")
    first_scanners = first.status["scanners"]
    second_scanners = second.status["scanners"]
    for slot in range(2):
        if second_scanners[slot]["uptime_ms"] < first_scanners[slot]["uptime_ms"]:
            raise _RetryablePort(f"scanner slot {slot} uptime regressed")


def wait_for_stable_runtime(
    uplink: UsbDevice,
    assignment: TopologyAssignment,
    bundle: LiteFactoryBundle,
    *,
    timeout_s: float = 75,
    serial_factory: Callable[[str], Any] | None = None,
    candidate_ports: Callable[[], Sequence[str]] | None = None,
) -> LiteRuntimeSnapshot:
    if normalize_mac(uplink.mac) != normalize_mac(assignment.uplink_mac):
        raise VerificationError("runtime uplink does not belong to the Lite graph")
    ports = candidate_ports or _default_candidate_ports
    opener = serial_factory or _descriptor_bound_serial_factory(uplink.mac)
    deadline = time.monotonic() + timeout_s
    last_error = "no fresh matching Lite runtime received"
    while time.monotonic() < deadline:
        try:
            candidates = sorted({str(port) for port in ports() if str(port)})
        except OSError as exc:
            last_error = f"application-port enumeration failed: {exc}"
            time.sleep(0.05)
            continue
        for port in candidates:
            handle = None
            try:
                handle = opener(port)
                first = _query_once(
                    handle,
                    assignment=assignment,
                    bundle=bundle,
                    deadline=min(deadline, time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S),
                )
                time.sleep(0.25)
                second = _query_once(
                    handle,
                    assignment=assignment,
                    bundle=bundle,
                    deadline=min(deadline, time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S),
                )
                _verify_stable_samples(first, second)
                return second
            except (OSError, VerificationError, ValueError) as exc:
                last_error = str(exc)
            finally:
                _safe_close(handle)
        time.sleep(0.05)
    raise VerificationError(f"Lite runtime gate timed out: {last_error}")


def verify_reboot_transition(
    before: LiteRuntimeSnapshot,
    after: LiteRuntimeSnapshot,
) -> None:
    if before.config != after.config:
        raise VerificationError("Lite blank configuration changed across reboot")
    if before.status["identity"] != after.status["identity"]:
        raise VerificationError("Lite uplink identity changed across reboot")
    if before.status["mac"] != after.status["mac"]:
        raise VerificationError("Lite uplink MAC changed across reboot")
    if before.status["boot_id"] == after.status["boot_id"]:
        raise VerificationError("Lite uplink boot ID did not change")
    before_scanners = before.status["scanners"]
    after_scanners = after.status["scanners"]
    for slot in range(2):
        for key in ("slot", "mac", "profile", "identity"):
            if before_scanners[slot][key] != after_scanners[slot][key]:
                raise VerificationError(f"scanner slot {slot} binding changed across reboot")
        if before_scanners[slot]["boot_id"] == after_scanners[slot]["boot_id"]:
            raise VerificationError(f"scanner slot {slot} boot ID did not change")


def runtime_evidence(
    before: LiteRuntimeSnapshot,
    after: LiteRuntimeSnapshot,
) -> dict[str, Any]:
    """Retain identity/health proof without RF observations or secrets."""

    return {
        "first": {
            "status": dict(before.status),
            "config": dict(before.config),
            "boot": dict(before.boot),
            "health": dict(before.health),
        },
        "reboot": {
            "status": dict(after.status),
            "config": dict(after.config),
            "boot": dict(after.boot),
            "health": dict(after.health),
        },
    }
