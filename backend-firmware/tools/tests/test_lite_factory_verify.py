from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from types import SimpleNamespace

import pytest

from tools.lite_factory_flasher.models import LiteRuntimeSnapshot
from tools.lite_factory_flasher.verify import (
    REQUIRED_CAPABILITIES,
    VerificationError,
    _verify_stable_samples,
    wait_for_stable_runtime,
    verify_reboot_transition,
    verify_runtime_snapshot,
)
from tools.badge_flasher.models import TopologyAssignment, UsbDevice
from tools.backend_lite_usb_fixture import RedactedConfig, RedactedNetwork


VERSION = "0.2.0-backend"
SCANNER_VERSION = "0.67.2-badge-defcon34"
ASSIGNMENT = TopologyAssignment(
    uplink_mac="AA:BB:CC:DD:EE:01",
    ble_leaf_mac="AA:BB:CC:DD:EE:02",
    wifi_leaf_mac="AA:BB:CC:DD:EE:03",
)
BUNDLE = SimpleNamespace(version=VERSION, scanner_version=SCANNER_VERSION)


def _config() -> RedactedConfig:
    return RedactedConfig(
        schema_version=1,
        generation=1,
        networks=(),
        backend_url="http://192.168.4.2:8000",
        device_id="uplink_DDEE01",
        display_name="uplink_DDEE01",
        ap_password_set=True,
        auto_update_enabled=False,
        has_location=False,
        latitude=None,
        longitude=None,
        altitude_m=None,
    )


def _scanner(slot: int, mac: str, boot_id: int) -> dict[str, object]:
    return {
        "slot": slot,
        "connected": True,
        "identity_valid": True,
        "status_available": True,
        "protocol": "production_uart",
        "identity": {
            "target": "scanner-s3-combo-fof_badge",
            "project": "fof_badge_scanner",
            "hardware": "seeed_xiao_esp32s3",
            "version": SCANNER_VERSION,
            "hardware_id": mac,
            "boot_id": boot_id,
        },
        "profile": slot + 1,
        "health": {"command": True, "radio": True, "role_acked": True},
        "errors": {"rx": 0, "tx_drops": 0},
        "uptime_ms": 9000 + slot,
    }


def _status(
    *,
    uplink_boot: int = 100,
    scanner_boots: tuple[int, int] = (200, 300),
) -> dict[str, object]:
    summaries = [
        _scanner(0, ASSIGNMENT.ble_leaf_mac, scanner_boots[0]),
        _scanner(1, ASSIGNMENT.wifi_leaf_mac, scanner_boots[1]),
    ]
    return {
        "product_family": "badge_lite",
        "target": "uplink-s3-backend",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "version": VERSION,
        "firmware_name": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "mac": ASSIGNMENT.uplink_mac,
        "hardware_id": ASSIGNMENT.uplink_mac,
        "boot_id": uplink_boot,
        "mode": "headless",
        "capabilities": sorted(REQUIRED_CAPABILITIES),
        "config_generation": 1,
        "wifi": {
            "configured": False,
            "connected": False,
            "full_pass_failed": False,
        },
        "recovery": {"reason": "wifi_unconfigured", "ap_running": True},
        "ota_ready": True,
        "usb": {
            "available": True,
            "host_connected": True,
            "output_poisoned": False,
            "required_failures": 0,
            "bytes_transmitted": 400,
            "bytes_received": 200,
        },
        "scanner": [
            {"slot": 0, "connected": True, "identity_valid": True},
            {"slot": 1, "connected": True, "identity_valid": True},
        ],
        "scanner_summaries": summaries,
    }


def _boot_health(boot_id: int = 100) -> tuple[dict[str, object], dict[str, object]]:
    common = {
        "product_family": "badge_lite",
        "firmware_line": "backend",
        "component": "uplink",
        "target": "uplink-s3-backend",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "version": VERSION,
        "mac": ASSIGNMENT.uplink_mac,
        "boot_id": boot_id,
        "device_id": "uplink_DDEE01",
        "config_state": "loaded",
        "config_generation": 1,
        "auto_update_enabled": False,
        "uart0_started": True,
        "uart1_started": True,
        "network_state": "ap",
    }
    boot = {**common, "nvs_erased": False, "ota_state": "valid"}
    health = {
        **common,
        "nvs_loaded": True,
        "nvs_erased": False,
        "coordinator_started": True,
        "rollback_clear": True,
    }
    return boot, health


def _verify(
    *,
    status: dict[str, object] | None = None,
    config: RedactedConfig | None = None,
    boot: dict[str, object] | None = None,
    health: dict[str, object] | None = None,
) -> LiteRuntimeSnapshot:
    default_boot, default_health = _boot_health()
    return verify_runtime_snapshot(
        status=deepcopy(status or _status()),
        config=config or _config(),
        boot=deepcopy(boot or default_boot),
        health=deepcopy(health or default_health),
        assignment=ASSIGNMENT,
        bundle=BUNDLE,
    )


def test_runtime_gate_accepts_only_the_complete_blank_lite_graph() -> None:
    snapshot = _verify()

    assert snapshot.status["identity"]["target"] == "uplink-s3-backend"
    assert snapshot.status["mac"] == ASSIGNMENT.uplink_mac
    assert snapshot.config["networks"] == ()
    assert [item["mac"] for item in snapshot.status["scanners"]] == [
        ASSIGNMENT.ble_leaf_mac,
        ASSIGNMENT.wifi_leaf_mac,
    ]
    assert all(
        item["identity"]["target"] == "scanner-s3-combo-fof_badge"
        for item in snapshot.status["scanners"]
    )
    assert snapshot.health["rollback_clear"] is True


