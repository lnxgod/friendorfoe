from __future__ import annotations

from contextlib import redirect_stderr
import http.client
import io
import json
import socket
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
        self.control_calls: list[tuple[str, object | None]] = []
        self.control_failure: Exception | None = None
        self.control_reply = ControlReply.from_payload({"message": "accepted"}, ok=True)

    def snapshot(self) -> dict[str, object]:
        return SNAPSHOT

    def query_history(self, query: HistoryQuery) -> HistoryPage:
        self.history_query = query
        if query.cursor == "bad":
            raise ValueError("invalid history cursor")
        return HistoryPage(items=(observation(),), next_cursor="next-page")

    def export_history(self, query: HistoryQuery):
        self.export_query = query
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

    def test_server_error_hook_never_prints_tracebacks_or_exception_text(self) -> None:
        output = io.StringIO()
        try:
            raise RuntimeError("secret exception detail")
        except RuntimeError:
            with redirect_stderr(output):
                self.server.handle_error(None, ("127.0.0.1", 1))

        self.assertEqual(output.getvalue(), "")


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
        )
        for query in cases:
            with self.subTest(query=query):
                response, body = self.request("GET", "/api/history?" + query)
                self.assertEqual(response.status, 400)
                self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")


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


if __name__ == "__main__":
    unittest.main()
