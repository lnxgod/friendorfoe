"""Immutable protocol models for the New Dash USB boundary."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from math import isfinite
from types import MappingProxyType
from typing import Any

SOURCE_NAMES = {
    0: "ble_rid",
    1: "wifi_ssid",
    2: "wifi_dji_ie",
    3: "wifi_rid",
    4: "wifi_oui",
    5: "wifi_probe",
    6: "ble_fingerprint",
    7: "wifi_assoc",
    8: "wifi_inventory",
}
REMOTE_ID_SOURCES = frozenset({"ble_rid", "wifi_rid"})
SQLITE_INTEGER_MIN = -(2 ** 63)
SQLITE_INTEGER_MAX = (2 ** 63) - 1
_MISSING = object()


def _optional_text(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, (str, int, float)) and not isinstance(value, bool):
        return str(value)
    return None


def _optional_number(value: Any) -> float | None:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        converted = float(value)
        if isfinite(converted):
            return converted
    return None


def _optional_integer(value: Any) -> int | None:
    if (
        isinstance(value, int)
        and not isinstance(value, bool)
        and SQLITE_INTEGER_MIN <= value <= SQLITE_INTEGER_MAX
    ):
        return value
    return None


def _optional_numeric_integer(value: Any) -> int | None:
    if isinstance(value, int) and not isinstance(value, bool):
        converted = value
    elif isinstance(value, float) and isfinite(value):
        converted = int(value)
    else:
        return None
    if SQLITE_INTEGER_MIN <= converted <= SQLITE_INTEGER_MAX:
        return converted
    return None


def _optional_bool(value: Any) -> bool | None:
    return value if isinstance(value, bool) else None


def _coordinate_pair(latitude: Any, longitude: Any) -> tuple[float | None, float | None]:
    normalized_latitude = _optional_number(latitude)
    normalized_longitude = _optional_number(longitude)
    if (
        normalized_latitude is None
        or normalized_longitude is None
        or not -90 <= normalized_latitude <= 90
        or not -180 <= normalized_longitude <= 180
    ):
        return None, None
    return normalized_latitude, normalized_longitude


def _freeze_json(value: Any) -> Any:
    if isinstance(value, dict):
        return MappingProxyType({key: _freeze_json(item) for key, item in value.items()})
    if isinstance(value, list):
        return tuple(_freeze_json(item) for item in value)
    return value


def _json_safe(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_json_safe(item) for item in value]
    if isinstance(value, float) and not isfinite(value):
        return None
    return value


@dataclass(frozen=True, slots=True)
class DetectionEvent:
    """A normalized event emitted by the badge's ``FOF_DET`` stream."""

    detection_id: str | None
    manufacturer: str | None
    badge_label: str | None
    badge_class: str | None
    badge_entity_key: str | None
    source_id: int | None
    source: str
    confidence: float | None
    threat_score: float | None
    rssi: int | None

    def __post_init__(self) -> None:
        object.__setattr__(self, "rssi", _optional_numeric_integer(self.rssi))

    @classmethod
    def from_payload(cls, payload: object) -> "DetectionEvent":
        if not isinstance(payload, dict):
            raise ValueError("detection payload must be an object")

        source_value = payload.get("source")
        source_id = _optional_integer(source_value)
        if source_id is not None:
            source = SOURCE_NAMES.get(source_id, f"unknown_{source_id}")
        else:
            source = "unknown"

        return cls(
            detection_id=_optional_text(payload.get("id")),
            manufacturer=_optional_text(payload.get("manufacturer")),
            badge_label=_optional_text(payload.get("badge_label")),
            badge_class=_optional_text(payload.get("badge_class")),
            badge_entity_key=_optional_text(payload.get("badge_entity_key")),
            source_id=source_id,
            source=source,
            confidence=_optional_number(payload.get("confidence")),
            threat_score=_optional_number(payload.get("threat_score")),
            rssi=_optional_numeric_integer(payload.get("rssi")),
        )

    @property
    def stable_key(self) -> str:
        return self.badge_entity_key or f"{self.source}:{self.detection_id}"

    def to_dict(self) -> dict[str, object]:
        return {
            "detection_id": self.detection_id,
            "manufacturer": self.manufacturer,
            "badge_label": self.badge_label,
            "badge_class": self.badge_class,
            "badge_entity_key": self.badge_entity_key,
            "source_id": self.source_id,
            "source": self.source,
            "confidence": self.confidence,
            "threat_score": self.threat_score,
            "rssi": self.rssi,
        }


