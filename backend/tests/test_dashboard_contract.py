from pathlib import Path
import re


def test_dashboard_uses_backend_drone_ssid_metadata():
    html = (Path(__file__).resolve().parents[1] / "app/static/dashboard.html").read_text()

    assert "DRONE_SSID_RE" not in html
    assert "drone_ssid_match" in html
    assert "drone_ssid_matches" in html
    assert "function isDroneRelatedDevice" in html


def test_dashboard_uses_backend_diagnostics_and_explanations():
    html = (Path(__file__).resolve().parents[1] / "app/static/dashboard.html").read_text()

    assert "/detections/diagnostics" in html
    assert "tab-diagnostics" in html
    assert "detection_explanation" in html
    assert "primary_reason" in html


def test_dashboard_has_no_duplicate_tab_ids_and_keeps_all_tabs():
    html = (Path(__file__).resolve().parents[1] / "app/static/dashboard.html").read_text()
    ids = re.findall(r'id="(tab-[^"]+)"', html)
    duplicates = sorted({item for item in ids if ids.count(item) > 1})

    assert duplicates == []
    for tab in [
        "tab-overview", "tab-map", "tab-alerts", "tab-diagnostics",
        "tab-wifi", "tab-ble", "tab-mobile", "tab-probes",
        "tab-detections", "tab-nodes", "tab-whitelist", "tab-events",
        "tab-entities", "tab-anomalies",
    ]:
        assert tab in ids


def test_dashboard_detail_drawer_and_grouped_nav_exist():
    html = (Path(__file__).resolve().parents[1] / "app/static/dashboard.html").read_text()

    assert 'id="detail-drawer"' in html
    assert 'id="ov-readiness-strip"' in html
    assert 'id="ov-signal-summary"' in html
    assert 'id="ov-map-summary"' in html
    assert 'id="ov-sensor-roster"' in html
    assert "function openSignalDrawer" in html
    assert "function closeSignalDrawer" in html
    assert "openSignalDrawer" in html
    assert "tab-group-label" in html
    assert "Operate" in html
    assert "Signals" in html
    assert "Manage" in html
    assert "prefers-reduced-motion" in html


def test_dashboard_distinguishes_backend_health_from_uplink_reporting():
    html = (Path(__file__).resolve().parents[1] / "app/static/dashboard.html").read_text()

    assert "lastHealthOk" in html
    assert "backend online · no uplink batches" in html
    assert "No uplink batches received at /detections/drones" in html
    assert "Node status API fetch failed" in html
    assert "ui error:" in html
    assert 'id="diag-ingest"' in html
    assert "ingest_freshness" in html
    assert "Expected uplink POST" in html


def test_dashboard_has_scanner_fleet_firmware_controls():
    html = (Path(__file__).resolve().parents[1] / "app/static/dashboard.html").read_text()

    assert 'id="fleet-firmware-panel"' in html
    assert "/nodes/firmware/scanner/readiness" in html
    assert "/nodes/firmware/scanner/stage-fleet" in html
    assert "/nodes/firmware/scanner/trigger-check" in html
    assert "/nodes/firmware/scanner/rollout" in html
    assert "/nodes/firmware/rollouts/" in html
    assert "Stage Fleet" in html
    assert "Continue Fleet" in html
    assert "USB recovery" in html or "USB Recovery" in html
