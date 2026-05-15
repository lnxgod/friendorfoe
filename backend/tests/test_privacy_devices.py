from app.services.privacy_devices import (
    classify_privacy_device,
    privacy_summary,
)


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
