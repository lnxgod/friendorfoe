import pytest

from app.services.personal_recorders import match_personal_recorder
from app.services.privacy_devices import classify_privacy_device
from app.services.rf_identity import enrich_rf_evidence


@pytest.mark.parametrize("name,brand", [
    ("PLAUDAB12", "Plaud"), ("PLAUD NOTE", "Plaud"), ("Plaud Note Pro", "Plaud"),
    ("NotePin_123", "Plaud"), ("plaud-ab12", "Plaud"),
    ("omi", "Omi"), ("Limitless Pendant", "Limitless"),
    ("Bee_AB12", "Bee"), ("friend_1234", "Friend"), ("Fieldy", "Fieldy"),
])
def test_recorder_name_and_presentation(name, brand):
    match = match_personal_recorder(name)
    assert match["manufacturer"] == brand
    assert match["confidence"] == 0.75
    entry = classify_privacy_device({"ble_name": name, "source": "ble_fingerprint", "current_rssi": -55})
    assert entry["privacy_kind"] == "VOICE_RECORDER"
    assert entry["risk_level"] == "medium"
    assert "recording status unknown" in entry["display_detail"]
    rf = enrich_rf_evidence(source="ble_fingerprint", ble_name=name)
    assert rf["device_class"] == "voice_recorder"
    assert rf["device_family"] == "audio"


@pytest.mark.parametrize("uuid,brand", [
    ("19b10000-e8f2-537e-4f6c-d104768a1214", "Omi"),
    ("632de001-604c-446b-a80f-7963e950f3fb", "Limitless"),
    ("03d5d5c4-a86c-11ee-9d89-8f2089a49e7e", "Bee"),
    ("1a3fd0e7-b1f3-ac9e-2e49-b647b2c4f8da", "Friend"),
])
def test_custom_uuid_without_name_and_with_conflicting_name(uuid, brand):
    assert match_personal_recorder(None, "180F," + uuid.upper())["manufacturer"] == brand
    assert match_personal_recorder("PLAUD NOTE", uuid)["manufacturer"] == brand
    assert match_personal_recorder(None, uuid[:-1] + "0") is None
    rf = enrich_rf_evidence(source="ble_fingerprint", ble_svc_uuids=uuid)
    assert rf["device_class"] == "voice_recorder"
    assert rf["device_family"] == "audio"
    assert classify_privacy_device({"ble_svc_uuids": uuid})["privacy_kind"] == "VOICE_RECORDER"


@pytest.mark.parametrize("name", [
    None, "", "Pebblebee", "Beech", "Beeline", "OMIRON", "Friend", "Friendly Speaker",
    "Pendant", "Compass", "My PLAUD phone", "Plaudify", "Fieldyard", "LimitlessTV",
])
def test_unrelated_names_do_not_match(name):
    assert match_personal_recorder(name) is None


@pytest.mark.parametrize("uuid", [
    "1910", "00001910-0000-1000-8000-00805f9b34fb",
    "4fafc201-1fb5-459e-8fcc-c5c9c331914b", "180F", "180A",
])
def test_shared_or_generic_services_are_not_recorder_evidence(uuid):
    assert match_personal_recorder(None, uuid) is None


def test_scanner_label_retains_recorder_class():
    entry = {"source": "ble_fingerprint", "manufacturer": "AI Voice Recorder", "class_reason": "recorder:name:Plaud"}
    assert classify_privacy_device(entry)["privacy_kind"] == "VOICE_RECORDER"
    rf = enrich_rf_evidence(source=entry["source"], manufacturer=entry["manufacturer"], class_reason=entry["class_reason"])
    assert rf["device_class"] == "voice_recorder"


def test_live_device_retains_recorder_name_evidence():
    import time
    from app.services.enrichment_ble import BLEEnricher

    enricher = BLEEnricher()
    enricher.ingest(drone_id="BLE:12345678:AI Voice Recorder", source="ble_fingerprint",
                    confidence=0.75, rssi=-55, bssid="CA:00:00:00:00:01",
                    manufacturer="AI Voice Recorder", model="FP:12345678",
                    device_id="test", received_at=time.time(), ble_name="PLAUD NOTE",
                    class_reason="recorder:name:Plaud")
    entry = enricher.get_live_devices()[0]
    assert entry["ble_name"] == "PLAUD NOTE"
    assert entry["class_reason"] == "recorder:name:Plaud"
    result = classify_privacy_device(entry)
    assert result["privacy_kind"] == "VOICE_RECORDER"
    assert result["display_detail"].startswith("Plaud;")


@pytest.mark.asyncio
async def test_live_api_exposes_recorder_brand_and_awareness(monkeypatch):
    from httpx import ASGITransport, AsyncClient
    from app.main import app
    from app.routers import detections

    monkeypatch.setattr(detections._ble_enricher, "prune_stale", lambda: None)
    monkeypatch.setattr(detections._ble_enricher, "get_summary", lambda: {"active": 1})
    monkeypatch.setattr(detections._ble_enricher, "get_live_devices", lambda **_: [{
        "fingerprint": "FP:12345678", "source": "ble_fingerprint",
        "device_type": "AI Voice Recorder", "manufacturer": "AI Voice Recorder",
        "ble_name": "PLAUDAB12", "class_reason": "recorder:name:Plaud",
        "confidence": 0.75, "current_rssi": -55,
    }])
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.get("/detections/devices/live")
    assert response.status_code == 200
    device = response.json()["devices"][0]
    assert device["brand"] == "Plaud"
    assert device["device_class"] == "voice_recorder"
    assert device["privacy_kind"] == "VOICE_RECORDER"
    assert device["display_label"] == "AI RECORDER"
    assert "recording status unknown" in device["display_detail"]
