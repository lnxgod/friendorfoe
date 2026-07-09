"""Privacy-oriented BLE/WiFi device presentation helpers.

The badge LCD stays intentionally terse, but Android and the dashboard can
show richer context for the same scanner evidence.  These helpers keep that
presentation layer backward-compatible with the existing live-device API.
"""

from __future__ import annotations

from collections import Counter
from typing import Any

from app.services.privacy_ble_signatures import first_privacy_ble_service_match


TRACKER_KIND = "TRACKER_NEAR"
REMOTE_LISTENING_KIND = "REMOTE_LISTENING"


def _text_blob(entry: dict[str, Any]) -> str:
    fields = (
        "privacy_kind",
        "device_type",
        "manufacturer",
        "display_label",
        "display_detail",
        "source",
        "ssid",
        "model",
        "class_reason",
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


def _field_starts_with_flock_oui(value: Any) -> bool:
    text = str(value or "").strip()
    if not text:
        return False

    hex_chars: list[str] = []
    for ch in text:
        if ch in ":-.":
            if not hex_chars:
                return False
            continue
        upper = ch.upper()
        if upper not in "0123456789ABCDEF":
            return False
        hex_chars.append(upper)
        if len(hex_chars) == 6:
            return "".join(hex_chars) == "B41E52"
    return False


def _has_supported_alpr_evidence(entry: dict[str, Any]) -> bool:
    source_l = str(entry.get("source") or "").lower()
    if not source_l.startswith("wifi"):
        return False

    reason_l = str(entry.get("class_reason") or "").lower()
    if "privacy:alpr:" in reason_l:
        return True
    if "wifi_oui:flock:b4:1e:52" in reason_l:
        return True

    return any(
        _field_starts_with_flock_oui(entry.get(field))
        for field in ("bssid", "mac", "drone_id", "display_id")
    )


def _risk_for_kind(kind: str, rssi: int | None) -> str:
    close = rssi is not None and rssi >= -60
    nearby = rssi is not None and rssi >= -72
    if kind in {"SKIMMER", "CAMERA_NEAR", "FLOCK_ALPR"}:
        return "high" if close else "medium"
    if kind == "WIFI_ATTACK_TOOL":
        return "high" if close else "medium"
    if kind == REMOTE_LISTENING_KIND:
        return "high" if close else "medium"
    if kind == "TRACKER_NEAR":
        return "high" if close else "medium"
    if kind == "MOBILE_KEY_LOCK":
        return "medium" if nearby else "low"
    if kind in {"BLE_HID", "EVENT_BADGE"}:
        return "medium" if close else "low"
    if kind == "VENUE_BEACON":
        return "medium" if close else "low"
    if kind in {"AURACAST", "APPLE_CONTINUITY"}:
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
        REMOTE_LISTENING_KIND: "POSSIBLE LISTENING",
        "WIFI_ATTACK_TOOL": "WIFI TOOL",
    }.get(kind, "PRIVACY SIGNAL")


def _apple_subtype_from_type(apple_type: Any) -> str | None:
    try:
        code = int(str(apple_type), 0)
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


def _is_apple_ibeacon(entry: dict[str, Any], apple_subtypes: list[str]) -> bool:
    if any(str(subtype).lower() == "ibeacon" for subtype in apple_subtypes):
        return True
    return "ibeacon" in str(entry.get("class_reason") or "").lower()


def _first_present(entry: dict[str, Any], *keys: str) -> Any:
    for key in keys:
        value = entry.get(key)
        if value is not None and value != "":
            return value
    return None