@dataclass(frozen=True, slots=True)
class BadgeEntity:
    """A normalized active entity from a status snapshot."""

    label: str | None
    detail: str | None
    evidence: str | None
    threat_class: str | None
    category: str | None
    code: str | None
    display_id: str | None
    source_id: int | None
    source: str
    score: float | None
    confidence_pct: float | None
    last_seen_seconds: float | None
    rssi: int | None
    best_rssi: int | None
    events: int | None
    seen_count: int | None
    stale: bool | None
    latitude: float | None
    longitude: float | None
    altitude_m: float | None
    operator_latitude: float | None
    operator_longitude: float | None
    operator_id: str | None
    ssid: str | None
    bssid: str | None
    manufacturer: str | None
    extras: Mapping[str, Any]

    @classmethod
    def from_payload(cls, payload: object) -> "BadgeEntity":
        if not isinstance(payload, dict):
            raise ValueError("status entity must be an object")
        source_id = _optional_integer(payload.get("source_id"))
        source_value = payload.get("source")
        if isinstance(source_value, str):
            source = source_value
        elif source_id is not None:
            source = SOURCE_NAMES.get(source_id, f"unknown_{source_id}")
        else:
            source = "unknown"
        latitude, longitude = _coordinate_pair(payload.get("lat"), payload.get("lon"))
        operator_latitude, operator_longitude = _coordinate_pair(
            payload.get("operator_lat"), payload.get("operator_lon")
        )
        known_keys = {
            "label", "detail", "evidence", "class", "category", "code", "display_id",
            "source", "source_id", "score", "confidence_pct", "last_seen_s", "rssi",
            "best_rssi", "events", "seen_count", "stale", "lat", "lon", "altitude_m",
            "operator_lat", "operator_lon", "operator_id", "ssid", "bssid", "manufacturer",
        }
        return cls(
            label=_optional_text(payload.get("label")),
            detail=_optional_text(payload.get("detail")),
            evidence=_optional_text(payload.get("evidence")),
            threat_class=_optional_text(payload.get("class")),
            category=_optional_text(payload.get("category")),
            code=_optional_text(payload.get("code")),
            display_id=_optional_text(payload.get("display_id")),
            source_id=source_id,
            source=source,
            score=_optional_number(payload.get("score")),
            confidence_pct=_optional_number(payload.get("confidence_pct")),
            last_seen_seconds=_optional_number(payload.get("last_seen_s")),
            rssi=_optional_integer(payload.get("rssi")),
            best_rssi=_optional_integer(payload.get("best_rssi")),
            events=_optional_integer(payload.get("events")),
            seen_count=_optional_integer(payload.get("seen_count")),
            stale=_optional_bool(payload.get("stale")),
            latitude=latitude,
            longitude=longitude,
            altitude_m=_optional_number(payload.get("altitude_m")),
            operator_latitude=operator_latitude,
            operator_longitude=operator_longitude,
            operator_id=_optional_text(payload.get("operator_id")),
            ssid=_optional_text(payload.get("ssid")),
            bssid=_optional_text(payload.get("bssid")),
            manufacturer=_optional_text(payload.get("manufacturer")),
            extras=_freeze_json({key: value for key, value in payload.items() if key not in known_keys}),
        )

    @property
    def stable_key(self) -> str:
        identity = self.display_id or self.bssid or self.ssid or self.label or "unknown"
        return f"{self.source}:{identity}"

    @property
    def is_remote_id(self) -> bool:
        return self.source in REMOTE_ID_SOURCES

    @property
    def has_position(self) -> bool:
        return (
            self.latitude is not None
            and self.longitude is not None
            and isfinite(self.latitude)
            and isfinite(self.longitude)
            and -90 <= self.latitude <= 90
            and -180 <= self.longitude <= 180
        )

    def to_dict(self) -> dict[str, object]:
        return _json_safe({
            "label": self.label,
            "detail": self.detail,
            "evidence": self.evidence,
            "class": self.threat_class,
            "category": self.category,
            "code": self.code,
            "display_id": self.display_id,
            "source_id": self.source_id,
            "source": self.source,
            "score": self.score,
            "confidence_pct": self.confidence_pct,
            "last_seen_s": self.last_seen_seconds,
            "rssi": self.rssi,
            "best_rssi": self.best_rssi,
            "events": self.events,
            "seen_count": self.seen_count,
            "stale": self.stale,
            "lat": self.latitude,
            "lon": self.longitude,
            "altitude_m": self.altitude_m,
            "operator_lat": self.operator_latitude,
            "operator_lon": self.operator_longitude,
            "operator_id": self.operator_id,
            "ssid": self.ssid,
            "bssid": self.bssid,
            "manufacturer": self.manufacturer,
            "extras": self.extras,
        })


