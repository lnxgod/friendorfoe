import math
import time
from collections import deque
from types import SimpleNamespace

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.models.schemas import StoredDetection
from app.services.triangulation import SensorTracker
import app.services.triangulation as tri


def _east_lon(lat: float, lon: float, meters: float) -> float:
    return lon + (meters / (111_320.0 * math.cos(math.radians(lat))))


@pytest_asyncio.fixture
async def client():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


@pytest.mark.asyncio
async def test_probe_map_is_drone_first_until_probe_opt_in(monkeypatch, client: AsyncClient):
    tracker = SensorTracker()
    monkeypatch.setattr(detections, "_sensor_tracker", tracker)

    lat = 37.0
    lon = -122.0
    lon_b = _east_lon(lat, lon, 20.0)
    now = time.time()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=lat,
        device_lon=lon,
        device_alt=0.0,
        drone_id="probe_AA:AA:AA:AA:AA:01",
        rssi=-60,
        estimated_distance_m=10.0,
        source="wifi_probe_request",
        classification="wifi_device",
        ssid="DJI-1234",
        bssid="AA:AA:AA:AA:AA:01",
        ie_hash="A1B2C3D4",
        timestamp=now,
    )
    tracker.ingest(
        device_id="sensor-b",
        device_lat=lat,
        device_lon=lon_b,
        device_alt=0.0,
        drone_id="probe_BB:BB:BB:BB:BB:02",
        rssi=-60,
        estimated_distance_m=10.0,
        source="wifi_probe_request",
        classification="wifi_device",
        ssid="DJI-1234",
        bssid="BB:BB:BB:BB:BB:02",
        ie_hash="A1B2C3D4",
        timestamp=now,
    )

    assert "PROBE:A1B2C3D4" in tracker.observations

    resp = await client.get("/detections/drones/map", params={"exclude_known": "false"})
    assert resp.status_code == 200
    assert resp.json()["drone_count"] == 0

    resp = await client.get(
        "/detections/drones/map",
        params={"exclude_known": "false", "include_probes": "true"},
    )
    assert resp.status_code == 200
    payload = resp.json()
    assert payload["drone_count"] == 1
    assert payload["drones"][0]["drone_id"] == "PROBE:A1B2C3D4"
    assert payload["drones"][0]["position_source"] == "intersection"


@pytest.mark.asyncio
async def test_probe_summary_groups_by_ie_hash(monkeypatch, client: AsyncClient):
    tracker = SensorTracker()
    monkeypatch.setattr(detections, "_sensor_tracker", tracker)

    lat = 37.0
    lon = -122.0
    lon_b = _east_lon(lat, lon, 20.0)
    now = time.time()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=lat,
        device_lon=lon,
        device_alt=0.0,
        drone_id="probe_AA:AA:AA:AA:AA:01",
        rssi=-60,
        estimated_distance_m=10.0,
        source="wifi_probe_request",
        classification="wifi_device",
        ssid="DJI-1234",
        bssid="AA:AA:AA:AA:AA:01",
        ie_hash="A1B2C3D4",
        timestamp=now,
    )
    tracker.ingest(
        device_id="sensor-b",
        device_lat=lat,
        device_lon=lon_b,
        device_alt=0.0,
        drone_id="probe_BB:BB:BB:BB:BB:02",
        rssi=-61,
        estimated_distance_m=10.0,
        source="wifi_probe_request",
        classification="wifi_device",
        ssid="DJI-1234",
        bssid="BB:BB:BB:BB:BB:02",
        ie_hash="A1B2C3D4",
        timestamp=now,
    )

    prev_recent = detections._recent_detections
    detections._recent_detections = deque(maxlen=50000)
    try:
        detections._recent_detections.extend([
            StoredDetection(
                drone_id="probe_AA:AA:AA:AA:AA:01",
                source="wifi_probe_request",
                confidence=0.05,
                timestamp=int(now * 1000),
                latitude=None,
                longitude=None,
                altitude_m=None,
                heading_deg=None,
                speed_mps=None,
                rssi=-60,
                estimated_distance_m=10.0,
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
                probed_ssids=["DJI-1234"],
                ie_hash="A1B2C3D4",
                device_id="sensor-a",
                device_lat=lat,
                device_lon=lon,
                received_at=now,
                classification="wifi_device",
            ),
            StoredDetection(
                drone_id="probe_BB:BB:BB:BB:BB:02",
                source="wifi_probe_request",
                confidence=0.05,
                timestamp=int(now * 1000),
                latitude=None,
                longitude=None,
                altitude_m=None,
                heading_deg=None,
                speed_mps=None,
                rssi=-61,
                estimated_distance_m=10.0,
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
                probed_ssids=["DJI-1234"],
                ie_hash="A1B2C3D4",
                device_id="sensor-b",
                device_lat=lat,
                device_lon=lon_b,
                received_at=now,
                classification="wifi_device",
            ),
        ])

        resp = await client.get("/detections/probes", params={"max_age_s": 600})
        assert resp.status_code == 200
        payload = resp.json()
        assert payload["count"] == 1
        device = payload["devices"][0]
        assert device["identity"] == "PROBE:A1B2C3D4"
        assert device["ie_hash"] == "A1B2C3D4"
        assert sorted(device["macs"]) == [
            "AA:AA:AA:AA:AA:01",
            "BB:BB:BB:BB:BB:02",
        ]
        assert device["sensor_count"] == 2
        assert device["lat"] is not None
        assert device["lon"] is not None
    finally:
        detections._recent_detections = prev_recent


@pytest.mark.asyncio
async def test_calibration_model_truthfully_flags_untrusted_defaults(monkeypatch, client: AsyncClient):
    monkeypatch.setattr(detections._applied_cal_store, "record", None)
    monkeypatch.setattr(tri, "RSSI_REF", -40)
    monkeypatch.setattr(tri, "PATH_LOSS_OUTDOOR", 2.5)
    monkeypatch.setattr(tri, "PER_LISTENER_MODEL", {})

    resp = await client.get("/detections/calibrate/model")
    assert resp.status_code == 200
    payload = resp.json()
    assert payload["is_calibrated"] is False
    assert payload["is_active"] is False
    assert payload["is_trusted"] is False
    assert payload["active_model_source"] == "defaults"
    assert payload["applied_listener_count"] == 0
