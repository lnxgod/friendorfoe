from __future__ import annotations

from base64 import urlsafe_b64encode
from contextlib import redirect_stderr
import csv
from dataclasses import replace
from html.parser import HTMLParser
import http.client
import io
import json
from pathlib import Path
import shutil
import socket
import subprocess
import threading
import unittest
from urllib.parse import quote

from new_dash.controls import (
    ControlValidationError,
    build_display_nav,
    build_display_policy,
    build_theme,
)
from new_dash.models import ControlReply, Observation
from new_dash.serial_transport import ControlTimeout, TransportUnavailable
from new_dash.storage import HistoryPage, HistoryQuery
from new_dash.web import create_http_server
from tests.test_controls import THEME, complete_policy


STATIC_ROOT = Path(__file__).parents[1] / "src" / "new_dash" / "static"
PROJECT_ROOT = Path(__file__).parents[1]

STATIC_ASSETS = {
    "/static/styles.css": "text/css; charset=utf-8",
    "/static/api.js": "text/javascript; charset=utf-8",
    "/static/ui.js": "text/javascript; charset=utf-8",
    "/static/views/live.js": "text/javascript; charset=utf-8",
    "/static/views/map.js": "text/javascript; charset=utf-8",
    "/static/views/history.js": "text/javascript; charset=utf-8",
    "/static/views/badge.js": "text/javascript; charset=utf-8",
    "/static/app.js": "text/javascript; charset=utf-8",
    "/static/vendor/leaflet/leaflet.css": "text/css; charset=utf-8",
    "/static/vendor/leaflet/leaflet.js": "text/javascript; charset=utf-8",
    "/static/vendor/leaflet/LICENSE": "text/plain; charset=utf-8",
}


class DashboardHTMLParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.h1_text: list[str] = []
        self.navigation_targets: list[str] = []
        self.view_labels: list[str] = []
        self.inline_handlers: list[str] = []
        self.local_urls: list[str] = []
        self.connection_live_regions = 0
        self.main_landmarks = 0
        self._in_h1 = False

    def handle_starttag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        attributes = dict(attrs)
        if tag == "h1":
            self._in_h1 = True
        if tag == "button" and "data-view-target" in attributes:
            self.navigation_targets.append(attributes["data-view-target"] or "")
        if tag == "section" and "aria-label" in attributes:
            self.view_labels.append(attributes["aria-label"] or "")
        if tag == "main":
            self.main_landmarks += 1
        if attributes.get("aria-live") == "polite" and attributes.get("id") == "connection-status":
            self.connection_live_regions += 1
        for name, value in attrs:
            if name.lower().startswith("on"):
                self.inline_handlers.append(name)
            if name in {"href", "src"} and value is not None:
                self.local_urls.append(value)

    def handle_endtag(self, tag: str) -> None:
        if tag == "h1":
            self._in_h1 = False

    def handle_data(self, data: str) -> None:
        if self._in_h1:
            self.h1_text.append(data)


SNAPSHOT = {
    "connection": {
        "phase": "live",
        "detail": "status_valid",
        "port": "/dev/cu.usbmodem1",
        "candidates": [],
        "firmware_version": "0.64.66",
        "reconnect_attempt": 0,
    },
    "freshness": {"state": "fresh", "age_s": 0.4},
    "status": {
        "version": "0.64.66",
        "uptime_s": 123.0,
        "mode": "usb_only",
        "mode_label": "USB only",
        "safe_mode": False,
        "safe_reason": None,
        "recovery_mode": None,
        "threat_score": 80.0,
        "counts": {"drone": 1},
        "scanners": [],
        "entities": [],
        "reporting": None,
        "memory": None,
        "display_state": None,
        "theme": None,
        "display_policy": None,
        "future_firmware_detail": {"usb_text": "<untrusted>&visible"},
    },
    "recent_events": [],
    "diagnostics": {
        "malformed_lines": 0,
        "overlong_lines": 0,
        "history_available": True,
        "history_error": None,
        "persistence_queue_depth": 0,
        "persistence_drops": 0,
    },
}


