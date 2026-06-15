from app.services.privacy_devices import (
    classify_privacy_device,
    privacy_summary,
)
from app.services.privacy_signature_catalog import (
    match_privacy_wifi_ssid,
    validate_privacy_signature_catalog,
)
from app.services.rf_identity import enrich_rf_evidence


def test_privacy_device_fields_for_skimmer():
    entry = {
        "device_type": "Card Skimmer (suspect)",
        "manufacturer": "Card Skimmer (suspect)",
        "current_rssi": -54,
        "source": "ble_fingerprint",
    }

    enriched = classify_privacy_device(entry)

    assert enriched["privacy_kind"] == "SKIMMER"
    assert enriched["risk_level"] == "high"
    assert enriched["display_label"] == "SKIMMER"
    assert "54dB" in enriched["display_detail"]


def test_privacy_summary_counts_beacon_density_and_kinds():
    devices = [
        classify_privacy_device({
            "device_type": "Venue Beacon",
            "manufacturer": "Estimote",
            "current_rssi": -64,
            "ble_svc_uuids": "FEAA",
        }),
        classify_privacy_device({
            "device_type": "Event Badge",
            "manufacturer": "Bizzabo",
            "current_rssi": -62,
        }),
    ]

    summary = privacy_summary(devices)

    assert summary["privacy_kind_counts"]["VENUE_BEACON"] == 1
    assert summary["privacy_kind_counts"]["EVENT_BADGE"] == 1
    assert summary["beacon_density"] == 1


def test_apple_continuity_is_sanitized_and_informational():
    entry = {
        "device_type": "Apple Device",
        "manufacturer": "Apple",
        "current_rssi": -58,
        "ble_apple_type": 0x10,
        "apple_continuity": {
            "activity": "nearby_action",
            "auth_tag": "ABCDEF",
            "auth_tag_hash": "hash-ok",
        },
    }

    enriched = classify_privacy_device(entry)

    assert enriched["privacy_kind"] == "APPLE_CONTINUITY"
    assert enriched["risk_level"] == "info"
    assert enriched["apple_continuity"]["auth_tag_hash"] == "hash-ok"
    assert "auth_tag" not in enriched["apple_continuity"]


def test_findmy_remains_tracker_privacy_kind():
    enriched = classify_privacy_device({
        "device_type": "FindMy Accessory",
        "manufacturer": "Apple",
        "is_tracker": True,
        "current_rssi": -52,
        "ble_apple_type": 0x12,
    })

    assert enriched["privacy_kind"] == "TRACKER_NEAR"
    assert enriched["display_label"] == "TRACKER NEAR"


def test_ble_service_uuid_privacy_signatures_classify_without_text_labels():
    tracker = classify_privacy_device({
        "source": "ble_fingerprint",
        "ble_svc_uuids": "FCB2",
        "current_rssi": -55,
    })
    assert tracker["privacy_kind"] == "TRACKER_NEAR"
    assert tracker["display_label"] == "TRACKER NEAR"

    findmy = classify_privacy_device({
        "source": "ble_fingerprint",
        "ble_svc_uuids": "FD44",
        "current_rssi": -54,
    })
    assert findmy["privacy_kind"] == "TRACKER_NEAR"
    assert findmy["display_label"] == "TRACKER NEAR"

    chipolo = classify_privacy_device({
        "source": "ble_fingerprint",
        "ble_svc_uuids": "FE33",
        "current_rssi": -56,
    })
    assert chipolo["privacy_kind"] == "TRACKER_NEAR"
    assert any(item["field"] == "ble_service_signature" for item in chipolo["evidence"])

    exposure = classify_privacy_device({
        "source": "ble_fingerprint",
        "ble_svc_uuids": "FD6F",
        "current_rssi": -55,
    })
    assert exposure["privacy_kind"] != "TRACKER_NEAR"

    camera = classify_privacy_device({
        "source": "ble_fingerprint",
        "ble_svc_uuids": "0000fd3a-0000-1000-8000-00805f9b34fb",
        "current_rssi": -57,
    })
    assert camera["privacy_kind"] == "CAMERA_NEAR"
    assert camera["display_label"] == "CAMERA NEAR"
    assert any(item["field"] == "ble_service_signature" for item in camera["evidence"])

    hid = classify_privacy_device({
        "source": "ble_fingerprint",
        "ble_svc_uuids": "1812",
        "current_rssi": -58,
    })
    assert hid["privacy_kind"] == "BLE_HID"
    assert hid["display_label"] == "HID NEAR"


