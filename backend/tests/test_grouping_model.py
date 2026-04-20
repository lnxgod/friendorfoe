"""Regression tests for the backend-side fingerprint grouping helper.

`_grouping_model` promotes the scanner's BLE-JA3 hash into a stable
`FP:<ja3>` model string for MAC-rotating high-risk device classes
(Meta / Ray-Ban / Oakley / Luxottica / Quest). Every downstream
fingerprint-aware consumer — `EntityTracker`, `BLEEnricher`,
`AnomalyDetector`, `IdentityCorrelator`, `EventDetector`,
`triangulation.ingest` — already keys on `model.startswith("FP:")`,
so getting this helper right is what makes "3 Meta glasses appear
as 3 durable threats" possible on the privacy dashboard.

Mirror of the Android-side rule in
`android/.../detection/GlassesDetector.computeFingerprintKey`.
Keep the two in sync.
"""

from dataclasses import dataclass

from app.routers.detections import _grouping_model


@dataclass
class FakeDet:
    """Minimal stand-in for DroneDetectionItem with only the fields
    `_grouping_model` reads."""
    model: str = ""
    manufacturer: str = ""
    ble_ja3: str = ""


def test_three_meta_rotations_collapse_to_one_key():
    """Three Meta advertisements with identical JA3 but different source MACs
    must produce the same `FP:` grouping key — that's the whole point of the
    fingerprint fix."""
    ja3 = "deadbeef"
    k1 = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3=ja3))
    k2 = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3=ja3))
    k3 = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3=ja3))
    assert k1 == k2 == k3 == f"FP:{ja3}"


def test_different_ja3_hashes_stay_separate():
    """Two physical Meta pairs with different advertisement structures must
    not collapse — we'd lose the ability to see them as distinct threats."""
    a = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3="11111111"))
    b = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3="22222222"))
    assert a != b


def test_airtag_keeps_original_model_so_two_trackers_never_merge():
    """AirTags don't need FP grouping — each physical AirTag is a distinct
    threat and must stay separate. Same logic as the Android rule."""
    orig = "AirTag (Separated)"
    k = _grouping_model(FakeDet(model=orig, manufacturer="Apple", ble_ja3="cafebabe"))
    assert k == orig


def test_unknown_manufacturer_passes_through():
    k = _grouping_model(FakeDet(model="Smart Speaker", manufacturer="Samsung", ble_ja3="abc12345"))
    assert k == "Smart Speaker"


def test_missing_ja3_falls_back_to_original_model():
    k = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3=""))
    assert k == "Ray-Ban Meta"


def test_zero_ja3_string_falls_back():
    """Firmware emits '00000000' for records with no JA3 — treat as absent."""
    k = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3="00000000"))
    assert k == "Ray-Ban Meta"


def test_already_fingerprinted_model_passes_through():
    """Remote ID drones arrive with model already FP:-prefixed — do not
    double-prefix and do not overwrite with JA3."""
    k = _grouping_model(FakeDet(model="FP:abc12345", manufacturer="DJI", ble_ja3="99999999"))
    assert k == "FP:abc12345"


def test_oakley_hstn_is_rotating():
    k = _grouping_model(FakeDet(model="Oakley HSTN", manufacturer="Luxottica", ble_ja3="feedface"))
    assert k == "FP:feedface"


def test_meta_quest_is_rotating():
    k = _grouping_model(FakeDet(model="Meta Quest 3", manufacturer="Meta", ble_ja3="12345678"))
    assert k == "FP:12345678"


def test_ja3_case_is_normalised():
    """Scanner may emit hex in upper or lower case; the key must be stable
    regardless so the EntityTracker dict lookup never misses."""
    upper = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3="DEADBEEF"))
    lower = _grouping_model(FakeDet(model="Ray-Ban Meta", manufacturer="Meta", ble_ja3="deadbeef"))
    assert upper == lower == "FP:deadbeef"


def test_luxottica_manufacturer_without_meta_model_still_rotates():
    """Some Ray-Ban Stories Gen1 advertise with manufacturer='Luxottica'
    and only a generic device name — still the same privacy concern."""
    k = _grouping_model(FakeDet(model="Wearable", manufacturer="Luxottica", ble_ja3="11223344"))
    assert k == "FP:11223344"
