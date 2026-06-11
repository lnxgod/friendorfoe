from app.services.anomaly_detector import AnomalyDetector


def test_wifi_attack_alerts_are_cooled_down_per_node_slot_and_type():
    detector = AnomalyDetector()

    detector.record_wifi_attack(
        device_id="node-a",
        scanner_slot="1",
        deauth_count=9,
        disassoc_count=0,
        deauth_flood=True,
        beacon_spam=True,
        timestamp=1_000.0,
    )
    detector.record_wifi_attack(
        device_id="node-a",
        scanner_slot="1",
        deauth_count=8,
        disassoc_count=0,
        deauth_flood=True,
        beacon_spam=True,
        timestamp=1_030.0,
    )
    detector.record_wifi_attack(
        device_id="node-a",
        scanner_slot="1",
        deauth_count=8,
        disassoc_count=0,
        deauth_flood=True,
        beacon_spam=False,
        timestamp=1_601.0,
    )

    alerts = detector.get_alerts(limit=10)
    alert_types = [alert["alert_type"] for alert in alerts]

    assert alert_types.count("wifi_deauth_flood") == 2
    assert alert_types.count("wifi_beacon_spam") == 1


def test_wifi_attack_volume_escalates_bulk_deauth_and_disassoc():
    detector = AnomalyDetector()

    detector.record_wifi_attack(
        device_id="node-a",
        scanner_slot="2",
        deauth_count=150,
        disassoc_count=75,
        deauth_flood=False,
        beacon_spam=False,
        timestamp=2_000.0,
    )

    alerts = detector.get_alerts(limit=5)

    assert alerts[0]["alert_type"] == "wifi_attack_volume"
    assert alerts[0]["severity"] == "critical"
    assert alerts[0]["details"]["deauth_count"] == 150
    assert alerts[0]["details"]["disassoc_count"] == 75