def test_runtime_gate_rejects_cross_product_scanner_identity() -> None:
    status = _status()
    status["scanner_summaries"][0]["identity"]["target"] = (
        "scanner-s3-combo-backend"
    )

    with pytest.raises(VerificationError, match="scanner slot 0 target mismatch"):
        _verify(status=status)


def test_runtime_gate_rejects_swapped_topology_and_unhealthy_scanner() -> None:
    swapped = _status()
    swapped["scanner_summaries"][0]["identity"]["hardware_id"] = (
        ASSIGNMENT.wifi_leaf_mac
    )
    with pytest.raises(VerificationError, match="MAC/topology mismatch"):
        _verify(status=swapped)

    unhealthy = _status()
    unhealthy["scanner_summaries"][1]["health"]["role_acked"] = False
    with pytest.raises(VerificationError, match="health did not converge"):
        _verify(status=unhealthy)

    noisy = _status()
    noisy["scanner_summaries"][1]["errors"]["rx"] = 1
    with pytest.raises(VerificationError, match="transport is not clean"):
        _verify(status=noisy)


def test_runtime_gate_accepts_historical_backpressure_counters() -> None:
    status = _status()
    status["usb"]["required_failures"] = 60
    status["scanner_summaries"][0]["errors"]["tx_drops"] = 492

    snapshot = _verify(status=status)

    assert snapshot.status["usb"]["required_failures"] == 60
    assert snapshot.status["scanners"][0]["errors"]["tx_drops"] == 492


def test_stable_runtime_requires_clean_live_usb_progress() -> None:
    first_status = _status()
    first_status["usb"]["required_failures"] = 60
    first = _verify(status=first_status)

    stable_status = _status()
    stable_status["usb"].update({
        "required_failures": 60,
        "bytes_transmitted": 500,
        "bytes_received": 300,
    })
    stable = _verify(status=stable_status)
    _verify_stable_samples(first, stable)

    failing_status = deepcopy(stable_status)
    failing_status["usb"]["required_failures"] = 61
    with pytest.raises(VerificationError, match="required failures increased"):
        _verify_stable_samples(first, _verify(status=failing_status))

    stalled_status = deepcopy(first_status)
    with pytest.raises(VerificationError, match="did not make progress"):
        _verify_stable_samples(first, _verify(status=stalled_status))


def test_runtime_wait_preserves_the_matching_uplink_error() -> None:
    class WrongVersionHandle:
        def __init__(self) -> None:
            self.reply = b""

        def reset_input_buffer(self) -> None:
            self.reply = b""

        def write(self, value: bytes) -> int:
            self.reply = b"FOF_PONG:wrong-version\n"
            return len(value)

        def read(self, _size: int) -> bytes:
            reply, self.reply = self.reply, b""
            return reply

        def close(self) -> None:
            pass

    def opener(port: str) -> WrongVersionHandle:
        if port == "/dev/cu.bound-uplink":
            return WrongVersionHandle()
        raise OSError("scanner candidate")

    uplink = UsbDevice(
        mac=ASSIGNMENT.uplink_mac,
        port="/dev/cu.bound-uplink",
        chip="ESP32-S3",
        revision="v0.2",
        flash_size="8MB",
        psram_size="8MB",
    )
    with pytest.raises(VerificationError, match="PONG version mismatch"):
        wait_for_stable_runtime(
            uplink,
            ASSIGNMENT,
            BUNDLE,
            timeout_s=0.1,
            serial_factory=opener,
            candidate_ports=lambda: [
                "/dev/cu.bound-uplink",
                "/dev/cu.scanner",
            ],
        )


def test_runtime_gate_rejects_nonblank_config_and_poisoned_usb() -> None:
    configured = replace(
        _config(),
        networks=(RedactedNetwork(ssid="field-network", password_set=True),),
    )
    with pytest.raises(VerificationError, match="contains a Wi-Fi network"):
        _verify(config=configured)

    status = _status()
    status["usb"]["output_poisoned"] = True
    with pytest.raises(VerificationError, match="USB transport is not healthy"):
        _verify(status=status)


def test_runtime_gate_rejects_missing_rollback_proof() -> None:
    boot, health = _boot_health()
    health["rollback_clear"] = False

    with pytest.raises(VerificationError, match="health/rollback proof failed"):
        _verify(boot=boot, health=health)


def _snapshot(
    uplink_boot: int,
    scanner_boots: tuple[int, int],
) -> LiteRuntimeSnapshot:
    status = _status(uplink_boot=uplink_boot, scanner_boots=scanner_boots)
    boot, health = _boot_health(uplink_boot)
    return _verify(status=status, boot=boot, health=health)


def test_reboot_gate_requires_all_three_boot_ids_to_change() -> None:
    before = _snapshot(100, (200, 300))
    after = _snapshot(101, (201, 301))

    verify_reboot_transition(before, after)

    with pytest.raises(VerificationError, match="uplink boot ID did not change"):
        verify_reboot_transition(before, _snapshot(100, (201, 301)))
    with pytest.raises(VerificationError, match="scanner slot 1 boot ID did not change"):
        verify_reboot_transition(before, _snapshot(101, (201, 300)))
