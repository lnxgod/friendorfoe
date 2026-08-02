from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_SOURCE = ROOT / "shared" / "backend_portal_contract.c"
PORTAL_SOURCE = ROOT / "uplink" / "main" / "network" / "backend_config_portal.c"


def test_portal_is_config_only_and_registers_the_runtime_allowlist():
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
    }
    assert "backend_portal_routes(&route_count)" in portal
    assert "for (size_t index = 0; index < route_count; ++index)" in portal
    for source in (contract, portal):
        assert "/api/ota" not in source
        assert "/firmware" not in source


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
    assert "Host" not in portal


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
