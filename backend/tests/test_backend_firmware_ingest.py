import asyncio
import hashlib
import json
import struct
import time
import uuid
import zlib
from collections import deque
from unittest.mock import AsyncMock

import pytest
from httpx import ASGITransport, AsyncClient
from sqlalchemy import func, select

from app.main import app
from app.models.db_models import DroneDetection, SensorNode, TriangulatedPosition
from app.models.schemas import DroneDetectionBatch, DroneDetectionItem
from app.routers import detections, nodes
from app.services import firmware_manager
from app.services.applied_calibration import AppliedCalibrationStore
from app.services.backend_node_status import (
    bounded_detection_time,
    bounded_observation_time,
    merge_backend_heartbeat,
)
from app.services.database import get_db
from app.services.signal_tracker import SignalTracker
from app.services import triangulation
from app.services.triangulation import SensorTracker


PORTABLE_EVIDENCE = {
    "drone_id": "RID:backend-test",
    "source": "wifi_beacon_rid",
    "confidence": 0.71,
    "fused_confidence": 0.93,
    "timestamp": 1785600000123,
    "vertical_speed_mps": -1.25,
    "freq_mhz": 2437,
    "channel": 6,
    "channel_width_mhz": 20,
    "ua_type": 2,
    "id_type": 1,
    "self_id_desc_type": 0,
    "height_agl_m": 37.5,
    "geodetic_alt_m": 151.25,
    "h_accuracy_m": 3.0,
    "v_accuracy_m": 5.0,
    "area_count": 4,
    "area_radius": 20,
    "area_ceiling": 250.0,
    "area_floor": 10.0,
    "classification_type": 1,
    "first_seen_ms": 1785600000000,
    "last_updated_ms": 1785600000123,
    "wifi_generation": 6,
    "auth_m": 0,
    "ie_hash": "00abcdef",
    "ble_ja3": "0123abcd",
    "ble_apple_auth": "00ff7a",
    "ble_activity": 0,
    "ble_apple_flags": 0,
    "ble_raw_mfr": "4c001007000000000000",
    "ble_adv_interval": 125.5,
    "ble_svc_uuids": "fd5f,180f,12345678-1234-5678-9abc-def012345678",
    "ble_threat_kind": 1,
    "ble_prompt_family_mask": 3,
    "ble_unique_macs": 9,
    "ble_observation_count": 14,
    "ble_serial_service_uuid": 0xFFE0,
    "ble_threat_evidence_mask": 5,
    "scanner_slot": 1,
    "scanner_slots_seen": 3,
}


def _backend_image(target: str, *, valid_crc: bool = True) -> bytes:
    info = firmware_manager.FIRMWARE_TYPES[target]
    version = "0.1.0-backend"
    app_desc = bytearray(112)
    struct.pack_into("<I", app_desc, 0, 0xABCD5432)
    app_desc[16:48] = version.encode().ljust(32, b"\0")
    app_desc[48:80] = info["project"].encode().ljust(32, b"\0")
    app_desc[80:96] = b"12:00:00".ljust(16, b"\0")
    app_desc[96:112] = b"2026-08-01".ljust(16, b"\0")
    record = bytearray(firmware_manager._BACKEND_IDENTITY_STRUCT.size)
    firmware_manager._BACKEND_IDENTITY_STRUCT.pack_into(
        record, 0, 0x42464F46, 1, info["image_kind"],
        target.encode().ljust(40, b"\0"),
        info["project"].encode().ljust(40, b"\0"),
        info["hardware"].encode().ljust(40, b"\0"),
        version.encode().ljust(32, b"\0"), 0,
    )
    crc = zlib.crc32(record[:160]) & 0xFFFFFFFF
    struct.pack_into("<I", record, 160, crc if valid_crc else crc ^ 1)
    image = bytearray(1200)
    image[0] = 0xE9
    image[0x20:0x20 + len(app_desc)] = app_desc
    image[256:256 + len(record)] = record
    return bytes(image)


PERSISTED_BACKEND_FIELDS = (
    "fused_confidence", "vertical_speed_mps", "freq_mhz", "channel",
    "channel_width_mhz", "ua_type", "id_type", "self_id_desc_type",
    "height_agl_m", "geodetic_alt_m", "h_accuracy_m", "v_accuracy_m",
    "area_count", "area_radius", "area_ceiling", "area_floor",
    "classification_type", "first_seen_ms", "last_updated_ms",
    "wifi_generation", "auth_m", "ie_hash", "scanner_slot", "scanner_slots_seen",
    "ble_ja3", "ble_apple_auth", "ble_activity", "ble_apple_flags",
    "ble_raw_mfr", "ble_adv_interval", "ble_svc_uuids",
    "ble_threat_kind", "ble_prompt_family_mask", "ble_unique_macs",
    "ble_observation_count", "ble_serial_service_uuid",
    "ble_threat_evidence_mask",
)


async def _post_with_failed_primary_commit(
    client, backend_sensor_session_factory, monkeypatch, body,
):
    normal_override = app.dependency_overrides[get_db]
    async with backend_sensor_session_factory() as failing_session:
        async def fail_commit():
            raise RuntimeError("forced primary commit failure")

        monkeypatch.setattr(failing_session, "commit", fail_commit)

        async def failing_get_db():
            yield failing_session

        app.dependency_overrides[get_db] = failing_get_db
        try:
            return await client.post("/detections/drones", json=body)
        finally:
            app.dependency_overrides[get_db] = normal_override


def test_backend_detection_preserves_every_ported_evidence_field():
    item = DroneDetectionItem.model_validate(PORTABLE_EVIDENCE)
    dumped = item.model_dump()
    for key, expected in PORTABLE_EVIDENCE.items():
        assert dumped[key] == expected
    # Frequency is the measured MHz value; channel is independently derived.
    assert dumped["freq_mhz"] == 2437
    assert dumped["channel"] == 6
    assert dumped["freq_mhz"] != dumped["channel"]
    # Zero is real evidence for these firmware enums/flags, never "absent".
    assert dumped["auth_m"] == 0
    assert dumped["ble_activity"] == 0
    assert dumped["ble_apple_flags"] == 0


