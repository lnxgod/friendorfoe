"""Regression tests for range propagation and diagnostics."""

import math
import time

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.services.triangulation import (
    SensorTracker,
    _haversine_m,
    rssi_to_distance_m,
    update_calibration,
)


def _east_lon(lat: float, lon: float, meters: float) -> float:
    """Rough local-meter to longitude conversion for tiny test offsets."""
    return lon + (meters / (111_320.0 * math.cos(math.radians(lat))))


def test_ingest_prefers_scanner_distance_when_present():
    tracker = SensorTracker()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=37.0,
        device_lon=-122.0,
        device_alt=0.0,
        drone_id="privacy-cam-1",
        rssi=-70,
        estimated_distance_m=42.5,
        source="wifi_ssid",
        ssid="TestCam",
        timestamp=1_700_000_000.0,
    )

    obs = tracker.observations["privacy-cam-1"]["sensor-a"]
    assert obs.scanner_estimated_distance_m == pytest.approx(42.5)
    assert obs.backend_estimated_distance_m == pytest.approx(
        rssi_to_distance_m(-70, indoor=False, device_id="sensor-a")
    )
    assert obs.estimated_distance_m == pytest.approx(42.5)
    assert obs.distance_source == "scanner"
    assert obs.range_model == "global_outdoor"


def test_ingest_falls_back_to_backend_rssi_when_scanner_distance_missing():
    tracker = SensorTracker()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=37.0,
        device_lon=-122.0,
        device_alt=0.0,
        drone_id="privacy-cam-2",
        rssi=-65,
        estimated_distance_m=0.0,
        source="wifi_ssid",
        ssid="TestCam",
        timestamp=1_700_000_000.0,
    )

    obs = tracker.observations["privacy-cam-2"]["sensor-a"]
    assert obs.scanner_estimated_distance_m is None
    assert obs.backend_estimated_distance_m == pytest.approx(10.0)
    assert obs.estimated_distance_m == pytest.approx(10.0)
    assert obs.distance_source == "backend_rssi"
    assert obs.range_model == "global_outdoor"


def test_trusted_calibration_makes_backend_rssi_range_authoritative():
    update_calibration(
        rssi_ref=-50,
        path_loss=2.0,
        per_listener_model={},
        trusted=True,
    )
    try:
        tracker = SensorTracker()
        tracker.ingest(
            device_id="sensor-a",
            device_lat=37.0,
            device_lon=-122.0,
            device_alt=0.0,
            drone_id="calibrated-device",
            rssi=-50,
            estimated_distance_m=99.0,
            source="wifi_ssid",
            ssid="TestCam",
            timestamp=1_700_000_000.0,
        )

        obs = tracker.observations["calibrated-device"]["sensor-a"]
        assert obs.scanner_estimated_distance_m == pytest.approx(99.0)
        assert obs.backend_estimated_distance_m == pytest.approx(1.0)
        assert obs.estimated_distance_m == pytest.approx(1.0)
        assert obs.distance_source == "backend_rssi"
    finally:
        update_calibration(
            rssi_ref=-40,
            path_loss=2.5,
            per_listener_model={},
            trusted=False,
        )


def test_calibration_session_can_force_backend_rssi_range_authority():
    tracker = SensorTracker()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=37.0,
        device_lon=-122.0,
        device_alt=0.0,
        drone_id="BLE:12345678:Calibration Beacon",
        rssi=-65,
        estimated_distance_m=75.0,
        source="ble_fingerprint",
        manufacturer="Calibration Beacon",
        model="FP:CAL-test",
        timestamp=1_700_000_000.0,
        range_authority="backend_rssi",
    )

    obs = tracker.observations["FP:CAL-test"]["sensor-a"]
    assert obs.scanner_estimated_distance_m == pytest.approx(75.0)
    assert obs.estimated_distance_m == pytest.approx(
        rssi_to_distance_m(-65, indoor=False, device_id="sensor-a")
    )
    assert obs.distance_source == "backend_rssi"


def test_stationary_wifi_uses_long_aggregation_window_for_two_sensors():
    tracker = SensorTracker()
    lat = 37.0
    lon = -122.0
    lon_b = _east_lon(lat, lon, 20.0)
    midpoint_lon = _east_lon(lat, lon, 10.0)
    now = time.time()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=lat,
        device_lon=lon,
        device_alt=0.0,
        drone_id="cam-midpoint",
        rssi=-60,
        estimated_distance_m=10.0,
        source="wifi_ssid",
        ssid="TestCam",
        bssid="AA:BB:CC:DD:EE:01",
        timestamp=now - 30.0,
    )
    tracker.ingest(
        device_id="sensor-b",
        device_lat=lat,
        device_lon=lon_b,
        device_alt=0.0,
        drone_id="cam-midpoint",
        rssi=-60,
        estimated_distance_m=10.0,
        source="wifi_ssid",
        ssid="TestCam",
        bssid="AA:BB:CC:DD:EE:01",
        timestamp=now,
    )

    located = tracker.get_located_drones()
    assert len(located) == 1
    device = located[0]
    assert device.position_source == "stationary_intersection"
    assert device.sensor_count == 2
    assert _haversine_m(device.lat, device.lon, lat, midpoint_lon) < 2.0


