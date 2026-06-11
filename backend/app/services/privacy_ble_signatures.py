"""Curated BLE service UUID privacy signatures."""

from __future__ import annotations

import re
from typing import Any


_STD_UUID_RE = re.compile(
    r"0000([0-9a-fA-F]{4})-0000-1000-8000-00805f9b34fb",
    re.IGNORECASE,
)
_TOKEN_RE = re.compile(r"(?<![0-9a-fA-F])(?:0x)?([0-9a-fA-F]{4})(?![0-9a-fA-F])")

_BLE_SERVICE_SIGNATURES: dict[int, dict[str, Any]] = {
    # Trackers and unwanted-location-tracker interoperability.
    0xFCB2: {
        "manufacturer": "DULT",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.90,
        "class_reason": "privacy:tracker:dult",
        "source_note": "Detecting Unwanted Location Trackers service UUID.",
    },
    0xFEED: {
        "manufacturer": "Tile",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.85,
        "class_reason": "privacy:tracker:tile",
        "source_note": "Tile tracker member service UUID.",
    },
    0xFEEC: {
        "manufacturer": "Tile",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.85,
        "class_reason": "privacy:tracker:tile",
        "source_note": "Tile tracker member service UUID.",
    },
    0xFD84: {
        "manufacturer": "Tile",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.85,
        "class_reason": "privacy:tracker:tile",
        "source_note": "Tile tracker member service UUID.",
    },
    0xFD59: {
        "manufacturer": "Samsung",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.85,
        "class_reason": "privacy:tracker:smarttag",
        "source_note": "Samsung SmartTag member service UUID.",
    },
    0xFD5A: {
        "manufacturer": "Samsung",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.90,
        "class_reason": "privacy:tracker:smarttag",
        "source_note": "Samsung SmartTag member service UUID.",
    },
    0xFD69: {
        "manufacturer": "Samsung",
        "device_type": "BLE Tracker",
        "privacy_kind": "TRACKER_NEAR",
        "device_family": "tracker",
        "device_class": "tracker",
        "confidence": 0.85,
        "class_reason": "privacy:tracker:smarttag",
        "source_note": "Samsung SmartTag lost/offline service UUID.",
    },
    # Smart glasses and camera/fleet devices.
    0xFD5F: {
        "manufacturer": "Meta",
        "device_type": "Smart Glasses",
        "privacy_kind": "META_GLASSES",
        "device_family": "wearable",
        "device_class": "smart_glasses",
        "confidence": 0.95,
        "class_reason": "privacy:glasses:meta",
        "source_note": "Meta Ray-Ban/Oakley smart-glasses service UUID.",
    },
    0xFC81: {
        "manufacturer": "Axon",
        "device_type": "Body Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.90,
        "class_reason": "privacy:bodycam:axon",
        "source_note": "Axon Enterprise member service UUID.",
    },
    0xFC86: {
        "manufacturer": "Samsara",
        "device_type": "Fleet Dashcam",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:fleetcam:samsara",
        "source_note": "Samsara Networks member service UUID.",
    },
    0xFC87: {
        "manufacturer": "Samsara",
        "device_type": "Fleet Dashcam",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:fleetcam:samsara",
        "source_note": "Samsara Networks member service UUID.",
    },
    0xFE9B: {
        "manufacturer": "Samsara",
        "device_type": "Fleet Dashcam",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:fleetcam:samsara",
        "source_note": "Samsara Networks member service UUID.",
    },
    0xFC6D: {
        "manufacturer": "Motive",
        "device_type": "Fleet Dashcam",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:fleetcam:motive",
        "source_note": "Motive Technologies member service UUID.",
    },
    0xFC70: {
        "manufacturer": "Motive",
        "device_type": "Fleet Dashcam",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:fleetcam:motive",
        "source_note": "Motive Technologies member service UUID.",
    },
    0xFD3A: {
        "manufacturer": "Verkada",
        "device_type": "Surveillance Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:camera:verkada",
        "source_note": "Verkada member service UUID.",
    },
    0xFD3B: {
        "manufacturer": "Verkada",
        "device_type": "Surveillance Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:camera:verkada",
        "source_note": "Verkada member service UUID.",
    },
    0xFD7B: {
        "manufacturer": "Wyze",
        "device_type": "Surveillance Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.80,
        "class_reason": "privacy:camera:wyze",
        "source_note": "Wyze Labs member service UUID.",
    },
    0xFDA9: {
        "manufacturer": "Rhombus",
        "device_type": "Surveillance Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:camera:rhombus",
        "source_note": "Rhombus Systems member service UUID.",
    },
    0xFD8E: {
        "manufacturer": "Motorola",
        "device_type": "Body Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.80,
        "class_reason": "privacy:bodycam:motorola",
        "source_note": "Motorola Solutions member service UUID.",
    },
    0xFD4D: {
        "manufacturer": "70mai",
        "device_type": "Dash Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.80,
        "class_reason": "privacy:dashcam:70mai",
        "source_note": "70mai member service UUID.",
    },
    0xFD4E: {
        "manufacturer": "70mai",
        "device_type": "Dash Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.80,
        "class_reason": "privacy:dashcam:70mai",
        "source_note": "70mai member service UUID.",
    },
    0xFEA5: {
        "manufacturer": "GoPro",
        "device_type": "Action Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.85,
        "class_reason": "privacy:actioncam:gopro",
        "source_note": "GoPro member service UUID.",
    },
    0xFEA6: {
        "manufacturer": "GoPro",
        "device_type": "Action Camera",
        "privacy_kind": "CAMERA_NEAR",
        "device_family": "camera_or_video",
        "device_class": "surveillance_camera",
        "confidence": 0.90,
        "class_reason": "privacy:actioncam:gopro",
        "source_note": "GoPro member service UUID.",
    },
    # Ambient privacy signals.
    0x1812: {
        "manufacturer": "Bluetooth SIG",
        "device_type": "BLE HID",
        "privacy_kind": "BLE_HID",
        "device_family": "input_device",
        "device_class": "ble_hid",
        "confidence": 0.75,
        "class_reason": "privacy:hid:ble",
        "source_note": "Human Interface Device service UUID.",
    },
    0x184F: {
        "manufacturer": "Bluetooth SIG",
        "device_type": "Auracast",
        "privacy_kind": "AURACAST",
        "device_family": "audio",
        "device_class": "auracast",
        "confidence": 0.75,
        "class_reason": "privacy:audio:auracast",
        "source_note": "Broadcast Audio Scan Service UUID.",
    },
    0x1850: {
        "manufacturer": "Bluetooth SIG",
        "device_type": "Auracast",
        "privacy_kind": "AURACAST",
        "device_family": "audio",
        "device_class": "auracast",
        "confidence": 0.75,
        "class_reason": "privacy:audio:auracast",
        "source_note": "Published Audio Capabilities Service UUID.",
    },
    0xFEAA: {
        "manufacturer": "Google",
        "device_type": "Venue Beacon",
        "privacy_kind": "VENUE_BEACON",
        "device_family": "venue_beacon",
        "device_class": "venue_beacon",
        "confidence": 0.70,
        "class_reason": "privacy:beacon:eddystone",
        "source_note": "Eddystone beacon service UUID.",
    },
}


def iter_ble_service_uuid_ints(value: str | None) -> list[int]:
    """Parse comma-separated 16-bit BLE service UUID evidence."""
    if not value:
        return []
    text = str(value)
    found: list[int] = []
    for match in _STD_UUID_RE.finditer(text):
        code = int(match.group(1), 16)
        if code not in found:
            found.append(code)
    for match in _TOKEN_RE.finditer(text):
        code = int(match.group(1), 16)
        if code not in found:
            found.append(code)
    return found


def privacy_ble_service_matches(value: str | None) -> list[dict[str, Any]]:
    matches: list[dict[str, Any]] = []
    for uuid16 in iter_ble_service_uuid_ints(value):
        entry = _BLE_SERVICE_SIGNATURES.get(uuid16)
        if entry:
            matched = dict(entry)
            matched["uuid16"] = uuid16
            matched["uuid16_hex"] = f"{uuid16:04X}"
            matches.append(matched)
    return matches


def first_privacy_ble_service_match(value: str | None) -> dict[str, Any] | None:
    matches = privacy_ble_service_matches(value)
    if not matches:
        return None
    return max(matches, key=lambda item: float(item.get("confidence") or 0.0))