def test_backend_batch_preserves_identity_health_and_queue_metadata():
    payload = {
        "device_id": "uplink_CB77A4",
        "firmware_version": "0.1.0-backend",
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_mac": "A4:CF:12:CB:77:A4",
        "capabilities": ["dual_scanner", "ble_investigation", "uart_ota"],
        "node_name": "Roof backend sensor",
        "scanners": [{
            "uart": "ble", "slot": 0,
            "firmware_target": "scanner-s3-combo-backend",
            "app_project": "fof_backend_scanner",
            "hardware_type": "seeed_xiao_esp32s3",
            "firmware_version": "0.1.0-backend",
            "mac": "AA:BB:CC:DD:EE:01", "boot_id": 305419896,
            "profile": "ble_primary", "status_sequence": 12,
            "role_generation": 4, "role_acked": True,
            "command_ingress": True, "radio_healthy": True,
            "ble_healthy": True, "wifi_healthy": False,
            "ota_state": "idle", "rollback_state": "valid",
        }],
        "led_state": "drone_meta",
        "health": {
            "clock_valid": True,
            "epoch_ms": 1785600000123,
            "ap_active": False,
            "config_generation": 9,
            "command_success_count": 11,
            "command_failure_count": 2,
            "uptime_ms": 9000,
        },
        "upload_queue": {"depth_batches": 7, "capacity_batches": 512, "overflow_dropped_batches": 2, "quarantined_batches": 1},
        "upload": {"ok": 11, "failed": 3, "retry_count": 4, "last_success_age_s": 8},
        "detections": [PORTABLE_EVIDENCE],
    }
    batch = DroneDetectionBatch.model_validate(payload)
    assert batch.model_dump()["firmware_target"] == "uplink-s3-backend"
    assert batch.model_dump()["upload_queue"]["capacity_batches"] == 512
    assert batch.model_dump()["health"] == payload["health"]
    assert batch.model_dump()["detections"][0]["fused_confidence"] == 0.93
    scanner = batch.model_dump()["scanners"][0]
    assert scanner["uart"] == "ble"
    assert scanner["firmware_target"] == "scanner-s3-combo-backend"
    assert scanner["app_project"] == "fof_backend_scanner"
    assert scanner["hardware_type"] == "seeed_xiao_esp32s3"
    assert scanner["boot_id"] == 305419896
    assert scanner["rollback_state"] == "valid"


def test_backend_lite_health_rejects_coerced_wire_types():
    health = {
        "clock_valid": False,
        "ap_active": True,
        "config_generation": 4,
        "command_success_count": 3,
        "command_failure_count": 1,
        "uptime_ms": 8000,
    }
    for field, invalid in (
        ("clock_valid", "false"),
        ("ap_active", 1),
        ("config_generation", "4"),
        ("command_success_count", False),
        ("uptime_ms", 8000.0),
    ):
        malformed = dict(health)
        malformed[field] = invalid
        with pytest.raises(ValueError):
            DroneDetectionBatch.model_validate({
                "device_id": "uplink_CB77A4",
                "health": malformed,
                "detections": [],
            })


@pytest.mark.asyncio
async def test_diagnostics_use_canonical_scanner_target_and_version(client, monkeypatch):
    device_id = "uplink_CANON_DIAG"
    monkeypatch.setattr(detections, "_node_heartbeats", {
        device_id: {
            "device_id": device_id,
            "ip": "192.168.1.71",
            "last_seen": time.time(),
            "firmware_target": "uplink-s3-backend",
            "firmware_version": "0.1.0-backend",
            "scanners": [{
                "firmware_target": "scanner-s3-combo-backend",
                "app_project": "fof_backend_scanner",
                "hardware_type": "seeed_xiao_esp32s3",
                "firmware_version": "0.1.0-backend",
                "ver": "legacy-shadow-version",
                "mac": "AA:BB:CC:DD:EE:71",
                "boot_id": 71,
                "uart": "ble",
            }],
        },
    })

    response = await client.get("/detections/diagnostics")

    assert response.status_code == 200, response.text
    payload = response.json()
    assert payload["firmware_readiness"]["scanner_versions_seen"] == {"0.1.0-backend": 1}
    assert [
        warning for warning in payload["system_warnings"]
        if warning.get("device_id") == device_id
        and warning["code"].startswith("scanner_firmware_")
    ] == []


@pytest.mark.asyncio
async def test_backend_heartbeat_accepts_empty_detection_array(client):
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": 1_785_600_000,
        "firmware_target": "uplink-s3-backend",
        "detections": [],
    })
    assert response.status_code == 200
    assert response.json() == {
        "status": "ok",
        "accepted": 0,
        "device_id": "uplink_CB77A4",
        "processed": 0,
        "deduplicated": 0,
        "filtered": 0,
    }


@pytest.mark.asyncio
async def test_backend_lite_health_round_trips_to_node_status(client):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    health = {
        "clock_valid": False,
        "ap_active": True,
        "config_generation": 4,
        "command_success_count": 3,
        "command_failure_count": 1,
        "uptime_ms": 8000,
    }
    response = await client.post("/detections/drones", json={
        "device_id": device_id,
        "firmware_target": "uplink-s3-backend",
        "health": health,
        "detections": [],
    })
    assert response.status_code == 200, response.text

    status = await client.get("/detections/nodes/status")
    assert status.status_code == 200, status.text
    node = next(
        row for row in status.json()["nodes"]
        if row["device_id"] == device_id
    )
    assert node["health"] == health


@pytest.mark.asyncio
async def test_invalid_backend_detection_is_http_400(client):
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "detections": [{"source": "ble_rid", "confidence": 0.8}],
    })
    assert response.status_code == 400


@pytest.mark.asyncio
async def test_replayed_non_rid_batch_is_acknowledged_without_duplicate_accept(client):
    body = {
        "device_id": "uplink_CB77A4",
        "timestamp": 1_785_600_000,
        "detections": [{
            "drone_id": "BLE:AA:BB:CC:DD:EE:FF",
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    }
    first = await client.post("/detections/drones", json=body)
    second = await client.post("/detections/drones", json=body)
    assert first.json()["accepted"] == 1
    assert first.json()["processed"] == 1
    assert second.json()["accepted"] == 1
    assert second.json()["processed"] == 0
    assert second.json()["deduplicated"] == 1
    assert second.json()["device_id"] == "uplink_CB77A4"


@pytest.mark.asyncio
async def test_locally_filtered_item_is_transport_accepted(client):
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": "AP:FOF-SELF",
            "source": "wifi_ap",
            "confidence": 0.8,
            "ssid": "FoF-Uplink-CB77A4",
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    })
    assert response.status_code == 200
    assert response.json() == {
        "status": "ok", "accepted": 1, "device_id": "uplink_CB77A4",
        "processed": 0, "deduplicated": 0, "filtered": 1,
    }


