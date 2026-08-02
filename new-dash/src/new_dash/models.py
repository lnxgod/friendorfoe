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
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    return None


def _optional_bool(value: Any) -> bool | None:
    return value if isinstance(value, bool) else None


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
    rssi: float | None

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
            rssi=_optional_number(payload.get("rssi")),
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
            latitude=_optional_number(payload.get("lat")),
            longitude=_optional_number(payload.get("lon")),
            altitude_m=_optional_number(payload.get("altitude_m")),
            operator_latitude=_optional_number(payload.get("operator_lat")),
            operator_longitude=_optional_number(payload.get("operator_lon")),
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
    uptime_seconds: float
    mode: str | None
    mode_label: str | None
    threat_score: float | None
    counts: Mapping[str, Any] | None
    safe_mode: bool | None
    recovery_mode: str | None
    entities: tuple[BadgeEntity, ...]
    scanners: tuple[Mapping[str, Any], ...]
    raw: Mapping[str, Any]

    @classmethod
    def from_payload(cls, payload: object) -> "BadgeStatus":
        if not isinstance(payload, dict):
            raise ValueError("status payload must be an object")
        version = payload.get("version")
        if not isinstance(version, str) or not version.strip():
            raise ValueError("status version must be a nonempty string")
        uptime = payload.get("uptime_s")
        if isinstance(uptime, bool) or not isinstance(uptime, (int, float)) or not isfinite(uptime):
            raise ValueError("status uptime_s must be finite numeric")
        entity_payloads = payload.get("entities", [])
        scanner_payloads = payload.get("scanners", [])
        if not isinstance(entity_payloads, list) or not isinstance(scanner_payloads, list):
            raise ValueError("status entities and scanners must be arrays")
        if not all(isinstance(entity, dict) for entity in entity_payloads):
            raise ValueError("status entities must contain objects")
        if not all(isinstance(scanner, dict) for scanner in scanner_payloads):
            raise ValueError("status scanners must contain objects")
        counts = payload.get("counts")
        return cls(
            version=version,
            uptime_seconds=float(uptime),
            mode=_optional_text(payload.get("mode")),
            mode_label=_optional_text(payload.get("mode_label")),
            threat_score=_optional_number(payload.get("threat_score")),
            counts=_freeze_json(counts) if isinstance(counts, dict) else None,
            safe_mode=_optional_bool(payload.get("safe_mode")),
            recovery_mode=_optional_text(payload.get("recovery_mode")),
            entities=tuple(BadgeEntity.from_payload(entity) for entity in entity_payloads),
            scanners=tuple(_freeze_json(scanner) for scanner in scanner_payloads),
            raw=_freeze_json(payload),
        )

    def to_dict(self) -> dict[str, object]:
        result = _json_safe(self.raw)
        result["entities"] = [entity.to_dict() for entity in self.entities]
        result["scanners"] = _json_safe(self.scanners)
        return result


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
