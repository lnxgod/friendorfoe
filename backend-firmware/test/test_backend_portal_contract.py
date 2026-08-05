from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_SOURCE = ROOT / "shared" / "backend_portal_contract.c"
PORTAL_SOURCE = ROOT / "uplink" / "main" / "network" / "backend_config_portal.c"
PAGE_SOURCE = ROOT / "uplink" / "main" / "network" / "backend_dashboard_page.c"
PLATFORMIO = ROOT / "platformio.ini"


def test_required_and_lite_dashboard_registries_are_separate_and_exact():
    contract = CONTRACT_SOURCE.read_text(encoding="utf-8")
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    pairs = set(
        re.findall(
            r"\{\s*BACKEND_PORTAL_(GET|POST),\s*\"([^\"]+)\"\s*,",
            contract,
        )
    )
    assert pairs == {
        ("GET", "/"),
        ("GET", "/api/status"),
        ("GET", "/api/config"),
        ("POST", "/api/config"),
        ("POST", "/api/backend/test"),
        ("GET", "/dashboard"),
        ("GET", "/api/dashboard/status"),
        ("GET", "/api/events"),
    }
    assert "backend_portal_required_routes(&route_count)" in portal
    assert "backend_portal_dashboard_routes(&route_count)" in portal
    assert '"route_registration_failed"' in portal
    assert "platform_unregister_route" in portal
    assert contract.index("FOF_BACKEND_PROFILE_BADGE_LITE") < contract.index(
        '"/dashboard"'
    )
    for source in (contract, portal):
        assert "/api/ota" not in source
        assert "/firmware" not in source


def test_dashboard_page_compiles_only_in_lite_native_profile():
    platformio = PLATFORMIO.read_text(encoding="utf-8")
    lite = platformio[
        platformio.index("[env:backend-native]") :
        platformio.index("[env:backend-native-fullsize]")
    ]
    fullsize = platformio[platformio.index("[env:backend-native-fullsize]") :]
    assert "+<uplink/main/network/backend_dashboard_page.c>" in lite
    assert "backend_dashboard_page.c" not in fullsize
    assert platformio.count("backend_dashboard_page.c") == 1


def test_portal_sources_cannot_serialize_credentials():
    contract = CONTRACT_SOURCE.read_text(encoding="utf-8")
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    combined = contract + portal
    assert '"password":"%s"' not in combined
    assert '"ap_password":"%s"' not in combined
    assert "password_set" in contract
    assert "ap_password_set" in contract


def test_apsta_http_requests_are_gated_by_the_socket_local_destination():
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    assert "httpd_req_to_sockfd" in portal
    assert "getsockname" in portal
    assert "backend_config_portal_local_ipv4_allowed" in portal
    assert "request->uri" in portal
    handler = portal[portal.index("static esp_err_t portal_http_handler") :]
    assert handler.index("request_uses_ap_local_destination") < handler.index(
        "request->uri"
    )
    assert handler.index("request_uses_ap_local_destination") < handler.index(
        "BACKEND_PORTAL_DASHBOARD"
    )
    assert "Host" not in portal


def test_events_are_copied_before_chunked_event_by_event_serialization():
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    handler = portal[
        portal.index("static esp_err_t send_dashboard_events") :
        portal.index("static esp_err_t portal_http_handler")
    ]
    assert "BACKEND_DASHBOARD_DEFAULT_LIMIT" in handler
    assert "parsed.limit > BACKEND_DASHBOARD_MAX_LIMIT" in handler
    assert handler.index("backend_config_portal_copy_dashboard_events") < handler.index(
        "httpd_resp_send_chunk"
    )
    assert "index < snapshot.count" in handler
    assert "backend_dashboard_event_encode_json" in handler
    assert re.search(
        r"httpd_resp_send_chunk\(\s*request,\s*event_json,",
        handler,
    )
    assert "mutex" not in handler.lower()
    assert "lock" not in handler.lower()


def test_embedded_page_is_session_only_and_polls_both_apis_each_second():
    page = PAGE_SOURCE.read_text(encoding="utf-8")
    assert "fetch('/api/dashboard/status')" in page
    assert "fetch('/api/events?after='" in page
    assert "setInterval(poll,1000)" in page
    assert "localStorage" not in page
    assert "sessionStorage" not in page
    assert "indexedDB" not in page
    assert "document.cookie" not in page
    assert "http://" not in page
    assert "https://" not in page
    assert "<link" not in page.lower()
    assert "<img" not in page.lower()


def test_dashboard_status_rejects_credential_fields_and_values():
    page = PAGE_SOURCE.read_text(encoding="utf-8")
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    for forbidden in ("password", "ssid", "backend_url", "credential", "secret"):
        assert f'"{forbidden}"' in page
    status = portal[
        portal.index("bool backend_config_portal_dashboard_status") :
        portal.index("bool backend_config_portal_copy_dashboard_events")
    ]
    assert "backend_dashboard_status_is_redacted" in status
    assert "portal->config.networks[index].ssid" in status
    assert "portal->config.networks[index].password" in status
    assert "portal->config.backend_url" in status
    assert "portal->config.ap_password" in status


def test_ap_ssid_format_is_exact_for_each_profile():
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    assert '"FriendOrFoe-Lite-%02X%02X%02X"' in portal
    assert '"FriendOrFoe-Backend-%02X%02X%02X"' in portal
    lite_guard = portal.index("FOF_BACKEND_PROFILE_BADGE_LITE")
    assert lite_guard < portal.index('"FriendOrFoe-Lite-%02X%02X%02X"')


def test_activation_failures_share_rollback_and_config_get_uses_contract_bound():
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    assert "platform_rollback(portal)" in portal
    assert "char config_json[BACKEND_PORTAL_CONFIG_BODY_MAX + 1U]" in portal
    assert "reconnect_failed" in portal
    assert '\\"saved\\\":true' in portal


def test_update_authorization_is_explicit_and_not_catalog_driven():
    contract = CONTRACT_SOURCE.read_text(encoding="utf-8")
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    combined = contract + portal
    assert "auto_update_enabled" in combined
    assert "confirm_auto_update" in combined
    assert "catalog" not in combined.lower()


def test_html_describes_future_write_authorization_and_defaults_off():
    portal = PORTAL_SOURCE.read_text(encoding="utf-8")
    assert "future firmware-write authorization" in portal
    assert r'name=\"auto_update_enabled\"' in portal
    assert r'name=\"confirm_auto_update\"' in portal
    assert not re.search(r'<input[^>]+\schecked(?:\s|>)', portal, re.IGNORECASE)
