"""Unit tests for the low-latency RSSI signal tracker."""

from app.services.signal_tracker import SignalEvent, SignalTracker, SignalTrackerConfig


def make_tracker() -> SignalTracker:
    return SignalTracker(
        SignalTrackerConfig(
            ema_tau_s=0.4,
            regression_window_s=1.5,
            min_regression_points=4,
            history_size=32,
        )
    )


def test_smoothing_moves_toward_raw_without_following_it_exactly():
    tracker = make_tracker()

    samples = [
        (-80, 1000.0),
        (-60, 1000.2),
        (-58, 1000.4),
    ]
    last = None
    for rssi, ts in samples:
        last = tracker.ingest(
            SignalEvent(
                drone_id="RID12345",
                source="wifi_beacon_rid",
                confidence=0.9,
                rssi=rssi,
                ssid="RID12345",
                bssid="AA:BB:CC:DD:EE:FF",
                manufacturer="DJI",
                device_id="sensor-a",
                received_at=ts,
                channel=6,
            )
        )

    assert last is not None
    assert last["raw_rssi"] == -58
    assert -70.0 < last["smoothed_rssi"] < -58.0


def test_positive_rssi_slope_maps_to_positive_approach_speed():
    tracker = make_tracker()

    readings = [-78, -73, -69, -65, -61]
    snapshot = None
    for idx, rssi in enumerate(readings):
        snapshot = tracker.ingest(
            SignalEvent(
                drone_id="RID-APPROACH",
                source="ble_rid",
                confidence=0.95,
                rssi=rssi,
                ssid=None,
                bssid="11:22:33:44:55:66",
                manufacturer="Skydio",
                device_id="sensor-a",
                received_at=2000.0 + (idx * 0.25),
                channel=None,
            )
        )

    assert snapshot is not None
    assert snapshot["rssi_slope_dbps"] > 0
    assert snapshot["approach_speed_mps"] > 0


def test_wifi_and_ble_merge_on_strong_identity():
    tracker = make_tracker()

    tracker.ingest(
        SignalEvent(
            drone_id="RID-SERIAL-001",
            source="wifi_beacon_rid",
            confidence=0.9,
            rssi=-67,
            ssid="RID-SERIAL-001",
            bssid="22:33:44:55:66:77",
            manufacturer="DJI",
            device_id="sensor-a",
            received_at=3000.0,
            channel=44,
        )
    )

    tracker.ingest(
        SignalEvent(
            drone_id="RID-SERIAL-001",
            source="ble_rid",
            confidence=0.95,
            rssi=-65,
            ssid=None,
            bssid=None,
            manufacturer="DJI",
            device_id="sensor-a",
            received_at=3000.6,
            channel=None,
        )
    )

    live = tracker.get_live_tracks(now=3001.0)
    assert live["count"] == 1
    track = live["tracks"][0]
    assert set(track["sources"]) == {"ble_rid", "wifi_beacon_rid"}
    assert track["display_name"] == "RID-SERIAL-001"
