"""Deterministic standalone browser fixture for New Dash Live and Map QA."""

from __future__ import annotations

import argparse
from copy import deepcopy
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


STATIC_ROOT = Path(__file__).parents[1] / "src" / "new_dash" / "static"
CONTROL_TOKEN = "fixture-control-token"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8", True),
    "/static/styles.css": ("styles.css", "text/css; charset=utf-8", False),
    "/static/api.js": ("api.js", "text/javascript; charset=utf-8", False),
    "/static/ui.js": ("ui.js", "text/javascript; charset=utf-8", False),
    "/static/views/live.js": ("views/live.js", "text/javascript; charset=utf-8", False),
    "/static/views/map.js": ("views/map.js", "text/javascript; charset=utf-8", False),
    "/static/app.js": ("app.js", "text/javascript; charset=utf-8", False),
    "/static/vendor/leaflet/leaflet.css": (
        "vendor/leaflet/leaflet.css",
        "text/css; charset=utf-8",
        False,
    ),
    "/static/vendor/leaflet/leaflet.js": (
        "vendor/leaflet/leaflet.js",
        "text/javascript; charset=utf-8",
        False,
    ),
    "/static/vendor/leaflet/LICENSE": (
        "vendor/leaflet/LICENSE",
        "text/plain; charset=utf-8",
        False,
    ),
}
CSP = (
    "default-src 'self'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data: https://*.tile.openstreetmap.org; "
    "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; object-src 'none'"
)
HOSTILE_USB_TEXT = "<script>window.fixturePwned=true</script>"

