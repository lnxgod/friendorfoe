import time
from collections import deque

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.models.schemas import StoredDetection
from app.routers import detections
from app.services.rf_identity import classify_mac_identity, enrich_rf_evidence


def test_randomized_wifi_mac_has_no_fake_brand():
    meta = enrich_rf_evidence(
        source="wifi_probe_request",
        drone_id="probe_3A:11:22:33:44:55",
        bssid="3A:11:22:33:44:55",
        probed_ssids=["HomeNet", "DJI-1234"],
        ie_hash="A1B2C3D4",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is True
    assert meta["mac_identity_kind"] == "randomized"
    assert meta["brand"] is None
    assert meta["brand_source"] == "none_randomized_mac"
    assert meta["identity_source"] == "probe_ie_hash"
    assert "Probe IE hash: A1B2C3D4" in meta["evidence"]


def test_ble_ambiguous_camera_name_demotes_to_suspect_iot():
    meta = enrich_rf_evidence(
        source="ble_fingerprint",
        drone_id="BLE:12345678:Smart Home",
        bssid="4A:22:33:44:55:66",
        manufacturer="Smart Home",
        ble_name="ELK-BLEDOM",
        class_reason="ambiguous_iot_ble_name",
        classification="unknown_device",
    )

    assert meta["device_class"] == "suspect_iot"
    assert meta["device_class_confidence"] < 0.5
    assert meta["brand"] == "Smart Home"
    assert "BLE local name: ELK-BLEDOM" in meta["evidence"]


def test_ble_rpa_classification_uses_addr_type():
    mac = classify_mac_identity("00:11:22:33:44:55", source="ble_fingerprint", ble_addr_type=2)

    assert mac.mac_is_randomized is True
    assert mac.mac_identity_kind == "ble_rpa"
    assert mac.mac_reason == "ble_addr_type_rpa"


@pytest.mark.asyncio
async def test_probe_endpoint_exposes_evidence_and_ie_hash_relation(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="probe_3A:11:22:33:44:55",
            source="wifi_probe_request",
            confidence=0.05,
            rssi=-55,
            ssid="HomeNet",
            bssid="3A:11:22:33:44:55",
            probed_ssids=["HomeNet", "DJI-1234"],
            ie_hash="A1B2C3D4",
            device_id="node-a",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 5,
            classification="wifi_device",
        ),
        StoredDetection(
            drone_id="probe_7E:AA:BB:CC:DD:EE",
            source="wifi_probe_request",
            confidence=0.05,
            rssi=-60,
            ssid="HomeNet",
            bssid="7E:AA:BB:CC:DD:EE",
            probed_ssids=["HomeNet"],
            ie_hash="A1B2C3D4",
            device_id="node-b",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 4,
            classification="wifi_device",
        ),
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/probes", params={"max_age_s": 60})

    assert resp.status_code == 200
    device = resp.json()["devices"][0]
    assert device["identity"] == "PROBE:A1B2C3D4"
    assert device["identity_source"] == "probe_ie_hash"
    assert device["mac_is_randomized"] is True
    assert set(device["probed_ssids"]) == {"HomeNet", "DJI-1234"}
    assert any(r["relation_type"] == "likely_same_device" for r in device["related_entities"])


@pytest.mark.asyncio
async def test_ap_inventory_endpoint_is_diagnostic(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="00:11:22:33:44:55",
            source="wifi_ap_inventory",
            confidence=0.01,
            rssi=-45,
            ssid="GarageAP",
            bssid="00:11:22:33:44:55",
            auth_m=3,
            channel=2412,
            manufacturer="Unknown",
            device_id="node-a",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 3,
            classification="wifi_device",
        )
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/wifi/ap-inventory", params={"max_age_s": 60})
        map_resp = await client.get("/detections/drones/map")

    assert resp.status_code == 200
    ap = resp.json()["aps"][0]
    assert ap["ssid"] == "GarageAP"
    assert ap["device_class"] == "wifi_ap"
    assert map_resp.status_code == 200
    assert all(d["drone_id"] != "00:11:22:33:44:55" for d in map_resp.json()["drones"])
