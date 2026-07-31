"""Production USB identity and runtime health gates."""

from __future__ import annotations

import json
import time
from collections.abc import Callable, Sequence
from typing import Any

from .models import SeedRebootProof, TopologyAssignment, UsbDevice
from .topology import normalize_mac


class VerificationError(RuntimeError):
    pass


class _RetryableApplicationPort(VerificationError):
    """The candidate may be booting, stale, or re-enumerating."""


GAME_SEEDS = ("normal", "infected", "immune")
UPLINK_TARGET = "uplink-s3-fof_badge"
APPLICATION_ATTEMPT_TIMEOUT_S = 1.0
_EXPECTED_REBOOT_MAGIC = 0xF0F0B007
_UINT32_MAX = 0xFFFFFFFF


def _exact_uint32(value: object, *, nonzero: bool = False) -> bool:
    return (
        type(value) is int
        and 0 <= value <= _UINT32_MAX
        and (not nonzero or value != 0)
    )


def _expected_reboot_successor(prior_generation: int) -> int:
    if not _exact_uint32(prior_generation):
        raise VerificationError("seed proof reboot generation is invalid")
    successor = (prior_generation + 1) & _UINT32_MAX
    if successor == 0:
        successor = 1
    if successor == _EXPECTED_REBOOT_MAGIC:
        successor += 1
    return successor


def verify_status(
    status: dict[str, Any],
    assignment: TopologyAssignment,
    version: str,
    game_seed: str,
    *,
    reboot_proof: SeedRebootProof | None = None,
    expected_target: str = UPLINK_TARGET,
) -> dict[str, Any]:
    if game_seed not in GAME_SEEDS:
        raise VerificationError(f"unsupported game seed: {game_seed!r}")
    verify_preseed_runtime(
        status,
        assignment,
        version,
        expected_target=expected_target,
    )
    status_hardware_id = normalize_mac(str(status["hardware_id"]))
    if reboot_proof is not None:
        if status_hardware_id != normalize_mac(reboot_proof.hardware_id):
            raise VerificationError("uplink hardware ID does not match seed proof")
        if not _exact_uint32(reboot_proof.pre_reboot_generation):
            raise VerificationError("seed proof reboot generation is invalid")
        if not _exact_uint32(
            reboot_proof.pre_reboot_responses_completed,
            nonzero=True,
        ):
            raise VerificationError(
                "seed proof pre-reboot response counter is invalid"
            )
    generation = status.get("last_expected_reboot_generation")
    if not _exact_uint32(generation, nonzero=True):
        raise VerificationError("uplink reboot generation is missing or invalid")
    if (
        reboot_proof is not None
        and generation != _expected_reboot_successor(
            reboot_proof.pre_reboot_generation
        )
    ):
        raise VerificationError(
            "uplink reboot generation is not the exact successor"
        )
    if status.get("last_expected_reboot_reason") != "usb_reboot":
        raise VerificationError("uplink status is not from the seeded reboot")
    for field in ("game_seed", "game_state"):
        if status.get(field) != game_seed:
            raise VerificationError(
                f"uplink {field} mismatch for selected game seed"
            )
    if status.get("game_active") is not False:
        raise VerificationError("uplink game must be inactive after factory seed")
    shield = status.get("game_shield")
    if type(shield) is not int or shield != 0:
        raise VerificationError("uplink game shield must be integer zero")
    return status


def verify_preseed_runtime(
    status: dict[str, Any],
    assignment: TopologyAssignment,
    version: str,
    *,
    expected_target: str = UPLINK_TARGET,
) -> dict[str, Any]:
    """Verify the accepted graph before mutating its selected game role."""
    expected_uplink = {
        "version": version,
        "target": expected_target,
        "firmware_name": expected_target,
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
    }
    for field, wanted in expected_uplink.items():
        got = status.get(field)
        if got != wanted:
            raise VerificationError(f"uplink {field} mismatch: got {got!r}, wanted {wanted!r}")
    try:
        status_hardware_id = normalize_mac(str(status.get("hardware_id")))
    except ValueError as exc:
        raise VerificationError("uplink hardware ID is missing or invalid") from exc
    if status_hardware_id != normalize_mac(assignment.uplink_mac):
        raise VerificationError("uplink hardware ID does not match assignment")
    if status.get("safe_mode") is not False or status.get("recovery_mode") != "normal":
        raise VerificationError("uplink is in safe/recovery mode")
    usb_health = status.get("usb_health")
    if not isinstance(usb_health, dict):
        raise VerificationError("uplink USB response counter is missing")
    responses_completed = usb_health.get("responses_completed")
    if not _exact_uint32(responses_completed, nonzero=True):
        raise VerificationError("uplink USB response counter is not fresh")
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
        if (info.get("ver") or info.get("version")) != version:
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
        "game_seed", "game_state", "game_active", "game_shield",
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


