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
    "4C": "Apple",
    "94:E6:86": "Samsung",
    "00:1A:7D": "Samsung",
    "FC:A8:9A": "Samsung",
    "D0:6B:76": "Samsung",
    "60:D1:D2": "Samsung",
    "10:B4:1D": "Espressif",
    "94:3C:C6": "Espressif",
    "30:AE:A4": "Espressif",
    "A4:CF:12": "Espressif",
    "24:0A:C4": "Espressif",
    "F0:72:EA": "TP-Link",
    "28:BD:89": "TP-Link",
    "B0:E4:D5": "TP-Link",
    "74:EB:51": "Xiaomi",
    "64:CE:73": "Xiaomi",
    "E4:AA:EC": "Tile",
    "54:EF:FE": "Tile",
    "DC:54:75": "Google",
    "A4:77:33": "Google",
    "F4:F5:D8": "Google",
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


# ---------------------------------------------------------------------------
# Device enrichment engine
# ---------------------------------------------------------------------------

class BLEEnricher:
    """Deduplicates, enriches, and aggregates BLE/WiFi detections by fingerprint."""

    MAX_RSSI_HISTORY = 30
    STALE_TIMEOUT_S = 120

    def __init__(self):
        self.devices: dict[str, EnrichedDevice] = {}

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
            )

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

            result.append({
                "fingerprint": dev.fingerprint,
                "device_type": dev.device_type,
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
        classified = [d for d in active if d.device_type != "Unknown"]

        return {
            "total_tracked": len(self.devices),
            "active": len(active),
            "trackers_active": len(trackers),
            "classified": len(classified),
            "unclassified": len(active) - len(classified),
        }

    def prune_stale(self):
        """Remove devices not seen for a long time."""
        now = time.time()
        stale = [fp for fp, dev in self.devices.items()
                 if (now - dev.last_seen) > 600]
        for fp in stale:
            del self.devices[fp]