FIXTURE_STATE = {
    "connection": {
        "phase": "live",
        "detail": "verified",
        "port": "/dev/cu.usbmodem-fixture",
        "candidates": [],
        "firmware_version": "0.64.66-fixture",
        "reconnect_attempt": 0,
    },
    "freshness": {"state": "fresh", "age_s": 1.0},
    "status": {
        "version": "0.64.66-fixture",
        "uptime_s": 4321.0,
        "mode": "usb_only",
        "mode_label": "USB only",
        "safe_mode": False,
        "safe_reason": None,
        "recovery_mode": "normal",
        "threat_score": 82.0,
        "counts": {"remote_id": 2, "drone": 3, "meta": 1},
        "scanners": [
            {"uart": "ble", "role": "BLE", "connected": True, "health": "healthy", "firmware": "1.3.7"},
            {"uart": "wifi", "role": "Wi-Fi", "connected": True, "health": "healthy", "firmware": "1.2.4"},
        ],
        "entities": [
            {
                "label": "RID-ABC123",
                "detail": "OpenDroneID",
                "evidence": "ASTM BLE Remote ID",
                "class": "drone",
                "category": "DRONE",
                "display_id": "RID-ABC123",
                "source_id": 0,
                "source": "ble_rid",
                "score": 95.0,
                "confidence_pct": 96.0,
                "last_seen_s": 1.0,
                "rssi": -48,
                "events": 7,
                "seen_count": 9,
                "stale": False,
                "lat": 37.7749,
                "lon": -122.4194,
                "altitude_m": 122.5,
                "operator_lat": 37.7754,
                "operator_lon": -122.4188,
                "operator_id": "OP-7",
                "manufacturer": "Fixture Aircraft",
                "is_remote_id": True,
                "has_position": True,
            },
            {
                "label": "RID-XYZ789",
                "detail": "Wi-Fi beacon",
                "evidence": HOSTILE_USB_TEXT,
                "class": "drone",
                "category": "DRONE",
                "display_id": "RID-XYZ789",
                "source_id": 3,
                "source": "wifi_rid",
                "score": 88.0,
                "confidence_pct": 89.0,
                "last_seen_s": 2.0,
                "rssi": -63,
                "events": 5,
                "seen_count": 6,
                "stale": False,
                "lat": 37.7560,
                "lon": -122.4140,
                "altitude_m": 98.1,
                "operator_lat": None,
                "operator_lon": None,
                "operator_id": None,
                "manufacturer": None,
                "is_remote_id": True,
                "has_position": True,
            },
            {
                "label": "Likely DJI Mini 3 Pro",
                "detail": "OcuSync 2.4 GHz",
                "evidence": "DJI vendor IE (0xD0 0x23)",
                "class": "drone",
                "category": "DRONE",
                "display_id": None,
                "source_id": 2,
                "source": "wifi_dji_ie",
                "score": 65.0,
                "confidence_pct": 71.0,
                "last_seen_s": 2.5,
                "rssi": -72,
                "events": 3,
                "seen_count": 3,
                "stale": False,
                "lat": None,
                "lon": None,
                "altitude_m": None,
                "operator_lat": None,
                "operator_lon": None,
                "operator_id": None,
                "manufacturer": "DJI",
                "is_remote_id": False,
                "has_position": False,
            },
            {
                "label": "Expired firmware row",
                "detail": "Must not render",
                "evidence": "Firmware marked stale",
                "class": "meta",
                "category": "META",
                "display_id": "STALE-1",
                "source_id": 6,
                "source": "ble_fingerprint",
                "score": 40.0,
                "confidence_pct": 40.0,
                "last_seen_s": 90.0,
                "rssi": -90,
                "stale": True,
                "lat": None,
                "lon": None,
                "is_remote_id": False,
                "has_position": False,
            },
        ],
        "remote_id_entities": [],
        "reporting": {"interval_s": 1.0},
        "memory": {"heap_free": 186328},
        "display_state": None,
        "theme": None,
        "display_policy": None,
        "sensing_health": "healthy",
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

TRAILS = {
    "ble_rid:RID-ABC123": [
        (1700000001.0, 37.7720, -122.4230, 116.0),
        (1700000002.0, 37.7734, -122.4212, 119.0),
        (1700000003.0, 37.7749, -122.4194, 122.5),
    ],
    "wifi_rid:RID-XYZ789": [
        (1700000001.0, 37.7520, -122.4180, 94.0),
        (1700000002.0, 37.7540, -122.4160, 96.0),
        (1700000003.0, 37.7560, -122.4140, 98.1),
    ],
}


def fixture_state(*, stale: bool, safe_usb: bool) -> dict[str, object]:
    state = deepcopy(FIXTURE_STATE)
    if stale:
        state["connection"]["phase"] = "reconnecting"
        state["connection"]["detail"] = "read_error"
        state["connection"]["reconnect_attempt"] = 2
        state["freshness"] = {"state": "stale", "age_s": 8.4}
        state["status"]["sensing_health"] = "degraded"
    if safe_usb:
        state["status"]["safe_mode"] = True
        state["status"]["safe_reason"] = "fixture_safe_usb"
        state["status"]["recovery_mode"] = "safe_usb"
        state["status"]["sensing_health"] = "safe_usb"
    return state


def trail_items(stable_key: str, cursor: str | None) -> tuple[list[dict[str, object]], str | None]:
    points = TRAILS.get(stable_key, [])
    start = 2 if cursor == "fixture-page-2" else 0
    stop = len(points) if start else min(2, len(points))
    items = [
        {
            "row_id": index + 1,
            "kind": "track",
            "received_at": observed_at + 0.2,
            "observed_at": observed_at,
            "stable_key": stable_key,
            "source": stable_key.split(":", 1)[0],
            "latitude": latitude,
            "longitude": longitude,
            "altitude_m": altitude,
        }
        for index, (observed_at, latitude, longitude, altitude) in enumerate(points[start:stop], start=start)
    ]
    next_cursor = "fixture-page-2" if start == 0 and len(points) > stop else None
    return items, next_cursor


class FixtureHandler(BaseHTTPRequestHandler):
    stale = False
    safe_usb = False

    def log_message(self, format: str, *args: object) -> None:
        return

    def do_GET(self) -> None:
        split = urlsplit(self.path)
        if split.path == "/api/state":
            self.send_json({"ok": True, "data": fixture_state(stale=self.stale, safe_usb=self.safe_usb)})
            return
        if split.path == "/api/history":
            params = parse_qs(split.query)
            stable_key = params.get("text", [""])[0]
            cursor = params.get("cursor", [None])[0]
            items, next_cursor = trail_items(stable_key, cursor)
            self.send_json({"ok": True, "data": {"items": items, "next_cursor": next_cursor}})
            return
        static = STATIC_FILES.get(split.path)
        if static is None or split.query:
            self.send_json({"ok": False, "error": {"code": "not_found", "message": "Route not found."}}, status=404)
            return
        filename, content_type, substitute_token = static
        body = (STATIC_ROOT / filename).read_bytes()
        if substitute_token:
            body = body.decode("utf-8").replace(
                "{{CONTROL_TOKEN}}", escape(CONTROL_TOKEN, quote=True)
            ).encode("utf-8")
        self.send_bytes(body, content_type)

    def send_json(self, value: object, status: int = 200) -> None:
        self.send_bytes(
            json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8"),
            "application/json; charset=utf-8",
            status,
        )

    def send_bytes(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8876)
    parser.add_argument("--stale", action="store_true", help="serve a host-stale reconnecting snapshot")
    parser.add_argument("--safe-usb", action="store_true", help="serve a healthy USB / disabled sensing snapshot")
    args = parser.parse_args()
    FixtureHandler.stale = args.stale
    FixtureHandler.safe_usb = args.safe_usb
    server = ThreadingHTTPServer(("127.0.0.1", args.port), FixtureHandler)
    print(f"New Dash browser fixture: http://127.0.0.1:{server.server_port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
