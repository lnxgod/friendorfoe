"""Apple Continuity manufacturer-data decoding.

This is defensive evidence enrichment only. We keep Apple labels cautious and
hash auth tags before surfacing them so rotating/private identifiers are not
published raw through APIs or dashboard chips.
"""

from __future__ import annotations

import hashlib
import hmac
import os
import re
from typing import Any

APPLE_COMPANY_ID_LE = b"\x4c\x00"

MESSAGE_TYPES = {
    0x02: "iBeacon",
    0x05: "AirDrop",
    0x06: "HomeKit",
    0x07: "AirPods",
    0x08: "Hey Siri",
    0x09: "AirPlay",
    0x0B: "Apple Watch",
    0x0C: "Handoff",
    0x0D: "WiFi Settings",
    0x0E: "Instant Hotspot",
    0x0F: "Nearby Action",
    0x10: "Nearby Info",
    0x12: "Find My",
}

NEARBY_ACTIONS = {
    0x01: "Apple TV Setup",
    0x04: "Pair Nearby Device",
    0x05: "Internet Relay",
    0x06: "Developer Tools",
    0x07: "WiFi Password Share",
    0x08: "Repair",
    0x09: "Setup New Device",
    0x0A: "Transfer Number",
    0x0B: "Vision Pro Setup",
}

FLAG_BITS = {
    0: "airpods_connected",
    1: "wifi_on",
    2: "watch_paired",
    3: "primary_icloud",
    4: "auth_tag_present",
    5: "screen_on",
}

ACTIVITY_HINTS = {
    0: "idle",
    1: "audio",
    2: "phone",
    3: "video",
}


def decode_apple_continuity(
    *,
    raw_mfr_hex: str | None = None,
    apple_type: int | None = None,
    apple_flags: int | None = None,
    apple_activity: int | None = None,
    apple_auth: str | None = None,
) -> dict[str, Any] | None:
    payload = _hex_bytes(raw_mfr_hex)
    messages = _parse_tlvs(payload)

    if apple_type is not None and not any(m["type"] == apple_type for m in messages):
        messages.append(_message_from_legacy_fields(apple_type, apple_flags, apple_activity))

    # A default ble_apple_flags=0 can appear on non-Apple rows in mixed
    # firmware payloads. Do not create Apple evidence unless we saw an Apple
    # TLV, an explicit Apple type, or an auth tag.
    if not messages and apple_type is None and not apple_auth:
        return None

    flags = sorted({
        flag
        for msg in messages
        for flag in (msg.get("flags") or [])
    })
    if apple_flags is not None:
        flags = sorted(set(flags) | set(_flags_from_byte(apple_flags)))

    evidence: list[str] = []
    message_types = []
    confidence = 0.62
    label = "Apple Device"
    nearby_actions: list[str] = []
    activity = ACTIVITY_HINTS.get(int(apple_activity)) if apple_activity is not None else None

    for msg in messages:
        subtype = msg["subtype_name"]
        message_types.append(subtype)
        evidence.append(f"Apple Continuity: {subtype}")
        if subtype in ("Find My", "AirPods", "AirPlay", "Handoff"):
            confidence = max(confidence, 0.78)
        if subtype == "AirPods":
            label = "Apple AirPods/Audio Device"
        elif subtype == "Find My" and label == "Apple Device":
            label = "Apple Find My-capable Device"
        elif subtype in ("AirPlay", "Handoff") and label == "Apple Device":
            label = "Apple Continuity Device"
        if msg.get("nearby_action"):
            nearby_actions.append(str(msg["nearby_action"]))
            evidence.append(f"Nearby Action: {msg['nearby_action']}")
        if not activity and msg.get("activity"):
            activity = str(msg["activity"])

    if flags:
        evidence.append("Apple flags: " + ", ".join(flags))
    if activity:
        evidence.append(f"Apple activity: {activity}")

    auth_hash = _hash_auth_tag(apple_auth)
    if auth_hash:
        evidence.append("Apple auth tag hash present")
        confidence = max(confidence, 0.72)

    return {
        "label": label,
        "confidence": round(confidence, 2),
        "source": "apple_continuity",
        "source_url": "https://github.com/furiousMAC/continuity",
        "message_types": sorted(set(message_types)),
        "messages": messages,
        "flags": flags,
        "activity": activity,
        "nearby_actions": sorted(set(nearby_actions)),
        "auth_tag_present": bool(auth_hash),
        "auth_tag_hash": auth_hash,
        "evidence": evidence,
    }


def _hex_bytes(value: str | None) -> bytes:
    if not value:
        return b""
    cleaned = re.sub(r"[^0-9A-Fa-f]", "", value)
    if len(cleaned) % 2:
        cleaned = cleaned[:-1]
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        return b""


def _parse_tlvs(payload: bytes) -> list[dict[str, Any]]:
    if not payload:
        return []
    if payload.startswith(APPLE_COMPANY_ID_LE):
        payload = payload[2:]
    messages: list[dict[str, Any]] = []
    idx = 0
    while idx + 2 <= len(payload):
        msg_type = payload[idx]
        length = payload[idx + 1]
        value = payload[idx + 2: idx + 2 + length]
        if length > len(payload) - idx - 2:
            break
        if msg_type in MESSAGE_TYPES:
            messages.append(_message(msg_type, value))
        idx += 2 + length
    return messages


def _message(msg_type: int, value: bytes) -> dict[str, Any]:
    subtype_name = MESSAGE_TYPES.get(msg_type, f"Continuity 0x{msg_type:02X}")
    flags = _flags_from_byte(value[0]) if msg_type == 0x10 and value else []
    nearby_action = None
    if msg_type == 0x0F and value:
        nearby_action = NEARBY_ACTIONS.get(value[0], f"Nearby Action 0x{value[0]:02X}")
    activity = None
    if msg_type in (0x10, 0x0C) and len(value) >= 2:
        activity = ACTIVITY_HINTS.get(value[1])
    return {
        "type": msg_type,
        "type_hex": f"0x{msg_type:02X}",
        "subtype_name": subtype_name,
        "length": len(value),
        "flags": flags,
        "nearby_action": nearby_action,
        "activity": activity,
    }


def _message_from_legacy_fields(
    apple_type: int,
    apple_flags: int | None,
    apple_activity: int | None,
) -> dict[str, Any]:
    return {
        "type": apple_type,
        "type_hex": f"0x{apple_type:02X}",
        "subtype_name": MESSAGE_TYPES.get(apple_type, f"Continuity 0x{apple_type:02X}"),
        "length": None,
        "flags": _flags_from_byte(apple_flags) if apple_flags is not None else [],
        "nearby_action": None,
        "activity": ACTIVITY_HINTS.get(int(apple_activity)) if apple_activity is not None else None,
    }


def _flags_from_byte(value: int | None) -> list[str]:
    if value is None:
        return []
    try:
        flag_byte = int(value)
    except Exception:
        return []
    return [name for bit, name in FLAG_BITS.items() if flag_byte & (1 << bit)]


def _hash_auth_tag(value: str | None) -> str | None:
    raw = _hex_bytes(value)
    if not raw:
        return None
    key = (
        os.environ.get("FOF_RF_HASH_KEY")
        or os.environ.get("FOF_CAL_TOKEN")
        or "friendorfoe-local-rf-correlation"
    ).encode("utf-8")
    return hmac.new(key, raw, hashlib.sha256).hexdigest()[:24]
