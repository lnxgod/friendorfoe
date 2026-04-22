import json
import time
from collections import deque
from datetime import datetime, timedelta, timezone

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient
from sqlalchemy.ext.asyncio import async_sessionmaker, create_async_engine

from app.main import app
from app.models.db_models import Event
from app.models.schemas import StoredDetection
from app.routers import detections
from app.services.database import Base, get_db
from app.services.event_detector import EventDetector


@pytest_asyncio.fixture
async def db_session_factory():
    engine = create_async_engine("sqlite+aiosqlite:///:memory:")
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

    session_factory = async_sessionmaker(engine, expire_on_commit=False)
    try:
        yield session_factory
    finally:
        await engine.dispose()


@pytest_asyncio.fixture
async def client(db_session_factory):
    async def override_get_db():
        async with db_session_factory() as session:
            yield session

    app.dependency_overrides[get_db] = override_get_db
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac
    app.dependency_overrides.pop(get_db, None)


def test_event_detector_emits_new_probe_identity_from_ie_hash():
    detector = EventDetector()

    first = detector.ingest(
        source="wifi_probe_request",
        classification="wifi_device",
        drone_id="probe_AA:AA:AA:AA:AA:01",
        bssid="AA:AA:AA:AA:AA:01",
        ssid="DJI-1234",
        manufacturer="Unknown",
        model=None,
        probed_ssids=["DJI-1234"],
        ie_hash="A1B2C3D4",
        rssi=-60,
        confidence=0.25,
        sensor_id="sensor-a",
        ts=1.0,
    )
    second = detector.ingest(
        source="wifi_probe_request",
        classification="wifi_device",
        drone_id="probe_BB:BB:BB:BB:BB:02",
        bssid="BB:BB:BB:BB:BB:02",
        ssid="DJI-1234",
        manufacturer="Unknown",
        model=None,
        probed_ssids=["DJI-1234"],
        ie_hash="A1B2C3D4",
        rssi=-58,
        confidence=0.25,
        sensor_id="sensor-b",
        ts=2.0,
    )

    assert first == []
    assert ("new_probe_identity", "PROBE:A1B2C3D4") in second
    assert not any(ev_type == "new_probe_mac" for ev_type, _ in second)


def test_event_detector_keeps_legacy_new_probe_mac_only_on_mac_fallback():
    detector = EventDetector()

    detector.ingest(
        source="wifi_probe_request",
        classification="wifi_device",
        drone_id="probe_AA:AA:AA:AA:AA:01",
        bssid="AA:AA:AA:AA:AA:01",
        ssid="DJI-1234",
        manufacturer="Unknown",
        model=None,
        probed_ssids=["DJI-1234"],
        ie_hash=None,
        rssi=-60,
        confidence=0.25,
        sensor_id="sensor-a",
        ts=1.0,
    )
    emitted = detector.ingest(
        source="wifi_probe_request",
        classification="wifi_device",
        drone_id="probe_AA:AA:AA:AA:AA:01",
        bssid="AA:AA:AA:AA:AA:01",
        ssid="DJI-1234",
        manufacturer="Unknown",
        model=None,
        probed_ssids=["DJI-1234"],
        ie_hash=None,
        rssi=-59,
        confidence=0.25,
        sensor_id="sensor-a",
        ts=2.0,
    )

    assert ("new_probe_identity", "PROBE:AA:AA:AA:AA:AA:01") in emitted
    assert ("new_probe_mac", "AA:AA:AA:AA:AA:01") in emitted


