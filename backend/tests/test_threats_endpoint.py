"""Threat dashboard endpoint shape + filter behaviour.

Doesn't try to mock every upstream service — instead boots the real app
and asserts on the response contract (keys, types, severity sorting,
filter application). That's what the frontend actually depends on.
"""

import pytest
from httpx import AsyncClient, ASGITransport

from app.main import app


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