class FakeApplication:
    def __init__(self) -> None:
        self.history_query: HistoryQuery | None = None
        self.export_query: HistoryQuery | None = None
        self.history_failure: Exception | None = None
        self.export_failure: Exception | None = None
        self.control_calls: list[tuple[str, object | None]] = []
        self.control_failure: Exception | None = None
        self.control_reply = ControlReply.from_payload({"message": "accepted"}, ok=True)

    def snapshot(self) -> dict[str, object]:
        return SNAPSHOT

    def query_history(self, query: HistoryQuery) -> HistoryPage:
        self.history_query = query
        if self.history_failure is not None:
            raise self.history_failure
        if query.cursor == "bad":
            raise ValueError("invalid history cursor")
        return HistoryPage(items=(observation(),), next_cursor="next-page")

    def export_history(self, query: HistoryQuery):
        self.export_query = query
        if self.export_failure is not None:
            raise self.export_failure
        yield observation()

    def clear_history(self) -> int:
        self._before_control("clear_history", None)
        return 3

    def display_nav(self, action: str) -> ControlReply:
        build_display_nav(action)
        return self._before_control("display_nav", action)

    def set_theme(self, payload: object) -> ControlReply:
        build_theme(payload)
        return self._before_control("set_theme", payload)

    def reset_theme(self) -> ControlReply:
        return self._before_control("reset_theme", None)

    def set_display_policy(self, payload: object) -> ControlReply:
        build_display_policy(payload)
        return self._before_control("set_display_policy", payload)

    def reset_display_policy(self) -> ControlReply:
        return self._before_control("reset_display_policy", None)

    def _before_control(self, name: str, payload: object | None) -> ControlReply:
        self.control_calls.append((name, payload))
        if self.control_failure is not None:
            raise self.control_failure
        return self.control_reply


def observation() -> Observation:
    return Observation(
        row_id=7,
        kind="event",
        received_at=1_700_000_000.5,
        observed_at=1_700_000_000.0,
        stable_key="ble_rid:RID-7",
        source_id=0,
        source="ble_rid",
        threat_class="drone",
        category="remote_id",
        label="REMOTE ID",
        display_id="RID-7",
        manufacturer="DJI, Inc.",
        confidence=0.95,
        score=80.0,
        rssi=-48,
        events=3,
        seen_count=4,
        latitude=37.7749,
        longitude=-122.4194,
        altitude_m=42.0,
        operator_latitude=None,
        operator_longitude=None,
        operator_id="operator-7",
        extras={"note": "USB text, not markup"},
    )


CSV_HEADERS = (
    "row_id",
    "kind",
    "received_at",
    "observed_at",
    "stable_key",
    "source_id",
    "source",
    "threat_class",
    "category",
    "label",
    "display_id",
    "manufacturer",
    "confidence",
    "score",
    "rssi",
    "events",
    "seen_count",
    "latitude",
    "longitude",
    "altitude_m",
    "operator_latitude",
    "operator_longitude",
    "operator_id",
    "extras",
)


def history_cursor(row_id: int) -> str:
    payload = json.dumps(
        {"received_at": 1.0, "id": row_id}, separators=(",", ":")
    ).encode("utf-8")
    return urlsafe_b64encode(payload).decode("ascii")


class WebServerTestCase(unittest.TestCase):
    server: object

    def setUp(self) -> None:
        self.application = FakeApplication()
        self.server = create_http_server(
            self.application, requested_port=0, token="test-control-token"
        )
        self.thread = self.server.serve_in_thread()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2.0)

    def request(
        self,
        method: str,
        path: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes | None = None,
    ) -> tuple[http.client.HTTPResponse, bytes]:
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=2.0
        )
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        payload = response.read()
        connection.close()
        return response, payload

    def assert_security_headers(self, response: http.client.HTTPResponse) -> None:
        csp = response.getheader("Content-Security-Policy")
        self.assertIsNotNone(csp)
        self.assertIn("default-src 'self'", csp)
        self.assertIn("img-src 'self' data: https://*.tile.openstreetmap.org", csp)
        self.assertIn("frame-ancestors 'none'", csp)
        self.assertEqual(response.getheader("X-Content-Type-Options"), "nosniff")
        self.assertEqual(response.getheader("Referrer-Policy"), "no-referrer")
        self.assertIsNone(response.getheader("Access-Control-Allow-Origin"))


class LoopbackBindingTest(unittest.TestCase):
    def test_explicit_ephemeral_port_binds_only_ipv4_loopback(self) -> None:
        server = create_http_server(FakeApplication(), requested_port=0, token="token")
        try:
            self.assertEqual(server.server_address[0], "127.0.0.1")
            self.assertGreater(server.server_address[1], 0)
            self.assertEqual(server.url, f"http://127.0.0.1:{server.server_port}")
        finally:
            server.server_close()

    def test_default_port_is_8765_when_available(self) -> None:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("127.0.0.1", 8765))
        except OSError:
            self.skipTest("loopback port 8765 is already occupied")
        finally:
            probe.close()

        server = create_http_server(FakeApplication(), requested_port=None, token="token")
        try:
            self.assertEqual(server.server_port, 8765)
        finally:
            server.server_close()

    def test_default_port_falls_back_to_ephemeral_when_8765_is_occupied(self) -> None:
        occupied = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        occupied.bind(("127.0.0.1", 8765))
        occupied.listen()
        try:
            server = create_http_server(
                FakeApplication(), requested_port=None, token="token"
            )
            try:
                self.assertNotEqual(server.server_port, 8765)
                self.assertEqual(server.server_address[0], "127.0.0.1")
            finally:
                server.server_close()
        finally:
            occupied.close()

    def test_explicit_occupied_port_raises(self) -> None:
        occupied = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        port = occupied.getsockname()[1]
        try:
            with self.assertRaises(OSError):
                create_http_server(
                    FakeApplication(), requested_port=port, token="token"
                )
        finally:
            occupied.close()

    def test_default_control_token_is_random_per_server(self) -> None:
        first = create_http_server(FakeApplication(), requested_port=0)
        second = create_http_server(FakeApplication(), requested_port=0)
        try:
            self.assertNotEqual(first.control_token, second.control_token)
            self.assertGreaterEqual(len(first.control_token), 32)
        finally:
            first.server_close()
            second.server_close()


