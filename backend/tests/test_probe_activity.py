import time
from collections import deque

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.models.schemas import StoredDetection
from app.routers import detections
from app.services.probe_activity import probe_activity

NOW = 1_800_000_000.0


def probe(**overrides):
    values = dict(drone_id="probe_02:11:22:33:44:55", bssid="02:11:22:33:44:55",
                  device_id="nearby", source="wifi_probe_request", confidence=0.05,
                  received_at=NOW - 2, rssi=-65, ssid="Home, Wi-Fi", ie_hash="12345678")
    values.update(overrides)
    return StoredDetection(**values)


def test_scope_names_and_signal_do_not_leak_between_sensors_or_same_fingerprints():
    rows = probe_activity([
        probe(probed_ssids=["Home", " Wi-Fi"]),
        probe(device_id="remote", ssid="Remote", rssi=-20),
        probe(bssid="04:11:22:33:44:55", ssid="Other phone"),
        probe(received_at=NOW - 10, rssi=-30),
    ], now=NOW, sensor_id="nearby")
    assert len(rows) == 2
    row = next(r for r in rows if r["mac"].startswith("02"))
    assert row["rssi"] == -65
    assert row["locally_administered"] is True
    assert row["reports"] == 2
    assert [t["ssid"] for t in row["targets"]] == ["Home, Wi-Fi"]


def test_wildcards_unknown_names_stale_delayed_and_invalid_reports():
    rows = probe_activity([
        probe(ssid=None, class_reason="Wi-Fi wildcard probe"),
        probe(ssid=None),
        probe(received_at=NOW - 301),
        probe(timestamp=int((NOW - 600) * 1000)),
        probe(bssid="FF:FF:FF:FF:FF:FF"),
        probe(bssid="not a mac"),
        probe(received_at=NOW + 100),
        probe(source="wifi_ssid"),
    ], now=NOW, sensor_id="nearby")
    assert len(rows) == 1
    assert rows[0]["reports"] == 2
    assert rows[0]["wildcard_reports"] == 1
    assert rows[0]["unknown_reports"] == 1
    assert rows[0]["targets"] == []


def test_boundary_and_unknown_signal():
    row = probe_activity([probe(received_at=NOW - 300, rssi=0)], now=NOW, sensor_id="nearby")[0]
    assert row["rssi"] is None
    assert row["age_s"] == 300


@pytest.mark.asyncio
async def test_endpoint_requires_scanner_selection_and_validates_window(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_recent_detections", deque([probe(received_at=now)]))
    monkeypatch.setattr(detections, "_node_heartbeats", {})
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        body = (await client.get("/detections/probes/activity")).json()
        assert body["transmitters"] == []
        assert body["observers"][0]["sensor_id"] == "nearby"
        scoped = (await client.get("/detections/probes/activity?sensor_id=nearby")).json()
        assert scoped["transmitters"][0]["mac"] == "02:11:22:33:44:55"
        assert (await client.get("/detections/probes/activity?max_age_s=0")).status_code == 400


@pytest.mark.parametrize("channel,expected", [(2412, 1), (2484, 14), (5180, 36), (6, 6), (9999, None)])
def test_legacy_frequency_field_is_presented_as_channel(channel, expected):
    assert probe_activity([probe(channel=channel)], now=NOW, sensor_id="nearby")[0]["channel"] == expected


def test_literal_broadcast_network_name_is_not_a_wildcard():
    row = probe_activity([probe(ssid="(broadcast)", probed_ssids=["(broadcast)"])], now=NOW, sensor_id="nearby")[0]
    assert row["wildcard_reports"] == 0
    assert row["targets"][0]["ssid"] == "(broadcast)"
