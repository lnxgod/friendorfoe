"""Unit tests for the in-memory RF anomaly detector."""

from datetime import datetime

from app.services.rf_anomaly import RFAnomalyConfig, RFAnomalyDetector, RFDetectionEvent


def make_detector(**overrides) -> RFAnomalyDetector:
    config = RFAnomalyConfig(
        min_confidence=0.4,
        default_cooldown_s=0.0,
        state_ttl_s=10 * 24 * 3600.0,
        rssi_spike_db=10,
        rssi_spike_min_samples=2,
        disappearance_min_seen=3,
        disappearance_min_presence_s=15.0,
        disappearance_grace_s=20.0,
        new_device_confirmations=2,
        new_device_window_s=20.0,
        velocity_min_speed_mps=2.0,
        velocity_min_rssi_delta_db=5,
        signal_baseline_min_samples=5,
        signal_min_abs_delta_db=10,
        signal_zscore_threshold=2.0,
        spoofing_distinct_bssids=3,
        spoofing_window_s=60.0,
        time_pattern_min_hits=6,
        time_pattern_min_days=3,
        time_pattern_window_hours=2,
        time_pattern_fraction=0.75,
        correlation_window_s=10.0,
        **overrides,
    )
    return RFAnomalyDetector(config=config)


def make_event(
    ts: float,
    *,
    drone_id: str = "drone-1",
    source: str = "wifi_ssid",
    confidence: float = 0.9,
    rssi: int | None = -70,
    ssid: str | None = "RID-1",
    bssid: str | None = "00:11:22:33:44:55",
    manufacturer: str | None = "DJI",
    device_id: str = "sensor-a",
    channel: int | None = None,
) -> RFDetectionEvent:
    return RFDetectionEvent(
        drone_id=drone_id,
        source=source,
        confidence=confidence,
        rssi=rssi,
        ssid=ssid,
        bssid=bssid,
        manufacturer=manufacturer,
        device_id=device_id,
        received_at=ts,
        channel=channel,
    )


def test_new_device_requires_confirmation():
    detector = make_detector()

    first = detector.process_event(make_event(1_000.0))
    second = detector.process_event(make_event(1_005.0))

    assert first == []
    assert [alert.anomaly_type for alert in second] == ["new_device"]


def test_rssi_spike_and_velocity_alert():
    detector = make_detector()

    detector.process_event(make_event(1_000.0, rssi=-80))
    detector.process_event(make_event(1_005.0, rssi=-76))
    alerts = detector.process_event(make_event(1_010.0, rssi=-60))

    alert_types = {alert.anomaly_type for alert in alerts}
    assert "rssi_spike" in alert_types
    assert "rssi_velocity" in alert_types


def test_disappearance_alert_from_sweep():
    detector = make_detector()

    detector.process_event(make_event(1_000.0))
    detector.process_event(make_event(1_010.0))
    detector.process_event(make_event(1_020.0))

    alerts = detector.sweep(1_050.0)

    assert [alert.anomaly_type for alert in alerts] == ["device_disappearance"]


def test_channel_hopping_alerts_when_multiple_channels_seen_quickly():
    detector = make_detector()

    detector.process_event(make_event(1_000.0, channel=1))
    detector.process_event(make_event(1_003.0, channel=6))
    alerts = detector.process_event(make_event(1_006.0, channel=11))

    assert "channel_hopping" in {alert.anomaly_type for alert in alerts}


def test_signal_strength_outlier_uses_manufacturer_baseline():
    detector = make_detector()

    baseline = [-72, -71, -70, -69, -72]
    for idx, rssi in enumerate(baseline):
        detector.process_event(
            make_event(
                1_000.0 + idx,
                rssi=rssi,
                bssid=f"00:11:22:33:44:{50 + idx:02X}",
            )
        )

    alerts = detector.process_event(
        make_event(
            1_010.0,
            rssi=-42,
            bssid="00:11:22:33:44:99",
        )
    )

    assert "signal_strength_outlier" in {alert.anomaly_type for alert in alerts}


def test_spoofing_and_ble_correlation_alerts():
    detector = make_detector()

    detector.process_event(
        make_event(
            1_000.0,
            drone_id="rid-42",
            ssid="DroneNet",
            bssid="00:11:22:33:44:01",
        )
    )
    detector.process_event(
        make_event(
            1_005.0,
            drone_id="rid-42",
            ssid="DroneNet",
            bssid="00:11:22:33:44:02",
        )
    )
    spoofing_alerts = detector.process_event(
        make_event(
            1_010.0,
            drone_id="rid-42",
            ssid="DroneNet",
            bssid="10:AA:22:33:44:03",
        )
    )

    detector.process_event(
        make_event(
            1_020.0,
            drone_id="rid-ble",
            source="wifi_ssid",
            ssid="RID-BLE",
            bssid="00:11:22:33:44:88",
        )
    )
    correlation_alerts = detector.process_event(
        make_event(
            1_024.0,
            drone_id="rid-ble",
            source="ble_rid",
            ssid=None,
            bssid=None,
            rssi=-66,
        )
    )

    spoofing_types = {alert.anomaly_type for alert in spoofing_alerts}
    correlation_types = {alert.anomaly_type for alert in correlation_alerts}

    assert "ssid_spoofing" in spoofing_types
    assert "bssid_churn" in spoofing_types
    assert "wifi_ble_correlation" in correlation_types


def test_time_of_day_pattern_alerts_after_multi_day_window():
    detector = make_detector()

    timestamps = [
        datetime(2026, 3, 1, 2, 5).timestamp(),
        datetime(2026, 3, 1, 2, 45).timestamp(),
        datetime(2026, 3, 2, 2, 10).timestamp(),
        datetime(2026, 3, 2, 2, 50).timestamp(),
        datetime(2026, 3, 3, 2, 15).timestamp(),
    ]
    for ts in timestamps:
        detector.process_event(make_event(ts))

    alerts = detector.process_event(make_event(datetime(2026, 3, 3, 2, 55).timestamp()))

    assert "time_of_day_pattern" in {alert.anomaly_type for alert in alerts}