def _default_serial_factory(port: str) -> Any:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise VerificationError(
            "pyserial is required for runtime verification"
        ) from exc
    # Native USB-Serial/JTAG applies modem-line state while opening. Build a
    # closed handle first so neither DTR nor RTS can create an accidental
    # reset edge during application-port discovery.
    handle = serial.Serial(
        port=None,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1,
    )
    try:
        handle.dtr = False
        handle.rts = False
        handle.port = port
        handle.open()
        return handle
    except Exception:
        try:
            handle.close()
        except OSError:
            pass
        raise


def _descriptor_bound_serial_factory(
    hardware_id: str,
) -> Callable[[str], Any]:
    """Build a reset-neutral opener restricted to one immutable uplink ID."""
    try:
        from scripts import usb_descriptor_binding
    except ImportError as exc:
        raise VerificationError(
            "descriptor-bound application transport is unavailable"
        ) from exc
    try:
        trusted_serial = usb_descriptor_binding.canonical_usb_serial(
            hardware_id
        )
    except usb_descriptor_binding.UsbDescriptorBindingError as exc:
        raise VerificationError(
            "descriptor-bound uplink identity is invalid"
        ) from exc

    def open_candidate(port: str) -> Any:
        try:
            matches = [
                record
                for record in
                usb_descriptor_binding.take_usb_descriptor_census()
                if record.device == port
                and record.serial_number == trusted_serial
            ]
            if len(matches) != 1:
                raise _RetryableApplicationPort(
                    "candidate is not the descriptor-bound uplink"
                )
            return usb_descriptor_binding.open_bound_application_serial(
                matches[0],
                expected_uplink_serial=trusted_serial,
                baudrate=115200,
                timeout=0.1,
                write_timeout=1,
            )
        except usb_descriptor_binding.UsbDescriptorBindingError as exc:
            raise _RetryableApplicationPort(
                "descriptor-bound uplink open failed"
            ) from exc

    return open_candidate


def _open_native_console(
    port: str,
    serial_factory: Callable[[str], Any] | None,
) -> Any:
    factory = serial_factory or _default_serial_factory
    return factory(port)


def _default_candidate_ports() -> list[str]:
    # Deliberately perform only application-port enumeration here. ROM/esptool
    # probing would reset candidates and invalidate the reboot proof.
    import glob

    from .devices import PORT_PATTERNS

    return sorted({
        port
        for pattern in PORT_PATTERNS
        for port in glob.glob(pattern)
    })


def _protocol_line(raw: bytes) -> str:
    """Decode one LF-delimited line, trimming only the CR from CRLF."""
    return raw.decode("utf-8", "replace").removesuffix("\r")


def _safe_close(handle: Any | None) -> None:
    if handle is None:
        return
    try:
        handle.close()
    except OSError:
        pass


def _await_exact_response(
    handle: Any,
    expected: str,
    *,
    deadline: float,
    phase: str,
) -> None:
    buffer = bytearray()
    while time.monotonic() < deadline:
        chunk = handle.read(1024)
        if chunk:
            buffer.extend(chunk)
        while b"\n" in buffer:
            raw, _, remainder = buffer.partition(b"\n")
            buffer[:] = remainder
            line = _protocol_line(raw)
            if line == expected:
                return
            if line.startswith("FOF_ERROR:"):
                raise VerificationError(
                    f"{phase} rejected by firmware: {line}"
                )
            if (
                line.startswith("FOF_OK:") or
                line.startswith("FOF_REBOOT:") or
                line.startswith("FOF_PONG:")
            ):
                raise VerificationError(
                    f"{phase} acknowledgment mismatch: {line}"
                )
        if len(buffer) > 64 * 1024:
            buffer.clear()
        time.sleep(0.02)
    raise _RetryableApplicationPort(
        f"{phase} timed out waiting for {expected}"
    )