@pytest.mark.asyncio
async def test_backend_named_image_is_not_sent_to_badge_uplink(client, monkeypatch):
    detections._node_heartbeats["uplink_BADGE1"] = {
        "device_id": "uplink_BADGE1", "ip": "10.0.0.20",
        "firmware_target": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "last_seen": time.time(),
    }
    monkeypatch.setattr(
        nodes._firmware_mgr, "get_firmware_binary",
        AsyncMock(side_effect=AssertionError("must reject before loading image")),
    )

    response = await client.post("/nodes/uplink_BADGE1/ota/uplink-s3-backend")

    assert response.status_code == 409
    assert response.json()["detail"] == "running firmware identity is incompatible with uplink-s3-backend"


@pytest.mark.asyncio
async def test_backend_uplink_is_not_sent_production_image(client, monkeypatch):
    detections._node_heartbeats["uplink_BACK01"] = {
        "device_id": "uplink_BACK01", "ip": "10.0.0.21",
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "firmware_version": "0.1.0-backend", "last_seen": time.time(),
    }
    monkeypatch.setattr(
        nodes._firmware_mgr, "get_firmware_binary",
        AsyncMock(side_effect=AssertionError("must reject before loading image")),
    )

    response = await client.post("/nodes/uplink_BACK01/ota/uplink-s3")

    assert response.status_code == 409


@pytest.mark.asyncio
async def test_named_ota_rebinds_to_fresh_heartbeat_after_firmware_load(client, monkeypatch):
    now = time.time()
    detections._node_heartbeats["uplink_REBIND"] = {
        "device_id": "uplink_REBIND", "ip": "10.0.0.10", "last_seen": now,
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
    }
    calls: list[str] = []

    async def swap_heartbeat(name: str) -> bytes:
        assert name == "uplink-s3-backend"
        detections._node_heartbeats["uplink_REBIND"] = {
            **detections._node_heartbeats["uplink_REBIND"],
            "ip": "10.0.0.11", "last_seen": time.time(),
        }
        return _backend_image(name)

    async def capture_upload(cmd, **kwargs):
        calls.append(next(part for part in cmd if isinstance(part, str) and part.startswith("http://")))
        return __import__("subprocess").CompletedProcess(cmd, 0, b'{"ok":true}', b"")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", swap_heartbeat)
    monkeypatch.setattr(nodes, "_run_subprocess", capture_upload)

    response = await client.post("/nodes/uplink_REBIND/ota/uplink-s3-backend")

    assert response.status_code == 200, response.text
    assert calls == ["http://10.0.0.11/api/ota"]


@pytest.mark.asyncio
async def test_direct_ota_rejects_stale_heartbeat_before_upload(client, monkeypatch):
    detections._node_heartbeats["uplink_STALE"] = {
        "device_id": "uplink_STALE", "ip": "10.0.0.30", "last_seen": time.time() - 120,
    }
    upload = AsyncMock(side_effect=AssertionError("must reject stale heartbeat"))
    monkeypatch.setattr(nodes, "_run_subprocess", upload)

    responses = []
    for last_seen in (time.time() - 120, None, "not-a-timestamp"):
        detections._node_heartbeats["uplink_STALE"]["last_seen"] = last_seen
        responses.append(await client.post(
            "/nodes/uplink_STALE/ota",
            files={"firmware": ("legacy.bin", b"L" * 1200, "application/octet-stream")},
        ))

    assert all(response.status_code == 409 for response in responses)
    assert all("stale" in response.json()["detail"] for response in responses)
    upload.assert_not_awaited()


@pytest.mark.asyncio
async def test_direct_ota_upload_rejects_cross_family_and_invalid_backend_identity(client, monkeypatch):
    detections._node_heartbeats["uplink_BADGE1"] = {
        "device_id": "uplink_BADGE1", "ip": "10.0.0.20",
        "firmware_target": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3", "last_seen": time.time(),
    }
    detections._node_heartbeats["uplink_BACK01"] = {
        "device_id": "uplink_BACK01", "ip": "10.0.0.21",
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3", "last_seen": time.time(),
    }
    monkeypatch.setattr(
        nodes, "_run_subprocess",
        AsyncMock(side_effect=AssertionError("must reject before upload")),
    )

    badge = await client.post(
        "/nodes/uplink_BADGE1/ota",
        files={"firmware": ("backend.bin", _backend_image("uplink-s3-backend"), "application/octet-stream")},
    )
    invalid = await client.post(
        "/nodes/uplink_BADGE1/ota",
        files={"firmware": ("invalid.bin", _backend_image("uplink-s3-backend", valid_crc=False), "application/octet-stream")},
    )
    legacy = await client.post(
        "/nodes/uplink_BACK01/ota",
        files={"firmware": ("legacy.bin", b"L" * 1200, "application/octet-stream")},
    )

    assert badge.status_code == 409
    assert invalid.status_code == 400
    assert legacy.status_code == 409


@pytest.mark.asyncio
async def test_backend_evidence_survives_history_persistence(client):
    drone_id = f"RID-BACKEND-{uuid.uuid4().hex}"
    item = {**PORTABLE_EVIDENCE, "drone_id": drone_id}
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": int(time.time()),
        "detections": [item],
    })
    assert response.status_code == 200
    history = await client.get(
        "/detections/drones/history", params={"hours": 1, "limit": 200},
    )
    row = next(entry for entry in history.json()["detections"] if entry["drone_id"] == drone_id)
    for field in PERSISTED_BACKEND_FIELDS:
        assert row[field] == item[field]

    node_history = await client.get("/nodes/uplink_CB77A4/detections")
    node_row = next(
        entry for entry in node_history.json()["detections"]
        if entry["drone_id"] == drone_id
    )
    for field in PERSISTED_BACKEND_FIELDS:
        assert node_row[field] == item[field]


@pytest.mark.asyncio
async def test_primary_db_outage_preserves_full_recent_detection_ring(
    client, backend_sensor_session_factory, monkeypatch,
):
    existing = [object() for _ in range(50000)]
    full_ring = deque(existing, maxlen=50000)
    monkeypatch.setattr(detections, "_recent_detections", full_ring)
    body = {
        "device_id": "uplink_CB77A4",
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": f"BLE:{uuid.uuid4().hex}",
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "rssi": -48,
        }],
    }

    response = await _post_with_failed_primary_commit(
        client, backend_sensor_session_factory, monkeypatch, body,
    )

    assert response.status_code == 503
    assert len(full_ring) == 50000
    assert full_ring[0] is existing[0]
    assert full_ring[-1] is existing[-1]