def _venue_beacon_detail(
    entry: dict[str, Any],
    apple_subtypes: list[str],
    service_match: dict[str, Any] | None,
    rssi: int | None,
) -> str:
    text = _text_blob(entry)
    service_uuid = str(service_match.get("uuid16_hex") if service_match else "").lower()
    is_ibeacon = _is_apple_ibeacon(entry, apple_subtypes)
    is_eddystone = (
        service_uuid == "0xfeaa"
        or "feaa" in str(entry.get("ble_svc_uuids") or "").lower()
        or "eddystone" in text
        or any(str(key).startswith("eddystone_") and entry.get(key) for key in entry)
    )

    if is_ibeacon:
        protocol = "iBeacon"
    elif is_eddystone:
        protocol = "Eddystone"
    else:
        protocol = "Venue Beacon"

    parts = [protocol]
    if protocol == "iBeacon":
        uuid = _first_present(entry, "ibeacon_uuid", "beacon_uuid")
        major = _first_present(entry, "ibeacon_major", "beacon_major", "major")
        minor = _first_present(entry, "ibeacon_minor", "beacon_minor", "minor")
        if uuid:
            parts.append(str(uuid))
        if major is not None and minor is not None:
            parts.append(f"{major}/{minor}")
    elif protocol == "Eddystone":
        frame = _first_present(entry, "eddystone_frame_type", "beacon_frame", "frame_type")
        url = _first_present(entry, "eddystone_url", "beacon_url", "url")
        namespace = _first_present(entry, "eddystone_namespace", "beacon_namespace")
        instance = _first_present(entry, "eddystone_instance", "beacon_instance")
        eid = _first_present(entry, "eddystone_eid", "beacon_eid")
        if frame:
            parts.append(str(frame))
        if url:
            parts.append(str(url))
        elif namespace or instance:
            parts.append("/".join(str(item) for item in (namespace, instance) if item))
        elif eid:
            parts.append(str(eid))
    else:
        manufacturer = entry.get("manufacturer")
        if manufacturer and manufacturer != "Unknown":
            parts.append(str(manufacturer))

    if rssi is not None:
        parts.append(f"{rssi}dB")
    return " ".join(parts).strip()


def apple_remote_listening_hint(
    entry: dict[str, Any],
    rssi: int | None = None,
) -> dict[str, Any] | None:
    raw = sanitize_apple_continuity(entry.get("apple_continuity"))
    if not isinstance(raw, dict):
        return None

    hint = raw.get("remote_listening")
    if not isinstance(hint, dict):
        flags = raw.get("flags")
        activity = raw.get("activity")
        flags_set = {str(flag) for flag in flags} if isinstance(flags, list) else set()
        if "airpods_connected" not in flags_set:
            return None
        hint = {
            "label": "Apple AirPods connection nearby",
            "risk_hint": "low",
            "confidence": 0.48,
            "signals": ["airpods_connected"],
            "evidence": "Apple AirPods connected flag observed",
        }
        if activity in {"audio", "phone", "video"}:
            hint.update({
                "label": "Possible Apple remote listening path",
                "risk_hint": "medium",
                "confidence": 0.72,
                "signals": ["airpods_connected", f"apple_activity_{activity}"],
                "evidence": f"Possible remote listening path: AirPods connected with {activity} activity",
            })

    signals = {str(item) for item in hint.get("signals", []) if item}
    confidence = float(hint.get("confidence") or 0.0)
    close = rssi is not None and rssi >= -60
    active_path = any(
        signal in signals
        for signal in ("apple_activity_audio", "apple_activity_phone", "apple_activity_video")
    )

    # Require more than generic AirPods proximity before creating a privacy alert.
    # RSSI can raise severity once an active path exists, but proximity alone
    # should remain an informational Apple Continuity row.
    if "airpods_connected" not in signals:
        return None
    if not active_path:
        return None

    risk_level = "high" if close and active_path else "medium"
    detail_bits = []
    if active_path:
        detail_bits.append("AirPods connected + audio activity")
    else:
        detail_bits.append("AirPods connected")
    if rssi is not None:
        detail_bits.append(f"{rssi}dB")

    result = dict(hint)
    result.update({
        "privacy_kind": REMOTE_LISTENING_KIND,
        "risk_level": risk_level,
        "display_detail": " ".join(detail_bits),
    })
    if confidence < 0.55 and close:
        result["confidence"] = 0.60
    return result