def test_privacy_signature_catalog_matches_wifi_privacy_aps():
    assert validate_privacy_signature_catalog() == []

    tapo = match_privacy_wifi_ssid("Tapo_Cam_ABCD")
    assert tapo is not None
    assert tapo["manufacturer"] == "TP-Link"
    assert tapo["class_reason"] == "privacy:camera:tapo"

    swann = match_privacy_wifi_ssid("Swann-SWIFI-1a2b3c")
    assert swann is not None
    assert swann["manufacturer"] == "Swann"
    assert swann["class_reason"] == "privacy:camera:swann"

    viofo = match_privacy_wifi_ssid("VIOFO-A229-Pro")
    assert viofo is not None
    assert viofo["manufacturer"] == "Viofo"
    assert viofo["class_reason"] == "privacy:dashcam:viofo"

    arlo = match_privacy_wifi_ssid("Arlo-VMB-1234567")
    assert arlo is not None
    assert arlo["manufacturer"] == "Arlo"

    deauther = match_privacy_wifi_ssid("pwnd")
    assert deauther is not None
    assert deauther["privacy_kind"] == "WIFI_ATTACK_TOOL"
    assert deauther["class_reason"] == "attack_tool:deauther"

    pineapple = match_privacy_wifi_ssid("Pineapple_ABCD")
    assert pineapple is not None
    assert pineapple["confidence"] == 0.95

    assert match_privacy_wifi_ssid("Campus-WiFi") is None
    assert match_privacy_wifi_ssid("UFO-Arcade") is None


def test_wifi_privacy_ap_maps_to_camera_privacy_kind():
    enriched = classify_privacy_device({
        "source": "wifi_ap_inventory",
        "ssid": "Ring Setup 12",
        "manufacturer": "Ring",
        "device_type": "Doorbell Camera",
        "class_reason": "privacy:doorbell:ring",
        "current_rssi": -52,
    })

    assert enriched["privacy_kind"] == "CAMERA_NEAR"
    assert enriched["risk_level"] == "high"
    assert enriched["display_label"] == "CAMERA NEAR"


def test_attack_tool_wifi_ap_maps_to_wifi_tool_privacy_kind():
    enriched = classify_privacy_device({
        "source": "wifi_ap_inventory",
        "ssid": "pwnd",
        "manufacturer": "Spacehuhn",
        "device_type": "Attack Tool",
        "class_reason": "attack_tool:deauther",
        "current_rssi": -62,
    })

    assert enriched["privacy_kind"] == "WIFI_ATTACK_TOOL"
    assert enriched["display_label"] == "WIFI TOOL"


def test_rf_identity_uses_privacy_signature_for_wifi_inventory():
    meta = enrich_rf_evidence(
        source="wifi_ap_inventory",
        drone_id="privacy_wifi:001122334455",
        bssid="00:11:22:33:44:55",
        ssid="Tapo_Cam_ABCD",
        manufacturer="TP-Link",
        model="IP Camera",
        classification="wifi_device",
        class_reason="privacy:camera:tapo",
    )

    assert meta["device_class"] == "suspect_camera"
    assert meta["device_family"] == "camera_or_video"
    assert meta["family_source"] == "privacy_rf_signature"
    assert any("privacy:camera:tapo" in item for item in meta["evidence"])