@pytest.mark.asyncio
async def test_primary_db_outage_does_not_advance_signal_tracker(
    client, backend_sensor_session_factory, monkeypatch,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    signal_tracker = SignalTracker()
    monkeypatch.setattr(detections, "_signal_tracker", signal_tracker)
    body = {
        "device_id": "uplink_CB77A4",
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "rssi": -48,
        }],
    }

    first = await _post_with_failed_primary_commit(
        client, backend_sensor_session_factory, monkeypatch, body,
    )

    assert first.status_code == 503
    assert signal_tracker.tracks == {}
    assert signal_tracker.alias_to_track == {}

    retry = await client.post("/detections/drones", json=body)
    assert retry.status_code == 200
    assert retry.json()["processed"] == 1
    live = signal_tracker.get_live_tracks(now=body["timestamp"])
    assert live["count"] == 1
    assert live["tracks"][0]["sample_count"] == 1


@pytest.mark.asyncio
async def test_secondary_position_commit_failure_keeps_primary_acknowledged(
    client, backend_sensor_session_factory, monkeypatch,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    device_id = f"fixed-{uuid.uuid4().hex}"
    monkeypatch.setattr(detections, "_sensor_tracker", SensorTracker())
    async with backend_sensor_session_factory() as setup_session:
        setup_session.add(SensorNode(
            device_id=device_id,
            name=device_id,
            lat=37.3340,
            lon=-122.4450,
            alt=12.0,
            is_fixed=True,
        ))
        await setup_session.commit()

    body = {
        "device_id": device_id,
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "rssi": -48,
            "latitude": 37.3345,
            "longitude": -122.4455,
            "altitude_m": 42.0,
        }],
    }
    normal_override = app.dependency_overrides[get_db]
    async with backend_sensor_session_factory() as failing_session:
        real_commit = failing_session.commit
        commit_count = 0

        async def fail_second_commit():
            nonlocal commit_count
            commit_count += 1
            if commit_count == 2:
                raise RuntimeError("forced secondary commit failure")
            await real_commit()

        monkeypatch.setattr(failing_session, "commit", fail_second_commit)

        async def failing_get_db():
            yield failing_session

        app.dependency_overrides[get_db] = failing_get_db
        try:
            response = await client.post("/detections/drones", json=body)
        finally:
            app.dependency_overrides[get_db] = normal_override

    assert response.status_code == 200
    assert response.json()["accepted"] == 1
    assert response.json()["processed"] == 1
    assert response.json()["deduplicated"] == 0
    assert response.json()["filtered"] == 0
    assert commit_count == 2
    assert f"_last_pos_{drone_id}" not in detections._position_dedup
    async with backend_sensor_session_factory() as verification_session:
        detection_count = await verification_session.scalar(
            select(func.count(DroneDetection.id)).where(
                DroneDetection.drone_id == drone_id,
            )
        )
        position_count = await verification_session.scalar(
            select(func.count(TriangulatedPosition.id)).where(
                TriangulatedPosition.drone_id == drone_id,
            )
        )
    assert detection_count == 1
    assert position_count == 0


@pytest.mark.asyncio
async def test_secondary_position_commit_and_rollback_failure_stays_acknowledged(
    client, backend_sensor_session_factory, monkeypatch,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    device_id = f"fixed-{uuid.uuid4().hex}"
    monkeypatch.setattr(detections, "_sensor_tracker", SensorTracker())
    async with backend_sensor_session_factory() as setup_session:
        setup_session.add(SensorNode(
            device_id=device_id,
            name=device_id,
            lat=37.3340,
            lon=-122.4450,
            alt=12.0,
            is_fixed=True,
        ))
        await setup_session.commit()

    body = {
        "device_id": device_id,
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "rssi": -48,
            "latitude": 37.3345,
            "longitude": -122.4455,
            "altitude_m": 42.0,
        }],
    }
    normal_override = app.dependency_overrides[get_db]
    async with backend_sensor_session_factory() as failing_session:
        real_commit = failing_session.commit
        commit_count = 0

        async def fail_secondary_commit():
            nonlocal commit_count
            commit_count += 1
            if commit_count == 2:
                raise RuntimeError("forced secondary commit failure")
            await real_commit()

        async def fail_rollback():
            raise RuntimeError("forced secondary rollback failure")

        monkeypatch.setattr(failing_session, "commit", fail_secondary_commit)
        monkeypatch.setattr(failing_session, "rollback", fail_rollback)

        async def failing_get_db():
            yield failing_session

        app.dependency_overrides[get_db] = failing_get_db
        try:
            response = await client.post("/detections/drones", json=body)
        finally:
            app.dependency_overrides[get_db] = normal_override

    assert response.status_code == 200
    assert response.json()["accepted"] == 1
    assert response.json()["processed"] == 1
    assert response.json()["deduplicated"] == 0
    assert response.json()["filtered"] == 0
    assert f"_last_pos_{drone_id}" not in detections._position_dedup
    async with backend_sensor_session_factory() as verification_session:
        detection_count = await verification_session.scalar(
            select(func.count(DroneDetection.id)).where(
                DroneDetection.drone_id == drone_id,
            )
        )
        position_count = await verification_session.scalar(
            select(func.count(TriangulatedPosition.id)).where(
                TriangulatedPosition.drone_id == drone_id,
            )
        )
    assert detection_count == 1
    assert position_count == 0


@pytest.mark.parametrize(
    "wake_completed",
    (False, True),
    ids=("wake-callback-pending", "future-completed-task-stopped"),
)
def test_loop_independent_lock_reclaims_stopped_loop_handoff(wake_completed):
    """An unacknowledged grant on a stopped loop cannot strand the mutex."""
    lock = detections._LoopIndependentAsyncLock()
    owner_loop = asyncio.new_event_loop()
    target_loop = asyncio.new_event_loop()
    later_loop = asyncio.new_event_loop()
    target_acquired = False
    target_task = None
    later_task = None

    async def target_waiter():
        nonlocal target_acquired
        await lock.acquire()
        target_acquired = True
        lock.release()

    async def later_acquirer_after_one_turn():
        task = asyncio.create_task(lock.acquire())
        turn_complete = asyncio.get_running_loop().create_future()
        asyncio.get_running_loop().call_soon(
            turn_complete.set_result, None,
        )
        await turn_complete
        return task

    try:
        owner_loop.run_until_complete(lock.acquire())

        target_task = target_loop.create_task(target_waiter())
        target_loop.call_soon(target_loop.stop)
        target_loop.run_forever()
        assert not target_task.done()

        # The regression implementation called call_soon_threadsafe() here:
        # a stopped-but-open loop accepted the grant and queued its callback.
        # A lifecycle-aware implementation may revoke before scheduling it.
        lock.release()
        if wake_completed:
            # Run exactly the queued wake callback and stop in the same loop
            # turn. The Future completes, but target_waiter cannot resume.
            target_loop.call_soon(target_loop.stop)
            target_loop.run_forever()
            assert not target_task.done()

        later_task = later_loop.run_until_complete(
            later_acquirer_after_one_turn(),
        )
        assert later_task.done(), {
            "later_request": "timed_out",
            "lock_still_locked": lock.locked(),
        }
        assert later_task.result() is True

        # Restart the target loop while the later acquirer owns the lock. A
        # stale wake may make the old waiter retry, but cannot grant a second
        # owner. Two explicit loop turns let that retry reach the mutex.
        def stop_after_next_turn():
            target_loop.call_soon(target_loop.stop)

        target_loop.call_soon(stop_after_next_turn)
        target_loop.run_forever()
        assert not target_task.done()
        assert not target_acquired

        lock.release()
        target_loop.run_until_complete(target_task)
        assert target_acquired
        assert not lock.locked()
    finally:
        later_acquired = bool(
            later_task is not None
            and later_task.done()
            and not later_task.cancelled()
            and later_task.exception() is None
        )
        if later_task is not None and not later_task.done():
            later_task.cancel()
            later_loop.run_until_complete(
                asyncio.gather(later_task, return_exceptions=True),
            )
        if later_acquired and lock.locked():
            lock.release()
        if target_task is not None and not target_task.done():
            try:
                target_loop.run_until_complete(target_task)
            except BaseException:
                pass
        owner_loop.close()
        target_loop.close()
        later_loop.close()


