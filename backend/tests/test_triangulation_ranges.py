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
