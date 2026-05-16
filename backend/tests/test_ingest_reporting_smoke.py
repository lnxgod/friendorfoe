import time

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.services.database import create_tables

SMOKE_VERSION = "0.64.38-badge-live"


@pytest_asyncio.fixture
async def client(monkeypatch: pytest.MonkeyPatch):
    await create_tables()
    monkeypatch.setattr(detections, "_node_heartbeats", {})
    detections._ingest_rejection_events.clear()
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


def _smoke_batch(device_id: str = "uplink_SMOKE01") -> dict:
    return {
        "device_id": device_id,
        "device_lat": 37.333,
        "device_lon": -122.444,
        "device_alt": 12.0,
        "timestamp": int(time.time()),
        "firmware_version": SMOKE_VERSION,
        "board_type": "uplink-s3",
        "scan_profile": "field",
        "reporting": {
            "network_mode": "backend",
            "backend_enabled": True,
            "uploads_ok": 3,
            "uploads_fail": 0,
            "last_upload_age_s": 1,
        },
        "scanners": [
            {
                "slot": 0,
                "uart": "ble",
                "connected": True,
                "board": "scanner-s3-combo",
                "ver": SMOKE_VERSION,
                "caps": "ble,wifi",
                "health": "ok",
                "role_acked": True,
                "scan_profile": "field",
                "time_sync_state": "fresh",
                "time_valid_count": 3,
                "time_last_valid_age_s": 1,
                "ble_adv_seen": 42,
            }
        ],
        "detections": [],
    }


@pytest.mark.asyncio
async def test_empty_detection_batch_creates_dashboard_visible_node(client: AsyncClient):
    ingest = await client.post("/detections/drones", json=_smoke_batch())
    assert ingest.status_code == 200, ingest.text
    assert ingest.json()["status"] == "ok"
    assert ingest.json()["accepted"] == 0

    nodes_resp = await client.get("/detections/nodes/status")
    assert nodes_resp.status_code == 200
    nodes = nodes_resp.json()["nodes"]
    node = next(n for n in nodes if n["device_id"] == "uplink_SMOKE01")
    assert node["online"] is True
    assert node["firmware_version"] == SMOKE_VERSION
    assert node["board_type"] == "uplink-s3"
    assert node["total_batches"] == 1
    assert node["total_detections"] == 0
    assert node["ip"] == "127.0.0.1"
    assert node["scanners"][0]["uart"] == "ble"
    assert node["scanners"][0]["connected"] is True
    assert node["scanners"][0]["ver"] == SMOKE_VERSION
    assert node["scanners"][0]["health"] == "ok"
    assert node["scanners"][0]["ble_adv_seen"] == 42
    assert node["reporting"]["network_mode"] == "backend"
    assert node["reporting"]["uploads_ok"] == 3

    diag_resp = await client.get("/detections/diagnostics")
    assert diag_resp.status_code == 200
    diag = diag_resp.json()
    ingest_freshness = diag["ingest_freshness"]
    assert ingest_freshness["expected_backend_url"] == "http://fof-server.local:8000/detections/drones"
    assert ingest_freshness["posting_node_count"] == 1
    assert ingest_freshness["online_posting_node_count"] == 1
    assert ingest_freshness["last_ingest_age_s"] is not None
    assert ingest_freshness["last_ingest_age_s"] < 5
    assert ingest_freshness["total_batches"] == 1
    assert "127.0.0.1" in ingest_freshness["last_posting_ips"]

    map_resp = await client.get("/detections/drones/map?exclude_known=false&include_probes=false")
    assert map_resp.status_code == 200
    map_payload = map_resp.json()
    assert map_payload["sensor_count"] >= 1
    assert any(s["device_id"] == "uplink_SMOKE01" for s in map_payload["sensors"])


@pytest.mark.asyncio
async def test_coordinate_drone_detection_reaches_map_payload(client: AsyncClient):
    batch = _smoke_batch("uplink_COORD01")
    batch["detections"] = [
        {
            "drone_id": "RID-COORD-1",
            "source": "ble_rid",
            "confidence": 0.95,
            "rssi": -42,
            "latitude": 37.3341,
            "longitude": -122.4452,
            "altitude_m": 41.0,
            "operator_lat": 37.3300,
            "operator_lon": -122.4400,
            "operator_id": "OP-123",
            "manufacturer": "DJI",
        }
    ]
    ingest = await client.post("/detections/drones", json=batch)
    assert ingest.status_code == 200, ingest.text
    assert ingest.json()["accepted"] == 1

    map_resp = await client.get("/detections/drones/map?exclude_known=false")
    assert map_resp.status_code == 200
    drone = next(d for d in map_resp.json()["drones"] if d["drone_id"] == "RID-COORD-1")
    assert drone["lat"] == pytest.approx(37.3341)
    assert drone["lon"] == pytest.approx(-122.4452)
    assert drone["operator_lat"] == pytest.approx(37.3300)
    assert drone["operator_lon"] == pytest.approx(-122.4400)


@pytest.mark.asyncio
async def test_diagnostics_warns_when_backend_has_no_uplink_batches(client: AsyncClient):
    resp = await client.get("/detections/diagnostics")
    assert resp.status_code == 200
    payload = resp.json()
    assert payload["ingest_freshness"]["posting_node_count"] == 0
    assert payload["ingest_freshness"]["last_ingest_at"] is None
    assert any(
        w["code"] == "no_uplink_batches_received"
        and "no uplink batches" in w["message"]
        for w in payload["system_warnings"]
    )


@pytest.mark.asyncio
async def test_ingest_tolerates_legacy_detection_fields(client: AsyncClient):
    batch = _smoke_batch("uplink_LEGACY01")
    batch.pop("timestamp")
    batch["detections"] = [
        {
            "drone_id": "legacy_probe",
            "source": 5,
            "confidence": 1.4,
            "rssi": -52,
            "probed": "WiFiUFO-123,E88-TEST",
        }
    ]

    resp = await client.post("/detections/drones", json=batch)
    assert resp.status_code == 200, resp.text
    assert resp.json()["accepted"] == 1

    recent = await client.get("/detections/drones/recent")
    assert recent.status_code == 200
    row = next(d for d in recent.json()["detections"] if d["drone_id"] == "legacy_probe")
    assert row["source"] == "wifi_probe_request"
    assert row["confidence"] == 1.0
    assert row["probed_ssids"] == ["WiFiUFO-123", "E88-TEST"]


@pytest.mark.asyncio
async def test_diagnostics_reports_rejected_ingest_batches(client: AsyncClient):
    resp = await client.post("/detections/drones", json={"detections": []})
    assert resp.status_code == 400

    diag_resp = await client.get("/detections/diagnostics")
    assert diag_resp.status_code == 200
    ingest = diag_resp.json()["ingest_freshness"]
    assert ingest["recent_rejected_batches"] == 1
    assert ingest["rejected_by_ip"]["127.0.0.1"] == 1
    assert any(
        w["code"] == "ingest_rejections_detected"
        for w in diag_resp.json()["system_warnings"]
    )