@pytest.mark.parametrize(
    "cancel_after_grant",
    (False, True),
    ids=("queued", "grant-in-transit"),
)
@pytest.mark.asyncio
async def test_loop_independent_lock_cancellation_does_not_leak(
    cancel_after_grant,
):
    lock = detections._LoopIndependentAsyncLock()
    await lock.acquire()
    contender_started = asyncio.Event()

    async def contend():
        contender_started.set()
        await lock.acquire()

    contender = asyncio.create_task(contend())
    await contender_started.wait()
    if cancel_after_grant:
        lock.release()
    contender.cancel()
    with pytest.raises(asyncio.CancelledError):
        await contender
    if not cancel_after_grant:
        lock.release()

    assert not lock.locked()
    assert await lock.acquire() is True
    lock.release()


@pytest.mark.asyncio
async def test_position_persistence_serializes_across_event_loops(
    client, backend_sensor_session_factory, monkeypatch,
):
    """A contended serializer remains usable by a later event loop."""
    device_id = f"fixed-{uuid.uuid4().hex}"
    drone_id = f"BLE:{uuid.uuid4().hex}"
    position = (37.3345, -122.4455)
    async with backend_sensor_session_factory() as setup_session:
        setup_session.add(SensorNode(
            device_id=device_id,
            name=device_id,
            lat=37.3340,
            lon=-122.4450,
            alt=12.0,
            is_fixed=True,
        ))
        await setup_session.commit()

    serializer = type(detections._position_persistence_lock)()
    monkeypatch.setattr(
        detections, "_position_persistence_lock", serializer,
    )

    async def contend_on_first_loop():
        await serializer.acquire()
        contender_started = asyncio.Event()

        async def contend():
            contender_started.set()
            await serializer.acquire()
            serializer.release()

        contender = asyncio.create_task(contend())
        await contender_started.wait()
        serializer.release()
        await contender

    await asyncio.to_thread(lambda: asyncio.run(contend_on_first_loop()))

    body = {
        "device_id": device_id,
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "rssi": -48,
            "latitude": position[0],
            "longitude": position[1],
            "altitude_m": 42.0,
        }],
    }

    async def ingest_while_contended_on_second_loop():
        tracker = SensorTracker()
        persistence_reached = asyncio.Event()
        real_get_located_drones = tracker.get_located_drones

        def get_located_drones():
            located = real_get_located_drones()
            persistence_reached.set()
            return located

        monkeypatch.setattr(tracker, "get_located_drones", get_located_drones)
        monkeypatch.setattr(detections, "_sensor_tracker", tracker)

        await serializer.acquire()
        try:
            transport = ASGITransport(app=app, raise_app_exceptions=False)
            async with AsyncClient(
                transport=transport, base_url="http://test",
            ) as loop_client:
                response_task = asyncio.create_task(
                    loop_client.post("/detections/drones", json=body),
                )
                await persistence_reached.wait()
                serializer.release()
                return await response_task
        finally:
            if serializer.locked():
                serializer.release()

    response = await asyncio.to_thread(
        lambda: asyncio.run(ingest_while_contended_on_second_loop()),
    )

    assert response.status_code == 200
    assert response.json() == {
        "status": "ok",
        "accepted": 1,
        "device_id": device_id,
        "processed": 1,
        "deduplicated": 0,
        "filtered": 0,
    }
    async with backend_sensor_session_factory() as verification_session:
        detection_count = await verification_session.scalar(
            select(func.count(DroneDetection.id)).where(
                DroneDetection.drone_id == drone_id,
            )
        )
        position_count = await verification_session.scalar(
            select(func.count(TriangulatedPosition.id)).where(
                TriangulatedPosition.drone_id == drone_id,
            )
        )
    assert detection_count == 1
    assert position_count == 1
    assert detections._position_dedup[f"_last_pos_{drone_id}"] == position


@pytest.mark.parametrize(
    ("failure", "expected_position_count"),
    (("acquire", 0), ("release", 1)),
)
@pytest.mark.asyncio
async def test_position_serializer_failure_stays_acknowledged(
    client, backend_sensor_session_factory, monkeypatch,
    failure, expected_position_count,
):
    device_id = f"fixed-{uuid.uuid4().hex}"
    drone_id = f"BLE:{uuid.uuid4().hex}"
    position = (37.3345, -122.4455)
    monkeypatch.setattr(detections, "_sensor_tracker", SensorTracker())
    serializer = type(detections._position_persistence_lock)()
    monkeypatch.setattr(
        detections, "_position_persistence_lock", serializer,
    )
    async with backend_sensor_session_factory() as setup_session:
        setup_session.add(SensorNode(
            device_id=device_id,
            name=device_id,
            lat=37.3340,
            lon=-122.4450,
            alt=12.0,
            is_fixed=True,
        ))
        await setup_session.commit()

    real_release = serializer.release
    if failure == "acquire":
        async def fail_acquire():
            raise RuntimeError("forced serializer acquire failure")

        monkeypatch.setattr(serializer, "acquire", fail_acquire)
    else:
        def fail_release():
            raise RuntimeError("forced serializer release failure")

        monkeypatch.setattr(serializer, "release", fail_release)

    body = {
        "device_id": device_id,
        "timestamp": int(time.time()),
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "rssi": -48,
            "latitude": position[0],
            "longitude": position[1],
            "altitude_m": 42.0,
        }],
    }
    try:
        transport = ASGITransport(app=app, raise_app_exceptions=False)
        async with AsyncClient(
            transport=transport, base_url="http://test",
        ) as loop_client:
            response = await loop_client.post("/detections/drones", json=body)
    finally:
        monkeypatch.setattr(serializer, "release", real_release)
        if serializer.locked():
            serializer.release()

    assert response.status_code == 200
    assert response.json()["accepted"] == 1
    assert response.json()["processed"] == 1
    async with backend_sensor_session_factory() as verification_session:
        detection_count = await verification_session.scalar(
            select(func.count(DroneDetection.id)).where(
                DroneDetection.drone_id == drone_id,
            )
        )
        position_count = await verification_session.scalar(
            select(func.count(TriangulatedPosition.id)).where(
                TriangulatedPosition.drone_id == drone_id,
            )
        )
    assert detection_count == 1
    assert position_count == expected_position_count
    assert (f"_last_pos_{drone_id}" in detections._position_dedup) is bool(
        expected_position_count,
    )


