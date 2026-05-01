"""Tests for wifi_assoc → connected-AP enrichment in /detections/grouped."""

import time
from collections import deque
from types import SimpleNamespace

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.models.schemas import StoredDetection


def test_record_ap_observation_stores_bssid_to_ssid(monkeypatch):
    monkeypatch.setattr(detections, "_bssid_to_ap", {})
    det = SimpleNamespace(
        source="wifi_ap_inventory",
        bssid="aa:bb:cc:11:22:33",
        ssid="HomeNet-5G",
        manufacturer="Eero",
        channel=2412,
        rssi=-55,
    )
    detections._record_ap_observation(det)
    hit = detections._lookup_connected_ap("AA:BB:CC:11:22:33")
    assert hit is not None
    assert hit["ssid"] == "HomeNet-5G"
    assert hit["vendor"] == "Eero"


def test_record_ap_observation_ignores_non_ap_sources(monkeypatch):
    monkeypatch.setattr(detections, "_bssid_to_ap", {})
    det = SimpleNamespace(
        source="wifi_assoc",  # not an AP-bearing source — clients don't beacon
        bssid="aa:bb:cc:11:22:34",
        ssid="ShouldNotCache",
        manufacturer="Anything",
        channel=2412,
        rssi=-55,
    )
    detections._record_ap_observation(det)
    assert detections._lookup_connected_ap("aa:bb:cc:11:22:34") is None


def test_lookup_returns_none_for_unknown_bssid(monkeypatch):
    monkeypatch.setattr(detections, "_bssid_to_ap", {})
    assert detections._lookup_connected_ap("11:22:33:44:55:66") is None
    assert detections._lookup_connected_ap(None) is None
    assert detections._lookup_connected_ap("") is None


def test_lookup_expires_stale_entries(monkeypatch):
    monkeypatch.setattr(detections, "_bssid_to_ap", {
        "AA:BB:CC:11:22:33": {
            "ssid": "Stale",
            "vendor": None,
            "channel": None,
            "rssi": None,
            "ts": time.time() - detections._BSSID_AP_TTL_S - 5,
        },
    })
    assert detections._lookup_connected_ap("AA:BB:CC:11:22:33") is None
    assert "AA:BB:CC:11:22:33" not in detections._bssid_to_ap


@pytest.mark.asyncio
async def test_grouped_endpoint_enriches_wifi_assoc_with_connected_ap(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_bssid_to_ap", {
        "AA:BB:CC:11:22:33": {
            "ssid": "HomeNet-5G",
            "vendor": "Eero",
            "channel": 2412,
            "rssi": -50,
            "ts": now,
        },
    })
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="DE:AD:BE:EF:00:01",  # client MAC
            source="wifi_assoc",
            confidence=0.5,
            rssi=-60,
            ssid=None,  # association events often have no SSID — that's the gap we're filling
            bssid="AA:BB:CC:11:22:33",
            channel=2412,
            device_id="node-a",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 1,
            classification="wifi_device",
        ),
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/grouped", params={"max_age_s": 60, "exclude_known_ap": "false"})

    assert resp.status_code == 200, resp.text
    devices = resp.json()["devices"]
    assoc = next(d for d in devices if d["source"] == "wifi_assoc")
    assert assoc["connected_ap_ssid"] == "HomeNet-5G"
    assert assoc["connected_ap_vendor"] == "Eero"


@pytest.mark.asyncio
async def test_grouped_endpoint_omits_connected_ap_for_non_assoc(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_bssid_to_ap", {
        "AA:BB:CC:11:22:33": {
            "ssid": "HomeNet-5G", "vendor": "Eero",
            "channel": None, "rssi": None, "ts": now,
        },
    })
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="AA:BB:CC:11:22:33",
            source="wifi_ap_inventory",
            confidence=0.01,
            rssi=-45,
            ssid="HomeNet-5G",
            bssid="AA:BB:CC:11:22:33",
            channel=2412,
            device_id="node-a",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 1,
            classification="wifi_device",
        ),
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/grouped", params={"max_age_s": 60, "exclude_known_ap": "false"})

    assert resp.status_code == 200, resp.text
    devices = resp.json()["devices"]
    inv = next(d for d in devices if d["source"] == "wifi_ap_inventory")
    assert "connected_ap_ssid" not in inv
