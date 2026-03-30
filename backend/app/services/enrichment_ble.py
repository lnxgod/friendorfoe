"""BLE device enrichment — OUI lookups, fingerprint classification, deduplication.

Transforms raw noisy BLE detections into clean, enriched device records
suitable for dashboard display.
"""

import time
import logging
from collections import defaultdict
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# BLE OUI prefix database (first 3 bytes of MAC → manufacturer)
# ---------------------------------------------------------------------------

_BLE_OUI = {
    # ── Apple (catch-all by first byte, plus specific blocks) ────────────
    "4C": "Apple",
    # ── Samsung ──────────────────────────────────────────────────────────
    "94:E6:86": "Samsung", "00:1A:7D": "Samsung", "FC:A8:9A": "Samsung",
    "D0:6B:76": "Samsung", "60:D1:D2": "Samsung", "C0:BD:C8": "Samsung",
    "A8:1E:84": "Samsung", "8C:F5:A3": "Samsung", "B4:3A:28": "Samsung",
    "CC:07:AB": "Samsung", "50:B7:C3": "Samsung", "AC:5F:3E": "Samsung",
    # ── Google / Nest ────────────────────────────────────────────────────
    "DC:54:75": "Google", "A4:77:33": "Google", "F4:F5:D8": "Google",
    "48:D6:D5": "Google", "30:FD:38": "Google", "54:60:09": "Google",
    "F8:8F:CA": "Google",
    # ── Espressif (ESP32/ESP8266) ────────────────────────────────────────
    "10:B4:1D": "Espressif", "94:3C:C6": "Espressif", "30:AE:A4": "Espressif",
    "A4:CF:12": "Espressif", "24:0A:C4": "Espressif", "7C:DF:A1": "Espressif",
    "CC:50:E3": "Espressif", "84:F3:EB": "Espressif", "3C:71:BF": "Espressif",
    "EC:FA:BC": "Espressif", "08:3A:F2": "Espressif", "34:85:18": "Espressif",
    "24:62:AB": "Espressif", "24:6F:28": "Espressif", "2C:3A:E8": "Espressif",
    "40:22:D8": "Espressif", "48:3F:DA": "Espressif", "50:02:91": "Espressif",
    "54:5A:A6": "Espressif", "58:BF:25": "Espressif", "60:01:94": "Espressif",
    "68:C6:3A": "Espressif", "7C:87:CE": "Espressif", "80:7D:3A": "Espressif",
    "84:0D:8E": "Espressif", "84:CC:A8": "Espressif", "8C:AA:B5": "Espressif",
    "90:97:D5": "Espressif",
    # ── Amazon (Echo, Fire, Ring, Eero) ──────────────────────────────────
    "FC:65:DE": "Amazon", "44:65:0D": "Amazon", "A0:02:DC": "Amazon",
    "34:D2:70": "Amazon", "40:B4:CD": "Amazon", "74:C2:46": "Amazon",
    "F0:F0:A4": "Amazon",
    # ── Xiaomi / Redmi ───────────────────────────────────────────────────
    "74:EB:51": "Xiaomi", "64:CE:73": "Xiaomi", "50:EC:50": "Xiaomi",
    "78:02:F8": "Xiaomi", "28:6C:07": "Xiaomi", "7C:49:EB": "Xiaomi",
    "58:44:98": "Xiaomi", "9C:2E:A1": "Xiaomi",
    # ── Huawei / Honor ───────────────────────────────────────────────────
    "88:28:B3": "Huawei", "D4:6A:A8": "Huawei", "48:8E:EF": "Huawei",
    "70:8C:B6": "Huawei", "A4:93:3F": "Huawei", "CC:A2:23": "Huawei",
    # ── Microsoft (Xbox, Surface) ────────────────────────────────────────
    "7C:1E:52": "Microsoft", "28:18:78": "Microsoft", "B4:0E:DE": "Microsoft",
    # ── Sony (PlayStation, headphones) ───────────────────────────────────
    "AC:89:95": "Sony", "78:C8:81": "Sony", "04:5D:4B": "Sony",
    "B0:05:94": "Sony", "FC:0F:E6": "Sony",
    # ── Nintendo ─────────────────────────────────────────────────────────
    "7C:BB:8A": "Nintendo", "D8:6B:F7": "Nintendo", "58:BD:A3": "Nintendo",
    "98:41:5C": "Nintendo", "04:03:D6": "Nintendo",
    # ── Bose ─────────────────────────────────────────────────────────────
    "04:52:C7": "Bose", "2C:41:A1": "Bose", "D8:90:E8": "Bose",
    # ── JBL / Harman ─────────────────────────────────────────────────────
    "00:18:6B": "JBL", "40:ED:98": "JBL", "E8:07:BF": "Harman",
    # ── Sennheiser ───────────────────────────────────────────────────────
    "00:1B:66": "Sennheiser", "00:13:17": "Sennheiser",
    # ── Jabra / GN Audio ─────────────────────────────────────────────────
    "50:C9:71": "Jabra", "70:BF:92": "Jabra", "1C:48:F9": "Jabra",
    # ── Bang & Olufsen ───────────────────────────────────────────────────
    "F0:1D:BC": "B&O", "AC:71:FF": "B&O",
    # ── Beats (Apple subsidiary) ─────────────────────────────────────────
    "48:A9:D2": "Beats", "20:3C:AE": "Beats",
    # ── Anker / Soundcore ────────────────────────────────────────────────
    "EC:81:93": "Anker", "AC:12:2F": "Anker",
    # ── Skullcandy ───────────────────────────────────────────────────────
    "00:09:A7": "Skullcandy",
    # ── Sonos ────────────────────────────────────────────────────────────
    "78:28:CA": "Sonos", "B8:E9:37": "Sonos", "48:A6:B8": "Sonos",
    # ── TP-Link / Kasa ───────────────────────────────────────────────────
    "F0:72:EA": "TP-Link", "28:BD:89": "TP-Link", "B0:E4:D5": "TP-Link",
    "50:C7:BF": "TP-Link", "68:FF:7B": "TP-Link",
    # ── Tile ─────────────────────────────────────────────────────────────
    "E4:AA:EC": "Tile", "54:EF:FE": "Tile",
    # ── Chipolo ──────────────────────────────────────────────────────────
    "D0:03:4B": "Chipolo",
    # ── Fitbit ───────────────────────────────────────────────────────────
    "C8:FF:77": "Fitbit", "60:78:70": "Fitbit", "4C:EB:42": "Fitbit",
    # ── Garmin ───────────────────────────────────────────────────────────
    "C8:2B:96": "Garmin", "00:80:91": "Garmin", "C0:6B:55": "Garmin",
    # ── Polar ────────────────────────────────────────────────────────────
    "00:22:D0": "Polar",
    # ── Whoop ────────────────────────────────────────────────────────────
    "E8:EB:11": "Whoop",
    # ── Oura ─────────────────────────────────────────────────────────────
    "38:71:DE": "Oura",
    # ── Tesla ────────────────────────────────────────────────────────────
    "4C:FC:AA": "Tesla", "04:CB:88": "Tesla",
    # ── Ring doorbell ────────────────────────────────────────────────────
    "90:48:9A": "Ring",
    # ── Wyze ─────────────────────────────────────────────────────────────
    "2C:AA:8E": "Wyze",
    # ── Roku ─────────────────────────────────────────────────────────────
    "B8:3E:59": "Roku", "AC:3A:7A": "Roku",
    # ── Philips Hue ──────────────────────────────────────────────────────
    "00:17:88": "Philips Hue", "EC:B5:FA": "Philips Hue",
    # ── IKEA Tradfri ─────────────────────────────────────────────────────
    "D0:CF:5E": "IKEA",
    # ── Ecobee ───────────────────────────────────────────────────────────
    "44:61:32": "Ecobee",
    # ── iRobot / Roomba ──────────────────────────────────────────────────
    "50:14:79": "iRobot",
    # ── Honeywell / Resideo ──────────────────────────────────────────────
    "40:01:7A": "Honeywell",
    # ── August / Yale locks ──────────────────────────────────────────────
    "BC:7E:8B": "August",
    # ── Nordic Semiconductor (BLE chip vendor) ───────────────────────────
    "C8:FD:19": "Nordic Semi", "F0:CA:F0": "Nordic Semi",
    "E4:22:A5": "Nordic Semi", "D0:B5:C2": "Nordic Semi",
    # ── Texas Instruments (BLE chip vendor) ──────────────────────────────
    "B0:B4:48": "TI", "98:07:2D": "TI", "54:6C:0E": "TI",
    "90:59:AF": "TI", "A0:E6:F8": "TI", "20:CD:39": "TI",
    # ── Dialog Semiconductor ─────────────────────────────────────────────
    "80:EA:CA": "Dialog Semi", "80:E1:26": "Dialog Semi",
    # ── Silicon Labs ─────────────────────────────────────────────────────
    "84:2E:14": "Silicon Labs", "00:0B:57": "Silicon Labs",
    # ── Broadcom ─────────────────────────────────────────────────────────
    "20:02:AF": "Broadcom",
    # ── Raspberry Pi ─────────────────────────────────────────────────────
    "B8:27:EB": "Raspberry Pi", "DC:A6:32": "Raspberry Pi",
    "E4:5F:01": "Raspberry Pi",
    # ── OnePlus ──────────────────────────────────────────────────────────
    "C0:EE:FB": "OnePlus", "94:65:2D": "OnePlus",
    # ── Oppo / Realme ────────────────────────────────────────────────────
    "A8:93:4A": "Oppo",
    # ── Motorola ─────────────────────────────────────────────────────────
    "D8:E0:E1": "Motorola", "E8:B4:C8": "Motorola",
    # ── LG ───────────────────────────────────────────────────────────────
    "10:68:3F": "LG", "CC:2D:83": "LG",
    # ── Omron (medical) ──────────────────────────────────────────────────
    "00:30:D2": "Omron",
    # ── Withings ─────────────────────────────────────────────────────────
    "00:24:E4": "Withings",
    # ── Govee ────────────────────────────────────────────────────────────
    "A4:C1:38": "Govee",
    # ── Tuya IoT ─────────────────────────────────────────────────────────
    "D8:1F:12": "Tuya",
    # ── DJI (drones — 12 known OUI prefixes) ───────────────────────────
    "60:60:1F": "DJI", "48:1C:B9": "DJI", "8C:58:23": "DJI",
    "0C:9A:E6": "DJI", "E4:7A:2C": "DJI", "88:29:85": "DJI",
    "58:B8:58": "DJI", "34:D2:62": "DJI", "04:A8:5A": "DJI",
    "4C:43:F6": "DJI", "9C:5A:8A": "DJI", "EC:72:F7": "DJI",
    # ── Parrot (drones) ──────────────────────────────────────────────────
    "90:3A:E6": "Parrot", "00:12:1C": "Parrot", "90:03:B7": "Parrot",
    "A0:14:3D": "Parrot", "00:26:7E": "Parrot",
    # ── Skydio (drones) ──────────────────────────────────────────────────
    "38:1D:14": "Skydio", "58:D5:6E": "Skydio",
    # ── Autel (drones) ──────────────────────────────────────────────────
    "2C:DC:AD": "Autel", "78:8C:B5": "Autel",
    # ── More DJI (from drone reference) ──────────────────────────────────
    "08:D4:6A": "DJI", "D0:32:9A": "DJI", "C4:2F:90": "DJI",
    # ── Hikvision (surveillance cameras — 30 most common of 82 known blocks)
    "C0:56:E3": "Hikvision", "28:57:BE": "Hikvision", "44:19:B6": "Hikvision",
    "54:C4:15": "Hikvision", "4C:BD:8F": "Hikvision", "00:BC:99": "Hikvision",
    "04:03:12": "Hikvision", "08:3B:C1": "Hikvision", "08:A1:89": "Hikvision",
    "10:12:FB": "Hikvision", "18:68:CB": "Hikvision", "24:28:FD": "Hikvision",
    "48:78:5B": "Hikvision", "4C:F5:DC": "Hikvision", "50:E5:38": "Hikvision",
    "58:03:FB": "Hikvision", "64:DB:8B": "Hikvision", "80:48:9F": "Hikvision",
    "84:94:59": "Hikvision", "8C:E7:48": "Hikvision", "98:DF:82": "Hikvision",
    "A4:14:37": "Hikvision", "AC:B9:2F": "Hikvision", "BC:AD:28": "Hikvision",
    "C4:2F:90": "Hikvision", "D4:E8:53": "Hikvision", "E0:CA:3C": "Hikvision",
    "EC:C8:9C": "Hikvision", "F8:4D:FC": "Hikvision", "FC:9F:FD": "Hikvision",
    # ── EZVIZ (Hikvision subsidiary) ─────────────────────────────────────
    "94:EC:13": "EZVIZ", "20:BB:BC": "EZVIZ", "58:8F:CF": "EZVIZ",
    "64:F2:FB": "EZVIZ", "78:C1:AE": "EZVIZ",
    # ── Dahua (surveillance cameras — all 27 known blocks) ───────────────
    "3C:EF:8C": "Dahua", "A0:BD:1D": "Dahua", "E0:50:8B": "Dahua",
    "08:ED:ED": "Dahua", "14:A7:8B": "Dahua", "24:52:6A": "Dahua",
    "38:AF:29": "Dahua", "3C:E3:6B": "Dahua", "4C:11:BF": "Dahua",
    "5C:F5:1A": "Dahua", "64:FD:29": "Dahua", "6C:1C:71": "Dahua",
    "74:C9:29": "Dahua", "8C:E9:B4": "Dahua", "90:02:A9": "Dahua",
    "98:F9:CC": "Dahua", "9C:14:63": "Dahua", "B4:4C:3B": "Dahua",
    "BC:32:5F": "Dahua", "C0:39:5A": "Dahua", "C4:AA:C4": "Dahua",
    "D4:43:0E": "Dahua", "E0:2E:FE": "Dahua", "E4:24:6C": "Dahua",
    # ── Ubiquiti / UniFi (network cameras + access points) ───────────────
    "00:15:6D": "Ubiquiti", "04:18:D6": "Ubiquiti", "18:E8:29": "Ubiquiti",
    "24:5A:4C": "Ubiquiti", "44:D9:E7": "Ubiquiti", "68:72:51": "Ubiquiti",
    "74:AC:B9": "Ubiquiti", "78:8A:20": "Ubiquiti", "80:2A:A8": "Ubiquiti",
    "AC:8B:A9": "Ubiquiti", "B4:FB:E4": "Ubiquiti", "DC:9F:DB": "Ubiquiti",
    "E0:63:DA": "Ubiquiti", "F4:92:BF": "Ubiquiti", "FC:EC:DA": "Ubiquiti",
    # ── Axis Communications (surveillance cameras) ───────────────────────
    "00:40:8C": "Axis", "AC:CC:8E": "Axis", "B8:A4:4F": "Axis", "E8:27:25": "Axis",
    # ── Arlo ─────────────────────────────────────────────────────────────
    "00:01:A2": "Arlo", "40:5D:82": "Arlo", "A4:15:88": "Arlo", "E4:F4:C6": "Arlo",
    # ── Reolink ──────────────────────────────────────────────────────────
    "EC:71:DB": "Reolink",
    # ── Axon (body cameras) ──────────────────────────────────────────────
    "00:25:DF": "Axon", "40:BD:32": "Axon", "80:82:23": "Axon",
    # ── Motorola Solutions (radios, body cams) ───────────────────────────
    "00:0F:9F": "Motorola Solutions", "00:15:7A": "Motorola Solutions",
    # ══ Research batch 2: 76 new entries from Codex ══════════════════════
    # ── Apple (specific blocks beyond 4C catch-all) ──────────────────────
    "00:03:93": "Apple", "00:05:02": "Apple", "00:0A:27": "Apple",
    "00:0A:95": "Apple", "00:0D:93": "Apple", "00:10:FA": "Apple",
    "00:11:24": "Apple", "00:14:51": "Apple", "00:16:CB": "Apple",
    "00:17:F2": "Apple", "00:19:E3": "Apple", "00:1B:63": "Apple",
    "00:1C:B3": "Apple", "00:1D:4F": "Apple", "00:1E:52": "Apple",
    "00:1F:5B": "Apple", "00:1F:F3": "Apple", "00:21:E9": "Apple",
    "00:22:41": "Apple",
    # ── Ring (more OUI blocks) ───────────────────────────────────────────
    "00:40:40": "Ring", "18:7F:88": "Ring", "34:3E:A4": "Ring", "54:E0:19": "Ring",
    # ── Samsung (more OUI blocks) ────────────────────────────────────────
    "00:00:F0": "Samsung", "00:02:78": "Samsung", "00:07:AB": "Samsung",
    "00:09:18": "Samsung", "00:0D:AE": "Samsung", "00:12:47": "Samsung",
    "00:12:FB": "Samsung", "00:13:77": "Samsung", "00:15:99": "Samsung",
    # ── Tuya IoT (more blocks) ───────────────────────────────────────────
    "10:5A:17": "Tuya", "10:D5:61": "Tuya", "18:69:D8": "Tuya", "18:DE:50": "Tuya",
    # ── Sony ─────────────────────────────────────────────────────────────
    "00:01:4A": "Sony", "00:0A:D9": "Sony", "00:0E:07": "Sony",
    # ── Sony PlayStation ─────────────────────────────────────────────────
    "00:04:1F": "PlayStation", "00:13:15": "PlayStation",
    # ── Tesla (more blocks) ──────────────────────────────────────────────
    "0C:29:8F": "Tesla", "54:F8:F0": "Tesla",
    # ── BMW ──────────────────────────────────────────────────────────────
    "00:01:A9": "BMW",
    # ── Ford ─────────────────────────────────────────────────────────────
    "00:26:B4": "Ford",
    # ── General Motors ───────────────────────────────────────────────────
    "00:08:12": "GM",
    # ── Mercedes ─────────────────────────────────────────────────────────
    "3C:CE:15": "Mercedes",
    # ── HTC ──────────────────────────────────────────────────────────────
    "00:09:2D": "HTC", "00:23:76": "HTC",
    # ── Vivo ─────────────────────────────────────────────────────────────
    "00:9C:C0": "Vivo", "08:23:B2": "Vivo", "08:7F:98": "Vivo",
    # ── Huawei (more blocks) ─────────────────────────────────────────────
    "00:18:82": "Huawei", "00:1E:10": "Huawei", "00:22:A1": "Huawei",
    "00:25:68": "Huawei", "00:25:9E": "Huawei",
    # ── Xiaomi (more blocks) ─────────────────────────────────────────────
    "00:9E:C8": "Xiaomi", "00:C3:0A": "Xiaomi", "00:EC:0A": "Xiaomi",
    "04:10:6B": "Xiaomi", "04:7A:0B": "Xiaomi",
    # ── Chamberlain / myQ ────────────────────────────────────────────────
    "00:15:25": "Chamberlain", "0C:95:05": "Chamberlain",
    # ── Abbott (medical) ─────────────────────────────────────────────────
    "00:13:DD": "Abbott",
    # ── Medtronic (medical) ──────────────────────────────────────────────
    "54:FA:89": "Medtronic", "DC:16:A2": "Medtronic",
    # ── Meross (IoT) ────────────────────────────────────────────────────
    "48:E1:E9": "Meross", "C4:E7:AE": "Meross",
    # ── IKEA (more blocks) ──────────────────────────────────────────────
    "68:EC:8A": "IKEA",
    # ── Beats ────────────────────────────────────────────────────────────
    "04:88:E2": "Beats",
    # ── Amazfit / Huami ──────────────────────────────────────────────────
    "88:0F:10": "Amazfit", "D8:80:3C": "Amazfit",
    # ── More Fitbit ──────────────────────────────────────────────────────
    "18:00:DB": "Fitbit", "58:A8:7B": "Fitbit",
    # ── Yale locks ───────────────────────────────────────────────────────
    "B0:44:9C": "Yale",
    # ── Govee (more blocks) ──────────────────────────────────────────────
    "D4:AD:FC": "Govee",
}