class StaticAndStateRouteTest(WebServerTestCase):
    def test_index_substitutes_only_control_token_marker_and_is_not_cached(self) -> None:
        response, body = self.request("GET", "/")

        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "text/html; charset=utf-8")
        self.assertEqual(response.getheader("Cache-Control"), "no-store")
        self.assertIn(
            b'<meta name="new-dash-control-token" content="test-control-token">',
            body,
        )
        self.assertNotIn(b"{{CONTROL_TOKEN}}", body)
        self.assert_security_headers(response)

    def test_every_dashboard_asset_is_allowlisted_with_exact_mime_type(self) -> None:
        for path, content_type in STATIC_ASSETS.items():
            with self.subTest(path=path):
                response, body = self.request("GET", path)
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Content-Type"), content_type)
                self.assertTrue(body)
                self.assertEqual(response.getheader("Cache-Control"), "no-store")
                self.assert_security_headers(response)

    def test_static_assets_are_not_subject_to_control_token_substitution(self) -> None:
        for path in ("/static/app.js", "/static/styles.css", "/static/vendor/leaflet/leaflet.js"):
            with self.subTest(path=path):
                response, body = self.request("GET", path)
                self.assertEqual(response.status, 200)
                disk_body = (STATIC_ROOT / path.removeprefix("/static/")).read_bytes()
                self.assertEqual(body, disk_body)

    def test_dashboard_shell_has_semantic_views_and_local_assets(self) -> None:
        response, body = self.request("GET", "/")
        parser = DashboardHTMLParser()
        parser.feed(body.decode("utf-8"))

        self.assertEqual(response.status, 200)
        self.assertIn("New Dash", "".join(parser.h1_text))
        self.assertEqual(
            parser.navigation_targets,
            ["live", "map", "history", "badge"],
        )
        self.assertEqual(parser.connection_live_regions, 1)
        self.assertEqual(parser.main_landmarks, 1)
        self.assertEqual(parser.view_labels, ["Live", "Map", "History", "Badge"])
        self.assertEqual(parser.inline_handlers, [])
        self.assertIn("/static/vendor/leaflet/leaflet.css", parser.local_urls)
        self.assertIn("/static/vendor/leaflet/leaflet.js", parser.local_urls)
        self.assertIn("/static/app.js", parser.local_urls)
        self.assertFalse(
            any(url.startswith(("http://", "https://", "//")) for url in parser.local_urls)
        )

    def test_csp_allows_only_local_code_and_https_map_tiles(self) -> None:
        response, _ = self.request("GET", "/")
        csp = response.getheader("Content-Security-Policy") or ""

        self.assertIn("script-src 'self'", csp)
        self.assertIn("style-src 'self'", csp)
        self.assertIn("connect-src 'self'", csp)
        self.assertIn(
            "img-src 'self' data: https://*.tile.openstreetmap.org",
            csp,
        )

    def test_first_party_modules_use_safe_dom_and_bounded_browser_contracts(self) -> None:
        sources = {
            path.relative_to(STATIC_ROOT).as_posix(): path.read_text(encoding="utf-8")
            for path in STATIC_ROOT.rglob("*.js")
            if "vendor" not in path.parts
        }
        combined = "\n".join(sources.values())

        self.assertEqual(
            set(sources),
            {
                "api.js", "ui.js", "views/live.js", "views/map.js",
                "views/history.js", "views/badge.js", "app.js",
            },
        )
        self.assertNotIn(".innerHTML", combined)
        self.assertNotIn("insertAdjacentHTML", combined)
        self.assertNotIn("setInterval", combined)
        self.assertIn("setTimeout", sources["api.js"])
        self.assertIn("AbortController", sources["api.js"])
        self.assertIn("5000", sources["api.js"])
        self.assertIn("new URLSearchParams", combined)
        self.assertIn("hashchange", sources["app.js"])
        for key in ("ArrowLeft", "ArrowRight", "Home", "End"):
            self.assertIn(key, sources["ui.js"])
        self.assertIn("newDash.v1.", sources["app.js"])

    def test_history_and_badge_shell_expose_only_exact_safe_controls(self) -> None:
        html = (STATIC_ROOT / "index.html").read_text(encoding="utf-8")

        for control_id in (
            "history-since", "history-until", "history-kind", "history-class",
            "history-source", "history-text", "history-positioned",
            "history-previous", "history-next", "history-export-csv",
            "history-export-json", "history-clear-dialog", "history-clear-word",
            "badge-scanners", "badge-status-facts", "badge-display-state",
            "badge-theme-form", "badge-policy-form", "theme-reset-dialog",
            "policy-reset-dialog",
        ):
            with self.subTest(control_id=control_id):
                self.assertIn(f'id="{control_id}"', html)
        for action in ("next", "detail", "page", "back"):
            self.assertIn(f'data-nav-action="{action}"', html)
        for class_name in (
            "drone", "meta", "tracker", "wifi_attack", "skimmer", "camera",
            "flock", "lock", "hid", "beacon", "event_badge", "auracast",
            "scanner_status",
        ):
            self.assertIn(f'data-policy-class="{class_name}"', html)
        self.assertNotIn("<textarea", html.lower())
        for prohibited in (
            'id="firmware-control"', 'id="reboot-control"',
            'id="bootloader-control"', 'id="safe-mode-control"',
            'id="raw-command"',
        ):
            self.assertNotIn(prohibited, html)

    def test_live_and_map_modules_preserve_source_truth_and_budgets(self) -> None:
        live_path = STATIC_ROOT / "views" / "live.js"
        map_path = STATIC_ROOT / "views" / "map.js"
        self.assertTrue(live_path.is_file())
        self.assertTrue(map_path.is_file())
        live_source = live_path.read_text(encoding="utf-8")
        map_source = map_path.read_text(encoding="utf-8")

        self.assertIn('"ble_rid"', live_source)
        self.assertIn('"wifi_rid"', live_source)
        self.assertIn("DJI evidence — not Remote ID", live_source)
        self.assertIn("entity.stale === true", live_source)
        self.assertIn("Missing", live_source)
        self.assertIn("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", map_source)
        self.assertIn("https://www.openstreetmap.org/copyright", map_source)
        self.assertIn("Basemap offline — coordinates and observations remain available", map_source)
        self.assertIn("Host-observed trail", map_source)
        self.assertIn("MAX_TRAIL_PAGES = 4", map_source)
        self.assertIn("MAX_TRAIL_ROWS = 2000", map_source)
        for query_contract in ('"kind", "track"', '"positioned", "true"', '"limit", "500"'):
            self.assertIn(query_contract, map_source)

    def test_responsive_shell_keeps_desktop_compact_and_mobile_filters_reachable(self) -> None:
        html = (STATIC_ROOT / "index.html").read_text(encoding="utf-8")
        css = (STATIC_ROOT / "styles.css").read_text(encoding="utf-8")
        app_source = (STATIC_ROOT / "app.js").read_text(encoding="utf-8")

        self.assertIn('<details id="presentation-filter-panel"', html)
        self.assertIn("<summary", html)
        self.assertIn("@media (min-width: 760px)", css)
        self.assertIn(".presentation-filter-panel", css)
        self.assertIn(".view-panel:focus-visible", css)
        self.assertIn('matchMedia("(min-width: 760px)")', app_source)

    def test_leaflet_controls_have_touch_targets_focus_and_readable_attribution(self) -> None:
        css = (STATIC_ROOT / "styles.css").read_text(encoding="utf-8")

        self.assertRegex(
            css,
            r"\.leaflet-control-zoom a\s*\{[^}]*min-width:\s*44px;[^}]*min-height:\s*44px;",
        )
        self.assertIn(".leaflet-control-zoom a:focus-visible", css)
        self.assertRegex(
            css,
            r"\.leaflet-control-attribution a\s*\{[^}]*color:\s*#7ae4ef",
        )

    def test_browser_state_machine_behavior_suite(self) -> None:
        node = shutil.which("node")
        if node is None:
            self.skipTest("Node.js is unavailable for browser module behavior tests")
        result = subprocess.run(
            [node, "--test", "tests/browser_behavior_test.mjs"],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fixture_is_standalone_deterministic_and_includes_hostile_usb_text(self) -> None:
        fixture_path = PROJECT_ROOT / "tests" / "browser_fixture_server.py"
        self.assertTrue(fixture_path.is_file())
        fixture_source = fixture_path.read_text(encoding="utf-8")

        self.assertNotIn("serial_transport", fixture_source)
        self.assertNotIn("pyserial", fixture_source.lower())
        self.assertIn("<script>window.fixturePwned=true</script>", fixture_source)
        self.assertIn("--stale", fixture_source)
        self.assertIn("--safe-usb", fixture_source)
        ui_source = (STATIC_ROOT / "ui.js").read_text(encoding="utf-8")
        self.assertIn("textContent", ui_source)

    def test_state_returns_exact_snapshot_in_success_envelope(self) -> None:
        response, body = self.request("GET", "/api/state")

        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "application/json; charset=utf-8")
        self.assertEqual(response.getheader("Cache-Control"), "no-store")
        self.assertEqual(json.loads(body), {"ok": True, "data": SNAPSHOT})
        self.assertEqual(
            json.loads(body)["data"]["status"]["future_firmware_detail"],
            {"usb_text": "<untrusted>&visible"},
        )
        self.assert_security_headers(response)

    def test_unknown_route_returns_structured_json_404(self) -> None:
        response, body = self.request("GET", "/does-not-exist")

        self.assertEqual(response.status, 404)
        self.assertEqual(
            json.loads(body),
            {
                "ok": False,
                "error": {"code": "not_found", "message": "Route not found."},
            },
        )
        self.assert_security_headers(response)

    def test_parent_paths_never_escape_static_root(self) -> None:
        for path in ("/../pyproject.toml", "/%2e%2e/pyproject.toml"):
            with self.subTest(path=path):
                response, body = self.request("GET", path)
                self.assertEqual(response.status, 404)
                self.assertNotIn(b"setuptools", body)

    def test_invalid_host_is_rejected(self) -> None:
        response, body = self.request(
            "GET", "/api/state", headers={"Host": "attacker.invalid"}
        )

        self.assertEqual(response.status, 400)
        self.assertEqual(json.loads(body)["error"]["code"], "invalid_host")

    def test_options_does_not_authorize_mutations_or_emit_cors(self) -> None:
        response, body = self.request("OPTIONS", "/api/history/clear")

        self.assertEqual(response.status, 405)
        self.assertEqual(json.loads(body)["error"]["code"], "method_not_allowed")
        self.assertIsNone(response.getheader("Access-Control-Allow-Origin"))
        self.assertIsNone(response.getheader("Access-Control-Allow-Methods"))

    def test_unsupported_method_uses_structured_405_without_cors(self) -> None:
        response, body = self.request("PUT", "/api/control/display-nav", body=b"{}")

        self.assertEqual(response.status, 405)
        self.assertEqual(json.loads(body)["error"]["code"], "method_not_allowed")
        self.assertIsNone(response.getheader("Access-Control-Allow-Origin"))

    def test_head_and_arbitrary_methods_use_secured_structured_405(self) -> None:
        for method in ("HEAD", "PROPFIND"):
            with self.subTest(method=method):
                response, body = self.request(method, "/api/state")
                self.assertEqual(response.status, 405)
                self.assertEqual(
                    response.getheader("Content-Type"),
                    "application/json; charset=utf-8",
                )
                self.assert_security_headers(response)
                if method == "HEAD":
                    self.assertEqual(body, b"")
                else:
                    self.assertEqual(
                        json.loads(body)["error"]["code"], "method_not_allowed"
                    )

    def test_malformed_absolute_request_target_returns_structured_400(self) -> None:
        connection = socket.create_connection(
            ("127.0.0.1", self.server.server_port), timeout=2.0
        )
        connection.sendall(
            b"GET http://[bad/ HTTP/1.1\r\n"
            + f"Host: 127.0.0.1:{self.server.server_port}\r\n".encode("ascii")
            + b"Connection: close\r\n\r\n"
        )
        response = http.client.HTTPResponse(connection)
        response.begin()
        body = response.read()
        connection.close()

        self.assertEqual(response.status, 400)
        self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")
        self.assert_security_headers(response)

    def test_server_error_hook_logs_only_bounded_nonsensitive_notice(self) -> None:
        output = io.StringIO()
        try:
            raise RuntimeError(
                "secret exception detail token=test-control-token path=/api/raw"
            )
        except RuntimeError:
            with redirect_stderr(output):
                self.server.handle_error(None, ("127.0.0.1", 1))

        notice = output.getvalue()
        self.assertTrue(notice.strip())
        self.assertLessEqual(len(notice), 160)
        self.assertNotIn("secret exception detail", notice)
        self.assertNotIn("test-control-token", notice)
        self.assertNotIn("/api/raw", notice)
        self.assertNotIn("Traceback", notice)