def _status_generation(
    status: dict[str, Any],
    *,
    phase: str,
    require_nonzero: bool,
) -> int:
    generation = status.get("last_expected_reboot_generation")
    if (
        type(generation) is not int
        or generation < 0
        or (require_nonzero and generation == 0)
    ):
        raise VerificationError(f"{phase} reboot generation is invalid")
    return generation


def _require_status_identity(
    status: dict[str, Any],
    *,
    hardware_id: str,
    version: str,
    expected_target: str,
    phase: str,
) -> None:
    expected = {
        "version": version,
        "target": expected_target,
        "firmware_name": expected_target,
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
    }
    for field, wanted in expected.items():
        if status.get(field) != wanted:
            raise VerificationError(
                f"{phase} {field} mismatch"
            )
    try:
        got_hardware_id = normalize_mac(str(status.get("hardware_id")))
    except ValueError as exc:
        raise VerificationError(
            f"{phase} hardware ID is missing or invalid"
        ) from exc
    if got_hardware_id != normalize_mac(hardware_id):
        raise VerificationError(f"{phase} hardware ID mismatch")


def _query_fresh_status(
    handle: Any,
    *,
    hardware_id: str,
    version: str,
    expected_target: str,
    deadline: float,
    phase: str,
) -> dict[str, Any]:
    handle.write(b"FOF_PING\nFOF_STATUS\n")
    buffer = bytearray()
    pong_seen = False
    exact_pong = f"FOF_PONG:{version}"
    while time.monotonic() < deadline:
        chunk = handle.read(1024)
        if chunk:
            buffer.extend(chunk)
        while b"\n" in buffer:
            raw, _, remainder = buffer.partition(b"\n")
            buffer[:] = remainder
            line = _protocol_line(raw)
            if line == exact_pong:
                pong_seen = True
                continue
            if line.startswith("FOF_PONG:"):
                raise VerificationError(f"{phase} PONG version mismatch")
            if line == "FOF_ERROR:booting":
                raise _RetryableApplicationPort(
                    f"{phase} rejected while firmware is booting"
                )
            if line.startswith("FOF_ERROR:"):
                raise VerificationError(
                    f"{phase} rejected by firmware: {line}"
                )
            if not line.startswith("FOF_STATUS:"):
                continue
            if not pong_seen:
                # Contaminated input cannot share a handle with the proof.
                # Close it and retry the whole query on a clean no-reset open.
                raise _RetryableApplicationPort(
                    f"{phase} received status before fresh PONG"
                )
            try:
                status = json.loads(line[len("FOF_STATUS:"):])
            except json.JSONDecodeError as exc:
                raise VerificationError(
                    f"{phase} returned malformed status"
                ) from exc
            if not isinstance(status, dict):
                raise VerificationError(
                    f"{phase} returned non-object status"
                )
            _require_status_identity(
                status,
                hardware_id=hardware_id,
                version=version,
                expected_target=expected_target,
                phase=phase,
            )
            return status
        if len(buffer) > 256 * 1024:
            buffer.clear()
        time.sleep(0.02)
    raise _RetryableApplicationPort(
        f"{phase} timed out waiting for fresh status"
    )


def _candidate_snapshot(
    candidate_ports: Callable[[], Sequence[str]],
) -> list[str]:
    return sorted({
        str(port)
        for port in candidate_ports()
        if str(port)
    })


