import json
import subprocess
import sys
from pathlib import Path

from tools.emit_serializer_fixture import (
    PRODUCTION_DEPENDENCIES,
    PRODUCTION_SOURCES,
    emit_fixture,
)


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_serializer_fixture_links_only_exact_backend_owned_sources():
    assert PRODUCTION_SOURCES == (
        "shared/backend_upload_batch.c",
        "shared/backend_detection_codec.c",
        "shared/backend_json_writer.c",
    )
    assert PRODUCTION_DEPENDENCIES == (
        "shared/backend_identity.c",
        "shared/backend_scanner_topology.c",
        "shared/backend_json_reader.c",
    )


def test_real_serializer_emits_complete_bounded_fixture(tmp_path: Path):
    payload = emit_fixture(REPO_ROOT, compiler="cc", build_dir=tmp_path)
    body = json.loads(payload)

    assert len(payload) + 1 <= 4096
    assert not payload.endswith(b"\n")
    assert {
        key: body[key]
        for key in (
            "device_id",
            "firmware_target",
            "app_project",
            "hardware_type",
            "hardware_mac",
            "led_state",
            "wifi_ssid",
            "wifi_rssi",
            "timestamp",
        )
    } == {
        "device_id": "uplink_CB77A4",
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_mac": "A4:CF:12:CB:77:A4",
        "led_state": "drone_meta",
        "wifi_ssid": "FoF Lab",
        "wifi_rssi": -53,
        "timestamp": 1_785_600_000,
    }
    assert body["upload_queue"] == {
        "depth_batches": 7,
        "capacity_batches": 512,
        "overflow_dropped_batches": 2,
        "quarantined_batches": 1,
    }
    assert body["upload"] == {
        "ok": 11,
        "failed": 3,
        "retry_count": 4,
        "last_success_age_s": 8,
    }
    assert body["health"] == {
        "clock_valid": True,
        "epoch_ms": 1_785_600_000_999,
        "ap_active": True,
        "config_generation": 9,
        "command_success_count": 17,
        "command_failure_count": 2,
        "uptime_ms": 9_876_543_210,
    }
    assert len(body["scanners"]) == 2
    assert [scanner["uart"] for scanner in body["scanners"]] == [
        "ble",
        "wifi",
    ]
    assert {
        scanner["firmware_target"] for scanner in body["scanners"]
    } == {"scanner-s3-combo-backend"}
    assert {
        scanner["app_project"] for scanner in body["scanners"]
    } == {"fof_backend_scanner"}
    assert {
        scanner["hardware_type"] for scanner in body["scanners"]
    } == {"seeed_xiao_esp32s3"}
    assert len(body["detections"]) == 2
    drone, meta = body["detections"]
    assert {
        key: drone[key]
        for key in (
            "source",
            "confidence",
            "fused_confidence",
            "latitude",
            "longitude",
            "altitude_m",
            "heading_deg",
            "speed_mps",
            "vertical_speed_mps",
            "operator_id",
            "h_accuracy_m",
            "v_accuracy_m",
            "area_count",
            "area_radius",
            "area_ceiling",
            "area_floor",
            "freq_mhz",
            "channel",
            "channel_width_mhz",
            "wifi_generation",
            "probed_ssids",
            "scanner_slot",
            "scanner_slots_seen",
            "timestamp",
        )
    } == {
        "source": "wifi_probe_request",
        "confidence": 0.875,
        "fused_confidence": 0.9375,
        "latitude": 37.7749,
        "longitude": -122.4194,
        "altitude_m": 123.75,
        "heading_deg": 271.25,
        "speed_mps": 14.5,
        "vertical_speed_mps": -1.5,
        "operator_id": "OP-42",
        "h_accuracy_m": 1.75,
        "v_accuracy_m": 2.5,
        "area_count": 17,
        "area_radius": 250,
        "area_ceiling": 160.25,
        "area_floor": 15.5,
        "freq_mhz": 2437,
        "channel": 6,
        "channel_width_mhz": 80,
        "wifi_generation": 6,
        "probed_ssids": ["FieldNet", "Guest"],
        "scanner_slot": 0,
        "scanner_slots_seen": 3,
        "timestamp": 1_785_600_000_123,
    }
    assert {
        key: meta[key]
        for key in (
            "source",
            "confidence",
            "fused_confidence",
            "ble_company_id",
            "ble_apple_type",
            "ble_addr_type",
            "ble_ja3",
            "ble_name",
            "class_reason",
            "ble_apple_auth",
            "ble_activity",
            "ble_apple_flags",
            "ble_raw_mfr",
            "ble_adv_interval",
            "ble_svc_uuids",
            "ble_threat_kind",
            "ble_prompt_family_mask",
            "ble_unique_macs",
            "ble_observation_count",
            "ble_serial_service_uuid",
            "ble_threat_evidence_mask",
            "timestamp",
        )
    } == {
        "source": "ble_fingerprint",
        "confidence": 0.8125,
        "fused_confidence": 0.90625,
        "ble_company_id": 76,
        "ble_apple_type": 16,
        "ble_addr_type": 2,
        "ble_ja3": "0123abcd",
        "ble_name": "Ray-Ban Meta",
        "class_reason": "camera_service",
        "ble_apple_auth": "01a5ff",
        "ble_activity": 3,
        "ble_apple_flags": 5,
        "ble_raw_mfr": "4c0010070102",
        "ble_adv_interval": 125.5,
        "ble_svc_uuids": "180f,ffe0",
        "ble_threat_kind": 2,
        "ble_prompt_family_mask": 19,
        "ble_unique_macs": 12,
        "ble_observation_count": 23,
        "ble_serial_service_uuid": 65_504,
        "ble_threat_evidence_mask": 49,
        "timestamp": 1_785_600_000_456,
    }


def test_serializer_fixture_cli_writes_and_checks_exact_bytes(tmp_path: Path):
    script = REPO_ROOT / "backend-firmware/tools/emit_serializer_fixture.py"
    output = tmp_path / "fixture.json"
    emit = subprocess.run(
        [sys.executable, str(script), "--output", str(output)],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )

    assert emit.stdout == b""
    assert output.read_bytes().endswith(b"\n")
    assert not output.read_bytes().endswith(b"\n\n")
    subprocess.run(
        [sys.executable, str(script), "--check", str(output)],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )

    output.write_bytes(output.read_bytes() + b" ")
    mismatch = subprocess.run(
        [sys.executable, str(script), "--check", str(output)],
        cwd=REPO_ROOT,
        capture_output=True,
    )
    assert mismatch.returncode != 0