@pytest.mark.asyncio
async def test_concurrent_position_writes_deduplicate_and_keep_newest_cache(
    client, backend_sensor_session_factory, monkeypatch,
):
    """Force reversed secondary completion without relying on timing sleeps."""
    device_id = f"fixed-{uuid.uuid4().hex}"
    async with backend_sensor_session_factory() as setup_session:
        setup_session.add(SensorNode(
            device_id=device_id,
            name=device_id,
            lat=37.3340,
            lon=-122.4450,
            alt=12.0,
            is_fixed=True,
        ))
        await setup_session.commit()

    scenarios = (
        ((37.3345, -122.4455), (37.3345, -122.4455)),
        ((37.3350, -122.4460), (37.3355, -122.4465)),
    )
    observed = []
    for older_position, newer_position in scenarios:
        drone_id = f"BLE:{uuid.uuid4().hex}"
        monkeypatch.setattr(detections, "_sensor_tracker", SensorTracker())
        detections._position_dedup.clear()

        older_secondary_staged = asyncio.Event()
        newer_primary_committed = asyncio.Event()
        newer_secondary_staged = asyncio.Event()
        allow_older_secondary = asyncio.Event()
        allow_newer_secondary = asyncio.Event()
        request_index = 0

        async def interleaving_get_db():
            nonlocal request_index
            current_request = request_index
            request_index += 1
            async with backend_sensor_session_factory() as session:
                real_commit = session.commit
                commit_count = 0

                async def interleaved_commit():
                    nonlocal commit_count
                    commit_count += 1
                    if commit_count == 1:
                        await real_commit()
                        if current_request == 1:
                            newer_primary_committed.set()
                        return

                    if current_request == 0:
                        older_secondary_staged.set()
                        await allow_older_secondary.wait()
                    else:
                        newer_secondary_staged.set()
                        await allow_newer_secondary.wait()
                    await real_commit()

                monkeypatch.setattr(session, "commit", interleaved_commit)
                yield session

        def body(position, bssid):
            return {
                "device_id": device_id,
                "timestamp": int(time.time()),
                "detections": [{
                    "drone_id": drone_id,
                    "source": "ble_fingerprint",
                    "confidence": 0.8,
                    "bssid": bssid,
                    "rssi": -48,
                    "latitude": position[0],
                    "longitude": position[1],
                    "altitude_m": 42.0,
                }],
            }

        normal_override = app.dependency_overrides[get_db]
        app.dependency_overrides[get_db] = interleaving_get_db
        try:
            older_task = asyncio.create_task(
                client.post(
                    "/detections/drones",
                    json=body(older_position, "AA:BB:CC:DD:EE:01"),
                )
            )
            await older_secondary_staged.wait()
            newer_task = asyncio.create_task(
                client.post(
                    "/detections/drones",
                    json=body(newer_position, "AA:BB:CC:DD:EE:02"),
                )
            )
            await newer_primary_committed.wait()

            # Event.set() does not yield: once this waiter resumes, the newer
            # route is deterministically paused either at its secondary
            # commit (the bug) or at serialized position persistence.
            if newer_secondary_staged.is_set():
                allow_newer_secondary.set()
                newer_response = await newer_task
                allow_older_secondary.set()
                older_response = await older_task
            else:
                allow_older_secondary.set()
                allow_newer_secondary.set()
                older_response, newer_response = await asyncio.gather(
                    older_task, newer_task,
                )
        finally:
            allow_older_secondary.set()
            allow_newer_secondary.set()
            app.dependency_overrides[get_db] = normal_override

        assert older_response.status_code == 200
        assert newer_response.status_code == 200
        for response in (older_response, newer_response):
            assert response.json()["accepted"] == 1
            assert response.json()["processed"] == 1
            assert response.json()["deduplicated"] == 0
            assert response.json()["filtered"] == 0
        async with backend_sensor_session_factory() as verification_session:
            position_count = await verification_session.scalar(
                select(func.count(TriangulatedPosition.id)).where(
                    TriangulatedPosition.drone_id == drone_id,
                )
            )
        observed.append((
            position_count,
            detections._position_dedup.get(f"_last_pos_{drone_id}"),
        ))

    assert observed == [
        (1, scenarios[0][1]),
        (2, scenarios[1][1]),
    ]


