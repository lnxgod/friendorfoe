"""Detection classifier — assigns a category to each detection.

Pure function, no state. Called during ingestion to label each detection
as confirmed_drone, likely_drone, test_drone, possible_drone, tracker,
known_ap, or unknown_device.
"""

import logging
from fnmatch import fnmatch

try:
    from app.services.drone_signature_reference import match_drone_wifi_ssid
except ImportError:
    match_drone_wifi_ssid = None

logger = logging.getLogger(__name__)

# SSIDs that are known infrastructure (loaded from DB at startup, refreshed periodically)
# Only FoF-* is hardcoded — everything else comes from the whitelist table
WHITELIST_SSID_PATTERNS: set[str] = {"FoF-*"}


def load_whitelist_from_db(db_session) -> int:
    """Load SSID whitelist patterns from the database. Called at startup and on changes."""
    global WHITELIST_SSID_PATTERNS
    try:
        from app.models.db_models import WhitelistedSSID
        result = db_session.execute(
            db_session.query(WhitelistedSSID.pattern) if hasattr(db_session, 'query')
            else __import__('sqlalchemy').select(WhitelistedSSID.pattern)
        )
        patterns = {"FoF-*"}
        for row in result:
            p = row[0] if isinstance(row, tuple) else row.pattern
            if p:
                patterns.add(p)
        WHITELIST_SSID_PATTERNS = patterns
        logger.info("Loaded %d SSID whitelist patterns from DB", len(patterns))
        return len(patterns)
    except Exception as e:
        logger.warning("Failed to load SSID whitelist from DB: %s", e)
        return 0


async def async_load_whitelist(async_session) -> int:
    """Async version for FastAPI startup."""
    global WHITELIST_SSID_PATTERNS
    try:
        from sqlalchemy import select
        from app.models.db_models import WhitelistedSSID
        result = await async_session.execute(select(WhitelistedSSID.pattern))
        patterns = {"FoF-*"}
        for row in result.scalars().all():
            if row:
                patterns.add(row)
        WHITELIST_SSID_PATTERNS = patterns
        logger.info("Loaded %d SSID whitelist patterns from DB", len(patterns))
        return len(patterns)
    except Exception as e:
        logger.warning("Failed to load SSID whitelist from DB: %s", e)
        return 0

# BLE device types that are trackers
TRACKER_TYPES = {
    "airtag",
    "findmy accessory",
    "tile tracker",
    "smarttag",
    "google tracker",
    "tracker",
    "tracker (generic)",
}

# BLE device types that are known non-drone devices (phones, computers, etc.)
KNOWN_DEVICE_TYPES = {
    "iphone",
    "macbook",
    "ipad",
    "apple watch",
    "samsung phone",
    "samsung tablet",
    "pixel",
    "android",
    "unknown",
}

# Drone protocol sources — if the detection came from a real drone protocol
CONFIRMED_DRONE_SOURCES = {"ble_rid", "wifi_beacon_rid", "wifi_dji_ie"}


def classify_detection(
    source: str,
    confidence: float,
    ssid: str | None = None,
    manufacturer: str | None = None,
    drone_id: str | None = None,
    model: str | None = None,
) -> tuple[str, float]:
    """Classify a detection and optionally adjust confidence.

    Returns:
        (classification, adjusted_confidence)
    """
    # 1. Check device type from drone_id (BLE:HASH:TypeName format)
    #    or from model field (which may carry the original type)
    device_type_str = None
    if drone_id:
        parts = drone_id.split(":")
        if len(parts) >= 3:
            device_type_str = ":".join(parts[2:]).strip()

    # Also check model field for type info (original drone_id before FP: grouping)
    if not device_type_str and model:
        mparts = model.split(":")
        if len(mparts) >= 3:
            device_type_str = ":".join(mparts[2:]).strip()

    if device_type_str:
        dt_lower = device_type_str.lower()
        if dt_lower in TRACKER_TYPES:
            return "tracker", confidence
        if dt_lower in KNOWN_DEVICE_TYPES:
            return "unknown_device", confidence

    # 2. Fingerprint-grouped BLE (FP:XXXXXXXX) — these are generic BLE devices
    #    Not confirmed drones just because they're BLE — treat as unknown
    if drone_id and drone_id.startswith("FP:"):
        return "unknown_device", confidence

    # 3. Real drone protocols → confirmed_drone (only for non-FP grouped)
    if source in CONFIRMED_DRONE_SOURCES:
        return "confirmed_drone", confidence

    # 3. FOF- test drones (case-insensitive)
    if ssid and ssid.upper().startswith("FOF-"):
        return "test_drone", max(confidence, 0.70)

    # 3.5. WiFi probe requests — classify by probed SSID
    if source == "wifi_probe_request":
        if ssid and match_drone_wifi_ssid:
            drone_match = match_drone_wifi_ssid(ssid)
            if drone_match:
                return "likely_drone", max(confidence, 0.50)
        # Generic probe request — not a drone
        return "wifi_device", confidence

    # 3.6. Check drone SSID reference database (191 patterns)
    if ssid and match_drone_wifi_ssid:
        drone_match = match_drone_wifi_ssid(ssid)
        if drone_match:
            return "likely_drone", max(confidence, 0.50)

    # 4. Check whitelist — but only label as known_ap, don't return yet for low-conf wifi_oui
    is_whitelisted = False
    if ssid:
        for pattern in WHITELIST_SSID_PATTERNS:
            if fnmatch(ssid, pattern):
                is_whitelisted = True
                break

    # 5. Known drone SSID pattern (hard match from ESP32, confidence >= 0.25)
    #    Whitelist overrides — a whitelisted SSID is never a drone
    if source == "wifi_ssid" and confidence >= 0.25:
        return ("known_ap" if is_whitelisted else "likely_drone"), confidence

    # 6. Soft SSID match (possible drone) — whitelist overrides
    if source == "wifi_ssid" and confidence >= 0.10:
        return ("known_ap" if is_whitelisted else "possible_drone"), confidence

    # 7. OUI match — these are generic WiFi APs detected by OUI
    #    If whitelisted SSID → known_ap, otherwise → wifi_device (not drone)
    if source == "wifi_oui":
        return ("known_ap" if is_whitelisted else "wifi_device"), confidence

    # 8. Everything else
    return "unknown_device", confidence
