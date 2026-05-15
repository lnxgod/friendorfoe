"""Privacy-oriented BLE/WiFi device presentation helpers.

The badge LCD stays intentionally terse, but Android and the dashboard can
show richer context for the same scanner evidence.  These helpers keep that
presentation layer backward-compatible with the existing live-device API.
"""

from __future__ import annotations

from collections import Counter
from typing import Any


TRACKER_KIND = "TRACKER_NEAR"


def _text_blob(entry: dict[str, Any]) -> str:
    fields = (
        "privacy_kind",
        "device_type",
        "manufacturer",
        "display_label",
        "display_detail",
        "source",
        "ble_svc_uuids",
        "device_class",
        "device_family",
    )
    return " ".join(str(entry.get(field) or "") for field in fields).lower()


def _current_rssi(entry: dict[str, Any]) -> int | None:
    value = entry.get("current_rssi")
    if isinstance(value, (int, float)):
        return int(value)
    return None


def _risk_for_kind(kind: str, rssi: int | None) -> str:
    close = rssi is not None and rssi >= -60
    nearby = rssi is not None and rssi >= -72
    if kind in {"SKIMMER", "CAMERA_NEAR", "FLOCK_ALPR"}:
        return "high" if close else "medium"
    if kind == "TRACKER_NEAR":
        return "high" if close else "medium"
    if kind == "MOBILE_KEY_LOCK":
        return "medium" if nearby else "low"
    if kind in {"BLE_HID", "EVENT_BADGE"}:
        return "medium" if close else "low"
    if kind in {"VENUE_BEACON", "AURACAST", "APPLE_CONTINUITY"}:
        return "info"
    if kind == "META_GLASSES":
        return "medium" if nearby else "low"
    return "low"


def _display_label_for_kind(kind: str) -> str:
    return {
        "META_GLASSES": "META GLASSES",
        TRACKER_KIND: "TRACKER NEAR",
        "SKIMMER": "SKIMMER",
        "CAMERA_NEAR": "CAMERA NEAR",
        "FLOCK_ALPR": "FLOCK CAM",
        "VENUE_BEACON": "BEACON AREA",
        "EVENT_BADGE": "EVENT BADGE",
        "MOBILE_KEY_LOCK": "LOCK NEAR",
        "BLE_HID": "HID NEAR",
        "AURACAST": "AURACAST",
        "APPLE_CONTINUITY": "APPLE CONTINUITY",
    }.get(kind, "PRIVACY SIGNAL")


def _apple_subtype_from_type(apple_type: Any) -> str | None:
    try:
        code = int(apple_type)
    except (TypeError, ValueError):
        return None
    return {
        0x02: "iBeacon",
        0x05: "AirDrop",
        0x06: "HomeKit",
        0x07: "AirPods",
        0x08: "Hey Siri",
        0x09: "AirPlay",
        0x0C: "Handoff",
        0x0D: "WiFi Settings",
        0x0E: "Instant Hotspot",
        0x0F: "Nearby Info",
        0x10: "Nearby Action",
        0x12: "Find My",
    }.get(code)


def sanitize_apple_continuity(value: Any) -> Any:
    """Return Apple Continuity details without raw auth tags.

    Hashed auth fields are okay; raw rotating tags are not useful to users and
    should not leak into API responses.
    """
    if isinstance(value, dict):
        cleaned: dict[str, Any] = {}
        for key, child in value.items():
            key_l = str(key).lower()
            if "auth" in key_l and "hash" not in key_l:
                continue
            cleaned[key] = sanitize_apple_continuity(child)
        return cleaned
    if isinstance(value, list):
        return [sanitize_apple_continuity(item) for item in value]
    return value


def apple_continuity_subtypes(entry: dict[str, Any]) -> list[str]:
    raw = sanitize_apple_continuity(entry.get("apple_continuity"))
    found: list[str] = []
    if isinstance(raw, dict):
        for key in ("subtype", "message_type", "activity", "device_class"):
            value = raw.get(key)
            if isinstance(value, str) and value:
                found.append(value.replace("_", " ").title())
        message_types = raw.get("message_types")
        if isinstance(message_types, dict):
            found.extend(str(k).replace("_", " ").title()
                         for k, v in message_types.items() if v)
        elif isinstance(message_types, list):
            found.extend(str(v).replace("_", " ").title() for v in message_types)
    subtype = _apple_subtype_from_type(entry.get("ble_apple_type"))
    if subtype:
        found.append(subtype)
    deduped: list[str] = []
    for item in found:
        if item and item not in deduped:
            deduped.append(item)
    return deduped