def oui_lookup(mac: str) -> str | None:
    """Look up manufacturer from MAC address (colon-separated)."""
    if not mac or len(mac) < 8:
        return None
    prefix3 = mac[:8].upper()  # XX:XX:XX
    prefix1 = mac[:2].upper()  # XX (for Apple which uses many prefixes)
    if prefix3 in _BLE_OUI:
        return _BLE_OUI[prefix3]
    if prefix1 in _BLE_OUI:
        return _BLE_OUI[prefix1]
    return None


# ---------------------------------------------------------------------------
# Enriched device record
# ---------------------------------------------------------------------------

@dataclass
class EnrichedDevice:
    fingerprint: str            # FP:XXXXXXXX
    device_type: str            # iPhone, AirTag, MacBook, Unknown, etc.
    manufacturer: str           # Apple, Samsung, etc.
    current_rssi: int
    avg_rssi: float
    min_rssi: int
    max_rssi: int
    rssi_readings: int
    first_seen: float
    last_seen: float
    last_bssid: str             # Most recent MAC (for display)
    source: str                 # ble_rid, wifi_ssid, etc.
    confidence: float
    is_tracker: bool
    seen_by: set = field(default_factory=set)
    bssids_seen: set = field(default_factory=set)   # MAC rotation tracking
    rssi_history: list = field(default_factory=list)  # last N readings
    # BLE fingerprinting data (from ESP32 scanner)
    ble_company_id: int = 0         # BT SIG company ID
    ble_apple_type: int = 0         # Apple Continuity sub-type
    ble_ad_type_count: int = 0      # Number of AD types
    ble_payload_len: int = 0        # Advertisement payload length
    ble_addr_type: int = 0          # Address type (public/random)


