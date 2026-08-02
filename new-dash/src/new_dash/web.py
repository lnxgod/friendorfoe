"""Secured IPv4-loopback HTTP adapter for New Dash."""

from __future__ import annotations

from base64 import b64decode, urlsafe_b64encode
from collections.abc import Iterator, Mapping
import csv
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import errno
import io
import json
from math import isfinite
from pathlib import Path
import secrets
import sys
import threading
from typing import Protocol
from urllib.parse import SplitResult, parse_qs, urlsplit

from .controls import ControlValidationError
from .models import ControlReply
from .serial_transport import ControlTimeout, TransportUnavailable
from .storage import HistoryPage, HistoryQuery


_DEFAULT_PORT = 8765
_REQUEST_TIMEOUT_SECONDS = 10.0
_MAX_REQUEST_BODY = 65_536
_MAX_QUERY_FIELDS = 9
_MAX_QUERY_STRING_BYTES = 8_192
_MAX_FILTER_TEXT = 256
_MAX_FILTER_SCALAR = 64
_MAX_CURSOR = 2_048
_MAX_SQLITE_ROW_ID = 9_223_372_036_854_775_807
_CSP = (
    "default-src 'self'; "
    "script-src 'self'; "
    "style-src 'self'; "
    "img-src 'self' data: https://*.tile.openstreetmap.org; "
    "connect-src 'self'; "
    "frame-ancestors 'none'; "
    "base-uri 'none'; object-src 'none'"
)
_STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8", True),
    "/static/styles.css": ("styles.css", "text/css; charset=utf-8", False),
    "/static/api.js": ("api.js", "text/javascript; charset=utf-8", False),
    "/static/ui.js": ("ui.js", "text/javascript; charset=utf-8", False),
    "/static/views/live.js": (
        "views/live.js",
        "text/javascript; charset=utf-8",
        False,
    ),
    "/static/views/map.js": (
        "views/map.js",
        "text/javascript; charset=utf-8",
        False,
    ),
    "/static/views/history.js": (
        "views/history.js",
        "text/javascript; charset=utf-8",
        False,
    ),
    "/static/views/badge.js": (
        "views/badge.js",
        "text/javascript; charset=utf-8",
        False,
    ),
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
_STATIC_ROOT = Path(__file__).with_name("static").resolve()
_HISTORY_PARAMETERS = frozenset(
    {"since", "until", "kind", "source", "class", "text", "positioned", "cursor", "limit"}
)
_EXPORT_PARAMETERS = frozenset(
    {"since", "until", "kind", "source", "class", "text", "positioned"}
)
_POST_ROUTES = frozenset(
    {
        "/api/history/clear",
        "/api/control/display-nav",
        "/api/control/theme",
        "/api/control/theme/reset",
        "/api/control/display-policy",
        "/api/control/display-policy/reset",
    }
)
_CSV_FIELDS = (
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
_MISSING = object()


class ApplicationLike(Protocol):
    """Application operations exposed to the HTTP adapter."""

    def snapshot(self) -> dict[str, object]: ...

    def query_history(self, query: HistoryQuery) -> HistoryPage: ...

    def export_history(self, query: HistoryQuery) -> Iterator[object]: ...

    def clear_history(self) -> int: ...

    def display_nav(self, action: str) -> ControlReply: ...

    def set_theme(self, payload: object) -> ControlReply: ...

    def reset_theme(self) -> ControlReply: ...

    def set_display_policy(self, payload: object) -> ControlReply: ...

    def reset_display_policy(self) -> ControlReply: ...


class NewDashHTTPServer(ThreadingHTTPServer):
    """Thread-per-request server fixed to one application and control token."""

    daemon_threads = True
    allow_reuse_address = False

    def __init__(
        self,
        application: ApplicationLike,
        port: int,
        control_token: str,
    ) -> None:
        self.application = application
        self.control_token = control_token
        super().__init__(("127.0.0.1", port), _NewDashRequestHandler)
        self.socket.settimeout(_REQUEST_TIMEOUT_SECONDS)

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server_port}"

    def serve_in_thread(self) -> threading.Thread:
        thread = threading.Thread(
            target=self.serve_forever,
            name="new-dash-http",
            daemon=True,
        )
        thread.start()
        return thread

    def handle_error(
        self, request: object, client_address: tuple[str, int]
    ) -> None:
        """Report escaped failures without traceback, request, or payload data."""

        print("New Dash HTTP request failed unexpectedly.", file=sys.stderr)