def provision_game_seed(
    uplink: UsbDevice,
    game_seed: str,
    version: str,
    *,
    timeout_s: float = 30,
    serial_factory: Callable[[str], Any] | None = None,
    candidate_ports: Callable[[], Sequence[str]] | None = None,
    expected_target: str = UPLINK_TARGET,
) -> SeedRebootProof:
    if game_seed not in GAME_SEEDS:
        raise VerificationError(f"unsupported game seed: {game_seed!r}")
    ports = candidate_ports or _default_candidate_ports
    descriptor_bound = serial_factory is None
    open_serial = (
        serial_factory
        if serial_factory is not None
        else _descriptor_bound_serial_factory(uplink.mac)
    )
    deadline = time.monotonic() + timeout_s
    last_error = "uplink application port not found"
    matching_error: str | None = None
    while time.monotonic() < deadline:
        try:
            candidates = _candidate_snapshot(ports)
        except OSError as exc:
            last_error = f"application-port enumeration failed: {exc}"
            time.sleep(0.05)
            continue
        for port in candidates:
            handle = None
            bound = False
            opened_expected = False
            try:
                handle = _open_native_console(port, open_serial)
                opened_expected = True
                handle.reset_input_buffer()
                query_deadline = min(
                    deadline,
                    time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S,
                )
                _query_fresh_status(
                    handle,
                    hardware_id=uplink.mac,
                    version=version,
                    expected_target=expected_target,
                    deadline=query_deadline,
                    phase="uplink pre-seed identity",
                )
                bound = True

                # Clear any trailing response bytes before the mutation so an
                # earlier acknowledgment can never satisfy this transaction.
                handle.reset_input_buffer()
                seed_command = (
                    f"FOF_SET:game_seed={game_seed}\n".encode("ascii")
                )
                handle.write(seed_command)
                _await_exact_response(
                    handle,
                    "FOF_OK:game_seed",
                    deadline=min(
                        deadline,
                        time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S,
                    ),
                    phase="game seed",
                )

                # The reboot generation must be sampled only after the exact
                # seed acknowledgment and behind a new exact PONG.
                handle.reset_input_buffer()
                before_reboot = _query_fresh_status(
                    handle,
                    hardware_id=uplink.mac,
                    version=version,
                    expected_target=expected_target,
                    deadline=min(
                        deadline,
                        time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S,
                    ),
                    phase="uplink pre-reboot status",
                )
                if before_reboot.get("game_seed") != game_seed:
                    raise VerificationError(
                        "uplink pre-reboot status did not retain game seed"
                    )
                generation = _status_generation(
                    before_reboot,
                    phase="uplink pre-reboot status",
                    require_nonzero=False,
                )
                usb_health = before_reboot.get("usb_health")
                responses_completed = (
                    usb_health.get("responses_completed")
                    if isinstance(usb_health, dict)
                    else None
                )
                if not _exact_uint32(responses_completed, nonzero=True):
                    raise VerificationError(
                        "uplink pre-reboot status response counter is invalid"
                    )

                handle.reset_input_buffer()
                handle.write(b"FOF_REBOOT\n")
                _await_exact_response(
                    handle,
                    "FOF_REBOOT:OK",
                    deadline=min(
                        deadline,
                        time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S,
                    ),
                    phase="seed reboot",
                )
                return SeedRebootProof(
                    hardware_id=normalize_mac(uplink.mac),
                    pre_reboot_generation=generation,
                    pre_reboot_responses_completed=responses_completed,
                    old_port=port,
                )
            except (OSError, _RetryableApplicationPort) as exc:
                if opened_expected:
                    matching_error = str(exc)
                else:
                    last_error = str(exc)
            except (VerificationError, ValueError) as exc:
                if opened_expected:
                    matching_error = str(exc)
                else:
                    last_error = str(exc)
                if bound or (descriptor_bound and opened_expected):
                    raise VerificationError(str(exc)) from exc
            finally:
                _safe_close(handle)
        time.sleep(0.05)
    detail = matching_error or last_error
    raise VerificationError(
        f"game seed provisioning timed out: {detail}"
    )


