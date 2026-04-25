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
# Must match type names from ESP32 ble_fingerprint.c
KNOWN_DEVICE_TYPES = {
    # Apple
    "iphone",
    "macbook",
    "ipad",
    "apple watch",
    "airpods",
    # Android / Samsung
    "samsung phone",
    "samsung tablet",
    "pixel",
    "android",
    # Consumer electronics
    "audio device",
    "smart home",
    "vehicle",
    "camera",
    "e-scooter",
    "medical",
    "gaming",
    "fitness tracker",
    "smartwatch",
    "beacon",
    # Security / novelty
    "meta glasses",
    "meta device",
    "flipper zero",
    # Fallback
    "unknown",
}

# BLE device types that are actual drones or drone controllers
BLE_DRONE_TYPES = {
    "drone controller",
    "drone",
}

# Drone protocol sources — if the detection came from a real drone protocol
CONFIRMED_DRONE_SOURCES = {"ble_rid", "wifi_beacon_rid", "wifi_dji_ie"}

# Minimum confidence required to *trust* a CONFIRMED_DRONE_SOURCES detection
# and label it `confirmed_drone`. Below this, we downgrade to `likely_drone`
# so an alert is still raised but without the "critical" severity.
# A genuine Remote ID frame from a drone parses cleanly and the scanner
# reports >= 0.85; confidences below 0.30 are usually weak/corrupt frames.
CONFIRMED_DRONE_MIN_CONFIDENCE = 0.30

# SSID patterns that indicate mobile hotspots, not infrastructure APs
MOBILE_HOTSPOT_KEYWORDS = {
    "iphone", "ipad", "galaxy", "pixel", "oneplus", "nothing phone",
    "motorola", "moto g", "moto e", "redmi", "poco", "realme", "oppo",
    "vivo", "huawei", "nokia", "tcl", "zte", "samsung",
    "androidap", "mobile hotspot", "my hotspot", "portable hotspot",
    "personal hotspot",
}
MOBILE_HOTSPOT_PREFIXES = ("DIRECT-", "AndroidAP")


def _is_locally_administered_mac(bssid: str | None) -> bool:
    """Check if BSSID has the locally administered bit set (randomized MAC).
    Infrastructure APs use burned-in MACs; phones/hotspots randomize."""
    if not bssid or len(bssid) < 2:
        return False
    try:
        first_byte = int(bssid.split(":")[0], 16)
        return (first_byte & 0x02) != 0
    except (ValueError, IndexError):
        return False


def _is_mobile_hotspot_ssid(ssid: str | None) -> bool:
    """Check if SSID looks like a mobile phone hotspot."""
    if not ssid:
        return False
    ssid_lower = ssid.lower()
    for prefix in MOBILE_HOTSPOT_PREFIXES:
        if ssid.startswith(prefix):
            return True
    for keyword in MOBILE_HOTSPOT_KEYWORDS:
        if keyword in ssid_lower:
            return True
    # SSIDs with possessive names like "John's iPhone" or "Hannah's Galaxy"
    if "'s " in ssid and any(kw in ssid_lower for kw in ("phone", "galaxy", "iphone", "ipad", "pixel")):
        return True
    return False


def _has_meaningful_position(value: float | None) -> bool:
    return value is not None and abs(value) > 1e-9


def normalize_detection_source(
    source: str,
    *,
    drone_id: str | None = None,
    manufacturer: str | None = None,
    latitude: float | None = None,
    longitude: float | None = None,
    operator_lat: float | None = None,
    operator_lon: float | None = None,
    operator_id: str | None = None,
    self_id_text: str | None = None,
) -> str:
    """Normalize legacy source labels into the production taxonomy.

    Older scanners/uplinks still send generic BLE fingerprint traffic as
    `ble_rid`, and AP<->STA association traffic as `wifi_oui`. Normalize
    those payloads so downstream services can reason about them correctly
    during a mixed-fleet rollout.
    """
    normalized = (source or "").strip()
    if normalized == "ble_rid":
        if (drone_id or "").startswith("rid_"):
            return normalized
        if operator_id or self_id_text:
            return normalized
        if any(
            _has_meaningful_position(v)
            for v in (latitude, longitude, operator_lat, operator_lon)
        ):
            return normalized
        return "ble_fingerprint"

    if normalized == "wifi_oui":
        did = drone_id or ""
        mfr = (manufacturer or "").strip().lower()
        if mfr == "wifi-assoc" or did.startswith("STA:") or "→AP:" in did or "->AP:" in did:
            return "wifi_assoc"

    return normalized