@pytest.mark.asyncio
async def test_primary_db_outage_returns_503_without_consuming_retry(
    client, backend_sensor_session_factory, monkeypatch,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    now = int(time.time())
    monkeypatch.setattr(detections, "_sensor_tracker", SensorTracker())
    monkeypatch.setattr(detections, "_recent_detections", deque(maxlen=50000))
    monkeypatch.setattr(detections, "_known_drones", {})
    monkeypatch.setattr(detections, "_drone_alerts", [])
    drone_tracker_sensor_before = detections._drone_tracker._sensor_tracker

    def fail_if_full_state_is_copied(_value):
        raise AssertionError("ingest must not copy accumulated runtime state")

    monkeypatch.setattr(
        detections, "deepcopy", fail_if_full_state_is_copied, raising=False,
    )
    body = {
        "device_id": "uplink_CB77A4",
        "device_lat": 37.3340,
        "device_lon": -122.4450,
        "device_alt": 12.0,
        "timestamp": now,
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "latitude": 37.3345,
            "longitude": -122.4455,
            "altitude_m": 42.0,
        }],
    }
    normal_override = app.dependency_overrides[get_db]
    async with backend_sensor_session_factory() as failing_session:
        async def fail_commit():
            raise RuntimeError("forced primary commit failure")

        monkeypatch.setattr(failing_session, "commit", fail_commit)

        async def failing_get_db():
            yield failing_session

        app.dependency_overrides[get_db] = failing_get_db
        try:
            first = await client.post("/detections/drones", json=body)
        finally:
            app.dependency_overrides[get_db] = normal_override

    assert first.status_code == 503
    assert not any(key[0] == drone_id for key in detections._ingest_dedup)
    assert not detections._recent_detections
    assert not detections._sensor_tracker.observations
    assert not detections._position_dedup
    assert detections._drone_tracker._sensor_tracker is drone_tracker_sensor_before

    retry = await client.post("/detections/drones", json=body)
    assert retry.status_code == 200
    assert retry.json()["processed"] == 1
    assert len(detections._recent_detections) == 1
    assert len(detections._sensor_tracker.observations[drone_id]) == 1
    assert detections._sensor_tracker.sensors[body["device_id"]].sensor_type == "outdoor"
    assert detections._position_dedup[f"_last_pos_{drone_id}"] == (
        37.3345, -122.4455,
    )
    async with backend_sensor_session_factory() as verification_session:
        detection_count = await verification_session.scalar(
            select(func.count(DroneDetection.id)).where(
                DroneDetection.drone_id == drone_id,
            )
        )
        position_count = await verification_session.scalar(
            select(func.count(TriangulatedPosition.id)).where(
                TriangulatedPosition.drone_id == drone_id,
            )
        )
    assert detection_count == 1
    assert position_count == 1


def test_future_clock_never_extends_online_liveness():
    observation_time, skew = bounded_observation_time(2_000_000_000, 1_785_600_000.0)
    assert observation_time == 1_785_600_000.0
    assert skew == 214_400_000.0


def test_offline_replay_preserves_valid_historical_scan_time():
    observation_time, skew = bounded_observation_time(
        1_784_000_000, 1_785_600_000.0,
    )
    assert observation_time == 1_784_000_000.0
    assert skew == -1_600_000.0


def test_future_detection_timestamp_falls_back_to_batch_observation():
    assert bounded_detection_time(
        2_000_000_000_000, 1_784_000_000.0, 1_785_600_000.0,
    ) == 1_784_000_000.0


def test_exact_firmware_epoch_boundary_is_valid_for_batch_and_detection():
    observation_time, skew = bounded_observation_time(
        1_700_000_000, 1_700_000_100.0,
    )
    assert observation_time == 1_700_000_000.0
    assert skew == -100.0
    assert bounded_detection_time(
        1_700_000_000_000, 1_699_999_999.0, 1_700_000_100.0,
    ) == 1_700_000_000.0


def test_backend_heartbeat_keeps_operational_device_id_and_sticky_metadata():
    previous = {
        "device_id": "uplink_CB77A4", "total_batches": 4,
        "node_name": "Roof", "lat": 36.1, "lon": -115.1, "alt": 700.0,
    }
    batch = DroneDetectionBatch(
        device_id="uplink_CB77A4",
        firmware_target="uplink-s3-backend",
        hardware_mac="A4:CF:12:CB:77:A4",
        health={
            "clock_valid": False,
            "ap_active": True,
            "config_generation": 4,
            "command_success_count": 3,
            "command_failure_count": 1,
            "uptime_ms": 8000,
        },
        detections=[],
    )
    merged = merge_backend_heartbeat(previous, batch, "10.0.0.15", 1000.0)
    assert merged["device_id"] == "uplink_CB77A4"
    assert merged["last_seen"] == 1000.0
    assert merged["total_batches"] == 5
    assert merged["node_name"] == "Roof"
    assert (merged["lat"], merged["lon"], merged["alt"]) == (36.1, -115.1, 700.0)
    assert merged["firmware_target"] == "uplink-s3-backend"
    assert merged["hardware_mac"] == "A4:CF:12:CB:77:A4"
    assert merged["health"]["config_generation"] == 4


@pytest.mark.asyncio
async def test_backend_upgrade_heartbeat_does_not_replace_registered_location(client):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    registered = await client.post("/nodes", json={
        "device_id": device_id,
        "name": "Roof",
        "lat": 36.1,
        "lon": -115.1,
        "alt": 700.0,
        "position_mode": "active",
    })
    assert registered.status_code == 201
    try:
        response = await client.post("/detections/drones", json={
            "device_id": device_id,
            "device_lat": 1.0,
            "device_lon": 2.0,
            "node_name": "Untrusted over-air rename",
            "firmware_target": "uplink-s3-backend",
            "detections": [],
        })
        assert response.status_code == 200
        status = await client.get("/detections/nodes/status")
        node = next(row for row in status.json()["nodes"] if row["device_id"] == device_id)
        assert (node["lat"], node["lon"], node["name"]) == (36.1, -115.1, "Roof")
    finally:
        await client.delete(f"/nodes/{device_id}")


@pytest.mark.asyncio
async def test_backend_upgrade_keeps_history_and_listener_calibration_on_device_id(client):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    drone_id = f"BLE:{uuid.uuid4().hex}"
    prior_model = triangulation.PER_LISTENER_MODEL.get(device_id)
    triangulation.PER_LISTENER_MODEL[device_id] = (-58.0, 2.1)
    try:
        created = await client.post("/nodes", json={
            "device_id": device_id,
            "name": "Existing roof node",
            "lat": 36.1,
            "lon": -115.1,
            "alt": 700.0,
            "position_mode": "active",
        })
        assert created.status_code == 201
        uploaded = await client.post("/detections/drones", json={
            "device_id": device_id,
            "firmware_version": "0.1.0-backend",
            "firmware_target": "uplink-s3-backend",
            "app_project": "fof_backend_uplink",
            "hardware_type": "seeed_xiao_esp32s3",
            "hardware_mac": "A4:CF:12:CB:77:A4",
            "detections": [{
                "drone_id": drone_id,
                "source": "ble_fingerprint",
                "confidence": 0.8,
                "bssid": "AA:BB:CC:DD:EE:FF",
            }],
        })
        assert uploaded.status_code == 200

        registry = (await client.get("/nodes")).json()
        matching = [n for n in registry["nodes"] if n["device_id"] == device_id]
        assert len(matching) == 1
        assert matching[0]["name"] == "Existing roof node"
        history = (await client.get(f"/nodes/{device_id}/detections")).json()
        assert any(
            row["device_id"] == device_id and row["drone_id"] == drone_id
            for row in history["detections"]
        )
        assert triangulation.PER_LISTENER_MODEL[device_id] == (-58.0, 2.1)
        assert "A4:CF:12:CB:77:A4" not in {
            node["device_id"] for node in registry["nodes"]
        }
    finally:
        await client.delete(f"/nodes/{device_id}")
        if prior_model is None:
            triangulation.PER_LISTENER_MODEL.pop(device_id, None)
        else:
            triangulation.PER_LISTENER_MODEL[device_id] = prior_model