def test_stationary_wifi_uses_median_range_not_latest_outlier():
    tracker = SensorTracker()
    lat = 37.0
    lon = -122.0
    lon_b = _east_lon(lat, lon, 20.0)
    midpoint_lon = _east_lon(lat, lon, 10.0)
    now = time.time()

    for ts, dist in (
        (now - 50.0, 10.0),
        (now - 30.0, 10.0),
        (now - 10.0, 40.0),
    ):
        tracker.ingest(
            device_id="sensor-a",
            device_lat=lat,
            device_lon=lon,
            device_alt=0.0,
            drone_id="cam-outlier",
            rssi=-60,
            estimated_distance_m=dist,
            source="wifi_ssid",
            ssid="TestCam",
            bssid="AA:BB:CC:DD:EE:02",
            timestamp=ts,
        )

    for ts in (now - 40.0, now - 20.0, now):
        tracker.ingest(
            device_id="sensor-b",
            device_lat=lat,
            device_lon=lon_b,
            device_alt=0.0,
            drone_id="cam-outlier",
            rssi=-60,
            estimated_distance_m=10.0,
            source="wifi_ssid",
            ssid="TestCam",
            bssid="AA:BB:CC:DD:EE:02",
            timestamp=ts,
        )

    located = tracker.get_located_drones()
    assert len(located) == 1
    device = located[0]
    assert device.position_source == "stationary_intersection"
    assert _haversine_m(device.lat, device.lon, lat, midpoint_lon) < 2.0
    sensor_a = next(o for o in device.observations if o.device_id == "sensor-a")
    assert sensor_a.estimated_distance_m == pytest.approx(10.0)


def test_moving_targets_keep_short_window_behavior():
    tracker = SensorTracker()
    lat = 37.0
    lon = -122.0
    lon_b = _east_lon(lat, lon, 20.0)
    now = time.time()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=lat,
        device_lon=lon,
        device_alt=0.0,
        drone_id="meta-glasses-1",
        rssi=-60,
        estimated_distance_m=10.0,
        source="ble_rid",
        manufacturer="Meta",
        model="Ray-Ban Meta",
        timestamp=now - 30.0,
    )
    tracker.ingest(
        device_id="sensor-b",
        device_lat=lat,
        device_lon=lon_b,
        device_alt=0.0,
        drone_id="meta-glasses-1",
        rssi=-60,
        estimated_distance_m=10.0,
        source="ble_rid",
        manufacturer="Meta",
        model="Ray-Ban Meta",
        timestamp=now,
    )

    located = tracker.get_located_drones()
    assert len(located) == 1
    device = located[0]
    assert device.position_source == "range_only"


def test_stale_filter_state_does_not_emit_kalman_with_one_current_sensor():
    tracker = SensorTracker()
    lat = 37.0
    lon = -122.0
    now = time.time()
    sensors = [
        ("sensor-a", lat, lon, 8.0),
        ("sensor-b", lat, _east_lon(lat, lon, 20.0), 12.0),
        ("sensor-c", lat + 0.00015, _east_lon(lat, lon, 10.0), 10.0),
    ]

    for step in range(3):
        ts = now - 10.0 + step
        for sid, slat, slon, dist in sensors:
            tracker.ingest(
                device_id=sid,
                device_lat=slat,
                device_lon=slon,
                device_alt=0.0,
                drone_id="rid_filter_stale",
                rssi=-62,
                estimated_distance_m=dist,
                source="ble_rid",
                timestamp=ts,
            )

    tracker.ingest(
        device_id="sensor-a",
        device_lat=lat,
        device_lon=lon,
        device_alt=0.0,
        drone_id="rid_filter_stale",
        rssi=-62,
        estimated_distance_m=8.0,
        source="ble_rid",
        timestamp=now,
    )

    located = tracker.get_located_drones()
    device = next(d for d in located if d.drone_id == "rid_filter_stale")
    assert device.position_source != "kalman"
    assert device.position_source != "particle"


@pytest_asyncio.fixture
async def client():
    """Create an async test client for the FastAPI app."""
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


@pytest.mark.asyncio
async def test_tracking_diagnostics_exposes_range_inputs(monkeypatch, client: AsyncClient):
    tracker = SensorTracker()
    monkeypatch.setattr(detections, "_sensor_tracker", tracker)
    now = time.time()

    tracker.ingest(
        device_id="sensor-a",
        device_lat=37.0,
        device_lon=-122.0,
        device_alt=0.0,
        drone_id="privacy-cam-3",
        rssi=-68,
        estimated_distance_m=18.25,
        source="wifi_ssid",
        ssid="TestCam",
        timestamp=now,
    )
    tracker.get_located_drones()

    resp = await client.get("/detections/tracking/diagnostics")
    assert resp.status_code == 200

    payload = resp.json()
    assert payload["stats"]["current_range_source_counts"]["scanner"] == 1
    assert payload["stats"]["range_defaults"]["rssi_ref"] == -40
    assert payload["stats"]["range_defaults"]["path_loss_outdoor"] == pytest.approx(2.5)

    drone = next(d for d in payload["drones"] if d["drone_id"] == "privacy-cam-3")
    assert drone["last_emit"]["position_source"] == "stationary_range_only"
    assert drone["range_source_mix"]["scanner"] == 1

    current = drone["current_ranges"][0]
    assert current["used_distance_m"] == pytest.approx(18.25)
    assert current["scanner_distance_m"] == pytest.approx(18.25)
    assert current["backend_distance_m"] == pytest.approx(
        rssi_to_distance_m(-68, indoor=False, device_id="sensor-a")
    )
    assert current["distance_source"] == "scanner"
    assert current["range_model"] == "global_outdoor"