def classify_privacy_device(entry: dict[str, Any]) -> dict[str, Any]:
    text = _text_blob(entry)
    rssi = _current_rssi(entry)
    services = str(entry.get("ble_svc_uuids") or "").lower()
    service_match = first_privacy_ble_service_match(services)
    is_tracker = bool(entry.get("is_tracker"))
    apple_subtypes = apple_continuity_subtypes(entry)
    has_apple = bool(entry.get("apple_continuity") or entry.get("ble_apple_type"))
    is_apple_ibeacon = _is_apple_ibeacon(entry, apple_subtypes)
    remote_listening = apple_remote_listening_hint(entry, rssi)

    if _has_supported_alpr_evidence(entry):
        kind = "FLOCK_ALPR"
    elif any(token in text for token in (
        "skimmer", "hc-05", "hc-06", "hm-10", "jdy", "bt05", "free2move"
    )):
        kind = "SKIMMER"
    elif any(token in text for token in (
        "attack_tool", "attack tool", "pwnagotchi", "pineapple",
        "deauther", "marauder", "wifi attack"
    )):
        kind = "WIFI_ATTACK_TOOL"
    elif is_apple_ibeacon:
        kind = "VENUE_BEACON"
    elif service_match:
        kind = str(service_match["privacy_kind"])
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
    elif remote_listening:
        kind = REMOTE_LISTENING_KIND
    elif has_apple:
        kind = "APPLE_CONTINUITY"
    else:
        kind = "PRIVACY_SIGNAL"

    label = _display_label_for_kind(kind)
    detail_parts = []
    subtype_detail = ", ".join(apple_subtypes[:3])
    if remote_listening and kind == REMOTE_LISTENING_KIND:
        detail_parts.append(str(remote_listening["display_detail"]))
    elif kind == "VENUE_BEACON":
        detail_parts.append(_venue_beacon_detail(entry, apple_subtypes, service_match, rssi))
    elif subtype_detail and kind == "APPLE_CONTINUITY":
        detail_parts.append(subtype_detail)
    elif entry.get("manufacturer") and entry.get("manufacturer") != "Unknown":
        detail_parts.append(str(entry["manufacturer"]))
    elif entry.get("device_type"):
        detail_parts.append(str(entry["device_type"]))
    if (
        rssi is not None
        and not (remote_listening and kind == REMOTE_LISTENING_KIND)
        and kind != "VENUE_BEACON"
    ):
        detail_parts.append(f"{rssi}dB")
    display_detail = " ".join(detail_parts).strip()

    evidence = []
    for key in (
        "device_type", "manufacturer", "source", "ssid", "class_reason",
        "ble_svc_uuids", "ble_apple_type", "ble_company_id", "ibeacon_uuid",
        "ibeacon_major", "ibeacon_minor", "beacon_uuid", "beacon_major",
        "beacon_minor", "eddystone_frame_type", "eddystone_url",
        "eddystone_namespace", "eddystone_instance", "eddystone_eid",
    ):
        value = entry.get(key)
        if value is not None and value != "":
            evidence.append({"field": key, "value": value})
    if service_match:
        evidence.append({
            "field": "ble_service_signature",
            "value": f"{service_match['uuid16_hex']} {service_match['class_reason']}",
        })
    if apple_subtypes:
        evidence.append({"field": "apple_subtypes", "value": apple_subtypes})
    if remote_listening:
        evidence.append({
            "field": "remote_listening",
            "value": {
                "confidence": remote_listening.get("confidence"),
                "signals": remote_listening.get("signals", []),
                "limitations": remote_listening.get("limitations", []),
            },
        })

    return {
        "privacy_kind": kind,
        "risk_level": (
            str(remote_listening["risk_level"])
            if remote_listening and kind == REMOTE_LISTENING_KIND
            else _risk_for_kind(kind, rssi)
        ),
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
