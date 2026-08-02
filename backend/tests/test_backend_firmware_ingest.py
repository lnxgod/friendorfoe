import asyncio
import time
import uuid
from collections import deque

import pytest
from httpx import ASGITransport, AsyncClient
from sqlalchemy import func, select

from app.main import app
from app.models.db_models import DroneDetection, SensorNode, TriangulatedPosition
from app.models.schemas import DroneDetectionBatch, DroneDetectionItem
from app.routers import detections
from app.services.database import get_db
from app.services.signal_tracker import SignalTracker
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