@dataclass(frozen=True, slots=True)
class BadgeStatus:
    """The complete, validated status snapshot supplied by the firmware."""

    version: str
    uptime_seconds: float | None
    mode: str | None
    mode_label: str | None
    threat_score: float | None
    counts: Mapping[str, Any] | None
    safe_mode: bool | None
    recovery_mode: str | None
    entities: tuple[BadgeEntity, ...] | None
    scanners: tuple[Mapping[str, Any], ...] | None
    raw: Mapping[str, Any]

    @classmethod
    def from_payload(cls, payload: object) -> "BadgeStatus":
        if not isinstance(payload, dict):
            raise ValueError("status payload must be an object")
        version = payload.get("version")
        if not isinstance(version, str) or not version.strip():
            raise ValueError("status version must be a nonempty string")
        uptime = payload.get("uptime_s", _MISSING)
        if uptime is _MISSING:
            normalized_uptime = None
        elif (
            isinstance(uptime, bool)
            or not isinstance(uptime, (int, float))
            or not isfinite(uptime)
        ):
            raise ValueError("status uptime_s must be finite numeric when present")
        else:
            normalized_uptime = float(uptime)
        entity_value = payload.get("entities", _MISSING)
        scanner_value = payload.get("scanners", _MISSING)
        if entity_value is not _MISSING and not isinstance(entity_value, list):
            raise ValueError("status entities and scanners must be arrays")
        if scanner_value is not _MISSING and not isinstance(scanner_value, list):
            raise ValueError("status entities and scanners must be arrays")
        entity_payloads = None if entity_value is _MISSING else entity_value
        scanner_payloads = None if scanner_value is _MISSING else scanner_value
        if entity_payloads is not None and not all(
            isinstance(entity, dict) for entity in entity_payloads
        ):
            raise ValueError("status entities must contain objects")
        if scanner_payloads is not None and not all(
            isinstance(scanner, dict) for scanner in scanner_payloads
        ):
            raise ValueError("status scanners must contain objects")
        counts = payload.get("counts")
        return cls(
            version=version,
            uptime_seconds=normalized_uptime,
            mode=_optional_text(payload.get("mode")),
            mode_label=_optional_text(payload.get("mode_label")),
            threat_score=_optional_number(payload.get("threat_score")),
            counts=_freeze_json(counts) if isinstance(counts, dict) else None,
            safe_mode=_optional_bool(payload.get("safe_mode")),
            recovery_mode=_optional_text(payload.get("recovery_mode")),
            entities=(
                tuple(BadgeEntity.from_payload(entity) for entity in entity_payloads)
                if entity_payloads is not None
                else None
            ),
            scanners=(
                tuple(_freeze_json(scanner) for scanner in scanner_payloads)
                if scanner_payloads is not None
                else None
            ),
            raw=_freeze_json(payload),
        )

    def to_dict(self) -> dict[str, object]:
        result = _json_safe(self.raw)
        if self.entities is not None:
            result["entities"] = [entity.to_dict() for entity in self.entities]
        if self.scanners is not None:
            result["scanners"] = _json_safe(self.scanners)
        return result


