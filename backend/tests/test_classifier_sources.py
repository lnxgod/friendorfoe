from app.services.classifier import classify_detection, normalize_detection_source
from app.routers.detections import _node_heartbeats, _wifi_assoc_mentions_known_node


def test_normalize_legacy_ble_rid_to_ble_fingerprint():
    assert normalize_detection_source(
        "ble_rid",
        drone_id="BLE:1234ABCD:Unknown",
    ) == "ble_fingerprint"


def test_keep_true_ble_remote_id_source():
    assert normalize_detection_source(
        "ble_rid",
        drone_id="rid_ABC123",
    ) == "ble_rid"


def test_keep_ble_remote_id_when_self_id_present():
    assert normalize_detection_source(
        "ble_rid",
        drone_id="BLE:1234ABCD:Unknown",
        self_id_text="Survey flight",
    ) == "ble_rid"


def test_normalize_wifi_assoc_out_of_wifi_oui():
    assert normalize_detection_source(
        "wifi_oui",
        drone_id="STA:AA:BB:CC:DD:EE:FF→AP:11:22:33:44:55:66",
        manufacturer="WiFi-Assoc",
    ) == "wifi_assoc"


def test_ble_fingerprint_trackers_stay_demoted():
    cls, conf = classify_detection(
        source="ble_fingerprint",
        confidence=0.4,
        drone_id="BLE:1234ABCD:AirTag",
        manufacturer="Apple",
    )
    assert cls == "tracker"
    assert conf == 0.4


def test_ble_fingerprint_drone_controller_is_possible_drone():
    cls, conf = classify_detection(
        source="ble_fingerprint",
        confidence=0.2,
        drone_id="BLE:1234ABCD:Drone Controller",
        manufacturer="Drone Controller",
    )
    assert cls == "possible_drone"
    assert conf == 0.35


def test_wifi_assoc_is_wifi_device():
    cls, conf = classify_detection(
        source="wifi_assoc",
        confidence=0.1,
        drone_id="STA:AA:BB:CC:DD:EE:FF→AP:11:22:33:44:55:66",
        manufacturer="WiFi-Assoc",
    )
    assert cls == "wifi_device"
    assert conf == 0.1


def test_legacy_generic_ble_rid_is_not_confirmed_drone():
    cls, _ = classify_detection(
        source="ble_rid",
        confidence=0.05,
        drone_id="BLE:1234ABCD:iPhone",
        manufacturer="Apple Device",
    )
    assert cls == "unknown_device"


def test_true_ble_remote_id_remains_confirmed_drone():
    cls, conf = classify_detection(
        source="ble_rid",
        confidence=0.95,
        drone_id="rid_SERIAL123",
        manufacturer="DJI",
    )
    assert cls == "confirmed_drone"
    assert conf == 0.95


def test_wifi_assoc_filter_catches_known_node_mac_suffix():
    prev = dict(_node_heartbeats)
    try:
        _node_heartbeats.clear()
        _node_heartbeats["uplink_D0A148"] = {}
        assert _wifi_assoc_mentions_known_node(
            "STA:10:B4:1D:D0:A1:48→AP:9A:AD:9F:0D:E0:71"
        )
        assert not _wifi_assoc_mentions_known_node(
            "STA:10:B4:1D:AA:BB:CC→AP:9A:AD:9F:0D:E0:71"
        )
    finally:
        _node_heartbeats.clear()
        _node_heartbeats.update(prev)