# ---------------------------------------------------------------------------
# Device enrichment engine
# ---------------------------------------------------------------------------

class BLEEnricher:
    """Deduplicates, enriches, and aggregates BLE/WiFi detections by fingerprint."""

    MAX_RSSI_HISTORY = 30
    STALE_TIMEOUT_S = 120

    def __init__(self):
        self.devices: dict[str, EnrichedDevice] = {}
        # Profile learning: maps behavioral signature → category
        # Signature = (manufacturer, rotation_bucket, rssi_stability_bucket)
        # Learned from high-confidence behavioral classifications
        self._learned_profiles: dict[tuple, dict] = {}  # signature → {category, count, confidence}

    def ingest(self, drone_id: str, source: str, confidence: float,
               rssi: int, bssid: str, manufacturer: str, model: str,
               device_id: str, received_at: float, **kwargs):
        """Process a detection and update enriched device state."""
        if not drone_id:
            return

        # Extract fingerprint from model field (FP:XXXXXXXX)
        fp = ""
        if model and model.startswith("FP:"):
            fp = model
        elif ":" in drone_id and len(drone_id.split(":")) >= 2:
            # Fallback: use drone_id hash portion
            parts = drone_id.split(":")
            if parts[0] == "BLE" and len(parts) >= 2:
                fp = f"FP:{parts[1]}" if len(parts[1]) == 8 else f"FP:{hash(drone_id) & 0xFFFFFFFF:08X}"
            else:
                fp = f"FP:{hash(drone_id) & 0xFFFFFFFF:08X}"
        else:
            fp = f"FP:{hash(drone_id) & 0xFFFFFFFF:08X}"

        now = received_at or time.time()

        # Enrich manufacturer from OUI if not already set
        if (not manufacturer or manufacturer in ("Unknown", "")) and bssid:
            oui_mfr = oui_lookup(bssid)
            if oui_mfr:
                manufacturer = oui_mfr

        # Device type from drone_id (BLE:HASH:TypeName)
        device_type = "Unknown"
        if ":" in drone_id:
            parts = drone_id.split(":")
            # Format: BLE:HASH:TypeName or BLE:XX:XX:XX:XX:XX:XX
            if parts[0] == "BLE" and len(parts) == 3 and not all(len(p) == 2 for p in parts[1:]):
                device_type = parts[2]

        is_tracker = device_type in ("AirTag", "FindMy Accessory", "Tile Tracker",
                                     "SmartTag", "Google Tracker", "Tracker (Generic)")

        if fp in self.devices:
            dev = self.devices[fp]
            dev.current_rssi = rssi
            dev.last_seen = now
            dev.last_bssid = bssid or dev.last_bssid
            dev.rssi_readings += 1
            dev.seen_by.add(device_id)
            if bssid:
                dev.bssids_seen.add(bssid)

            # Update RSSI stats
            if rssi < dev.min_rssi:
                dev.min_rssi = rssi
            if rssi > dev.max_rssi:
                dev.max_rssi = rssi
            dev.avg_rssi = dev.avg_rssi * 0.9 + rssi * 0.1  # EMA

            # RSSI history
            dev.rssi_history.append({"rssi": rssi, "t": now})
            if len(dev.rssi_history) > self.MAX_RSSI_HISTORY:
                dev.rssi_history.pop(0)

            # Update confidence (keep highest)
            if confidence > dev.confidence:
                dev.confidence = confidence

            # Update type if we got a better classification
            if device_type != "Unknown" and dev.device_type == "Unknown":
                dev.device_type = device_type
            if manufacturer and manufacturer != "Unknown" and dev.manufacturer == "Unknown":
                dev.manufacturer = manufacturer

        else:
            self.devices[fp] = EnrichedDevice(
                fingerprint=fp,
                device_type=device_type,
                manufacturer=manufacturer or "Unknown",
                current_rssi=rssi,
                avg_rssi=float(rssi),
                min_rssi=rssi,
                max_rssi=rssi,
                rssi_readings=1,
                first_seen=now,
                last_seen=now,
                last_bssid=bssid or "",
                source=source,
                confidence=confidence,
                is_tracker=is_tracker,
                seen_by={device_id},
                rssi_history=[{"rssi": rssi, "t": now}],
                ble_company_id=kwargs.get("ble_company_id", 0) or 0,
                ble_apple_type=kwargs.get("ble_apple_type", 0) or 0,
                ble_ad_type_count=kwargs.get("ble_ad_type_count", 0) or 0,
                ble_payload_len=kwargs.get("ble_payload_len", 0) or 0,
                ble_addr_type=kwargs.get("ble_addr_type", 0) or 0,
            )

        # Update BLE fields if provided (may arrive after initial creation)
        dev = self.devices[fp]
        cid = kwargs.get("ble_company_id", 0) or 0
        if cid and dev.ble_company_id == 0:
            dev.ble_company_id = cid
            dev.ble_apple_type = kwargs.get("ble_apple_type", 0) or 0
            dev.ble_ad_type_count = kwargs.get("ble_ad_type_count", 0) or 0
            dev.ble_payload_len = kwargs.get("ble_payload_len", 0) or 0
            dev.ble_addr_type = kwargs.get("ble_addr_type", 0) or 0
            # Use company ID for better manufacturer lookup
            from app.services.ble_company_lookup import lookup_company
            company_name, category = lookup_company(cid)
            if company_name != "Unknown" and dev.manufacturer in ("Unknown", ""):
                dev.manufacturer = company_name

    def _infer_device_category(self, dev: EnrichedDevice) -> str:
        """Infer device category from behavior when type is Unknown.

        Uses MAC rotation frequency, RSSI stability, manufacturer,
        detection duration, and observation patterns.
        """
        if dev.device_type != "Unknown":
            return dev.device_type

        mfr = dev.manufacturer
        rotations = len(dev.bssids_seen)
        age_s = dev.last_seen - dev.first_seen if dev.last_seen > dev.first_seen else 0
        readings = dev.rssi_readings
        sensor_count = len(dev.seen_by)

        # RSSI stability
        rssi_range = dev.max_rssi - dev.min_rssi if readings > 2 else 0
        # Approximate stdev from range (range ≈ 4*stdev for normal distribution)
        rssi_std_approx = rssi_range / 4.0 if readings > 3 else 99.0

        # MAC rotation rate (rotations per hour)
        age_h = age_s / 3600.0 if age_s > 0 else 0.01
        rot_per_hour = rotations / age_h if age_h > 0.01 else 0

        # Advertising rate (readings per minute)
        readings_per_min = (readings / (age_s / 60.0)) if age_s > 10 else 0

        # Position stability: if mostly seen by 1 sensor = stationary near that sensor
        # (approximated by sensor_count — low = more stable)

        # ── Known manufacturers ──────────────────────────────────────
        if mfr == "Apple":
            if rotations >= 3 and rot_per_hour > 3:
                return "iPhone/Watch"
            if rotations >= 1:
                return "Apple Device"
            return "Apple Accessory"
        if mfr == "Samsung":
            if rotations >= 2:
                return "Samsung Phone"
            return "Samsung Device"
        if mfr == "Google":
            return "Google/Nest"
        if mfr == "Espressif":
            return "IoT (ESP32)"
        if mfr in ("Xiaomi", "Oppo", "OnePlus"):
            return f"{mfr} Phone"
        if mfr == "Huawei":
            return "Huawei Device"
        if mfr == "Motorola":
            return "Motorola Phone"
        if mfr == "LG":
            return "LG Device"
        if mfr in ("Fitbit", "Garmin", "Polar", "Whoop", "Oura"):
            return "Wearable"
        if mfr in ("Sonos", "Bose", "JBL", "Harman", "Sennheiser",
                    "Jabra", "B&O", "Beats", "Anker", "Skullcandy"):
            return "Audio Device"
        if mfr in ("Amazon", "Roku", "Wyze", "Ecobee", "Honeywell",
                    "IKEA", "Govee", "Tuya", "Philips Hue"):
            return "Smart Home"
        if mfr == "Ring":
            return "Security Cam"
        if mfr == "iRobot":
            return "Robot Vacuum"
        if mfr == "August":
            return "Smart Lock"
        if mfr == "Tesla":
            return "Vehicle (Tesla)"
        if mfr in ("Microsoft", "Xbox"):
            return "Windows/Xbox"
        if mfr in ("Sony",):
            if rssi_std_approx < 4:
                return "Sony Audio"
            return "PlayStation"
        if mfr == "Nintendo":
            return "Nintendo"
        if mfr in ("TP-Link",):
            return "IoT Device"
        if mfr == "Tile":
            return "Tile Tracker"
        if mfr == "Chipolo":
            return "Chipolo Tracker"
        if mfr in ("Nordic Semi", "TI", "Dialog Semi", "Silicon Labs", "Broadcom"):
            return "IoT (BLE Chip)"
        if mfr == "Raspberry Pi":
            return "Raspberry Pi"
        if mfr == "Omron":
            return "Medical Device"
        if mfr == "Withings":
            return "Health Monitor"
        if mfr == "DJI":
            return "DJI Drone"
        if mfr in ("Parrot", "Skydio", "Autel"):
            return f"{mfr} Drone"
        if mfr in ("Hikvision", "Dahua", "EZVIZ", "Axis", "Arlo", "Reolink"):
            return "Surveillance Camera"
        if mfr in ("Ubiquiti",):
            return "Network Device"
        if mfr in ("Axon", "Motorola Solutions"):
            return "Body Camera"
        if mfr == "PlayStation":
            return "PlayStation"
        if mfr == "HTC":
            return "HTC Phone"
        if mfr == "Vivo":
            return "Vivo Phone"
        if mfr == "Amazfit":
            return "Wearable"
        if mfr == "Beats":
            return "Audio Device"
        if mfr == "Yale":
            return "Smart Lock"
        if mfr == "Chamberlain":
            return "Garage Door"
        if mfr == "Abbott":
            return "Medical Device"
        if mfr in ("BMW", "Ford", "GM", "Mercedes"):
            return f"Vehicle ({mfr})"
        if mfr == "Meross":
            return "Smart Home"

        # ── Behavioral classification (unknown manufacturer) ─────────

        # Vehicle passing: seen briefly, large RSSI swing, multiple sensors
        if age_s < 120 and rssi_range >= 18 and sensor_count >= 2:
            return "Vehicle Passing"

        # Stationary IoT: no MAC rotation, stable RSSI, long-lived
        if rotations <= 1 and rssi_std_approx < 4.5 and age_s > 300 and readings > 10:
            return "Stationary IoT"

        # Mobile phone: fast MAC rotation, seen by multiple sensors, unstable RSSI
        if rot_per_hour >= 3 and rssi_std_approx >= 5:
            return "Mobile Phone"

        # Slow rotation + moderate RSSI = wearable (watch, earbuds)
        if 0 < rot_per_hour < 2 and 3 <= rssi_std_approx <= 8 and age_s > 120:
            return "Wearable"

        # No rotation, stable, seen a while = stationary device
        if rotations <= 1 and rssi_std_approx < 6 and age_s > 60 and readings > 5:
            return "Stationary Device"

        # Rotating but not fast = Android phone
        if rotations >= 2 and rssi_std_approx >= 4:
            return "Mobile Device"

        # Brief appearance = visitor/passerby
        if age_s < 300 and readings < 10:
            return "Transient"

        # Check learned profiles
        sig = self._compute_profile_sig(dev)
        if sig in self._learned_profiles:
            learned = self._learned_profiles[sig]
            if learned["count"] >= 3 and learned["confidence"] > 0.6:
                return learned["category"]

        return "Unknown"

    def _compute_profile_sig(self, dev: EnrichedDevice) -> tuple:
        """Compute a behavioral profile signature for learning.

        Buckets continuous values so similar devices map to the same key.
        """
        mfr = dev.manufacturer
        rotations = len(dev.bssids_seen)
        age_s = dev.last_seen - dev.first_seen if dev.last_seen > dev.first_seen else 0
        rssi_range = dev.max_rssi - dev.min_rssi if dev.rssi_readings > 2 else 0

        # Rotation bucket: none(0), slow(1-2), medium(3-5), fast(6+)
        rot_bucket = 0 if rotations <= 1 else (1 if rotations <= 2 else (2 if rotations <= 5 else 3))
        # RSSI stability bucket: stable(<8), moderate(8-15), unstable(16+)
        rssi_bucket = 0 if rssi_range < 8 else (1 if rssi_range < 16 else 2)
        # Sensor count bucket
        sensor_bucket = min(len(dev.seen_by), 4)

        return (mfr, rot_bucket, rssi_bucket, sensor_bucket)

    def _learn_profile(self, dev: EnrichedDevice, category: str):
        """Record a successful classification to learn the profile."""
        if category in ("Unknown", "Transient"):
            return
        sig = self._compute_profile_sig(dev)
        if sig in self._learned_profiles:
            lp = self._learned_profiles[sig]
            if lp["category"] == category:
                lp["count"] += 1
                lp["confidence"] = min(0.95, lp["confidence"] + 0.02)
            else:
                # Conflict — reduce confidence
                lp["confidence"] = max(0.0, lp["confidence"] - 0.1)
                if lp["confidence"] < 0.3:
                    # Switch to new category
                    lp["category"] = category
                    lp["count"] = 1
                    lp["confidence"] = 0.5
        else:
            self._learned_profiles[sig] = {
                "category": category,
                "count": 1,
                "confidence": 0.5,
            }

    def get_live_devices(self, min_confidence: float = 0.0,
                         active_only: bool = True,
                         trackers_only: bool = False) -> list[dict]:
        """Return enriched device list for dashboard display."""
        now = time.time()
        result = []

        for fp, dev in self.devices.items():
            idle = now - dev.last_seen
            if active_only and idle > self.STALE_TIMEOUT_S:
                continue
            if min_confidence > 0 and dev.confidence < min_confidence:
                continue
            if trackers_only and not dev.is_tracker:
                continue

            inferred_type = self._infer_device_category(dev)
            # Learn from successful classifications
            if inferred_type != "Unknown":
                self._learn_profile(dev, inferred_type)

            result.append({
                "fingerprint": dev.fingerprint,
                "device_type": inferred_type,
                "manufacturer": dev.manufacturer,
                "is_tracker": dev.is_tracker,
                "current_rssi": dev.current_rssi,
                "avg_rssi": round(dev.avg_rssi, 1),
                "min_rssi": dev.min_rssi,
                "max_rssi": dev.max_rssi,
                "rssi_readings": dev.rssi_readings,
                "first_seen": dev.first_seen,
                "last_seen": dev.last_seen,
                "idle_s": round(idle, 1),
                "age_s": round(now - dev.first_seen, 1),
                "last_bssid": dev.last_bssid,
                "source": dev.source,
                "confidence": dev.confidence,
                "seen_by": list(dev.seen_by),
                "sensor_count": len(dev.seen_by),
                "mac_rotations": len(dev.bssids_seen),
            })

        # Sort: trackers first, then by RSSI (strongest first)
        result.sort(key=lambda d: (not d["is_tracker"], d["current_rssi"]))
        return result

    def get_summary(self) -> dict:
        """Quick stats for the dashboard header."""
        now = time.time()
        active = [d for d in self.devices.values()
                  if (now - d.last_seen) < 30]
        trackers = [d for d in active if d.is_tracker]
        classified = [d for d in active if self._infer_device_category(d) != "Unknown"]

        return {
            "total_tracked": len(self.devices),
            "active": len(active),
            "trackers_active": len(trackers),
            "classified": len(classified),
            "unclassified": len(active) - len(classified),
            "learned_profiles": len(self._learned_profiles),
        }

    def prune_stale(self):
        """Remove devices not seen for a long time."""
        now = time.time()
        stale = [fp for fp, dev in self.devices.items()
                 if (now - dev.last_seen) > 600]
        for fp in stale:
            del self.devices[fp]

    # ── Persistence ──────────────────────────────────────────────────────

    async def save_to_db(self, db) -> int:
        """Persist learned profiles and known devices to SQLite.

        Call periodically (e.g. every 60s) from the main app.
        """
        from app.models.db_models import KnownDevice, LearnedProfile
        from sqlalchemy import select
        from datetime import datetime, timezone
        saved = 0
        try:
            # Save learned profiles
            for sig_tuple, profile in self._learned_profiles.items():
                sig_str = str(sig_tuple)
                result = await db.execute(
                    select(LearnedProfile).where(LearnedProfile.signature == sig_str)
                )
                existing = result.scalar_one_or_none()
                if existing:
                    existing.category = profile["category"]
                    existing.count = profile["count"]
                    existing.confidence = profile["confidence"]
                    existing.updated_at = datetime.now(timezone.utc)
                else:
                    db.add(LearnedProfile(
                        signature=sig_str,
                        category=profile["category"],
                        count=profile["count"],
                        confidence=profile["confidence"],
                    ))
                saved += 1

            # Save known devices
            for fp, dev in self.devices.items():
                inferred = self._infer_device_category(dev)
                result = await db.execute(
                    select(KnownDevice).where(KnownDevice.fingerprint == fp)
                )
                existing = result.scalar_one_or_none()
                if existing:
                    existing.device_type = inferred
                    existing.manufacturer = dev.manufacturer
                    existing.category = inferred
                    existing.is_tracker = dev.is_tracker
                    existing.total_detections = dev.rssi_readings
                    existing.total_sensors = len(dev.seen_by)
                    existing.mac_rotations = len(dev.bssids_seen)
                    existing.best_rssi = dev.max_rssi
                    existing.avg_rssi = dev.avg_rssi
                    existing.last_seen = datetime.fromtimestamp(dev.last_seen, tz=timezone.utc)
                    existing.last_bssid = dev.last_bssid
                else:
                    db.add(KnownDevice(
                        fingerprint=fp,
                        device_type=inferred,
                        manufacturer=dev.manufacturer,
                        category=inferred,
                        is_tracker=dev.is_tracker,
                        total_detections=dev.rssi_readings,
                        total_sensors=len(dev.seen_by),
                        mac_rotations=len(dev.bssids_seen),
                        best_rssi=dev.max_rssi,
                        avg_rssi=dev.avg_rssi,
                        first_seen=datetime.fromtimestamp(dev.first_seen, tz=timezone.utc),
                        last_seen=datetime.fromtimestamp(dev.last_seen, tz=timezone.utc),
                        last_bssid=dev.last_bssid,
                    ))
                saved += 1

            await db.commit()
        except Exception as e:
            logger.warning("Failed to save BLE data to DB: %s", e)
        return saved

    async def load_from_db(self, db) -> int:
        """Load learned profiles and known device history from DB on startup."""
        from app.models.db_models import KnownDevice, LearnedProfile
        from sqlalchemy import select
        loaded = 0
        try:
            # Load learned profiles
            result = await db.execute(select(LearnedProfile))
            for lp in result.scalars().all():
                try:
                    sig_tuple = eval(lp.signature)  # Convert string back to tuple
                    self._learned_profiles[sig_tuple] = {
                        "category": lp.category,
                        "count": lp.count,
                        "confidence": lp.confidence,
                    }
                    loaded += 1
                except Exception:
                    pass

            # Load known devices (for quick classification on restart)
            result = await db.execute(select(KnownDevice))
            for kd in result.scalars().all():
                if kd.fingerprint not in self.devices:
                    # Create a minimal device record for classification lookup
                    self.devices[kd.fingerprint] = EnrichedDevice(
                        fingerprint=kd.fingerprint,
                        device_type=kd.device_type,
                        manufacturer=kd.manufacturer,
                        current_rssi=kd.best_rssi or -100,
                        avg_rssi=kd.avg_rssi or -100,
                        min_rssi=kd.best_rssi or -100,
                        max_rssi=kd.best_rssi or -100,
                        rssi_readings=kd.total_detections,
                        first_seen=kd.first_seen.timestamp() if kd.first_seen else 0,
                        last_seen=kd.last_seen.timestamp() if kd.last_seen else 0,
                        last_bssid=kd.last_bssid or "",
                        source="ble_rid",
                        confidence=0.5,
                        is_tracker=kd.is_tracker,
                    )
                    loaded += 1

            logger.info("Loaded %d BLE records from DB (%d profiles, %d devices)",
                        loaded, len(self._learned_profiles), len(self.devices))
        except Exception as e:
            logger.warning("Failed to load BLE data from DB: %s", e)
        return loaded