def wait_for_preseed_runtime(
    uplink: UsbDevice,
    assignment: TopologyAssignment,
    version: str,
    *,
    timeout_s: float = 60,
    serial_factory: Callable[[str], Any] | None = None,
    candidate_ports: Callable[[], Sequence[str]] | None = None,
    expected_target: str = UPLINK_TARGET,
) -> dict[str, Any]:
    """Poll the exact accepted uplink and prove its graph before mutation."""
    if normalize_mac(uplink.mac) != normalize_mac(assignment.uplink_mac):
        raise VerificationError("pre-seed uplink does not belong to badge graph")
    ports = candidate_ports or _default_candidate_ports
    open_serial = (
        serial_factory
        if serial_factory is not None
        else _descriptor_bound_serial_factory(uplink.mac)
    )
    deadline = time.monotonic() + timeout_s
    last_error = "no fresh matching uplink status received"
    while time.monotonic() < deadline:
        try:
            candidates = _candidate_snapshot(ports)
        except OSError as exc:
            last_error = f"application-port enumeration failed: {exc}"
            time.sleep(0.05)
            continue
        for port in candidates:
            handle = None
            try:
                handle = _open_native_console(port, open_serial)
                handle.reset_input_buffer()
                status = _query_fresh_status(
                    handle,
                    hardware_id=uplink.mac,
                    version=version,
                    expected_target=expected_target,
                    deadline=min(
                        deadline,
                        time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S,
                    ),
                    phase="uplink pre-seed status",
                )
                return verify_preseed_runtime(
                    status,
                    assignment,
                    version,
                    expected_target=expected_target,
                )
            except (OSError, VerificationError, ValueError) as exc:
                last_error = str(exc)
            finally:
                _safe_close(handle)
        time.sleep(0.05)
    raise VerificationError(f"pre-seed runtime gate timed out: {last_error}")


def wait_for_runtime(
    uplink: UsbDevice,
    assignment: TopologyAssignment,
    version: str,
    game_seed: str,
    *,
    reboot_proof: SeedRebootProof,
    timeout_s: float = 60,
    serial_factory: Callable[[str], Any] | None = None,
    candidate_ports: Callable[[], Sequence[str]] | None = None,
    expected_target: str = UPLINK_TARGET,
) -> dict[str, Any]:
    if normalize_mac(reboot_proof.hardware_id) != normalize_mac(uplink.mac):
        raise VerificationError("seed proof does not belong to uplink device")
    if normalize_mac(reboot_proof.hardware_id) != normalize_mac(
        assignment.uplink_mac
    ):
        raise VerificationError("seed proof does not belong to badge graph")
    if (
        not _exact_uint32(reboot_proof.pre_reboot_generation)
        or not _exact_uint32(
            reboot_proof.pre_reboot_responses_completed,
            nonzero=True,
        )
        or not reboot_proof.old_port
    ):
        raise VerificationError("seed proof is malformed")

    ports = candidate_ports or _default_candidate_ports
    open_serial = (
        serial_factory
        if serial_factory is not None
        else _descriptor_bound_serial_factory(uplink.mac)
    )
    deadline = time.monotonic() + timeout_s

    # Native USB-Serial/JTAG may retain the same /dev path across an
    # application reboot. The path is not identity or reboot evidence. Open a
    # new reset-neutral descriptor-bound session and require a fresh PONG plus
    # the exact successor reboot generation in verify_status() instead.
    last_error = "no fresh matching uplink status received"
    while time.monotonic() < deadline:
        try:
            candidates = _candidate_snapshot(ports)
        except OSError as exc:
            last_error = f"application-port enumeration failed: {exc}"
            time.sleep(0.05)
            continue
        for port in candidates:
            handle = None
            try:
                handle = _open_native_console(port, open_serial)
                handle.reset_input_buffer()
                status = _query_fresh_status(
                    handle,
                    hardware_id=reboot_proof.hardware_id,
                    version=version,
                    expected_target=expected_target,
                    deadline=min(
                        deadline,
                        time.monotonic() + APPLICATION_ATTEMPT_TIMEOUT_S,
                    ),
                    phase="uplink post-reboot status",
                )
                return verify_status(
                    status,
                    assignment,
                    version,
                    game_seed,
                    reboot_proof=reboot_proof,
                    expected_target=expected_target,
                )
            except (OSError, VerificationError, ValueError) as exc:
                last_error = str(exc)
            finally:
                _safe_close(handle)
        time.sleep(0.05)
    raise VerificationError(f"runtime gate timed out: {last_error}")
