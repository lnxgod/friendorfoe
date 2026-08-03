"""Pure transcript helpers for the Backend Badge Lite USB protocol.

The module deliberately has no serial-port dependency.  A host client owns
discovery and I/O; these helpers validate or construct one newline-delimited
protocol frame at a time.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import json
import math
from typing import Any


ESPRESSIF_USB_SERIAL_JTAG_VID = 0x303A
ESPRESSIF_USB_SERIAL_JTAG_PID = 0x1001

COMMAND_MAX_BYTES = 2047
STATUS_MAX_BYTES = 8192
DETECTION_MAX_BYTES = 1535
HEARTBEAT_MS = 5000
LEASE_MS = 15000

LITE_IDENTITY = (
    "badge_lite",
    "uplink-s3-backend",
    "fof_backend_uplink",
    "seeed_xiao_esp32s3",
)

LITE_CAPABILITIES = frozenset(
    {
        "display_none",
        "usb_live",
        "usb_live_ack",
        "usb_buffered",
        "usb_config",
        "http_uplink",
        "config_ap",
        "ap_dashboard",
        "remote_ota",
        "uart_relay_ota",
    }
)

_VERIFIED_DEVICE_PROOF = object()

_CONFIG_SET_KEYS = frozenset(
    {
        "networks",
        "backend_url",
        "display_name",
        "ap_password",
        "auto_update_enabled",
        "confirm_auto_update",
        "has_location",
        "latitude",
        "longitude",
        "altitude_m",
    }
)

_CONFIG_GET_KEYS = frozenset(
    {
        "schema_version",
        "generation",
        "networks",
        "backend_url",
        "device_id",
        "display_name",
        "ap_password_set",
        "auto_update_enabled",
        "has_location",
        "latitude",
        "longitude",
        "altitude_m",
    }
)

_DETECTION_KEYS = frozenset(
    {
        "id",
        "manufacturer",
        "badge_label",
        "badge_class",
        "badge_entity_key",
        "source",
        "confidence",
        "threat_score",
        "rssi",
    }
)

_STATUS_KEYS = frozenset(
    {
        "product_family",
        "target",
        "project",
        "hardware",
        "version",
        "mac",
        "boot_id",
        "mode",
        "mode_label",
        "config_generation",
        "capabilities",
        "wifi",
        "recovery",
        "scanner",
        "threats",
        "led",
        "ota_ready",
        "upload",
        "usb",
        "live",
        "history",
        "dashboard",
        "backend",
        "scanner_summaries",
    }
)

_LED_STATES = frozenset(
    {
        "healthy",
        "network_degraded",
        "drone",
        "meta",
        "drone_meta",
        "fatal",
        "uart_lost",
    }
)


class ProtocolError(ValueError):
    """The transcript is not valid for the current Lite protocol."""


def _clean_line(frame: str, *, maximum_bytes: int) -> str:
    if not isinstance(frame, str) or not frame:
        raise ProtocolError("frame must be a nonempty string")
    if frame.endswith("\r\n"):
        frame = frame[:-2]
    elif frame.endswith("\n"):
        frame = frame[:-1]
    if not frame or "\r" in frame or "\n" in frame or "\x00" in frame:
        raise ProtocolError("frame must contain exactly one complete line")
    if len(frame.encode("utf-8")) > maximum_bytes:
        raise ProtocolError("frame exceeds the protocol limit")
    return frame


def _no_duplicate_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    parsed: dict[str, Any] = {}
    for key, value in pairs:
        if key in parsed:
            raise ProtocolError(f"duplicate JSON member: {key}")
        parsed[key] = value
    return parsed


def _reject_json_constant(value: str) -> None:
    raise ProtocolError(f"invalid JSON number: {value}")


def _parse_json_frame(
    frame: str,
    prefix: str,
    *,
    maximum_bytes: int,
) -> dict[str, Any]:
    line = _clean_line(frame, maximum_bytes=maximum_bytes)
    if not line.startswith(prefix):
        raise ProtocolError(f"expected {prefix}")
    payload = line[len(prefix) :]
    try:
        decoded = json.loads(
            payload,
            object_pairs_hook=_no_duplicate_object,
            parse_constant=_reject_json_constant,
        )
    except ProtocolError:
        raise
    except (TypeError, ValueError, json.JSONDecodeError) as error:
        raise ProtocolError("malformed JSON payload") from error
    if not isinstance(decoded, dict):
        raise ProtocolError("payload must be a JSON object")
    return decoded


def _exact_keys(payload: Mapping[str, Any], expected: frozenset[str]) -> None:
    keys = frozenset(payload)
    if keys != expected:
        missing = sorted(expected - keys)
        extra = sorted(keys - expected)
        raise ProtocolError(
            f"unexpected JSON shape (missing={missing}, extra={extra})"
        )


def _required_keys(
    payload: Mapping[str, Any],
    expected: frozenset[str],
    name: str = "status",
) -> None:
    """Require the current contract while allowing future additive fields."""

    missing = sorted(expected - frozenset(payload))
    if missing:
        raise ProtocolError(f"{name} is missing required fields: {missing}")


def _text(value: Any, name: str, *, nonempty: bool = False) -> str:
    if not isinstance(value, str) or (nonempty and not value):
        raise ProtocolError(f"{name} must be a string")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise ProtocolError(f"{name} contains a control character")
    return value


def _integer(
    value: Any,
    name: str,
    *,
    minimum: int,
    maximum: int,
) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError(f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise ProtocolError(f"{name} is out of range")
    return value


def _number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProtocolError(f"{name} must be numeric")
    number = float(value)
    if not math.isfinite(number):
        raise ProtocolError(f"{name} must be finite")
    return number


def _boolean(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        raise ProtocolError(f"{name} must be a boolean")
    return value


def _deadline_after(now_ms: int, delay_ms: int) -> int:
    return min(now_ms + delay_ms, 2**63 - 1)


def _object(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ProtocolError(f"{name} must be an object")
    return value


def _nullable_text(value: Any, name: str) -> str | None:
    if value is None:
        return None
    return _text(value, name, nonempty=True)


def _nullable_integer(
    value: Any,
    name: str,
    *,
    minimum: int,
    maximum: int,
) -> int | None:
    if value is None:
        return None
    return _integer(value, name, minimum=minimum, maximum=maximum)


def build_ping() -> str:
    return "FOF_PING"


def build_status() -> str:
    return "FOF_STATUS"


def build_config_get() -> str:
    return "FOF_CONFIG_GET"


def build_live_start() -> str:
    return 'FOF_LIVE_START:{"client":"new_dash","protocol":1}'


def _parse_pong(frame: str) -> str:
    line = _clean_line(frame, maximum_bytes=STATUS_MAX_BYTES)
    prefix = "FOF_PONG:"
    if not line.startswith(prefix):
        raise ProtocolError("expected FOF_PONG")
    return _text(line[len(prefix) :], "PONG version", nonempty=True)


@dataclass(frozen=True, init=False)
class VerifiedLiteDevice:
    """Handshake proof required by mutation-building helpers."""

    version: str
    capabilities: frozenset[str]
    _proof: object

    def __init__(
        self,
        *,
        version: str,
        capabilities: frozenset[str],
        _proof: object | None = None,
    ) -> None:
        if _proof is not _VERIFIED_DEVICE_PROOF:
            raise ProtocolError("VerifiedLiteDevice requires handshake proof")
        object.__setattr__(self, "version", version)
        object.__setattr__(self, "capabilities", capabilities)
        object.__setattr__(self, "_proof", _proof)

    @property
    def identity(self) -> tuple[str, str, str, str]:
        return LITE_IDENTITY

    @property
    def mutation_enabled(self) -> bool:
        return (
            self._proof is _VERIFIED_DEVICE_PROOF
            and "usb_config" in self.capabilities
        )

    @property
    def screen_supported(self) -> bool:
        return False


def _validate_scanner_summary(value: Any, index: int) -> None:
    name = f"scanner_summaries[{index}]"
    summary = _object(value, name)
    _required_keys(
        summary,
        frozenset(
            {
                "slot",
                "connected",
                "identity_valid",
                "status_available",
                "identity",
                "profile",
                "health",
                "errors",
                "uptime_ms",
            }
        ),
        name,
    )
    slot = _integer(summary["slot"], f"{name}.slot", minimum=0, maximum=1)
    if slot != index:
        raise ProtocolError(f"{name}.slot is out of order")
    _boolean(summary["connected"], f"{name}.connected")
    _boolean(summary["identity_valid"], f"{name}.identity_valid")
    available = _boolean(
        summary["status_available"], f"{name}.status_available"
    )

    health = _object(summary["health"], f"{name}.health")
    _required_keys(
        health,
        frozenset({"command", "radio", "role_acked"}),
        f"{name}.health",
    )
    for key in ("command", "radio", "role_acked"):
        _boolean(health[key], f"{name}.health.{key}")

    if not available:
        for key in ("identity", "profile", "errors", "uptime_ms"):
            if summary[key] is not None:
                raise ProtocolError(
                    f"{name}.{key} must be null when status is unavailable"
                )
        return

    identity = _object(summary["identity"], f"{name}.identity")
    _required_keys(
        identity,
        frozenset({"target", "project", "hardware", "version"}),
        f"{name}.identity",
    )
    for key in ("target", "project", "hardware", "version"):
        text_value = _text(
            identity[key], f"{name}.identity.{key}", nonempty=True
        )
        if len(text_value.encode("utf-8")) > 64:
            raise ProtocolError(f"{name}.identity.{key} exceeds 64 bytes")
    _integer(summary["profile"], f"{name}.profile", minimum=0, maximum=3)
    errors = _object(summary["errors"], f"{name}.errors")
    _required_keys(
        errors,
        frozenset({"rx", "tx_drops"}),
        f"{name}.errors",
    )
    _integer(errors["rx"], f"{name}.errors.rx", minimum=0, maximum=2**32 - 1)
    _integer(
        errors["tx_drops"],
        f"{name}.errors.tx_drops",
        minimum=0,
        maximum=2**32 - 1,
    )
    _integer(
        summary["uptime_ms"],
        f"{name}.uptime_ms",
        minimum=0,
        maximum=2**64 - 1,
    )


def parse_status(frame: str) -> dict[str, Any]:
    """Parse the bounded final Lite status schema.

    The current fields are mandatory. Additive fields remain accepted so a
    deployed New Dash can continue to probe newer compatible Lite firmware.
    """

    status = _parse_json_frame(
        frame,
        "FOF_STATUS:",
        maximum_bytes=STATUS_MAX_BYTES,
    )
    _required_keys(status, _STATUS_KEYS)

    for key in (
        "product_family",
        "target",
        "project",
        "hardware",
        "version",
        "mac",
        "mode",
        "mode_label",
    ):
        _text(status[key], key, nonempty=True)
    _integer(status["boot_id"], "boot_id", minimum=0, maximum=2**32 - 1)
    _integer(
        status["config_generation"],
        "config_generation",
        minimum=1,
        maximum=2**32 - 1,
    )

    capability_values = status["capabilities"]
    if not isinstance(capability_values, list) or not capability_values:
        raise ProtocolError("capabilities must be a nonempty array")
    capabilities = [
        _text(value, "capability", nonempty=True)
        for value in capability_values
    ]
    if len(frozenset(capabilities)) != len(capabilities):
        raise ProtocolError("capabilities must not contain duplicates")

    wifi = _object(status["wifi"], "wifi")
    _required_keys(
        wifi,
        frozenset({"configured", "connected", "full_pass_failed"}),
        "wifi",
    )
    for key in ("configured", "connected", "full_pass_failed"):
        _boolean(wifi[key], f"wifi.{key}")

    recovery = _object(status["recovery"], "recovery")
    _required_keys(
        recovery,
        frozenset({"reason", "ap_running"}),
        "recovery",
    )
    recovery_reason = _text(recovery["reason"], "recovery.reason", nonempty=True)
    if recovery_reason not in {"none", "wifi_unconfigured", "wifi_join_failed"}:
        raise ProtocolError("recovery.reason is unknown")
    _boolean(recovery["ap_running"], "recovery.ap_running")

    scanners = status["scanner"]
    if not isinstance(scanners, list) or len(scanners) != 2:
        raise ProtocolError("scanner must contain exactly two slots")
    for index, value in enumerate(scanners):
        scanner = _object(value, f"scanner[{index}]")
        _required_keys(
            scanner,
            frozenset({"slot", "connected", "identity_valid"}),
            f"scanner[{index}]",
        )
        slot = _integer(
            scanner["slot"], f"scanner[{index}].slot", minimum=0, maximum=1
        )
        if slot != index:
            raise ProtocolError(f"scanner[{index}].slot is out of order")
        _boolean(scanner["connected"], f"scanner[{index}].connected")
        _boolean(
            scanner["identity_valid"], f"scanner[{index}].identity_valid"
        )

    threats = _object(status["threats"], "threats")
    _required_keys(
        threats,
        frozenset(
            {
                "drone_active",
                "meta_active",
                "drone_count",
                "meta_count",
                "drone_last_seen_age_ms",
                "meta_last_seen_age_ms",
            }
        ),
        "threats",
    )
    _boolean(threats["drone_active"], "threats.drone_active")
    _boolean(threats["meta_active"], "threats.meta_active")
    for key in ("drone_count", "meta_count"):
        _integer(threats[key], f"threats.{key}", minimum=0, maximum=2**32 - 1)
    for key in ("drone_last_seen_age_ms", "meta_last_seen_age_ms"):
        _integer(threats[key], f"threats.{key}", minimum=-1, maximum=2**63 - 1)

    led = _text(status["led"], "led", nonempty=True)
    if led not in _LED_STATES:
        raise ProtocolError("led is not a Lite LED state")
    _boolean(status["ota_ready"], "ota_ready")

    upload = _object(status["upload"], "upload")
    _required_keys(
        upload,
        frozenset({"depth", "capacity", "dropped", "ok", "failed", "retries"}),
        "upload",
    )
    depth = _integer(upload["depth"], "upload.depth", minimum=0, maximum=512)
    capacity = _integer(
        upload["capacity"], "upload.capacity", minimum=512, maximum=512
    )
    if depth > capacity:
        raise ProtocolError("upload.depth exceeds upload.capacity")
    for key in ("dropped", "ok", "failed", "retries"):
        _integer(upload[key], f"upload.{key}", minimum=0, maximum=2**64 - 1)

    usb = _object(status["usb"], "usb")
    _required_keys(
        usb,
        frozenset(
            {
                "available",
                "host_connected",
                "required_depth",
                "optional_depth",
                "optional_drops",
                "required_failures",
                "bytes_transmitted",
                "bytes_received",
                "output_poisoned",
            }
        ),
        "usb",
    )
    for key in ("available", "host_connected", "output_poisoned"):
        _boolean(usb[key], f"usb.{key}")
    for key in (
        "required_depth",
        "optional_depth",
        "optional_drops",
        "required_failures",
        "bytes_transmitted",
        "bytes_received",
    ):
        _integer(usb[key], f"usb.{key}", minimum=0, maximum=2**64 - 1)

    live = _object(status["live"], "live")
    _required_keys(
        live,
        frozenset(
            {
                "started",
                "session_id",
                "last_ack_sequence",
                "confirmed",
                "lease_remaining_ms",
            }
        ),
        "live",
    )
    _boolean(live["started"], "live.started")
    _text(live["session_id"], "live.session_id")
    _integer(
        live["last_ack_sequence"],
        "live.last_ack_sequence",
        minimum=0,
        maximum=2**64 - 1,
    )
    _boolean(live["confirmed"], "live.confirmed")
    _integer(
        live["lease_remaining_ms"],
        "live.lease_remaining_ms",
        minimum=0,
        maximum=2**63 - 1,
    )

    history = _object(status["history"], "history")
    _required_keys(
        history,
        frozenset({"available", "count", "contention_drops"}),
        "history",
    )
    _boolean(history["available"], "history.available")
    _integer(history["count"], "history.count", minimum=0, maximum=2**32 - 1)
    _integer(
        history["contention_drops"],
        "history.contention_drops",
        minimum=0,
        maximum=2**64 - 1,
    )

    dashboard = _object(status["dashboard"], "dashboard")
    _required_keys(
        dashboard,
        frozenset({"enabled", "degraded_reason"}),
        "dashboard",
    )
    _boolean(dashboard["enabled"], "dashboard.enabled")
    _nullable_text(dashboard["degraded_reason"], "dashboard.degraded_reason")

    backend = _object(status["backend"], "backend")
    _required_keys(
        backend,
        frozenset({"reachable", "last_success_age_s"}),
        "backend",
    )
    _boolean(backend["reachable"], "backend.reachable")
    _nullable_integer(
        backend["last_success_age_s"],
        "backend.last_success_age_s",
        minimum=0,
        maximum=2**32 - 1,
    )

    summaries = status["scanner_summaries"]
    if not isinstance(summaries, list) or len(summaries) != 2:
        raise ProtocolError("scanner_summaries must contain exactly two slots")
    for index, summary in enumerate(summaries):
        _validate_scanner_summary(summary, index)
    return status


def verify_lite_handshake(
    pong_frame: str,
    status_frame: str,
) -> VerifiedLiteDevice:
    """Require the exact Lite tuple and matching PONG/STATUS version."""

    pong_version = _parse_pong(pong_frame)
    status = parse_status(status_frame)
    identity = tuple(
        _text(status.get(key), key, nonempty=True)
        for key in ("product_family", "target", "project", "hardware")
    )
    if identity != LITE_IDENTITY:
        raise ProtocolError("status is not the exact Backend Badge Lite identity")
    version = _text(status.get("version"), "version", nonempty=True)
    if version != pong_version:
        raise ProtocolError("PONG and STATUS versions do not match")
    if status.get("mode") != "headless":
        raise ProtocolError("Lite status must report headless mode")

    capability_values = status.get("capabilities")
    if not isinstance(capability_values, list) or not capability_values:
        raise ProtocolError("capabilities must be a nonempty array")
    capabilities = frozenset(
        _text(value, "capability", nonempty=True) for value in capability_values
    )
    if len(capabilities) != len(capability_values):
        raise ProtocolError("capabilities must not contain duplicates")
    missing = LITE_CAPABILITIES - capabilities
    if missing:
        raise ProtocolError(f"Lite capabilities are missing: {sorted(missing)}")
    return VerifiedLiteDevice(
        version=version,
        capabilities=capabilities,
        _proof=_VERIFIED_DEVICE_PROOF,
    )


@dataclass
class LiveSession:
    session_id: str
    heartbeat_ms: int
    lease_ms: int
    _last_acked_sequence: int = 0
    _latest_sequence: int = 0
    _latest_sent_ms: int | None = None
    _lease_expires_ms: int | None = None

    @classmethod
    def from_ready(cls, frame: str) -> "LiveSession":
        payload = _parse_json_frame(
            frame,
            "FOF_LIVE_READY:",
            maximum_bytes=COMMAND_MAX_BYTES,
        )
        _exact_keys(
            payload,
            frozenset({"session_id", "heartbeat_ms", "lease_ms"}),
        )
        session_id = _text(
            payload["session_id"], "session_id", nonempty=True
        )
        if len(session_id.encode("utf-8")) > 32:
            raise ProtocolError("session_id exceeds 32 bytes")
        heartbeat_ms = _integer(
            payload["heartbeat_ms"],
            "heartbeat_ms",
            minimum=1,
            maximum=2**63 - 1,
        )
        lease_ms = _integer(
            payload["lease_ms"],
            "lease_ms",
            minimum=1,
            maximum=2**63 - 1,
        )
        if heartbeat_ms != HEARTBEAT_MS or lease_ms != LEASE_MS:
            raise ProtocolError("unexpected live timing contract")
        return cls(session_id, heartbeat_ms, lease_ms)

    @property
    def last_acked_sequence(self) -> int:
        return self._last_acked_sequence

    def _parse_heartbeat(self, heartbeat_frame: str) -> tuple[str, int]:
        payload = _parse_json_frame(
            heartbeat_frame,
            "FOF_LIVE_HEARTBEAT:",
            maximum_bytes=COMMAND_MAX_BYTES,
        )
        _exact_keys(payload, frozenset({"session_id", "sequence"}))
        session_id = _text(
            payload["session_id"], "session_id", nonempty=True
        )
        sequence = _integer(
            payload["sequence"],
            "sequence",
            minimum=1,
            maximum=2**64 - 1,
        )
        if session_id != self.session_id:
            raise ProtocolError("heartbeat belongs to a stale live session")
        return session_id, sequence

    def observe_heartbeat(self, heartbeat_frame: str, *, sent_at_ms: int) -> None:
        _, sequence = self._parse_heartbeat(heartbeat_frame)
        sent_at_ms = _integer(
            sent_at_ms,
            "sent_at_ms",
            minimum=0,
            maximum=2**63 - 1,
        )
        if sequence <= self._latest_sequence:
            raise ProtocolError("heartbeat sequence is stale or replayed")
        self._latest_sequence = sequence
        self._latest_sent_ms = sent_at_ms

    def ack(self, heartbeat_frame: str, *, now_ms: int) -> str:
        session_id, sequence = self._parse_heartbeat(heartbeat_frame)
        now_ms = _integer(
            now_ms,
            "now_ms",
            minimum=0,
            maximum=2**63 - 1,
        )
        freshness_deadline = (
            None
            if self._latest_sent_ms is None
            else _deadline_after(self._latest_sent_ms, self.lease_ms)
        )
        if (
            self._latest_sent_ms is None
            or freshness_deadline is None
            or sequence != self._latest_sequence
            or sequence <= self._last_acked_sequence
            or now_ms < self._latest_sent_ms
            or now_ms >= freshness_deadline
        ):
            raise ProtocolError("heartbeat is not the fresh latest transmission")
        self._last_acked_sequence = sequence
        self._lease_expires_ms = _deadline_after(now_ms, self.lease_ms)
        body = json.dumps(
            {"session_id": session_id, "sequence": sequence},
            separators=(",", ":"),
        )
        return f"FOF_LIVE_ACK:{body}"

    def is_confirmed(self, *, now_ms: int) -> bool:
        now_ms = _integer(
            now_ms,
            "now_ms",
            minimum=0,
            maximum=2**63 - 1,
        )
        return self._lease_expires_ms is not None and now_ms < self._lease_expires_ms

    def recovery_ap_running(
        self,
        *,
        now_ms: int,
        wifi_configured: bool,
        wifi_connected: bool,
        wifi_join_failed: bool,
    ) -> bool:
        eligible = not _boolean(wifi_configured, "wifi_configured") or _boolean(
            wifi_join_failed, "wifi_join_failed"
        )
        return (
            eligible
            and not _boolean(wifi_connected, "wifi_connected")
            and not self.is_confirmed(now_ms=now_ms)
        )


@dataclass(frozen=True)
class LiveStartAttempt:
    """Deterministic model tag; ``generation`` is not present on the wire."""

    generation: int
    frame: str


class LiveHandshake:
    """Contract model for firmware-synchronized LIVE_START replacement.

    Real hosts cannot recover this generation from LIVE_READY content. The
    model proves the firmware requirement that a new START invalidates queued
    READY/heartbeat controls from the preceding generation.
    """

    def __init__(self) -> None:
        self._generation = 0
        self._session: LiveSession | None = None

    def start(self) -> LiveStartAttempt:
        self._generation += 1
        self._session = None
        return LiveStartAttempt(self._generation, build_live_start())

    def accept_ready(
        self,
        attempt: LiveStartAttempt,
        ready_frame: str,
    ) -> LiveSession:
        if (
            not isinstance(attempt, LiveStartAttempt)
            or attempt.generation != self._generation
            or self._session is not None
        ):
            raise ProtocolError("LIVE_READY belongs to a stale start generation")
        session = LiveSession.from_ready(ready_frame)
        self._session = session
        return session


@dataclass(frozen=True)
class Detection:
    id: str
    manufacturer: str
    badge_label: str
    badge_class: str
    badge_entity_key: str
    source: int
    confidence: float
    threat_score: int
    rssi: int


def parse_detection(frame: str) -> Detection:
    payload = _parse_json_frame(
        frame,
        "FOF_DET:",
        maximum_bytes=DETECTION_MAX_BYTES,
    )
    _exact_keys(payload, _DETECTION_KEYS)
    return Detection(
        id=_text(payload["id"], "id"),
        manufacturer=_text(payload["manufacturer"], "manufacturer"),
        badge_label=_text(payload["badge_label"], "badge_label"),
        badge_class=_text(payload["badge_class"], "badge_class"),
        badge_entity_key=_text(
            payload["badge_entity_key"], "badge_entity_key", nonempty=True
        ),
        source=_integer(payload["source"], "source", minimum=0, maximum=255),
        confidence=_number(payload["confidence"], "confidence"),
        threat_score=_integer(
            payload["threat_score"],
            "threat_score",
            minimum=0,
            maximum=100,
        ),
        rssi=_integer(payload["rssi"], "rssi", minimum=-128, maximum=127),
    )


def _validate_config_set(update: Mapping[str, Any]) -> None:
    keys = frozenset(update)
    extra = keys - _CONFIG_SET_KEYS
    if extra:
        raise ProtocolError(f"unsupported CONFIG_SET fields: {sorted(extra)}")

    if "networks" in update:
        networks = update["networks"]
        if not isinstance(networks, list) or len(networks) > 4:
            raise ProtocolError("networks must contain zero to four entries")
        for index, network in enumerate(networks):
            if not isinstance(network, Mapping):
                raise ProtocolError(f"networks[{index}] must be an object")
            network_keys = frozenset(network)
            if not {"ssid"} <= network_keys <= {"ssid", "password"}:
                raise ProtocolError(f"networks[{index}] has an invalid shape")
            ssid = _text(network["ssid"], f"networks[{index}].ssid", nonempty=True)
            if len(ssid.encode("utf-8")) > 32:
                raise ProtocolError("SSID exceeds 32 bytes")
            if "password" in network:
                password = _text(
                    network["password"], f"networks[{index}].password"
                )
                if len(password.encode("utf-8")) > 64:
                    raise ProtocolError("Wi-Fi password exceeds 64 bytes")

    string_limits = {
        "backend_url": 191,
        "display_name": 64,
        "ap_password": 63,
    }
    for key, maximum in string_limits.items():
        if key in update:
            value = _text(update[key], key, nonempty=True)
            if len(value.encode("utf-8")) > maximum:
                raise ProtocolError(f"{key} exceeds {maximum} bytes")
    if "ap_password" in update and len(update["ap_password"].encode("utf-8")) < 8:
        raise ProtocolError("ap_password must be at least 8 bytes")

    for key in (
        "auto_update_enabled",
        "confirm_auto_update",
        "has_location",
    ):
        if key in update:
            _boolean(update[key], key)
    for key in ("latitude", "longitude", "altitude_m"):
        if key in update:
            _number(update[key], key)
    if "latitude" in update and not -90.0 <= float(update["latitude"]) <= 90.0:
        raise ProtocolError("latitude is out of range")
    if "longitude" in update and not -180.0 <= float(update["longitude"]) <= 180.0:
        raise ProtocolError("longitude is out of range")


def build_config_set(
    device: VerifiedLiteDevice | None,
    update: Mapping[str, Any],
) -> str:
    if (
        not isinstance(device, VerifiedLiteDevice)
        or getattr(device, "_proof", None) is not _VERIFIED_DEVICE_PROOF
        or not device.mutation_enabled
    ):
        raise ProtocolError("CONFIG_SET requires a verified Lite handshake")
    if not isinstance(update, Mapping):
        raise ProtocolError("CONFIG_SET update must be an object")
    _validate_config_set(update)
    try:
        payload = json.dumps(
            dict(update),
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        )
    except (TypeError, ValueError) as error:
        raise ProtocolError("CONFIG_SET is not JSON serializable") from error
    frame = f"FOF_CONFIG_SET:{payload}"
    if len(frame.encode("utf-8")) > COMMAND_MAX_BYTES:
        raise ProtocolError("CONFIG_SET exceeds 2047 bytes")
    return frame


@dataclass(frozen=True)
class RedactedNetwork:
    ssid: str
    password_set: bool


@dataclass(frozen=True)
class RedactedConfig:
    schema_version: int
    generation: int
    networks: tuple[RedactedNetwork, ...]
    backend_url: str
    device_id: str
    display_name: str
    ap_password_set: bool
    auto_update_enabled: bool
    has_location: bool
    latitude: float | None
    longitude: float | None
    altitude_m: float | None


@dataclass(frozen=True)
class ConfigResult:
    committed: bool
    reconnect: bool
    generation: int
    reason: str | None


@dataclass
class ConfigTransaction:
    """Deterministic host view of one canonical config generation."""

    generation: int

    def __post_init__(self) -> None:
        self.generation = _integer(
            self.generation,
            "generation",
            minimum=1,
            maximum=2**32 - 1,
        )

    def apply_response(self, frame: str) -> ConfigResult:
        line = _clean_line(frame, maximum_bytes=STATUS_MAX_BYTES)
        if line.startswith("FOF_CONFIG_OK:"):
            payload = _parse_json_frame(
                line,
                "FOF_CONFIG_OK:",
                maximum_bytes=STATUS_MAX_BYTES,
            )
            _exact_keys(payload, frozenset({"generation", "reconnect"}))
            generation = _integer(
                payload["generation"],
                "generation",
                minimum=1,
                maximum=2**32 - 1,
            )
            if self.generation == 2**32 - 1 or generation != self.generation + 1:
                raise ProtocolError("CONFIG_OK generation is not the next value")
            reconnect = _boolean(payload["reconnect"], "reconnect")
            self.generation = generation
            return ConfigResult(True, reconnect, generation, None)
        if line.startswith("FOF_CONFIG_ERROR:"):
            payload = _parse_json_frame(
                line,
                "FOF_CONFIG_ERROR:",
                maximum_bytes=STATUS_MAX_BYTES,
            )
            _exact_keys(payload, frozenset({"reason"}))
            reason = _text(payload["reason"], "reason", nonempty=True)
            return ConfigResult(False, False, self.generation, reason)
        raise ProtocolError("expected FOF_CONFIG_OK or FOF_CONFIG_ERROR")


def parse_config(frame: str) -> RedactedConfig:
    payload = _parse_json_frame(
        frame,
        "FOF_CONFIG:",
        maximum_bytes=STATUS_MAX_BYTES,
    )
    _exact_keys(payload, _CONFIG_GET_KEYS)
    networks_value = payload["networks"]
    if not isinstance(networks_value, list) or len(networks_value) > 4:
        raise ProtocolError("networks must be an array of at most four entries")
    networks: list[RedactedNetwork] = []
    for index, network in enumerate(networks_value):
        if not isinstance(network, dict):
            raise ProtocolError(f"networks[{index}] must be an object")
        _exact_keys(network, frozenset({"ssid", "password_set"}))
        networks.append(
            RedactedNetwork(
                ssid=_text(network["ssid"], f"networks[{index}].ssid", nonempty=True),
                password_set=_boolean(
                    network["password_set"], f"networks[{index}].password_set"
                ),
            )
        )

    has_location = _boolean(payload["has_location"], "has_location")
    location_values: list[float | None] = []
    for key in ("latitude", "longitude", "altitude_m"):
        value = payload[key]
        if value is None:
            location_values.append(None)
        else:
            location_values.append(_number(value, key))
    if has_location and any(value is None for value in location_values):
        raise ProtocolError("configured location must include all coordinates")
    if not has_location and any(value is not None for value in location_values):
        raise ProtocolError("unconfigured location must be fully redacted as null")

    return RedactedConfig(
        schema_version=_integer(
            payload["schema_version"],
            "schema_version",
            minimum=1,
            maximum=65535,
        ),
        generation=_integer(
            payload["generation"],
            "generation",
            minimum=1,
            maximum=2**32 - 1,
        ),
        networks=tuple(networks),
        backend_url=_text(payload["backend_url"], "backend_url", nonempty=True),
        device_id=_text(payload["device_id"], "device_id", nonempty=True),
        display_name=_text(payload["display_name"], "display_name"),
        ap_password_set=_boolean(payload["ap_password_set"], "ap_password_set"),
        auto_update_enabled=_boolean(
            payload["auto_update_enabled"], "auto_update_enabled"
        ),
        has_location=has_location,
        latitude=location_values[0],
        longitude=location_values[1],
        altitude_m=location_values[2],
    )
