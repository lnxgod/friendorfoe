import time
from collections import deque

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.models.schemas import StoredDetection
from app.routers import detections
from app.services.rf_identity import (
    build_detection_explanation,
    classify_mac_identity,
    enrich_rf_evidence,
)


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
    assert meta["vendor_long"] is None
    assert meta["oui_prefix"] is None
    assert meta["device_family"] == "wifi_client"
    assert meta["identity_source"] == "probe_ie_hash"
    assert "Probe IE hash: A1B2C3D4" in meta["evidence"]


def test_probe_group_uses_public_oui_majority_for_brand_with_randomized_members():
    meta = enrich_rf_evidence(
        source="wifi_probe_request",
        bssid="3A:11:22:33:44:55",
        macs=[
            "3A:11:22:33:44:55",
            "F4:65:0B:AA:BB:CC",
            "00:70:07:AA:BB:CC",
        ],
        probed_ssids=["TeamCharityCase", "PrinterSetup"],
        ie_hash="4F6E335E",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is True
    assert meta["brand"] == "Espressif"
    assert meta["brand_source"] == "group_oui_majority"
    assert meta["randomized_mac_count"] == 1
    assert meta["public_mac_count"] == 2
    assert meta["public_oui_brands"]["Espressif"] == 2
    assert meta["representative_public_mac"] == "F4:65:0B:AA:BB:CC"
    assert meta["representative_oui_prefix"] == "F4:65:0B"
    assert meta["known_network_label"] == "TeamCharityCase lab/property"
    assert meta["device_family"] == "operator_known_lab_device"
    assert any("Group OUI majority: Espressif" in e for e in meta["evidence"])


def test_randomized_only_known_network_gets_operator_label_not_fake_oui():
    meta = enrich_rf_evidence(
        source="wifi_ap_inventory",
        bssid="C2:07:1D:22:5E:E0",
        ssid="TeamCharityCase-2",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is True
    assert meta["brand"] == "TeamCharityCase lab/property"
    assert meta["brand_source"] == "known_network_label"
    assert meta["device_class"] == "known_owned"
    assert meta["device_family"] == "operator_known_lab_device"
    assert meta["known_network_label"] == "TeamCharityCase lab/property"
    assert any("Known network label" in e for e in meta["evidence"])
    assert "locally_administered_bit" in meta["evidence"]


def test_friendly_camera_oui_adds_family_without_hidden_camera_overclaim():
    meta = enrich_rf_evidence(
        source="wifi_ap_inventory",
        bssid="90:48:9A:12:34:56",
        ssid="FrontDoor",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is False
    assert meta["brand"] == "Ring"
    assert meta["brand_source"] == "friendly_oui"
    assert meta["oui_prefix"] == "90:48:9A"
    assert meta["device_family"] == "camera_or_video"
    assert meta["device_class"] == "surveillance_camera"
    assert any("Curated OUI prefix: Ring" in e for e in meta["evidence"])


def test_randomized_wled_ssid_gets_iot_family_not_fake_brand():
    meta = enrich_rf_evidence(
        source="wifi_ap_inventory",
        bssid="C2:07:1D:22:5E:E0",
        ssid="WLED-Garage",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is True
    assert meta["brand"] is None
    assert meta["brand_source"] == "none_randomized_mac"
    assert meta["device_family"] == "esp32_or_iot_dev_board"
    assert meta["family_source"] == "ssid_pattern"
    assert meta["device_class"] == "suspect_iot"
    assert any("ESP/IoT firmware SSID pattern" in e for e in meta["evidence"])


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


def test_ble_explicit_camera_evidence_stays_suspect_camera():
    meta = enrich_rf_evidence(
        source="ble_fingerprint",
        drone_id="BLE:12345678:Camera",
        bssid="00:11:22:33:44:55",
        manufacturer="Unknown",
        ble_name="YCC365-CAM",
        class_reason="explicit_camera_ble_name",
        classification="unknown_device",
    )

    assert meta["device_class"] == "suspect_camera"
    assert meta["device_class_confidence"] >= 0.7
    assert "BLE local name: YCC365-CAM" in meta["evidence"]


def test_ble_rpa_classification_uses_addr_type():
    mac = classify_mac_identity("00:11:22:33:44:55", source="ble_fingerprint", ble_addr_type=2)

    assert mac.mac_is_randomized is True
    assert mac.mac_identity_kind == "ble_rpa"
    assert mac.mac_reason == "ble_addr_type_rpa"


def test_wifi_assoc_scanner_label_does_not_become_brand():
    meta = enrich_rf_evidence(
        source="wifi_assoc",
        drone_id="STA:66:C0:E2:24:ED:E6→AP:D6:E2:CB:D3:28:65",
        bssid="66:C0:E2:24:ED:E6",
        manufacturer="WiFi-Assoc",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is True
    assert meta["brand"] is None
    assert meta["brand_source"] == "none_randomized_mac"
    assert "Scanner class label: WiFi-Assoc" in meta["evidence"]


def test_remote_id_explanation_is_confirmed_without_ssid_limit():
    meta = enrich_rf_evidence(
        source="ble_rid",
        drone_id="rid_TEST",
        manufacturer="DJI",
        classification="confirmed_drone",
    )

    explanation = build_detection_explanation(
        source="ble_rid",
        classification="confirmed_drone",
        rf_meta=meta,
        sensor_count=2,
    )

    assert explanation["primary_reason"] == "Remote ID broadcast"
    assert explanation["confidence_band"] == "confirmed"
    assert "SSID-only; no Remote ID" not in explanation["limitations"]


def test_fof_drone_explanation_is_test_signal():
    meta = enrich_rf_evidence(
        source="wifi_ssid",
        ssid="FOF-DRONE-TEST",
        classification="test_drone",
    )

    explanation = build_detection_explanation(
        source="wifi_ssid",
        classification="test_drone",
        rf_meta=meta,
        sensor_count=2,
    )

    assert explanation["primary_reason"] == "FOF test drone SSID"
    assert explanation["recommended_action"] == "test_signal"


def test_cheap_drone_ssid_explanation_is_likely_with_ssid_limit():
    meta = enrich_rf_evidence(
        source="wifi_ssid",
        ssid="WiFiUFO-1234",
        classification="likely_drone",
    )

    explanation = build_detection_explanation(
        source="wifi_ssid",
        classification="likely_drone",
        rf_meta=meta,
        sensor_count=1,
    )

    assert explanation["primary_reason"] == "Curated drone SSID prefix"
    assert explanation["confidence_band"] == "likely"
    assert "SSID-only; no Remote ID" in explanation["limitations"]
    assert "Single-sensor observation" in explanation["limitations"]


def test_known_infrastructure_explanation_is_known_ignore():
    meta = enrich_rf_evidence(
        source="wifi_ap_inventory",
        ssid="FoF-LAB",
        classification="known_ap",
    )

    explanation = build_detection_explanation(
        source="wifi_ap_inventory",
        classification="known_ap",
        rf_meta=meta,
        sensor_count=1,
    )

    assert explanation["primary_reason"] == "Known infrastructure"
    assert explanation["confidence_band"] == "known"
    assert explanation["recommended_action"] == "ignore_known"


def test_randomized_probe_explanation_marks_mac_limit():
    meta = enrich_rf_evidence(
        source="wifi_probe_request",
        bssid="3A:11:22:33:44:55",
        probed_ssids=["Coffee"],
        ie_hash="A1B2C3D4",
        classification="wifi_device",
    )

    explanation = build_detection_explanation(
        source="wifi_probe_request",
        classification="wifi_device",
        rf_meta=meta,
        sensor_count=1,
        identity_source=meta["identity_source"],
    )

    assert explanation["primary_reason"] == "Randomized probe identity"
    assert "Randomized MAC" in explanation["limitations"]
    assert "Single-sensor observation" in explanation["limitations"]


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
    assert device["randomized_mac_count"] == 2
    assert set(device["probed_ssids"]) == {"HomeNet", "DJI-1234"}
    assert any(r["relation_type"] == "likely_same_device" for r in device["related_entities"])


@pytest.mark.asyncio
async def test_live_devices_endpoint_adds_rf_meta(monkeypatch):
    monkeypatch.setattr(detections._ble_enricher, "prune_stale", lambda: None)
    monkeypatch.setattr(
        detections._ble_enricher,
        "get_live_devices",
        lambda **_: [{
            "fingerprint": "FP:12345678",
            "device_type": "Meta Smart Glasses",
            "manufacturer": "Meta",
            "last_bssid": "C2:11:22:33:44:55",
            "source": "ble_fingerprint",
            "confidence": 0.9,
            "ble_company_id": 0x0D53,
            "ble_addr_type": 1,
            "ble_ja3": "deadbeef",
            "ble_svc_uuids": "fd5f",
            "ble_apple_type": None,
            "ble_apple_flags": None,
        }],
    )
    monkeypatch.setattr(detections._ble_enricher, "get_summary", lambda: {"active": 1})

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/devices/live")

    assert resp.status_code == 200
    device = resp.json()["devices"][0]
    assert device["mac_is_randomized"] is True
    assert device["brand"]
    assert device["identity_source"] == "ble_ja3"
    assert device["evidence"]


@pytest.mark.asyncio
async def test_probe_endpoint_exposes_group_oui_rollup_and_known_network(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="probe_3A:11:22:33:44:55",
            source="wifi_probe_request",
            confidence=0.05,
            rssi=-55,
            ssid="TeamCharityCase",
            bssid="3A:11:22:33:44:55",
            probed_ssids=["TeamCharityCase"],
            ie_hash="4F6E335E",
            device_id="node-a",
            received_at=now - 5,
            classification="wifi_device",
        ),
        StoredDetection(
            drone_id="probe_F4:65:0B:AA:BB:CC",
            source="wifi_probe_request",
            confidence=0.05,
            rssi=-48,
            ssid="TeamCharityCase",
            bssid="F4:65:0B:AA:BB:CC",
            probed_ssids=["TeamCharityCase", "FoF-Lab"],
            ie_hash="4F6E335E",
            device_id="node-b",
            received_at=now - 3,
            classification="wifi_device",
        ),
        StoredDetection(
            drone_id="probe_00:70:07:AA:BB:CC",
            source="wifi_probe_request",
            confidence=0.05,
            rssi=-50,
            ssid="TeamCharityCase",
            bssid="00:70:07:AA:BB:CC",
            probed_ssids=["TeamCharityCase"],
            ie_hash="4F6E335E",
            device_id="node-c",
            received_at=now - 4,
            classification="wifi_device",
        ),
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/probes", params={"max_age_s": 60})

    assert resp.status_code == 200
    device = resp.json()["devices"][0]
    assert device["identity"] == "PROBE:4F6E335E"
    assert device["brand"] == "Espressif"
    assert device["brand_source"] == "group_oui_majority"
    assert device["public_oui_brands"]["Espressif"] == 2
    assert device["randomized_mac_count"] == 1
    assert device["known_network_label"] == "TeamCharityCase lab/property"
    assert device["device_family"] == "operator_known_lab_device"
    assert device["representative_oui_prefix"] in {"F4:65:0B", "00:70:07"}
    assert set(device["probed_ssids"]) == {"TeamCharityCase", "FoF-Lab"}


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
    assert ap["device_family"] == "wifi_ap"
    assert map_resp.status_code == 200
    assert all(d["drone_id"] != "00:11:22:33:44:55" for d in map_resp.json()["drones"])


@pytest.mark.asyncio
async def test_grouped_endpoint_exposes_curated_drone_ssid_match(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="WiFiUFO-1234",
            source="wifi_ssid",
            confidence=0.50,
            ssid="WiFiUFO-1234",
            bssid="AA:BB:CC:DD:EE:01",
            rssi=-45,
            device_id="node-a",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 1,
            classification="likely_drone",
        ),
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/grouped", params={"max_age_s": 60})

    assert resp.status_code == 200
    device = resp.json()["devices"][0]
    assert device["classification"] == "likely_drone"
    assert device["drone_ssid_match"]["ssid"] == "WiFiUFO-1234"
    assert device["drone_ssid_match"]["matched_prefix"] == "WiFiUFO-"
    assert device["drone_ssid_match"]["source"] == "drone_signature_reference"
    assert "Drone SSID match" in " | ".join(device["evidence"])


@pytest.mark.asyncio
async def test_probe_endpoint_uses_backend_drone_ssid_evidence(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="probe_3A:11:22:33:44:55",
            source="wifi_probe_request",
            confidence=0.50,
            rssi=-55,
            ssid="E88-ABCD",
            bssid="3A:11:22:33:44:55",
            probed_ssids=["E88-ABCD"],
            ie_hash="BEEFBEEF",
            device_id="node-a",
            device_lat=30.0,
            device_lon=-97.0,
            received_at=now - 5,
            classification="likely_drone",
        ),
    ], maxlen=20))

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/probes", params={"max_age_s": 60, "drone_only": "true"})

    assert resp.status_code == 200
    payload = resp.json()
    assert payload["count"] == 1
    device = payload["devices"][0]
    assert device["identity"] == "PROBE:BEEFBEEF"
    assert device["classification"] == "likely_drone"
    assert device["drone_ssid_match"]["ssid"] == "E88-ABCD"
    assert device["drone_ssid_match"]["matched_prefix"] == "E88-"


@pytest.mark.asyncio
async def test_diagnostics_endpoint_reports_drift_stale_calibration_and_drone_ssids(monkeypatch):
    now = time.time()
    monkeypatch.setattr(detections, "_node_heartbeats", {
        "uplink_TEST": {
            "device_id": "uplink_TEST",
            "last_seen": now,
            "detection_count": 1,
            "total_batches": 4,
            "total_detections": 1,
            "lat": 37.1,
            "lon": -122.1,
            "ip": "192.168.1.10",
            "firmware_version": "0.63.0-svc139",
            "board_type": "uplink-s3",
            "time_sync": {
                "time_source": "backend",
                "last_fetch_ok": True,
                "last_success_age_s": 2,
            },
            "scanners": [{
                "uart": "ble",
                "board": "scanner-s3-combo",
                "ver": "0.63.0-svc139",
                "time_sync_state": "stale",
                "time_last_valid_age_s": 120,
            }],
            "source_fixups_total": 0,
            "source_fixups_by_rule": {},
        }
    })
    monkeypatch.setattr(detections, "_recent_detections", deque([
        StoredDetection(
            drone_id="ssid_drone",
            source="wifi_ssid",
            confidence=0.65,
            rssi=-50,
            ssid="FOF-DRONE-TEST",
            bssid="02:11:22:33:44:55",
            device_id="uplink_TEST",
            device_lat=37.1,
            device_lon=-122.1,
            received_at=now - 5,
            classification="test_drone",
        )
    ], maxlen=20))
    monkeypatch.setattr(
        detections._applied_cal_store,
        "summary",
        lambda: {
            "is_trusted": False,
            "active_model_source": "defaults",
            "applied_listener_count": 0,
            "last_calibration": None,
        },
    )

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        resp = await client.get("/detections/diagnostics", params={"max_age_s": 60})

    assert resp.status_code == 200
    payload = resp.json()
    warning_codes = {w["code"] for w in payload["system_warnings"]}
    assert "uplink_firmware_drift" in warning_codes
    assert "scanner_firmware_drift" in warning_codes
    assert "stale_scanner_time" in warning_codes
    assert "calibration_not_ready" in warning_codes
    assert payload["calibration_readiness"]["ready"] is False
    assert payload["firmware_readiness"]["drift_count"] >= 2
    assert payload["recent_drone_ssid_matches"][0]["ssid"] == "FOF-DRONE-TEST"
    assert payload["top_explanations"]["by_primary_reason"]["FOF test drone SSID"] == 1
