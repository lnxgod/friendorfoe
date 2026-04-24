import time

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.services.database import create_tables


@pytest_asyncio.fixture
async def client():
    await create_tables()
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


@pytest.mark.asyncio
async def test_nodes_status_surfaces_uplink_and_scanner_time_health(
    monkeypatch: pytest.MonkeyPatch,
    client: AsyncClient,
):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_TEST01": {
                "device_id": "uplink_TEST01",
                "last_seen": now,
                "detection_count": 0,
                "total_batches": 10,
                "total_detections": 0,
                "lat": 37.1,
                "lon": -122.1,
                "ip": "192.168.42.101",
                "firmware_version": "0.63.0-svc133",
                "board_type": "uplink-s3",
                "time_sync": {
                    "time_source": "backend",
                    "last_fetch_ok": True,
                    "last_success_age_s": 5,
                    "fetch_fail_streak": 0,
                    "broadcast_valid_count": 12,
                    "broadcast_invalid_count": 0,
                },
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo",
                        "ver": "0.63.0-svc133",
                        "tcnt": 6,
                        "time_valid_count": 6,
                        "time_last_valid_age_s": 4,
                        "time_sync_state": "fresh",
                    }
                ],
                "source_fixups_total": 0,
                "source_fixups_by_rule": {},
            }
        },
    )

    resp = await client.get("/detections/nodes/status")
    assert resp.status_code == 200
    payload = resp.json()
    assert payload["count"] == 1
    node = payload["nodes"][0]
    assert node["time_sync"]["sync_health"] == "good"
    assert node["scanners"][0]["time_sync_health"] == "fresh"


@pytest.mark.asyncio
async def test_nodes_status_keeps_legacy_scanner_time_unknown(
    monkeypatch: pytest.MonkeyPatch,
    client: AsyncClient,
):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_LEGACY": {
                "device_id": "uplink_LEGACY",
                "last_seen": now,
                "detection_count": 0,
                "total_batches": 4,
                "total_detections": 0,
                "lat": 37.2,
                "lon": -122.2,
                "ip": "192.168.42.102",
                "firmware_version": "0.63.0-svc132",
                "board_type": "uplink-s3",
                "scanners": [
                    {
                        "uart": "wifi",
                        "board": "scanner-s3-combo",
                        "ver": "0.63.0-svc132",
                        "tcnt": 99,
                        "toff": 1776917681129,
                    }
                ],
                "source_fixups_total": 0,
                "source_fixups_by_rule": {},
            }
        },
    )

    resp = await client.get("/detections/nodes/status")
    assert resp.status_code == 200
    node = resp.json()["nodes"][0]
    assert node["scanners"][0]["tcnt"] == 99
    assert node["scanners"][0]["time_sync_health"] == "unknown"


@pytest.mark.asyncio
async def test_detection_batch_round_trips_time_sync_to_node_status(client: AsyncClient):
    now_ms = int(time.time() * 1000)
    resp = await client.post(
        "/detections/drones",
        json={
            "device_id": "uplink_TIME01",
            "device_lat": 37.3,
            "device_lon": -122.3,
            "device_alt": 0.0,
            "timestamp": int(time.time()),
            "time_sync": {
                "time_source": "local",
                "last_fetch_ok": False,
                "last_success_age_s": 12,
                "fetch_fail_streak": 2,
                "last_backend_epoch_ms": now_ms,
                "last_broadcast_epoch_ms": now_ms,
                "broadcast_valid_count": 5,
                "broadcast_invalid_count": 1,
            },
            "scanners": [
                {
                    "uart": "ble",
                    "board": "scanner-s3-combo-seed",
                    "ver": "0.63.0-svc133",
                    "tcnt": 3,
                    "time_valid_count": 2,
                    "time_last_valid_age_s": 9,
                    "time_sync_state": "fresh",
                }
            ],
            "detections": [],
        },
    )
    assert resp.status_code == 200, resp.text

    resp = await client.get("/detections/nodes/status")
    assert resp.status_code == 200
    node = next(n for n in resp.json()["nodes"] if n["device_id"] == "uplink_TIME01")
    assert node["time_sync"]["time_source"] == "local"
    assert node["time_sync"]["sync_health"] == "warning"
    assert node["scanners"][0]["time_sync_health"] == "fresh"