def test_event_detector_probe_activity_spike_dedupes_for_six_hours():
    detector = EventDetector()
    spikes = []
    for idx in range(25):
        spikes.extend(
            ev for ev in detector.ingest(
                source="wifi_probe_request",
                classification="wifi_device",
                drone_id="probe_AA:AA:AA:AA:AA:01",
                bssid="AA:AA:AA:AA:AA:01",
                ssid="DJI-1234",
                manufacturer="Unknown",
                model=None,
                probed_ssids=["DJI-1234"],
                ie_hash="A1B2C3D4",
                rssi=-60,
                confidence=0.25,
                sensor_id="sensor-a",
                ts=float(idx),
            )
            if ev[0] == "probe_activity_spike"
        )

    assert len(spikes) == 1
    assert spikes[0][1].startswith("PROBE:A1B2C3D4@")

    more_spikes = []
    for idx in range(25):
        more_spikes.extend(
            ev for ev in detector.ingest(
                source="wifi_probe_request",
                classification="wifi_device",
                drone_id="probe_AA:AA:AA:AA:AA:01",
                bssid="AA:AA:AA:AA:AA:01",
                ssid="DJI-1234",
                manufacturer="Unknown",
                model=None,
                probed_ssids=["DJI-1234"],
                ie_hash="A1B2C3D4",
                rssi=-59,
                confidence=0.25,
                sensor_id="sensor-a",
                ts=3600.0 + idx,
            )
            if ev[0] == "probe_activity_spike"
        )

    assert more_spikes == []

    later_spikes = []
    for idx in range(25):
        later_spikes.extend(
            ev for ev in detector.ingest(
                source="wifi_probe_request",
                classification="wifi_device",
                drone_id="probe_AA:AA:AA:AA:AA:01",
                bssid="AA:AA:AA:AA:AA:01",
                ssid="DJI-1234",
                manufacturer="Unknown",
                model=None,
                probed_ssids=["DJI-1234"],
                ie_hash="A1B2C3D4",
                rssi=-58,
                confidence=0.25,
                sensor_id="sensor-b",
                ts=(6 * 3600.0) + idx + 1,
            )
            if ev[0] == "probe_activity_spike"
        )

    assert len(later_spikes) == 1
    assert later_spikes[0][1] != spikes[0][1]


@pytest.mark.asyncio
async def test_events_endpoint_accepts_multi_type_filters_and_stats_include_unack_by_type(
    client: AsyncClient,
    db_session_factory,
):
    now = datetime.now(timezone.utc)
    async with db_session_factory() as session:
        session.add_all([
            Event(
                event_type="new_probe_identity",
                identifier="PROBE:PYTEST-IDENTITY",
                severity="info",
                title="Probe identity",
                message="identity",
                first_seen_at=now - timedelta(minutes=30),
                last_seen_at=now - timedelta(minutes=5),
                sighting_count=2,
                sensor_count=2,
                sensor_ids_json=json.dumps(["sensor-a", "sensor-b"]),
                best_rssi=-58,
                metadata_json=json.dumps({"probe_identity": "PROBE:PYTEST-IDENTITY"}),
                acknowledged=False,
            ),
            Event(
                event_type="new_probed_ssid",
                identifier="PYTEST-SSID",
                severity="info",
                title="Probe SSID",
                message="ssid",
                first_seen_at=now - timedelta(minutes=20),
                last_seen_at=now - timedelta(minutes=3),
                sighting_count=2,
                sensor_count=2,
                sensor_ids_json=json.dumps(["sensor-a", "sensor-b"]),
                best_rssi=-57,
                metadata_json=json.dumps({"probe_identity": "PROBE:PYTEST-IDENTITY"}),
                acknowledged=False,
            ),
            Event(
                event_type="probe_activity_spike",
                identifier="PROBE:PYTEST-IDENTITY@0",
                severity="warning",
                title="Probe activity",
                message="spike",
                first_seen_at=now - timedelta(minutes=10),
                last_seen_at=now - timedelta(minutes=1),
                sighting_count=25,
                sensor_count=3,
                sensor_ids_json=json.dumps(["sensor-a", "sensor-b", "sensor-c"]),
                best_rssi=-55,
                metadata_json=json.dumps({"probe_identity": "PROBE:PYTEST-IDENTITY"}),
                acknowledged=False,
            ),
            Event(
                event_type="new_rid_drone",
                identifier="rid_PYTEST",
                severity="warning",
                title="RID",
                message="rid",
                first_seen_at=now - timedelta(minutes=15),
                last_seen_at=now - timedelta(minutes=2),
                sighting_count=1,
                sensor_count=1,
                sensor_ids_json=json.dumps(["sensor-a"]),
                best_rssi=-50,
                metadata_json=json.dumps({}),
                acknowledged=True,
                acknowledged_at=now - timedelta(minutes=1),
            ),
        ])
        await session.commit()

    resp = await client.get(
        "/detections/events",
        params=[
            ("types", "new_probe_identity,new_probed_ssid"),
            ("types", "probe_activity_spike"),
            ("since_hours", "24"),
        ],
    )
    assert resp.status_code == 200
    payload = resp.json()
    assert payload["count"] == 3
    assert {item["event_type"] for item in payload["events"]} == {
        "new_probe_identity",
        "new_probed_ssid",
        "probe_activity_spike",
    }

    resp = await client.get("/detections/events/stats")
    assert resp.status_code == 200
    stats = resp.json()
    assert stats["unack_by_type"]["new_probe_identity"] == 1
    assert stats["unack_by_type"]["new_probed_ssid"] == 1
    assert stats["unack_by_type"]["probe_activity_spike"] == 1
    assert stats["unack_by_type"].get("new_rid_drone", 0) == 0