class HistoryRouteTest(WebServerTestCase):
    def test_history_parses_fixed_filters_and_clamps_limit(self) -> None:
        cursor = "eyJyZWNlaXZlZF9hdCI6MS4wLCJpZCI6MX0="
        response, body = self.request(
            "GET",
            "/api/history?since=1.25&until=2&kind=event&source=ble_rid"
            f"&class=drone&text={quote('RID ✈')}&positioned=true"
            f"&cursor={quote(cursor)}&limit=999",
        )

        self.assertEqual(response.status, 200)
        self.assertEqual(
            self.application.history_query,
            HistoryQuery(
                since=1.25,
                until=2.0,
                kind="event",
                source="ble_rid",
                threat_class="drone",
                text="RID ✈",
                positioned=True,
                cursor=cursor,
                limit=500,
            ),
        )
        self.assertEqual(
            json.loads(body),
            {
                "ok": True,
                "data": {
                    "items": [observation().to_dict()],
                    "next_cursor": "next-page",
                },
            },
        )

    def test_history_defaults_limit_and_clamps_zero_to_one(self) -> None:
        response, _ = self.request("GET", "/api/history")
        self.assertEqual(response.status, 200)
        self.assertEqual(self.application.history_query, HistoryQuery(limit=100))

        response, _ = self.request("GET", "/api/history?limit=0")
        self.assertEqual(response.status, 200)
        self.assertEqual(self.application.history_query, HistoryQuery(limit=1))

    def test_history_rejects_invalid_or_unbounded_queries(self) -> None:
        overflowing_cursor = urlsafe_b64encode(
            json.dumps({"received_at": 10**1000, "id": 1}).encode("utf-8")
        ).decode("ascii")
        cases = (
            "unknown=value",
            "limit=1&limit=2",
            "since=nan",
            "until=Infinity",
            "positioned=1",
            "positioned=True",
            "limit=1.5",
            "limit=%2B2",
            "kind=" + "k" * 65,
            "source=" + "s" * 65,
            "class=" + "c" * 65,
            "text=" + "t" * 257,
            "cursor=" + quote("é"),
            "cursor=" + "a" * 2049,
            "cursor=bad",
            "text=%ZZ",
            "text=%FF",
            "cursor=" + quote(overflowing_cursor),
        )
        for query in cases:
            with self.subTest(query=query):
                response, body = self.request("GET", "/api/history?" + query)
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_application_value_error_is_an_internal_failure_not_bad_query(self) -> None:
        self.application.history_failure = ValueError(
            "database value failure secret detail"
        )

        response, body = self.request("GET", "/api/history")

        self.assertEqual(response.status, 500)
        self.assertEqual(json.loads(body)["error"]["code"], "internal_error")
        self.assertNotIn(b"secret detail", body)

    def test_cursor_row_id_must_fit_sqlite_signed_64_bit_range(self) -> None:
        too_large = history_cursor(9_223_372_036_854_775_808)
        maximum = history_cursor(9_223_372_036_854_775_807)

        response, body = self.request(
            "GET", "/api/history?cursor=" + quote(too_large)
        )
        export_response, export_body = self.request(
            "GET", "/api/history/export.json?cursor=" + quote(too_large)
        )

        self.assertEqual(response.status, 400)
        self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")
        self.assertEqual(export_response.status, 400)
        self.assertEqual(
            json.loads(export_body)["error"]["code"], "invalid_request"
        )
        self.assertIsNone(self.application.history_query)

        maximum_response, _ = self.request(
            "GET", "/api/history?cursor=" + quote(maximum)
        )
        self.assertEqual(maximum_response.status, 200)
        self.assertEqual(self.application.history_query, HistoryQuery(cursor=maximum))