@dataclass(frozen=True, slots=True)
class LiteLiveReady:
    """A fresh acknowledged-live session offered by Backend Badge Lite."""

    session_id: str
    heartbeat_ms: int
    lease_ms: int

    @classmethod
    def from_payload(cls, payload: object) -> "LiteLiveReady":
        if not isinstance(payload, dict) or set(payload) != {
            "session_id", "heartbeat_ms", "lease_ms"
        }:
            raise ValueError("live ready payload has invalid fields")
        session_id = payload["session_id"]
        heartbeat_ms = payload["heartbeat_ms"]
        lease_ms = payload["lease_ms"]
        if not isinstance(session_id, str) or not session_id or len(session_id) > 128:
            raise ValueError("live ready session_id is invalid")
        if type(heartbeat_ms) is not int or heartbeat_ms != 5_000:
            raise ValueError("live ready heartbeat is incompatible")
        if type(lease_ms) is not int or lease_ms != 15_000:
            raise ValueError("live ready lease is incompatible")
        return cls(session_id, heartbeat_ms, lease_ms)


@dataclass(frozen=True, slots=True)
class LiteLiveHeartbeat:
    """One acknowledgeable heartbeat for the current Lite session."""

    session_id: str
    sequence: int

    @classmethod
    def from_payload(cls, payload: object) -> "LiteLiveHeartbeat":
        if not isinstance(payload, dict) or set(payload) != {"session_id", "sequence"}:
            raise ValueError("live heartbeat payload has invalid fields")
        session_id = payload["session_id"]
        sequence = payload["sequence"]
        if not isinstance(session_id, str) or not session_id or len(session_id) > 128:
            raise ValueError("live heartbeat session_id is invalid")
        if type(sequence) is not int or not 0 <= sequence <= (2 ** 64) - 1:
            raise ValueError("live heartbeat sequence is invalid")
        return cls(session_id, sequence)


@dataclass(frozen=True, slots=True)
class LiteLiveStopped:
    """Confirmation that a Lite acknowledged-live session was stopped."""

    session_id: str

    @classmethod
    def from_payload(cls, payload: object) -> "LiteLiveStopped":
        if not isinstance(payload, dict) or set(payload) != {"session_id"}:
            raise ValueError("live stopped payload has invalid fields")
        session_id = payload["session_id"]
        if not isinstance(session_id, str) or not session_id or len(session_id) > 128:
            raise ValueError("live stopped session_id is invalid")
        return cls(session_id)


@dataclass(frozen=True, slots=True)
class LiteConfiguration:
    """A strictly redacted canonical Backend Badge Lite configuration."""

    generation: int
    values: Mapping[str, Any]

    @classmethod
    def from_payload(cls, payload: object) -> "LiteConfiguration":
        required = {
            "schema_version", "generation", "networks", "backend_url",
            "device_id", "display_name", "ap_password_set",
            "auto_update_enabled", "has_location", "latitude", "longitude",
            "altitude_m",
        }
        if not isinstance(payload, dict) or set(payload) != required:
            raise ValueError("configuration payload has invalid fields")
        forbidden = {"password", "wifi_pass", "wifi_password", "ap_password"}
        stack: list[object] = [payload]
        while stack:
            value = stack.pop()
            if isinstance(value, dict):
                if any(key in forbidden for key in value):
                    raise ValueError("configuration response disclosed a secret")
                stack.extend(value.values())
            elif isinstance(value, list):
                stack.extend(value)
        if payload["schema_version"] != 1:
            raise ValueError("configuration schema is incompatible")
        generation = payload["generation"]
        if type(generation) is not int or not 0 <= generation <= (2 ** 32) - 1:
            raise ValueError("configuration generation is invalid")
        networks = payload["networks"]
        if not isinstance(networks, list) or len(networks) > 4:
            raise ValueError("configuration networks are invalid")
        for network in networks:
            if not isinstance(network, dict) or set(network) != {"ssid", "password_set"}:
                raise ValueError("configuration network is invalid")
            if not isinstance(network["ssid"], str) or not network["ssid"]:
                raise ValueError("configuration network SSID is invalid")
            if type(network["password_set"]) is not bool:
                raise ValueError("configuration password marker is invalid")
        for key in ("backend_url", "device_id", "display_name"):
            if not isinstance(payload[key], str):
                raise ValueError(f"configuration {key} is invalid")
        for key in ("ap_password_set", "auto_update_enabled", "has_location"):
            if type(payload[key]) is not bool:
                raise ValueError(f"configuration {key} is invalid")
        latitude = payload["latitude"]
        longitude = payload["longitude"]
        altitude = payload["altitude_m"]
        if payload["has_location"]:
            if any(type(value) not in {int, float} or not isfinite(value)
                   for value in (latitude, longitude, altitude)):
                raise ValueError("configuration location is invalid")
            if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
                raise ValueError("configuration coordinates are invalid")
        elif any(value is not None for value in (latitude, longitude, altitude)):
            raise ValueError("disabled configuration location must be redacted")
        return cls(generation=generation, values=_freeze_json(payload))

    def to_dict(self) -> dict[str, object]:
        return _json_safe(self.values)


