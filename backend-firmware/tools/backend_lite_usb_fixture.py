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


@dataclass(frozen=True)
class VerifiedLiteDevice:
    """Handshake proof required by mutation-building helpers."""

    version: str
    capabilities: frozenset[str]

    @property
    def identity(self) -> tuple[str, str, str, str]:
        return LITE_IDENTITY

    @property
    def mutation_enabled(self) -> bool:
        return "usb_config" in self.capabilities

    @property
    def screen_supported(self) -> bool:
        return False


def verify_lite_handshake(
    pong_frame: str,
    status_frame: str,
) -> VerifiedLiteDevice:
    """Require the exact Lite tuple and matching PONG/STATUS version."""

    pong_version = _parse_pong(pong_frame)
    status = _parse_json_frame(
        status_frame,
        "FOF_STATUS:",
        maximum_bytes=STATUS_MAX_BYTES,
    )
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
    return VerifiedLiteDevice(version=version, capabilities=capabilities)


@dataclass
class LiveSession:
    session_id: str
    heartbeat_ms: int
    lease_ms: int
    _last_acked_sequence: int = 0

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

    def ack(self, heartbeat_frame: str) -> str:
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
        if sequence <= self._last_acked_sequence:
            raise ProtocolError("heartbeat sequence is stale or replayed")
        self._last_acked_sequence = sequence
        body = json.dumps(
            {"session_id": session_id, "sequence": sequence},
            separators=(",", ":"),
        )
        return f"FOF_LIVE_ACK:{body}"


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
        if not isinstance(networks, list) or not 1 <= len(networks) <= 4:
            raise ProtocolError("networks must contain one to four entries")
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
    if not isinstance(device, VerifiedLiteDevice) or not device.mutation_enabled:
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