APPROVED_POSTS = (
    ("/api/history/clear", {"confirm": "clear-history"}, "clear_history"),
    ("/api/control/display-nav", {"action": "next"}, "display_nav"),
    ("/api/control/theme", THEME, "set_theme"),
    ("/api/control/theme/reset", {}, "reset_theme"),
    ("/api/control/display-policy", complete_policy(), "set_display_policy"),
    ("/api/control/display-policy/reset", {}, "reset_display_policy"),
)


class MutationRouteTest(WebServerTestCase):
    def post(
        self,
        path: str,
        payload: object,
        *,
        token: str | None = "test-control-token",
        origin: str | None = "same",
        content_type: str = "application/json",
        raw: bytes | None = None,
    ) -> tuple[http.client.HTTPResponse, bytes]:
        headers = {"Content-Type": content_type}
        if token is not None:
            headers["X-New-Dash-Token"] = token
        if origin is not None:
            headers["Origin"] = self.server.url if origin == "same" else origin
        body = (
            json.dumps(payload, separators=(",", ":")).encode("utf-8")
            if raw is None
            else raw
        )
        return self.request("POST", path, headers=headers, body=body)

    def test_every_approved_route_requires_exact_token(self) -> None:
        for path, payload, _ in APPROVED_POSTS:
            for token in (None, "wrong-token"):
                with self.subTest(path=path, token=token):
                    response, body = self.post(path, payload, token=token)
                    self.assertEqual(response.status, 403)
                    self.assertEqual(json.loads(body)["error"]["code"], "invalid_token")
        self.assertEqual(self.application.control_calls, [])

    def test_every_approved_route_requires_exact_same_origin(self) -> None:
        for path, payload, _ in APPROVED_POSTS:
            for origin in (None, "http://attacker.invalid"):
                with self.subTest(path=path, origin=origin):
                    response, body = self.post(path, payload, origin=origin)
                    self.assertEqual(response.status, 403)
                    self.assertEqual(json.loads(body)["error"]["code"], "invalid_origin")
        self.assertEqual(self.application.control_calls, [])

    def test_each_approved_route_calls_only_its_corresponding_facade(self) -> None:
        for path, payload, method in APPROVED_POSTS:
            with self.subTest(path=path):
                self.application.control_calls.clear()
                response, body = self.post(path, payload)
                self.assertEqual(response.status, 200)
                self.assertEqual([call[0] for call in self.application.control_calls], [method])
                data = json.loads(body)["data"]
                if path == "/api/history/clear":
                    self.assertEqual(data, {"deleted": 3})
                else:
                    self.assertTrue(data["ok"])

    def test_every_approved_route_rejects_non_json_and_arrays(self) -> None:
        for path, _, _ in APPROVED_POSTS:
            with self.subTest(path=path, case="content-type"):
                response, body = self.post(path, {}, content_type="text/plain")
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")
            with self.subTest(path=path, case="array"):
                response, body = self.post(path, [])
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_every_approved_route_rejects_oversized_bodies(self) -> None:
        oversized = b"{" + b" " * 65_535 + b"}"
        self.assertEqual(len(oversized), 65_537)
        for path, _, _ in APPROVED_POSTS:
            with self.subTest(path=path):
                response, body = self.post(path, {}, raw=oversized)
                self.assertEqual(response.status, 413)
                self.assertEqual(json.loads(body)["error"]["code"], "body_too_large")

    def test_every_approved_route_rejects_unknown_top_level_fields(self) -> None:
        invalid_payloads = {
            "/api/history/clear": {"confirm": "clear-history", "extra": True},
            "/api/control/display-nav": {"action": "next", "extra": True},
            "/api/control/theme": {**THEME, "extra": True},
            "/api/control/theme/reset": {"extra": True},
            "/api/control/display-policy": {**complete_policy(), "extra": True},
            "/api/control/display-policy/reset": {"extra": True},
        }
        for path, payload in invalid_payloads.items():
            with self.subTest(path=path):
                response, body = self.post(path, payload)
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_json_duplicate_fields_are_rejected(self) -> None:
        response, body = self.post(
            "/api/control/display-nav",
            {},
            raw=b'{"action":"next","action":"back"}',
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_exact_body_cap_is_accepted(self) -> None:
        body = b'{"action":"next"}'
        body += b" " * (65_536 - len(body))
        response, _ = self.post("/api/control/display-nav", {}, raw=body)
        self.assertEqual(response.status, 200)

    def test_clear_requires_exact_confirmation(self) -> None:
        for payload in ({}, {"confirm": "CLEAR"}, {"confirm": True}):
            with self.subTest(payload=payload):
                response, body = self.post("/api/history/clear", payload)
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_prohibited_mutation_routes_are_absent_even_with_credentials(self) -> None:
        paths = (
            "/api/control",
            "/api/raw",
            "/api/firmware",
            "/api/control/firmware",
            "/api/reboot",
            "/api/control/reboot",
            "/api/bootloader",
            "/api/control/bootloader",
        )
        for path in paths:
            with self.subTest(path=path):
                response, body = self.post(path, {})
                self.assertEqual(response.status, 404)
                self.assertEqual(json.loads(body)["error"]["code"], "not_found")

    def test_control_error_mapping_is_bounded_and_structured(self) -> None:
        cases = (
            (ControlValidationError("invalid action"), None, 400, "invalid_request"),
            (TransportUnavailable(), None, 409, "transport_unavailable"),
            (ControlTimeout(), None, 504, "control_timeout"),
            (RuntimeError("secret-token-do-not-leak"), None, 500, "internal_error"),
            (
                None,
                ControlReply.from_payload({"error": "firmware rejected it"}, ok=False),
                502,
                "firmware_rejected",
            ),
        )
        for failure, reply, status, code in cases:
            with self.subTest(code=code):
                self.application.control_failure = failure
                if reply is not None:
                    self.application.control_reply = reply
                response, body = self.post("/api/control/display-nav", {"action": "next"})
                self.assertEqual(response.status, status)
                decoded = json.loads(body)
                self.assertEqual(decoded["error"]["code"], code)
                self.assertNotIn("Traceback", body.decode())
                self.assertNotIn("secret-token-do-not-leak", body.decode())
                self.application.control_failure = None
                self.application.control_reply = ControlReply.from_payload(
                    {"message": "accepted"}, ok=True
                )


class HistoryExportRouteTest(WebServerTestCase):
    def test_csv_export_has_full_stable_header_and_escaped_cells(self) -> None:
        response, body = self.request(
            "GET", "/api/history/export.csv?source=ble_rid&positioned=false"
        )

        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "text/csv; charset=utf-8")
        self.assertEqual(
            response.getheader("Content-Disposition"),
            'attachment; filename="new-dash-history.csv"',
        )
        lines = body.decode("utf-8").splitlines()
        self.assertEqual(lines[0], ",".join(CSV_HEADERS))
        self.assertIn('"DJI, Inc."', lines[1])
        self.assertIn('"{""note"":""USB text, not markup""}"', lines[1])
        self.assertEqual(
            self.application.export_query,
            HistoryQuery(source="ble_rid", positioned=False),
        )

    def test_empty_csv_export_still_has_full_header(self) -> None:
        self.application.export_history = lambda query: iter(())
        response, body = self.request("GET", "/api/history/export.csv")

        self.assertEqual(response.status, 200)
        self.assertEqual(body.decode("utf-8").splitlines(), [",".join(CSV_HEADERS)])

    def test_json_export_sends_headers_before_consuming_whole_iterator(self) -> None:
        started = threading.Event()
        release = threading.Event()

        def blocking_export(query: HistoryQuery):
            yield observation()
            started.set()
            release.wait(2.0)
            yield observation()

        self.application.export_history = blocking_export
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=2.0
        )
        connection.request("GET", "/api/history/export.json")
        response = connection.getresponse()
        try:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("Content-Type"), "application/json; charset=utf-8")
            self.assertEqual(
                response.getheader("Content-Disposition"),
                'attachment; filename="new-dash-history.json"',
            )
            self.assertTrue(started.wait(1.0))
            release.set()
            body = response.read()
        finally:
            release.set()
            connection.close()

        self.assertEqual(json.loads(body), [observation().to_dict(), observation().to_dict()])

    def test_export_rejects_pagination_and_unknown_parameters(self) -> None:
        for suffix in ("?cursor=bad", "?limit=10", "?extra=value"):
            with self.subTest(suffix=suffix):
                response, body = self.request("GET", "/api/history/export.json" + suffix)
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_application_value_error_before_export_is_structured_500(self) -> None:
        self.application.export_failure = ValueError(
            "storage export value failure secret detail"
        )

        response, body = self.request("GET", "/api/history/export.json")

        self.assertEqual(response.status, 500)
        self.assertEqual(json.loads(body)["error"]["code"], "internal_error")
        self.assertNotIn(b"secret detail", body)

    def test_unserializable_first_export_row_fails_before_200(self) -> None:
        class UnserializableObservation:
            def to_dict(self) -> dict[str, object]:
                result = observation().to_dict()
                result["extras"] = {"bad": object()}
                return result

        self.application.export_history = lambda query: iter(
            (UnserializableObservation(),)
        )
        for suffix in ("json", "csv"):
            with self.subTest(suffix=suffix):
                response, body = self.request(
                    "GET", f"/api/history/export.{suffix}"
                )
                self.assertEqual(response.status, 500)
                self.assertEqual(
                    response.getheader("Content-Type"),
                    "application/json; charset=utf-8",
                )
                self.assertIsNone(response.getheader("Content-Disposition"))
                self.assertEqual(
                    json.loads(body)["error"]["code"], "internal_error"
                )

    def test_csv_neutralizes_formula_strings_but_json_and_numbers_stay_truthful(self) -> None:
        dangerous = replace(
            observation(),
            stable_key="+cmd",
            category="\rformula",
            label='=HYPERLINK("https://attacker.invalid")',
            display_id="\tformula",
            manufacturer="-formula",
            operator_id="@SUM(A1:A2)",
            rssi=-48,
        )
        self.application.export_history = lambda query: iter((dangerous,))

        csv_response, csv_body = self.request("GET", "/api/history/export.csv")
        json_response, json_body = self.request("GET", "/api/history/export.json")

        self.assertEqual(csv_response.status, 200)
        row = next(csv.DictReader(io.StringIO(csv_body.decode("utf-8"))))
        for field in (
            "stable_key",
            "category",
            "label",
            "display_id",
            "manufacturer",
            "operator_id",
        ):
            with self.subTest(field=field):
                self.assertTrue(row[field].startswith("'"))
                self.assertEqual(row[field][1:], dangerous.to_dict()[field])
        self.assertEqual(row["rssi"], "-48")

        self.assertEqual(json_response.status, 200)
        json_row = json.loads(json_body)[0]
        self.assertEqual(json_row["label"], dangerous.label)
        self.assertEqual(json_row["operator_id"], dangerous.operator_id)
        self.assertEqual(json_row["rssi"], -48)


if __name__ == "__main__":
    unittest.main()
