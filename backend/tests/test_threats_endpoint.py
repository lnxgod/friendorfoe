"""Threat dashboard endpoint shape + filter behaviour.

Doesn't try to mock every upstream service — instead boots the real app
and asserts on the response contract (keys, types, severity sorting,
filter application). That's what the frontend actually depends on.
"""

import pytest
from httpx import AsyncClient, ASGITransport

from app.main import app
from app.routers import detections


@pytest.mark.asyncio
async def test_threats_endpoint_is_reachable_and_well_shaped():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as c:
        r = await c.get("/detections/threats")
        assert r.status_code == 200, r.text
        body = r.json()
        assert "threats" in body
        assert "summary" in body
        assert "generated_at" in body
        # Summary shape — frontend header counts depend on these exact keys
        s = body["summary"]
        for k in ("total", "by_severity", "by_kind", "triangulated"):
            assert k in s, f"summary missing {k}"
        for k in ("critical", "warning", "info"):
            assert k in s["by_severity"], f"by_severity missing {k}"
        for k in ("entity", "anomaly", "rf_anomaly", "drone"):
            assert k in s["by_kind"], f"by_kind missing {k}"
        assert isinstance(body["threats"], list)


@pytest.mark.asyncio
async def test_threats_honours_min_severity_filter():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as c:
        r_all = await c.get("/detections/threats?min_severity=info")
        r_crit = await c.get("/detections/threats?min_severity=critical")
        assert r_all.status_code == 200 and r_crit.status_code == 200
        crit_total = r_crit.json()["summary"]["total"]
        all_total = r_all.json()["summary"]["total"]
        # critical-only must never exceed include-info
        assert crit_total <= all_total


@pytest.mark.asyncio
async def test_threats_severity_sorted_worst_first():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as c:
        r = await c.get("/detections/threats")
        threats = r.json()["threats"]
        rank = {"critical": 3, "warning": 2, "info": 1}
        last_rank = 99
        for t in threats:
            cur = rank.get(t["severity"], 0)
            assert cur <= last_rank, f"severity out of order: {t}"
            last_rank = cur


@pytest.mark.asyncio
async def test_threats_page_static_file_served():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as c:
        r = await c.get("/threats")
        assert r.status_code == 200
        assert "Threat Dashboard" in r.text


@pytest.mark.asyncio
async def test_threats_dedupes_repeated_drone_alerts(monkeypatch):
    original_alerts = list(detections._drone_alerts)
    detections._drone_alerts.clear()
    detections._drone_alerts.extend([
        {
            "alert_type": "drone_detected",
            "severity": "warning",
            "drone_id": "DRONE-ONE",
            "classification": "likely_drone",
            "source": "wifi_ssid",
            "rssi": -62,
            "manufacturer": "DJI",
            "device_id": "node-a",
            "timestamp": 1000.0,
        },
        {
            "alert_type": "drone_detected",
            "severity": "critical",
            "drone_id": "DRONE-ONE",
            "classification": "confirmed_drone",
            "source": "wifi_dji_ie",
            "rssi": -51,
            "manufacturer": "DJI",
            "device_id": "node-b",
            "timestamp": 1005.0,
        },
    ])

    try:
        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://testserver") as c:
            r = await c.get("/detections/threats", params={"include_rf_anomalies": "false"})
            assert r.status_code == 200, r.text
            drones = [t for t in r.json()["threats"] if t.get("kind") == "drone" and t.get("drone_id") == "DRONE-ONE"]
            assert len(drones) == 1
            assert drones[0]["severity"] == "critical"
            assert set(drones[0]["sensors_active"]) == {"node-a", "node-b"}
    finally:
        detections._drone_alerts.clear()
        detections._drone_alerts.extend(original_alerts)
