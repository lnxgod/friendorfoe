import time
import uuid
from collections import deque

import pytest
from sqlalchemy import func, select

from app.main import app
from app.models.db_models import DroneDetection, TriangulatedPosition
from app.models.schemas import DroneDetectionBatch, DroneDetectionItem
from app.routers import detections
from app.services.database import get_db
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
        "upload_queue": {"depth_batches": 7, "capacity_batches": 512, "overflow_dropped_batches": 2, "quarantined_batches": 1},
        "upload": {"ok": 11, "failed": 3, "retry_count": 4, "last_success_age_s": 8},
        "detections": [PORTABLE_EVIDENCE],
    }
    batch = DroneDetectionBatch.model_validate(payload)
    assert batch.model_dump()["firmware_target"] == "uplink-s3-backend"
    assert batch.model_dump()["upload_queue"]["capacity_batches"] == 512
    assert batch.model_dump()["detections"][0]["fused_confidence"] == 0.93
    scanner = batch.model_dump()["scanners"][0]
    assert scanner["uart"] == "ble"
    assert scanner["firmware_target"] == "scanner-s3-combo-backend"
    assert scanner["app_project"] == "fof_backend_scanner"
    assert scanner["hardware_type"] == "seeed_xiao_esp32s3"
    assert scanner["boot_id"] == 305419896
    assert scanner["rollback_state"] == "valid"


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
