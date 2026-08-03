"""Exact, allowlisted control commands for one Friend or Foe badge."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
from math import isfinite
from types import MappingProxyType
from typing import Mapping
from urllib.parse import urlsplit


NAV_ACTIONS = frozenset({"next", "detail", "page", "back"})
THEME_PALETTES = frozenset({"field", "night", "neon", "mono"})
THEME_BACKGROUNDS = frozenset({"dark", "dim", "scanline"})
THEME_ACCENTS = frozenset(
    {"drone", "meta", "tracker", "flock", "wifi_attack", "clear"}
)
POLICY_CLASSES = (
    "drone",
    "meta",
    "tracker",
    "wifi_attack",
    "skimmer",
    "camera",
    "flock",
    "lock",
    "hid",
    "beacon",
    "event_badge",
    "auracast",
    "scanner_status",
)
POLICY_LANES = frozenset({"off", "lower", "top", "both"})
POLICY_PROXIMITIES = frozenset({"present", "near", "close"})


class ControlValidationError(ValueError):
    """A requested safe control command does not meet the exact contract."""


LITE_CONFIG_FIELDS = frozenset({
    "networks", "backend_url", "display_name", "ap_password",
    "auto_update_enabled", "confirm_auto_update", "has_location",
    "latitude", "longitude", "altitude_m",
})


@dataclass(frozen=True)
class BadgeControlCommand:
    """A sealed approved command and the precise acknowledgement it expects."""

    payload: Mapping[str, object]
    expected_message: str
    _wire: bytes = field(init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        payload = _validate_control_command(self.payload, self.expected_message)
        wire = _encode_wire(payload)
        object.__setattr__(self, "payload", _freeze_mapping(payload))
        object.__setattr__(self, "_wire", wire)

    def to_wire(self) -> bytes:
        """Return the newline-terminated, firmware control record."""

        return self._wire


def build_display_nav(action: str) -> BadgeControlCommand:
    """Build the only transient display-navigation command family."""

    if type(action) is not str or action not in NAV_ACTIONS:
        raise ControlValidationError("invalid display navigation action")
    return BadgeControlCommand(
        payload={"cmd": "display_nav", "action": action},
        expected_message="display nav updated",
    )


def build_theme(payload: object) -> BadgeControlCommand:
    """Validate and normalize a complete version-1 badge theme."""

    theme = _validate_theme(payload)
    return BadgeControlCommand(
        payload={"cmd": "badge_theme", "persist": True, "theme": theme},
        expected_message="badge theme updated",
    )


def build_theme_reset() -> BadgeControlCommand:
    """Build the persistent firmware theme reset command."""

    return BadgeControlCommand(
        payload={"cmd": "badge_theme_reset", "persist": True},
        expected_message="badge theme reset",
    )


def build_display_policy(payload: object) -> BadgeControlCommand:
    """Validate and normalize a complete version-1 display policy."""

    policy = _validate_policy(payload)
    return BadgeControlCommand(
        payload={"cmd": "badge_display_policy", "persist": True, "policy": policy},
        expected_message="display policy updated",
    )


def build_display_policy_reset() -> BadgeControlCommand:
    """Build the persistent firmware display-policy reset command."""

    return BadgeControlCommand(
        payload={"cmd": "badge_display_policy_reset", "persist": True},
        expected_message="display policy reset",
    )


def build_lite_config_set(payload: object) -> bytes:
    """Validate and encode one atomic Backend Badge Lite configuration commit."""

    if type(payload) is not dict or not payload or not set(payload) <= LITE_CONFIG_FIELDS:
        raise ControlValidationError("invalid Lite configuration fields")
    normalized: dict[str, object] = {}
    if "networks" in payload:
        networks = payload["networks"]
        if type(networks) is not list or len(networks) > 4:
            raise ControlValidationError("Lite configuration accepts zero to four networks")
        normalized_networks: list[dict[str, str]] = []
        seen_ssids: set[str] = set()
        for network in networks:
            if type(network) is not dict or not set(network) <= {"ssid", "password"} or "ssid" not in network:
                raise ControlValidationError("invalid Lite network configuration")
            ssid = network["ssid"]
            if type(ssid) is not str or not ssid or _has_physical_control(ssid):
                raise ControlValidationError("invalid Lite network SSID")
            if len(ssid.encode("utf-8")) > 32 or ssid in seen_ssids:
                raise ControlValidationError("Lite network SSID is too long or duplicated")
            seen_ssids.add(ssid)
            normalized_network: dict[str, str] = {"ssid": ssid}
            if "password" in network:
                password = network["password"]
                if type(password) is not str or _has_physical_control(password):
                    raise ControlValidationError("invalid Lite network password")
                if len(password.encode("utf-8")) > 63:
                    raise ControlValidationError("Lite network password is too long")
                normalized_network["password"] = password
            normalized_networks.append(normalized_network)
        normalized["networks"] = normalized_networks
    if "backend_url" in payload:
        backend_url = payload["backend_url"]
        if type(backend_url) is not str or _has_physical_control(backend_url):
            raise ControlValidationError("invalid Lite backend URL")
        try:
            parsed_url = urlsplit(backend_url)
            hostname = parsed_url.hostname
            port = parsed_url.port
        except ValueError as error:
            raise ControlValidationError("invalid Lite backend URL") from error
        if (
            parsed_url.scheme != "http"
            or not hostname
            or parsed_url.username is not None
            or parsed_url.password is not None
            or parsed_url.fragment
            or port is not None and not 1 <= port <= 65_535
        ):
            raise ControlValidationError("Lite backend URL must be an HTTP URL")
        normalized["backend_url"] = backend_url
    if "display_name" in payload:
        display_name = payload["display_name"]
        if type(display_name) is not str or _has_physical_control(display_name):
            raise ControlValidationError("invalid Lite display name")
        if len(display_name.encode("utf-8")) > 64:
            raise ControlValidationError("Lite display name is too long")
        normalized["display_name"] = display_name
    if "ap_password" in payload:
        ap_password = payload["ap_password"]
        if type(ap_password) is not str or _has_physical_control(ap_password):
            raise ControlValidationError("invalid Lite AP password")
        ap_password_length = len(ap_password.encode("utf-8"))
        if not 8 <= ap_password_length <= 63:
            raise ControlValidationError("Lite AP password must be 8 to 63 bytes")
        normalized["ap_password"] = ap_password
    if "auto_update_enabled" in payload:
        auto_update = payload["auto_update_enabled"]
        if type(auto_update) is not bool:
            raise ControlValidationError("Lite auto-update flag must be boolean")
        normalized["auto_update_enabled"] = auto_update
        if auto_update:
            if payload.get("confirm_auto_update") is not True:
                raise ControlValidationError("enabling Lite auto-update requires confirmation")
            normalized["confirm_auto_update"] = True
        elif "confirm_auto_update" in payload:
            if type(payload["confirm_auto_update"]) is not bool:
                raise ControlValidationError("Lite auto-update confirmation must be boolean")
            normalized["confirm_auto_update"] = payload["confirm_auto_update"]
    elif "confirm_auto_update" in payload:
        raise ControlValidationError("Lite auto-update confirmation requires the setting")
    if "has_location" in payload:
        has_location = payload["has_location"]
        if type(has_location) is not bool:
            raise ControlValidationError("Lite location flag must be boolean")
        normalized["has_location"] = has_location
        location_keys = ("latitude", "longitude", "altitude_m")
        if has_location:
            if any(key not in payload for key in location_keys):
                raise ControlValidationError("enabled Lite location requires all coordinates")
            latitude, longitude, altitude = (payload[key] for key in location_keys)
            if any(type(value) not in {int, float} or not isfinite(value)
                   for value in (latitude, longitude, altitude)):
                raise ControlValidationError("Lite location values must be finite")
            if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
                raise ControlValidationError("Lite coordinates are outside valid ranges")
            normalized.update({
                "latitude": latitude,
                "longitude": longitude,
                "altitude_m": altitude,
            })
        elif any(key in payload for key in location_keys):
            raise ControlValidationError("disabled Lite location cannot include coordinates")
    elif any(key in payload for key in ("latitude", "longitude", "altitude_m")):
        raise ControlValidationError("Lite coordinates require has_location")
    try:
        encoded = json.dumps(
            normalized,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError) as error:
        raise ControlValidationError("Lite configuration is not finite JSON") from error
    wire = b"FOF_CONFIG_SET:" + encoded
    if len(wire) > 2047:
        raise ControlValidationError("Lite configuration command exceeds 2047 bytes")
    return wire + b"\n"


def _has_physical_control(value: str) -> bool:
    return any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)


def _validate_control_command(payload: object, expected_message: object) -> dict[str, object]:
    """Fail closed unless construction matches one approved command exactly."""

    if type(payload) is not dict or type(expected_message) is not str:
        raise ControlValidationError("control command is not an approved payload")
    command = payload.get("cmd")
    if command == "display_nav":
        _require_exact_keys(payload, {"cmd", "action"})
        action = payload["action"]
        if type(action) is not str or action not in NAV_ACTIONS:
            raise ControlValidationError("invalid display navigation action")
        _require_expected_message(expected_message, "display nav updated")
        return {"cmd": "display_nav", "action": action}
    if command == "badge_theme":
        _require_exact_keys(payload, {"cmd", "persist", "theme"})
        if payload["persist"] is not True:
            raise ControlValidationError("badge theme must persist")
        _require_expected_message(expected_message, "badge theme updated")
        return {"cmd": "badge_theme", "persist": True, "theme": _validate_theme(payload["theme"])}
    if command == "badge_theme_reset":
        _require_exact_keys(payload, {"cmd", "persist"})
        if payload["persist"] is not True:
            raise ControlValidationError("badge theme reset must persist")
        _require_expected_message(expected_message, "badge theme reset")
        return {"cmd": "badge_theme_reset", "persist": True}
    if command == "badge_display_policy":
        _require_exact_keys(payload, {"cmd", "persist", "policy"})
        if payload["persist"] is not True:
            raise ControlValidationError("display policy must persist")
        _require_expected_message(expected_message, "display policy updated")
        return {
            "cmd": "badge_display_policy",
            "persist": True,
            "policy": _validate_policy(payload["policy"]),
        }
    if command == "badge_display_policy_reset":
        _require_exact_keys(payload, {"cmd", "persist"})
        if payload["persist"] is not True:
            raise ControlValidationError("display policy reset must persist")
        _require_expected_message(expected_message, "display policy reset")
        return {"cmd": "badge_display_policy_reset", "persist": True}
    raise ControlValidationError("control command is not approved")


def _require_expected_message(actual: str, expected: str) -> None:
    if actual != expected:
        raise ControlValidationError("control acknowledgement does not match its payload")


def _encode_wire(payload: dict[str, object]) -> bytes:
    try:
        encoded = json.dumps(
            payload,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError) as error:
        raise ControlValidationError("control payload is not finite JSON") from error
    wire = b"FOF_CTL:" + encoded
    if len(wire) > 2047:
        raise ControlValidationError("control command exceeds 2047 bytes")
    return wire + b"\n"


def _freeze_mapping(payload: dict[str, object]) -> Mapping[str, object]:
    return MappingProxyType(
        {
            key: _freeze_mapping(value) if type(value) is dict else value
            for key, value in payload.items()
        }
    )


def _validate_theme(payload: object) -> dict[str, object]:
    if type(payload) is not dict:
        raise ControlValidationError("theme must be an object")
    _require_exact_keys(payload, {"version", "palette", "background", "brightness", "accents"})
    version = payload["version"]
    palette = payload["palette"]
    background = payload["background"]
    brightness = payload["brightness"]
    accents = payload["accents"]
    if type(version) is not int or version != 1:
        raise ControlValidationError("theme version must be 1")
    if type(palette) is not str or palette not in THEME_PALETTES:
        raise ControlValidationError("invalid theme palette")
    if type(background) is not str or background not in THEME_BACKGROUNDS:
        raise ControlValidationError("invalid theme background")
    if type(brightness) is not int or not 25 <= brightness <= 100:
        raise ControlValidationError("theme brightness must be an integer from 25 through 100")
    if type(accents) is not dict:
        raise ControlValidationError("theme accents must be an object")
    _require_exact_keys(accents, THEME_ACCENTS)
    normalized_accents: dict[str, int] = {}
    for accent in ("drone", "meta", "tracker", "flock", "wifi_attack", "clear"):
        color = accents[accent]
        if type(color) is not int or not 0 <= color <= 65535:
            raise ControlValidationError("theme accents must be RGB565 integers")
        normalized_accents[accent] = color
    return {
        "version": version,
        "palette": palette,
        "background": background,
        "brightness": brightness,
        "accents": normalized_accents,
    }


def _validate_policy(payload: object) -> dict[str, object]:
    if type(payload) is not dict:
        raise ControlValidationError("display policy must be an object")
    _require_exact_keys(payload, {"version", "classes"})
    version = payload["version"]
    classes = payload["classes"]
    if type(version) is not int or version != 1:
        raise ControlValidationError("display policy version must be 1")
    if type(classes) is not dict:
        raise ControlValidationError("display policy classes must be an object")
    _require_exact_keys(classes, set(POLICY_CLASSES))
    normalized_classes: dict[str, dict[str, object]] = {}
    for class_name in POLICY_CLASSES:
        class_policy = classes[class_name]
        if type(class_policy) is not dict:
            raise ControlValidationError("display policy class must be an object")
        _require_exact_keys(class_policy, {"enabled", "lane", "min_proximity", "priority"})
        enabled = class_policy["enabled"]
        lane = class_policy["lane"]
        proximity = class_policy["min_proximity"]
        priority = class_policy["priority"]
        if type(enabled) is not bool:
            raise ControlValidationError("display policy enabled must be boolean")
        if type(lane) is not str or lane not in POLICY_LANES:
            raise ControlValidationError("invalid display policy lane")
        if type(proximity) is not str or proximity not in POLICY_PROXIMITIES:
            raise ControlValidationError("invalid display policy proximity")
        if type(priority) is not int or not 0 <= priority <= 100:
            raise ControlValidationError("display policy priority must be an integer from 0 through 100")
        if (not enabled and lane != "off") or (enabled and lane == "off"):
            raise ControlValidationError("display policy enabled state and lane disagree")
        normalized_classes[class_name] = {
            "enabled": enabled,
            "lane": lane,
            "min_proximity": proximity,
            "priority": priority,
        }
    return {"version": version, "classes": normalized_classes}


def _require_exact_keys(payload: dict[object, object], expected: set[str] | frozenset[str]) -> None:
    if set(payload) != expected:
        raise ControlValidationError("control object has missing or unknown fields")