def classify_detection(
    source: str,
    confidence: float,
    ssid: str | None = None,
    manufacturer: str | None = None,
    drone_id: str | None = None,
    model: str | None = None,
    bssid: str | None = None,
    latitude: float | None = None,
    longitude: float | None = None,
    operator_lat: float | None = None,
    operator_lon: float | None = None,
    operator_id: str | None = None,
    self_id_text: str | None = None,
) -> tuple[str, float]:
    """Classify a detection and optionally adjust confidence.

    Returns:
        (classification, adjusted_confidence)
    """
    source = normalize_detection_source(
        source,
        drone_id=drone_id,
        manufacturer=manufacturer,
        latitude=latitude,
        longitude=longitude,
        operator_lat=operator_lat,
        operator_lon=operator_lon,
        operator_id=operator_id,
        self_id_text=self_id_text,
    )

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
        if dt_lower in BLE_DRONE_TYPES:
            if source == "ble_fingerprint":
                return "possible_drone", max(confidence, 0.35)
            return "confirmed_drone", confidence
        # "unknown" type should NOT suppress drone classification —
        # a BLE Remote ID drone with unrecognized fingerprint is still a drone.
        if dt_lower != "unknown" and dt_lower in KNOWN_DEVICE_TYPES:
            return "unknown_device", confidence

    # 2. Fingerprint-grouped BLE (FP:XXXXXXXX) — these are generic BLE devices
    #    Not confirmed drones just because they're BLE — treat as unknown.
    if source == "ble_fingerprint" and drone_id and drone_id.startswith("FP:"):
        return "unknown_device", confidence

    # 3. Pwnagotchi — scanner detected the DE:AD:BE:EF:DE:AD hardcoded beacon
    #    source MAC. This is a known hostile-WiFi-scanner signature (Marauder
    #    reference). Promote to its own "hostile_tool" class so alerts fire.
    if manufacturer == "Pwnagotchi" or (drone_id or "").lower() == "pwnagotchi":
        return "hostile_tool", max(confidence, 0.90)

    # 4. Real drone protocols → confirmed_drone.
    if source in CONFIRMED_DRONE_SOURCES:
        # Weak-confidence drone-protocol frames get downgraded to likely_drone
        # so an alert still fires, but at warning (not critical) severity.
        if confidence < CONFIRMED_DRONE_MIN_CONFIDENCE:
            return "likely_drone", confidence
        return "confirmed_drone", confidence

    if source == "ble_fingerprint":
        return "unknown_device", confidence

    # 3. FOF-Drone- test drones (case-insensitive)
    if ssid and ssid.upper().startswith("FOF-DRONE-"):
        return "test_drone", max(confidence, 0.70)

    # 3.4. Mobile hotspot detection — phone acting as AP, not infrastructure
    # Only match if SSID contains phone/device keywords (Galaxy, iPhone, DIRECT-, etc.)
    # Don't use locally-administered MAC alone — mesh nodes also use randomized MACs
    if source in ("wifi_oui", "wifi_ssid") and ssid:
        if _is_mobile_hotspot_ssid(ssid):
            return "mobile_hotspot", confidence

    # 3.5. WiFi probe requests — classify by probed SSID
    if source == "wifi_probe_request":
        if ssid and match_drone_wifi_ssid:
            drone_match = match_drone_wifi_ssid(ssid)
            if drone_match:
                return "likely_drone", max(confidence, 0.50)
        # Generic probe request — not a drone
        return "wifi_device", confidence

    if source == "wifi_assoc":
        return "wifi_device", confidence

    if source == "wifi_ap_inventory":
        return ("known_ap" if ssid and any(fnmatch(ssid, pattern) for pattern in WHITELIST_SSID_PATTERNS)
                else "wifi_device"), confidence

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

    # 6. Soft SSID match (possible drone) — whitelist overrides.
    # 0.10 was too permissive; consumer hotspots matched through.
    if source == "wifi_ssid" and confidence >= 0.20:
        return ("known_ap" if is_whitelisted else "possible_drone"), confidence

    # 7. OUI match — these are generic WiFi APs detected by OUI
    #    If whitelisted SSID → known_ap, otherwise → wifi_device (not drone)
    if source == "wifi_oui":
        return ("known_ap" if is_whitelisted else "wifi_device"), confidence

    # 8. Everything else
    return "unknown_device", confidence
