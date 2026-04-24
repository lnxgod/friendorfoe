import time
import uuid

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.services.database import create_tables
from app.services.triangulation import SensorTracker


CAL_TOKEN = "chompchomp"


@pytest_asyncio.fixture
async def client():
    await create_tables()
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


@pytest_asyncio.fixture
async def created_nodes(client: AsyncClient):
    device_ids: list[str] = []
    yield device_ids
    for device_id in device_ids:
        await client.delete(f"/nodes/{device_id}")


async def _create_node(
    client: AsyncClient,
    created_nodes: list[str],
    *,
    position_mode: str,
    lat: float,
    lon: float,
    name: str | None = None,
) -> str:
    device_id = f"test-{position_mode}-{uuid.uuid4().hex[:8]}"
    resp = await client.post(
        "/nodes",
        json={
            "device_id": device_id,
            "name": name or device_id,
            "lat": lat,
            "lon": lon,
            "alt": 0.0,
            "position_mode": position_mode,
        },
    )
    assert resp.status_code == 201, resp.text
    created_nodes.append(device_id)
    return device_id


@pytest.mark.asyncio
async def test_node_position_mode_round_trips(client: AsyncClient, created_nodes: list[str]):
    device_id = await _create_node(
        client,
        created_nodes,
        position_mode="excluded",
        lat=37.123456,
        lon=-122.123456,
        name="Canary",
    )

    resp = await client.put(f"/nodes/{device_id}", json={"position_mode": "active"})
    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["device_id"] == device_id
    assert payload["position_mode"] == "active"
    assert payload["geometry_enabled"] is True

    resp = await client.get("/nodes")
    assert resp.status_code == 200
    node = next(n for n in resp.json()["nodes"] if n["device_id"] == device_id)
    assert node["position_mode"] == "active"
    assert node["geometry_enabled"] is True


@pytest.mark.asyncio
async def test_nodes_status_keeps_saved_coords_but_marks_excluded(
    monkeypatch: pytest.MonkeyPatch,
    client: AsyncClient,
    created_nodes: list[str],
):
    device_id = await _create_node(
        client,
        created_nodes,
        position_mode="excluded",
        lat=37.500001,
        lon=-122.500001,
    )
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            device_id: {
                "device_id": device_id,
                "last_seen": now,
                "detection_count": 0,
                "total_batches": 1,
                "total_detections": 0,
                "lat": 11.0,
                "lon": 22.0,
                "ip": "192.168.42.202",
                "firmware_version": "0.63.9-test",
                "board_type": "uplink-s3",
                "scanners": [],
                "source_fixups_total": 0,
                "source_fixups_by_rule": {},
            }
        },
    )

    resp = await client.get("/detections/nodes/status")
    assert resp.status_code == 200
    node = resp.json()["nodes"][0]
    assert node["device_id"] == device_id
    assert node["lat"] == pytest.approx(37.500001)
    assert node["lon"] == pytest.approx(-122.500001)
    assert node["position_mode"] == "excluded"
    assert node["geometry_enabled"] is False
    assert node["geometry_status"] == "excluded_for_canary_testing"


@pytest.mark.asyncio
async def test_excluded_node_ingests_but_does_not_join_geometry(
    monkeypatch: pytest.MonkeyPatch,
    client: AsyncClient,
    created_nodes: list[str],
):
    device_id = await _create_node(
        client,
        created_nodes,
        position_mode="excluded",
        lat=37.700001,
        lon=-122.700001,
    )
    tracker = SensorTracker()
    monkeypatch.setattr(detections, "_sensor_tracker", tracker)

    resp = await client.post(
        "/detections/drones",
        json={
            "device_id": device_id,
            "device_lat": 1.0,
            "device_lon": 2.0,
            "device_alt": 0.0,
            "timestamp": int(time.time()),
            "detections": [
                {
                    "drone_id": "TEST-DRONE-1",
                    "source": "wifi_ssid",
                    "confidence": 0.9,
                    "rssi": -48,
                    "estimated_distance_m": 8.0,
                    "ssid": "DJI-TEST",
                    "bssid": "AA:BB:CC:DD:EE:FF",
                }
            ],
        },
    )
    assert resp.status_code == 200, resp.text

    assert device_id not in tracker.sensors
    obs = tracker.observations["AP:AA:BB:CC:DD:EE:FF"][device_id]
    assert obs.sensor_lat == 0.0
    assert obs.sensor_lon == 0.0

    resp = await client.get("/detections/drones/map", params={"exclude_known": "false"})
    assert resp.status_code == 200
    assert resp.json()["sensor_count"] == 0


@pytest.mark.asyncio
async def test_excluded_nodes_are_ignored_by_calibration_and_walk(
    monkeypatch: pytest.MonkeyPatch,
    client: AsyncClient,
    created_nodes: list[str],
):
    active_id = await _create_node(
        client,
        created_nodes,
        position_mode="active",
        lat=37.800001,
        lon=-122.800001,
    )
    excluded_id = await _create_node(
        client,
        created_nodes,
        position_mode="excluded",
        lat=37.810001,
        lon=-122.810001,
    )
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            active_id: {
                "device_id": active_id,
                "last_seen": now,
                "detection_count": 0,
                "total_batches": 1,
                "total_detections": 0,
                "lat": 37.800001,
                "lon": -122.800001,
                "ip": "192.168.42.10",
                "source_fixups_total": 0,
                "source_fixups_by_rule": {},
            },
            excluded_id: {
                "device_id": excluded_id,
                "last_seen": now,
                "detection_count": 0,
                "total_batches": 1,
                "total_detections": 0,
                "lat": 37.810001,
                "lon": -122.810001,
                "ip": "192.168.42.11",
                "source_fixups_total": 0,
                "source_fixups_by_rule": {},
            },
        },
    )

    resp = await client.get(
        "/detections/calibrate/walk/sensors",
        headers={"X-Cal-Token": CAL_TOKEN},
    )
    assert resp.status_code == 200
    sensor_ids = {s["device_id"] for s in resp.json()["sensors"]}
    assert active_id in sensor_ids
    assert excluded_id not in sensor_ids

    resp = await client.post(
        "/detections/calibrate/walk/checkpoint",
        headers={"X-Cal-Token": CAL_TOKEN},
        json={
            "session_id": "fake-session",
            "sensor_id": excluded_id,
            "lat": 37.81,
            "lon": -122.81,
        },
    )
    assert resp.status_code == 409
    assert "excluded from geometry" in resp.text

    resp = await client.post("/detections/calibrate")
    assert resp.status_code == 410
    assert "Android walk calibration workflow" in resp.text

    for path in (
        "/detections/calibrate/status",
        "/detections/calibrate/history",
        "/detections/calibrate/matrix",
        "/detections/calibrate/audit",
    ):
        resp = await client.get(path)
        assert resp.status_code == 410
        assert "Android walk calibration workflow" in resp.text