@dataclass(frozen=True, slots=True)
class LiteConfigWriteReply:
    """Atomic Lite configuration commit result."""

    ok: bool
    generation: int | None
    reconnect: bool | None
    reason: str | None

    @classmethod
    def from_payload(cls, payload: object, *, ok: bool) -> "LiteConfigWriteReply":
        if not isinstance(payload, dict):
            raise ValueError("configuration result must be an object")
        if ok:
            if set(payload) != {"generation", "reconnect"}:
                raise ValueError("configuration success has invalid fields")
            generation = payload["generation"]
            reconnect = payload["reconnect"]
            if type(generation) is not int or not 0 <= generation <= (2 ** 32) - 1:
                raise ValueError("configuration generation is invalid")
            if type(reconnect) is not bool:
                raise ValueError("configuration reconnect result is invalid")
            return cls(True, generation, reconnect, None)
        if set(payload) != {"reason"} or not isinstance(payload["reason"], str) or not payload["reason"]:
            raise ValueError("configuration error has invalid fields")
        return cls(False, None, None, payload["reason"][:128])

    def to_dict(self) -> dict[str, object]:
        return {
            "ok": self.ok,
            "generation": self.generation,
            "reconnect": self.reconnect,
            "reason": self.reason,
        }


@dataclass(frozen=True, slots=True)
class ControlReply:
    """A success or error result from an allowlisted badge control."""

    ok: bool
    message: str | None
    error: str | None
    details: Mapping[str, Any]

    @classmethod
    def from_payload(cls, payload: object, *, ok: bool) -> "ControlReply":
        if not isinstance(payload, dict):
            raise ValueError("control reply must be an object")
        return cls(
            ok=ok,
            message=_optional_text(payload.get("message")),
            error=_optional_text(payload.get("error")),
            details=_freeze_json(payload),
        )

    def to_dict(self) -> dict[str, object]:
        result = _json_safe(self.details)
        result.update({"ok": self.ok, "message": self.message, "error": self.error})
        return result


@dataclass(frozen=True, slots=True)
class Observation:
    """A JSON-safe persisted event or Remote ID track observation."""

    row_id: int | None
    kind: str
    received_at: float
    observed_at: float
    stable_key: str
    source_id: int | None
    source: str
    threat_class: str
    category: str
    label: str
    display_id: str
    manufacturer: str
    confidence: float | None
    score: float | None
    rssi: int | None
    events: int | None
    seen_count: int | None
    latitude: float | None
    longitude: float | None
    altitude_m: float | None
    operator_latitude: float | None
    operator_longitude: float | None
    operator_id: str
    extras: Mapping[str, Any]

    def __post_init__(self) -> None:
        object.__setattr__(self, "extras", _freeze_json(dict(self.extras)))

    def to_dict(self) -> dict[str, object]:
        return _json_safe({
            "row_id": self.row_id,
            "kind": self.kind,
            "received_at": self.received_at,
            "observed_at": self.observed_at,
            "stable_key": self.stable_key,
            "source_id": self.source_id,
            "source": self.source,
            "threat_class": self.threat_class,
            "category": self.category,
            "label": self.label,
            "display_id": self.display_id,
            "manufacturer": self.manufacturer,
            "confidence": self.confidence,
            "score": self.score,
            "rssi": self.rssi,
            "events": self.events,
            "seen_count": self.seen_count,
            "latitude": self.latitude,
            "longitude": self.longitude,
            "altitude_m": self.altitude_m,
            "operator_latitude": self.operator_latitude,
            "operator_longitude": self.operator_longitude,
            "operator_id": self.operator_id,
            "extras": self.extras,
        })


@dataclass(frozen=True, slots=True)
class MachineFrame:
    """A recognized record from the mixed USB console stream."""

    kind: str
    value: object

    def to_dict(self) -> dict[str, object]:
        value = self.value.to_dict() if hasattr(self.value, "to_dict") else self.value
        return {"kind": self.kind, "value": value}