@pytest.mark.asyncio
async def test_legacy_batch_without_timestamp_persists_non_null_observation_time(
    client, backend_sensor_session_factory,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    before = int(time.time())
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
        }],
    })
    assert response.status_code == 200
    async with backend_sensor_session_factory() as session:
        stored = await session.scalar(
            select(DroneDetection).where(DroneDetection.drone_id == drone_id)
        )
    assert stored is not None
    assert before <= stored.timestamp <= int(time.time())


@pytest.mark.asyncio
async def test_valid_historical_item_timestamp_is_persisted_not_batch_receipt(
    client, backend_sensor_session_factory,
):
    drone_id = f"BLE:{uuid.uuid4().hex}"
    historical_ms = 1_784_000_000_123
    response = await client.post("/detections/drones", json={
        "device_id": "uplink_CB77A4",
        "timestamp": 1_785_600_000,
        "detections": [{
            "drone_id": drone_id,
            "source": "ble_fingerprint",
            "confidence": 0.8,
            "bssid": "AA:BB:CC:DD:EE:FF",
            "timestamp": historical_ms,
        }],
    })
    assert response.status_code == 200
    async with backend_sensor_session_factory() as session:
        stored = await session.scalar(
            select(DroneDetection).where(DroneDetection.drone_id == drone_id)
        )
    assert stored is not None
    assert stored.timestamp == historical_ms // 1000


@pytest.mark.asyncio
async def test_ingest_node_name_is_creation_only_and_never_renames_registry(
    client, backend_sensor_session_factory,
):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    first = await client.post("/detections/drones", json={
        "device_id": device_id,
        "device_lat": 36.11,
        "device_lon": -115.11,
        "node_name": "Roof backend sensor",
        "detections": [],
    })
    assert first.status_code == 200
    async with backend_sensor_session_factory() as session:
        created = await session.scalar(
            select(SensorNode).where(SensorNode.device_id == device_id)
        )
        assert created is not None
        assert created.name == "Roof backend sensor"

    later = await client.post("/detections/drones", json={
        "device_id": device_id,
        "device_lat": 36.12,
        "device_lon": -115.12,
        "node_name": "Changed over radio",
        "detections": [],
    })
    assert later.status_code == 200
    async with backend_sensor_session_factory() as session:
        preserved = await session.scalar(
            select(SensorNode).where(SensorNode.device_id == device_id)
        )
        assert preserved is not None
        assert preserved.name == "Roof backend sensor"


def test_calibration_continuity_defaults_and_missing_listener_are_nonsecret():
    store = AppliedCalibrationStore()
    assert store.calibration_continuity("uplink_DEFAULT") == {
        "schema": 1,
        "device_id": "uplink_DEFAULT",
        "calibration_status": "defaults",
        "session_id": None,
        "applied_at": None,
        "listener_model_present": False,
        "listener_model_schema": "rssi-ref-path-loss-v1",
        "listener_model_sha256": None,
    }
    store.record = {
        "session_id": "0123456789ab",
        "applied_at": 1_785_600_000.25,
        "per_listener_model": {"uplink_OTHER": [-58.0, 2.1]},
        "verified_fit": {"model_validation": {"applyable": True, "reasons": []}},
    }
    missing = store.calibration_continuity("uplink_MISSING")
    assert missing["calibration_status"] == "trusted"
    assert missing["session_id"] == "0123456789ab"
    assert missing["listener_model_present"] is False
    assert missing["listener_model_sha256"] is None


@pytest.mark.parametrize("value", [[], [float("nan"), 2.1], [True, 2.1], [-58.0, "2.1"]])
def test_calibration_continuity_rejects_malformed_listener_model(value):
    store = AppliedCalibrationStore()
    store.record = {"per_listener_model": {"uplink_BAD": value}}
    with pytest.raises(ValueError, match="invalid listener model"):
        store.calibration_continuity("uplink_BAD")


@pytest.mark.asyncio
async def test_calibration_continuity_is_device_bound_stable_and_nonsecret(
    client, monkeypatch,
):
    device_id = f"uplink_{uuid.uuid4().hex[:6].upper()}"
    created = await client.post("/nodes", json={
        "device_id": device_id,
        "name": "Calibrated roof",
        "lat": 36.1,
        "lon": -115.1,
        "alt": 700.0,
        "position_mode": "active",
    })
    assert created.status_code == 201
    monkeypatch.setattr(detections._applied_cal_store, "record", {
        "session_id": "0123456789ab",
        "applied_at": 1_785_600_000.25,
        "per_listener_model": {
            device_id: [-58.0, 2.1],
            "uplink_OTHER1": [-61.0, 2.4],
        },
        "verified_fit": {
            "model_validation": {"applyable": True, "reasons": []},
            "private_raw_samples": ["must-not-escape"],
        },
        "provisional_fit": {"raw": "must-not-escape"},
    })
    canonical = json.dumps(
        {
            "device_id": device_id,
            "model_schema": "rssi-ref-path-loss-v1",
            "values": [-58.0, 2.1],
        },
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    expected = {
        "schema": 1,
        "device_id": device_id,
        "calibration_status": "trusted",
        "session_id": "0123456789ab",
        "applied_at": 1_785_600_000.25,
        "listener_model_present": True,
        "listener_model_schema": "rssi-ref-path-loss-v1",
        "listener_model_sha256": hashlib.sha256(canonical).hexdigest(),
    }

    before = await client.get(f"/detections/calibrate/continuity/{device_id}")
    assert before.status_code == 200
    assert before.json() == expected
    heartbeat = await client.post("/detections/drones", json={
        "device_id": device_id,
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_mac": "A4:CF:12:CB:77:A4",
        "node_name": "Must not rename calibrated node",
        "detections": [],
    })
    assert heartbeat.status_code == 200
    after = await client.get(f"/detections/calibrate/continuity/{device_id}")
    assert after.status_code == 200
    assert after.json() == before.json()
    serialized = json.dumps(after.json(), sort_keys=True)
    for forbidden in (
        "rssi_ref", "path_loss", "-58.0", "2.1", "verified_fit",
        "provisional_fit", "must-not-escape",
    ):
        assert forbidden not in serialized

    unknown = await client.get("/detections/calibrate/continuity/uplink_UNKNOWN")
    assert unknown.status_code == 404