def create_http_server(
    application: ApplicationLike,
    *,
    requested_port: int | None,
    token: str | None = None,
) -> NewDashHTTPServer:
    """Create, but do not start, the loopback-only dashboard server."""

    control_token = secrets.token_urlsafe(32) if token is None else token
    if requested_port is not None:
        return NewDashHTTPServer(application, requested_port, control_token)
    try:
        return NewDashHTTPServer(application, _DEFAULT_PORT, control_token)
    except OSError as error:
        if error.errno != errno.EADDRINUSE:
            raise
        return NewDashHTTPServer(application, 0, control_token)


class _NewDashRequestHandler(BaseHTTPRequestHandler):
    server: NewDashHTTPServer
    protocol_version = "HTTP/1.1"
    server_version = "NewDash"
    sys_version = ""

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(_REQUEST_TIMEOUT_SECONDS)

    def log_message(self, format: str, *args: object) -> None:
        """Keep the local dashboard quiet; composition owns logging."""

    def do_GET(self) -> None:
        if not self._valid_host():
            self._send_error(400, "invalid_host", "Invalid Host header.")
            return
        split = self._split_request_target()
        if split is None:
            return
        path = split.path
        if path == "/api/state":
            try:
                snapshot = self.server.application.snapshot()
            except Exception:
                self._send_error(500, "internal_error", "The request could not be completed.")
                return
            self._send_json(200, {"ok": True, "data": snapshot})
            return
        if path == "/api/history":
            self._send_history(split.query)
            return
        if path == "/api/history/export.csv":
            self._send_history_export(split.query, format="csv")
            return
        if path == "/api/history/export.json":
            self._send_history_export(split.query, format="json")
            return
        static = _STATIC_FILES.get(path)
        if static is not None:
            self._send_static(*static)
            return
        self._send_error(404, "not_found", "Route not found.")

    def do_POST(self) -> None:
        self.close_connection = True
        if not self._valid_host():
            self._send_error(400, "invalid_host", "Invalid Host header.")
            return
        split = self._split_request_target()
        if split is None:
            return
        path = split.path
        if path not in _POST_ROUTES:
            self._send_error(404, "not_found", "Route not found.")
            return
        if split.query:
            self._send_error(400, "invalid_request", "Invalid request.")
            return
        if not self._valid_token():
            self._send_error(403, "invalid_token", "Invalid control token.")
            return
        if not self._valid_origin():
            self._send_error(403, "invalid_origin", "Invalid request origin.")
            return
        try:
            payload = self._read_json_body()
        except _BodyTooLarge:
            self._send_error(413, "body_too_large", "Request body is too large.")
            return
        except ValueError:
            self._send_error(400, "invalid_request", "Invalid JSON request body.")
            return
        self._dispatch_post(path, payload)

    def do_OPTIONS(self) -> None:
        self._reject_method()

    def do_HEAD(self) -> None:
        self._reject_method()

    def do_PUT(self) -> None:
        self._reject_method()

    def do_PATCH(self) -> None:
        self._reject_method()

    def do_DELETE(self) -> None:
        self._reject_method()

    def do_TRACE(self) -> None:
        self._reject_method()

    def do_CONNECT(self) -> None:
        self._reject_method()

    def _reject_method(self) -> None:
        self.close_connection = True
        if not self._valid_host():
            self._send_error(400, "invalid_host", "Invalid Host header.")
            return
        self._send_error(405, "method_not_allowed", "Method not allowed.")

    def send_error(
        self,
        code: int,
        message: str | None = None,
        explain: str | None = None,
    ) -> None:
        """Replace inherited HTML parser/method errors with safe JSON."""

        self.close_connection = True
        if code == 501:
            if not self._valid_host():
                self._send_error(400, "invalid_host", "Invalid Host header.")
                return
            self._send_error(405, "method_not_allowed", "Method not allowed.")
            return
        if 400 <= code < 500:
            self._send_error(code, "invalid_request", "Invalid HTTP request.")
            return
        self._send_error(500, "internal_error", "The request could not be completed.")

    def _split_request_target(self) -> SplitResult | None:
        try:
            return urlsplit(self.path)
        except ValueError:
            self.close_connection = True
            self._send_error(400, "invalid_request", "Invalid request target.")
            return None

    def _valid_host(self) -> bool:
        return self.headers.get_all("Host", []) == [
            f"127.0.0.1:{self.server.server_port}"
        ]

    def _valid_token(self) -> bool:
        values = self.headers.get_all("X-New-Dash-Token", [])
        if len(values) != 1:
            return False
        try:
            return secrets.compare_digest(values[0], self.server.control_token)
        except TypeError:
            return False

    def _valid_origin(self) -> bool:
        return self.headers.get_all("Origin", []) == [self.server.url]

    def _read_json_body(self) -> dict[str, object]:
        if self.headers.get_all("Transfer-Encoding", []):
            raise ValueError("transfer encoding is not accepted")
        content_types = self.headers.get_all("Content-Type", [])
        if len(content_types) != 1:
            raise ValueError("content type is required")
        media_type, separator, parameters = content_types[0].partition(";")
        if media_type.strip().lower() != "application/json":
            raise ValueError("content type must be JSON")
        if separator:
            normalized = parameters.strip().lower().replace(" ", "")
            if normalized != "charset=utf-8":
                raise ValueError("JSON charset must be UTF-8")
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdecimal():
            raise ValueError("content length is required")
        length = int(lengths[0])
        if length > _MAX_REQUEST_BODY:
            raise _BodyTooLarge
        body = self.rfile.read(length)
        if len(body) != length:
            raise ValueError("incomplete request body")
        try:
            decoded = body.decode("utf-8")
            payload = json.loads(
                decoded,
                object_pairs_hook=_object_without_duplicate_keys,
                parse_constant=_reject_json_constant,
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError("invalid JSON request body") from error
        if type(payload) is not dict:
            raise ValueError("JSON request body must be an object")
        return payload

    def _dispatch_post(self, path: str, payload: dict[str, object]) -> None:
        try:
            if path == "/api/history/clear":
                if payload != {"confirm": "clear-history"}:
                    raise ControlValidationError("invalid clear confirmation")
                deleted = self.server.application.clear_history()
                if type(deleted) is not int or deleted < 0:
                    raise RuntimeError("invalid clear result")
                self._send_json(200, {"ok": True, "data": {"deleted": deleted}})
                return
            if path == "/api/control/display-nav":
                if set(payload) != {"action"}:
                    raise ControlValidationError("invalid display navigation payload")
                action = payload["action"]
                if type(action) is not str:
                    raise ControlValidationError("invalid display navigation action")
                reply = self.server.application.display_nav(action)
            elif path == "/api/control/theme":
                reply = self.server.application.set_theme(payload)
            elif path == "/api/control/theme/reset":
                if payload:
                    raise ControlValidationError("theme reset body must be empty")
                reply = self.server.application.reset_theme()
            elif path == "/api/control/display-policy":
                reply = self.server.application.set_display_policy(payload)
            else:
                if payload:
                    raise ControlValidationError(
                        "display policy reset body must be empty"
                    )
                reply = self.server.application.reset_display_policy()
        except ControlValidationError:
            self._send_error(400, "invalid_request", "Invalid control request.")
            return
        except TransportUnavailable as error:
            self._send_error(409, "transport_unavailable", _bounded_message(error.message))
            return
        except ControlTimeout as error:
            self._send_error(504, "control_timeout", _bounded_message(error.message))
            return
        except Exception:
            self._send_error(500, "internal_error", "The request could not be completed.")
            return
        if not isinstance(reply, ControlReply):
            self._send_error(500, "internal_error", "The request could not be completed.")
            return
        if not reply.ok:
            self._send_error(
                502,
                "firmware_rejected",
                _bounded_message(reply.error or "The badge rejected the control request."),
            )
            return
        self._send_json(200, {"ok": True, "data": reply.to_dict()})

    def _send_history(self, query_string: str) -> None:
        try:
            query = _parse_history_query(query_string, export=False)
            page = self.server.application.query_history(query)
            data = {
                "items": [_json_item(item) for item in page.items],
                "next_cursor": page.next_cursor,
            }
        except _QueryValidationError:
            self._send_error(400, "invalid_request", "Invalid history query.")
            return
        except Exception:
            self._send_error(500, "internal_error", "The request could not be completed.")
            return
        self._send_json(200, {"ok": True, "data": data})

    def _send_history_export(self, query_string: str, *, format: str) -> None:
        try:
            query = _parse_history_query(query_string, export=True)
            iterator = iter(self.server.application.export_history(query))
            first = next(iterator, _MISSING)
            if first is _MISSING:
                prepared_first = _MISSING
            elif format == "csv":
                prepared_first = _encode_csv_item(first)
            else:
                prepared_first = _encode_json_item(first)
        except _QueryValidationError:
            self._send_error(400, "invalid_request", "Invalid history query.")
            return
        except Exception:
            self._send_error(500, "internal_error", "The request could not be completed.")
            return
        if format == "csv":
            self._stream_csv(iterator, prepared_first)
        else:
            self._stream_json(iterator, prepared_first)

    def _stream_csv(self, iterator: Iterator[object], first: object) -> None:
        self._start_stream(
            "text/csv; charset=utf-8", 'attachment; filename="new-dash-history.csv"'
        )
        try:
            self.wfile.write(_encode_csv_header())
            if first is not _MISSING:
                self.wfile.write(first)
            self.wfile.flush()
            for item in iterator:
                self.wfile.write(_encode_csv_item(item))
                self.wfile.flush()
        except Exception:
            self.close_connection = True

    def _stream_json(self, iterator: Iterator[object], first: object) -> None:
        self._start_stream(
            "application/json; charset=utf-8",
            'attachment; filename="new-dash-history.json"',
        )
        try:
            self.wfile.write(b"[")
            separator = b""
            if first is not _MISSING:
                self.wfile.write(first)
                separator = b","
            self.wfile.flush()
            for item in iterator:
                self.wfile.write(separator + _encode_json_item(item))
                self.wfile.flush()
                separator = b","
            self.wfile.write(b"]")
            self.wfile.flush()
        except Exception:
            self.close_connection = True

    def _start_stream(self, content_type: str, disposition: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Disposition", disposition)
        self.send_header("Connection", "close")
        self._send_common_headers()
        self.end_headers()
        self.close_connection = True

    def _send_static(
        self,
        filename: str,
        content_type: str,
        substitute_control_token: bool,
    ) -> None:
        path = (_STATIC_ROOT / filename).resolve()
        if not path.is_relative_to(_STATIC_ROOT):
            self._send_error(404, "not_found", "Route not found.")
            return
        try:
            body = path.read_bytes()
        except OSError:
            self._send_error(500, "internal_error", "The request could not be completed.")
            return
        if substitute_control_token:
            body = path.read_text(encoding="utf-8").replace(
                "{{CONTROL_TOKEN}}", escape(self.server.control_token, quote=True)
            ).encode("utf-8")
        self._send_bytes(200, body, content_type)

    def _send_json(self, status: int, value: Mapping[str, object]) -> None:
        try:
            body = json.dumps(
                value,
                separators=(",", ":"),
                ensure_ascii=False,
                allow_nan=False,
            ).encode("utf-8")
        except (TypeError, ValueError):
            status = 500
            body = (
                b'{"ok":false,"error":{"code":"internal_error",'
                b'"message":"The request could not be completed."}}'
            )
        self._send_bytes(status, body, "application/json; charset=utf-8")

    def _send_error(self, status: int, code: str, message: str) -> None:
        self._send_json(
            status,
            {"ok": False, "error": {"code": code, "message": message}},
        )

    def _send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self._send_common_headers()
        self.end_headers()
        try:
            if self.command != "HEAD":
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_common_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", _CSP)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")


def _parse_history_query(query_string: str, *, export: bool) -> HistoryQuery:
    try:
        return _build_history_query(query_string, export=export)
    except _QueryValidationError:
        raise
    except (OverflowError, UnicodeError, ValueError) as error:
        raise _QueryValidationError("invalid history query") from error


def _build_history_query(query_string: str, *, export: bool) -> HistoryQuery:
    if len(query_string.encode("utf-8")) > _MAX_QUERY_STRING_BYTES:
        raise ValueError("history query is too long")
    _validate_percent_encoding(query_string)
    parsed = parse_qs(
        query_string,
        keep_blank_values=True,
        strict_parsing=True,
        encoding="utf-8",
        errors="strict",
        max_num_fields=_MAX_QUERY_FIELDS,
    )
    allowed = _EXPORT_PARAMETERS if export else _HISTORY_PARAMETERS
    if not set(parsed) <= allowed or any(len(values) != 1 for values in parsed.values()):
        raise ValueError("history query has unknown or repeated fields")

    def value(name: str) -> str | None:
        values = parsed.get(name)
        return None if values is None else values[0]

    since = _parse_time(value("since"), "since")
    until = _parse_time(value("until"), "until")
    kind = _bounded_scalar(value("kind"), "kind")
    if kind is not None and kind not in {"event", "track"}:
        raise ValueError("invalid history kind")
    source = _bounded_scalar(value("source"), "source")
    threat_class = _bounded_scalar(value("class"), "class")
    text = value("text")
    if text is not None and len(text) > _MAX_FILTER_TEXT:
        raise ValueError("history text is too long")
    positioned_value = value("positioned")
    if positioned_value is None:
        positioned = None
    elif positioned_value == "true":
        positioned = True
    elif positioned_value == "false":
        positioned = False
    else:
        raise ValueError("positioned must be true or false")

    cursor = value("cursor")
    if cursor is not None:
        if not cursor or len(cursor) > _MAX_CURSOR or not cursor.isascii():
            raise ValueError("invalid history cursor")
        _validate_cursor(cursor)

    limit_value = value("limit")
    if limit_value is None:
        limit = 100
    elif not limit_value.isascii() or not limit_value.isdecimal():
        raise ValueError("history limit must be a decimal integer")
    else:
        limit = min(max(int(limit_value), 1), 500)

    return HistoryQuery(
        since=since,
        until=until,
        kind=kind,
        source=source,
        threat_class=threat_class,
        text=text,
        positioned=positioned,
        cursor=cursor,
        limit=limit,
    )


def _parse_time(value: str | None, name: str) -> float | None:
    if value is None:
        return None
    try:
        parsed = float(value)
    except (ValueError, OverflowError) as error:
        raise ValueError(f"invalid {name}") from error
    if not isfinite(parsed):
        raise ValueError(f"invalid {name}")
    return parsed


def _validate_percent_encoding(query_string: str) -> None:
    hexadecimal = frozenset("0123456789abcdefABCDEF")
    index = 0
    while True:
        index = query_string.find("%", index)
        if index < 0:
            return
        if (
            index + 2 >= len(query_string)
            or query_string[index + 1] not in hexadecimal
            or query_string[index + 2] not in hexadecimal
        ):
            raise ValueError("invalid percent encoding")
        index += 3


def _validate_cursor(cursor: str) -> None:
    if len(cursor) % 4:
        raise ValueError("invalid history cursor")
    try:
        decoded = b64decode(cursor.encode("ascii"), altchars=b"-_", validate=True)
        if urlsafe_b64encode(decoded).decode("ascii") != cursor:
            raise ValueError("invalid history cursor")
        payload = json.loads(decoded.decode("utf-8"), parse_constant=_reject_json_constant)
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise ValueError("invalid history cursor") from error
    if not isinstance(payload, dict) or set(payload) != {"received_at", "id"}:
        raise ValueError("invalid history cursor")
    received_at = payload["received_at"]
    row_id = payload["id"]
    if (
        isinstance(received_at, bool)
        or not isinstance(received_at, (int, float))
        or not isfinite(float(received_at))
        or isinstance(row_id, bool)
        or not isinstance(row_id, int)
        or row_id < 1
        or row_id > _MAX_SQLITE_ROW_ID
    ):
        raise ValueError("invalid history cursor")


def _bounded_scalar(value: str | None, name: str) -> str | None:
    if value is not None and (not value or len(value) > _MAX_FILTER_SCALAR):
        raise ValueError(f"invalid {name}")
    return value


def _json_item(item: object) -> Mapping[str, object]:
    to_dict = getattr(item, "to_dict", None)
    if not callable(to_dict):
        raise TypeError("history item is not serializable")
    result = to_dict()
    if not isinstance(result, Mapping):
        raise TypeError("history item is not serializable")
    return result


def _encode_json_item(item: object) -> bytes:
    return json.dumps(
        _json_item(item),
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def _encode_csv_header() -> bytes:
    stream = io.StringIO(newline="")
    csv.DictWriter(stream, fieldnames=_CSV_FIELDS).writeheader()
    return stream.getvalue().encode("utf-8")


def _encode_csv_item(item: object) -> bytes:
    row = dict(_json_item(item))
    row["extras"] = json.dumps(
        row.get("extras", {}),
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    )
    row = {key: _csv_safe_cell(value) for key, value in row.items()}
    stream = io.StringIO(newline="")
    csv.DictWriter(
        stream,
        fieldnames=_CSV_FIELDS,
        extrasaction="ignore",
    ).writerow(row)
    return stream.getvalue().encode("utf-8")


def _csv_safe_cell(value: object) -> object:
    if isinstance(value, str) and value.startswith(("=", "+", "-", "@", "\t", "\r")):
        return "'" + value
    return value


class _BodyTooLarge(ValueError):
    """The declared JSON body exceeds the fixed adapter limit."""


class _QueryValidationError(ValueError):
    """A client-supplied history query fails the fixed HTTP contract."""


def _object_without_duplicate_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON object key")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> object:
    raise ValueError("non-finite JSON number")


def _bounded_message(message: object) -> str:
    if not isinstance(message, str) or not message:
        return "The request could not be completed."
    return message[:256]