def classify_privacy_device(entry: dict[str, Any]) -> dict[str, Any]:
    text = _text_blob(entry)
    rssi = _current_rssi(entry)
    services = str(entry.get("ble_svc_uuids") or "").lower()
    is_tracker = bool(entry.get("is_tracker"))
    apple_subtypes = apple_continuity_subtypes(entry)
    has_apple = bool(entry.get("apple_continuity") or entry.get("ble_apple_type"))

    if "flock" in text or "alpr" in text:
        kind = "FLOCK_ALPR"
    elif any(token in text for token in (
        "skimmer", "hc-05", "hc-06", "hm-10", "jdy", "bt05", "free2move"
    )):
        kind = "SKIMMER"
    elif any(token in text for token in (
        "hidden camera", "spy cam", "camera", "body cam", "dashcam",
        "dash cam", "fleet cam", "conference cam", "axon", "samsara",
        "verkada", "hikvision", "dahua", "gopro"
    )):
        kind = "CAMERA_NEAR"
    elif any(token in text for token in (
        "meta glasses", "ray-ban", "rayban", "oakley", "luxottica"
    )):
        kind = "META_GLASSES"
    elif is_tracker or any(token in text for token in (
        "airtag", "findmy", "find my", "tile", "smarttag",
        "google tracker", "chipolo", "pebblebee", "tracker"
    )):
        kind = TRACKER_KIND
    elif any(token in text for token in (
        "mobile key", "mobile access", "mobile key lock", "smart lock",
        "dormakaba", "saflok", "vingcard", "assa", "abloy", "salto",
        "onity", "kaba", "august", "schlage", "yale", "level lock"
    )):
        kind = "MOBILE_KEY_LOCK"
    elif "1812" in services or any(token in text for token in (
        "ble hid", "keyboard", "mouse", "input device", "presenter"
    )):
        kind = "BLE_HID"
    elif any(token in text for token in (
        "event badge", "smart badge", "attendee badge", "conference badge",
        "expo badge", "wristband", "bizzabo", "cvent", "klik"
    )):
        kind = "EVENT_BADGE"
    elif any(token in text for token in (
        "venue beacon", "ibeacon", "eddystone", "estimote", "kontakt",
        "gimbal", "retailnext", "vergesense", "beaconstac", "beacon"
    )) or "feaa" in services:
        kind = "VENUE_BEACON"
    elif any(token in text for token in ("auracast", "le audio", "broadcast audio")):
        kind = "AURACAST"
    elif has_apple:
        kind = "APPLE_CONTINUITY"
    else:
        kind = "PRIVACY_SIGNAL"

    label = _display_label_for_kind(kind)
    detail_parts = []
    subtype_detail = ", ".join(apple_subtypes[:3])
    if subtype_detail and kind == "APPLE_CONTINUITY":
        detail_parts.append(subtype_detail)
    elif entry.get("manufacturer") and entry.get("manufacturer") != "Unknown":
        detail_parts.append(str(entry["manufacturer"]))
    elif entry.get("device_type"):
        detail_parts.append(str(entry["device_type"]))
    if rssi is not None:
        detail_parts.append(f"{rssi}dB")
    display_detail = " ".join(detail_parts).strip()

    evidence = []
    for key in ("device_type", "manufacturer", "source", "ble_svc_uuids"):
        value = entry.get(key)
        if value:
            evidence.append({"field": key, "value": value})
    if apple_subtypes:
        evidence.append({"field": "apple_subtypes", "value": apple_subtypes})

    return {
        "privacy_kind": kind,
        "risk_level": _risk_for_kind(kind, rssi),
        "display_label": label,
        "display_detail": display_detail,
        "evidence": evidence,
        "apple_continuity": sanitize_apple_continuity(
            entry.get("apple_continuity")
        ),
    }


def privacy_summary(devices: list[dict[str, Any]]) -> dict[str, Any]:
    kinds: Counter[str] = Counter()
    apple: Counter[str] = Counter()
    for entry in devices:
        kind = entry.get("privacy_kind")
        if kind:
            kinds[str(kind)] += 1
        for subtype in apple_continuity_subtypes(entry):
            apple[subtype] += 1
    return {
        "privacy_kind_counts": dict(kinds),
        "apple_continuity_subtypes": dict(apple),
        "beacon_density": kinds.get("VENUE_BEACON", 0),
    }
