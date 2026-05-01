import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections
from app.services.apple_continuity import decode_apple_continuity
from app.services.rf_identity import enrich_rf_evidence
from app.services.rf_reference import resolve_mac, source_details
from app.services.wifi_fingerprint import build_wifi_fingerprint_v2


def test_rf_reference_resolves_longest_public_prefix():
    hit = resolve_mac("F4:65:0B:AA:BB:CC")

    assert hit.mac_is_randomized is False
    assert hit.vendor_short == "Espressif"
    assert hit.assignment_type in {"MA-L", "MA-M", "MA-S"}
    assert hit.prefix_bits in {24, 28, 36}
    assert hit.oui_prefix == "F4:65:0B"
    assert hit.vendor_confidence > 0.8


def test_rf_reference_never_assigns_vendor_from_randomized_mac_alone():
    hit = resolve_mac("3A:11:22:33:44:55")

    assert hit.mac_is_randomized is True
    assert hit.vendor_short is None
    assert hit.vendor_source == "randomized_mac"
    assert hit.oui_prefix is None


def test_enrichment_keeps_randomized_mac_truth_but_uses_group_evidence():
    meta = enrich_rf_evidence(
        source="wifi_probe_request",
        bssid="3A:11:22:33:44:55",
        macs=[
            "3A:11:22:33:44:55",
            "F4:65:0B:AA:BB:CC",
            "00:70:07:AA:BB:CC",
        ],
        probed_ssids=["TeamCharityCase-Lab"],
        ie_hash="AABBCCDD",
        classification="wifi_device",
    )

    assert meta["mac_is_randomized"] is True
    assert meta["brand"] == "Espressif"
    assert meta["brand_source"] == "group_oui_majority"
    assert meta["known_network_label"] == "TeamCharityCase lab/property"
    assert meta["vendor_source"] == "randomized_mac"
    assert meta["representative_oui_prefix"] == "F4:65:0B"
    assert meta["wifi_fingerprint_v2"]["stable_id"] == "IEH:AABBCCDD"
    source_ids = {s["id"] for s in meta["reference_sources"]}
    assert {"group_oui_majority", "operator_ssid_override", "wifi_fingerprint_v2", "randomized_mac"} <= source_ids
    assert any(s["url"] for s in meta["reference_sources"])


def test_source_details_expand_artifact_metadata_for_ui_provenance():
    refs = source_details("ieee_ma_l+nmap_mac_prefixes+wireshark_manuf")

    labels = {r["label"] for r in refs}
    assert {"IEEE RA MA-L", "Nmap mac-prefixes", "Wireshark manuf"} <= labels
    assert all(r["url"] for r in refs)


def test_apple_continuity_decodes_tlvs_and_hashes_auth_tags():
    decoded = decode_apple_continuity(
        raw_mfr_hex="4c0010011a0f0107",
        apple_auth="aabbccddeeff",
    )

    assert decoded is not None
    assert "Nearby Info" in decoded["message_types"]
    assert "Nearby Action" in decoded["message_types"]
    assert "WiFi Password Share" in decoded["nearby_actions"]
    assert decoded["auth_tag_present"] is True
    assert decoded["auth_tag_hash"]
    assert "aabbccddeeff" not in str(decoded)
    assert decoded["label"] == "Apple Device"


def test_empty_apple_flag_does_not_create_fake_apple_evidence():
    assert decode_apple_continuity(apple_flags=0) is None


def test_wifi_fingerprint_v2_is_stable_from_probe_ie_hash():
    fp1 = build_wifi_fingerprint_v2(
        source="wifi_probe_request",
        ie_hash="11223344",
        probed_ssids=["Home", "TeamCharityCase"],
        channel=6,
        auth_m=3,
    )
    fp2 = build_wifi_fingerprint_v2(
        source="wifi_probe_request",
        ie_hash="11223344",
        probed_ssids=["Home", "TeamCharityCase"],
        channel=6,
        auth_m=3,
    )

    assert fp1["stable_id"] == fp2["stable_id"] == "IEH:11223344"
    assert fp1["auth_mode"] == "wpa2_psk"
    assert fp1["band"] == "2.4GHz"
    assert fp1["confidence"] >= 0.8


def test_wifi_fingerprint_v2_accepts_frequency_mhz_channel_field():
    fp = build_wifi_fingerprint_v2(
        source="wifi_ap_inventory",
        ssid="LabNet",
        channel=2462,
        auth_m=3,
    )

    assert fp["band"] == "2.4GHz"
    assert fp["wifi_generation_hint"] == "2.4GHz-observed"


def test_wifi_fingerprint_does_not_treat_protocol_label_as_vendor():
    fp = build_wifi_fingerprint_v2(
        source="wifi_assoc",
        manufacturer="WiFi-Assoc",
    )

    assert "Scanner/AP vendor label: WiFi-Assoc" not in fp["evidence"]


@pytest.mark.asyncio
async def test_rf_reference_status_endpoint_reports_artifact_and_coverage():
    old_recent = detections._recent_detections
    detections._recent_detections = detections.deque(maxlen=10)
    try:
        detections._recent_detections.append(
            detections.StoredDetection(
                drone_id="probe_F4:65:0B:AA:BB:CC",
                source="wifi_probe_request",
                confidence=0.4,
                bssid="F4:65:0B:AA:BB:CC",
                device_id="node-a",
                received_at=detections.time.time(),
            )
        )
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
            resp = await client.get("/detections/rf/reference/status")
        assert resp.status_code == 200
        data = resp.json()
        assert data["mac_prefix_count"] > 50000
        assert data["ble_company_count"] > 3000
        assert data["recent_mac_enriched"] == 1
    finally:
        detections._recent_detections = old_recent