@pytest.mark.asyncio
async def test_probe_summary_includes_first_seen_and_activity_fields(
    monkeypatch,
    client: AsyncClient,
    db_session_factory,
):
    tracker = detections.SensorTracker()
    monkeypatch.setattr(detections, "_sensor_tracker", tracker)

    lat = 37.0
    lon = -122.0
    now = time.time()

    prev_recent = detections._recent_detections
    detections._recent_detections = deque(maxlen=50000)
    try:
        detections._recent_detections.extend([
            StoredDetection(
                drone_id="probe_AA:AA:AA:AA:AA:01",
                source="wifi_probe_request",
                confidence=0.05,
                timestamp=int((now - 120) * 1000),
                latitude=None,
                longitude=None,
                altitude_m=None,
                heading_deg=None,
                speed_mps=None,
                rssi=-60,
                estimated_distance_m=15.0,
                manufacturer="Unknown",
                model=None,
                operator_lat=None,
                operator_lon=None,
                operator_id=None,
                self_id_text=None,
                ssid="DJI-1234",
                bssid="AA:AA:AA:AA:AA:01",
                channel=None,
                auth_m=None,
                ble_company_id=None,
                ble_apple_type=None,
                ble_ad_type_count=None,
                ble_payload_len=None,
                ble_addr_type=None,
                ble_ja3=None,
                ble_apple_auth=None,
                ble_activity=None,
                ble_raw_mfr=None,
                ble_adv_interval=None,
                ble_svc_uuids=None,
                ble_apple_info=None,
                ble_apple_flags=None,
                probed_ssids=["DJI-1234", "HomeNet"],
                ie_hash="A1B2C3D4",
                device_id="sensor-a",
                device_lat=lat,
                device_lon=lon,
                received_at=now - 120,
                classification="wifi_device",
            ),
            StoredDetection(
                drone_id="probe_BB:BB:BB:BB:BB:02",
                source="wifi_probe_request",
                confidence=0.05,
                timestamp=int((now - 60) * 1000),
                latitude=None,
                longitude=None,
                altitude_m=None,
                heading_deg=None,
                speed_mps=None,
                rssi=-55,
                estimated_distance_m=12.0,
                manufacturer="Unknown",
                model=None,
                operator_lat=None,
                operator_lon=None,
                operator_id=None,
                self_id_text=None,
                ssid="DJI-1234",
                bssid="BB:BB:BB:BB:BB:02",
                channel=None,
                auth_m=None,
                ble_company_id=None,
                ble_apple_type=None,
                ble_ad_type_count=None,
                ble_payload_len=None,
                ble_addr_type=None,
                ble_ja3=None,
                ble_apple_auth=None,
                ble_activity=None,
                ble_raw_mfr=None,
                ble_adv_interval=None,
                ble_svc_uuids=None,
                ble_apple_info=None,
                ble_apple_flags=None,
                probed_ssids=["DJI-1234", "Hangar"],
                ie_hash="A1B2C3D4",
                device_id="sensor-b",
                device_lat=lat,
                device_lon=lon + 0.0002,
                received_at=now - 60,
                classification="wifi_device",
            ),
        ])

        tracker.ingest(
            device_id="sensor-a",
            device_lat=lat,
            device_lon=lon,
            device_alt=0.0,
            drone_id="probe_AA:AA:AA:AA:AA:01",
            rssi=-60,
            estimated_distance_m=15.0,
            source="wifi_probe_request",
            classification="wifi_device",
            ssid="DJI-1234",
            bssid="AA:AA:AA:AA:AA:01",
            ie_hash="A1B2C3D4",
            timestamp=now - 120,
        )
        tracker.ingest(
            device_id="sensor-b",
            device_lat=lat,
            device_lon=lon + 0.0002,
            device_alt=0.0,
            drone_id="probe_BB:BB:BB:BB:BB:02",
            rssi=-55,
            estimated_distance_m=12.0,
            source="wifi_probe_request",
            classification="wifi_device",
            ssid="DJI-1234",
            bssid="BB:BB:BB:BB:BB:02",
            ie_hash="A1B2C3D4",
            timestamp=now - 60,
        )

        event_now = datetime.now(timezone.utc)
        async with db_session_factory() as session:
            session.add_all([
                Event(
                    event_type="new_probe_identity",
                    identifier="PROBE:A1B2C3D4",
                    severity="info",
                    title="New probe identity",
                    message="identity",
                    first_seen_at=event_now - timedelta(hours=2),
                    last_seen_at=event_now - timedelta(minutes=10),
                    sighting_count=2,
                    sensor_count=2,
                    sensor_ids_json=json.dumps(["sensor-a", "sensor-b"]),
                    best_rssi=-55,
                    metadata_json=json.dumps({"probe_identity": "PROBE:A1B2C3D4"}),
                    acknowledged=False,
                ),
                Event(
                    event_type="new_probed_ssid",
                    identifier="DJI-1234",
                    severity="info",
                    title="New probed SSID",
                    message="ssid",
                    first_seen_at=event_now - timedelta(hours=1),
                    last_seen_at=event_now - timedelta(minutes=5),
                    sighting_count=2,
                    sensor_count=2,
                    sensor_ids_json=json.dumps(["sensor-a", "sensor-b"]),
                    best_rssi=-55,
                    metadata_json=json.dumps({"probe_identity": "PROBE:A1B2C3D4"}),
                    acknowledged=False,
                ),
                Event(
                    event_type="probe_activity_spike",
                    identifier="PROBE:A1B2C3D4@0",
                    severity="warning",
                    title="Probe activity spike",
                    message="spike",
                    first_seen_at=event_now - timedelta(minutes=15),
                    last_seen_at=event_now - timedelta(minutes=1),
                    sighting_count=30,
                    sensor_count=3,
                    sensor_ids_json=json.dumps(["sensor-a", "sensor-b", "sensor-c"]),
                    best_rssi=-55,
                    metadata_json=json.dumps({"probe_identity": "PROBE:A1B2C3D4"}),
                    acknowledged=False,
                ),
            ])
            await session.commit()

        resp = await client.get("/detections/probes", params={"max_age_s": 86400})
        assert resp.status_code == 200
        payload = resp.json()
        assert payload["count"] == 1
        device = payload["devices"][0]
        assert device["identity"] == "PROBE:A1B2C3D4"
        assert device["first_seen"] is not None
        assert device["first_seen_age_s"] > 0
        assert device["last_seen"] >= now - 60
        assert device["seen_24h_count"] == 2
        assert device["sensor_count_24h"] == 2
        assert set(device["latest_event_types"]) == {
            "new_probe_identity",
            "new_probed_ssid",
            "probe_activity_spike",
        }
        assert device["activity_level"] == "high"
    finally:
        detections._recent_detections = prev_recent
