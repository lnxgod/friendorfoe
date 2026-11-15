"""BLE device enrichment — OUI lookups, fingerprint classification, deduplication.

Transforms raw noisy BLE detections into clean, enriched device records
suitable for dashboard display.
"""

import time
import logging
from collections import defaultdict
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)

def oui_lookup(mac: str, *, source: str | None = None, ble_addr_type: int | None = None) -> str | None:
    """Resolve a vendor offline; an address prefix is never a product signature."""
    from app.services.rf_reference import resolve_mac
    return resolve_mac(mac, source=source, ble_addr_type=ble_addr_type).vendor_short


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
    ble_addr_type: int | None = None  # 0 is explicitly public; None is unknown
    ble_ja3: str = ""               # BLE-JA3 structural profile hash
    ble_adv_interval: float = 0     # Advertisement interval in ms
    ble_apple_auth: str = ""        # Apple auth tag hex (entity linking)
    ble_name: str = ""
    class_reason: str = ""
    ble_svc_uuids: str = ""         # Comma-separated service UUIDs (hex)
    ble_apple_flags: int = 0        # Apple Nearby Info data-flags byte
    ble_activity: int = 0           # Apple activity code (0=idle, 1=audio, 2=phone, 3=video)


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
        self._learned_profiles: dict[tuple, dict] = {}
        # Apple auth tag → fingerprint mapping (for MAC rotation linking)
        self._auth_tag_cache: dict[str, str] = {}  # auth_tag_hex → fingerprint
        self._auth_tag_links: dict[str, set] = {}  # fingerprint → set of linked fingerprints

    def get_linked_fingerprints(self, fp: str) -> set:
        """Return the set of fingerprints linked to `fp` via Apple auth-tag
        detection (same physical device across MAC rotations).

        Used by EntityTracker to merge entities that are actually one device
        appearing under multiple BLE fingerprints.
        """
        if not fp:
            return set()
        return set(self._auth_tag_links.get(fp, ()))

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
            oui_mfr = oui_lookup(bssid, source=source, ble_addr_type=kwargs.get("ble_addr_type"))
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
                                     "SmartTag", "Google Tracker", "Tracker (Generic)",
                                     "Pebblebee", "Chipolo")

        if fp in self.devices:
            dev = self.devices[fp]
            dev.current_rssi = rssi
            dev.last_seen = now
            if bssid:
                dev.ble_addr_type = kwargs.get("ble_addr_type")
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
                ble_addr_type=kwargs.get("ble_addr_type"),
                ble_activity=kwargs.get("ble_activity", 0) or 0,
            )

        # Update BLE fields if provided (may arrive after initial creation)
        dev = self.devices[fp]
        cid = kwargs.get("ble_company_id", 0) or 0
        if cid and dev.ble_company_id == 0:
            dev.ble_company_id = cid
            dev.ble_apple_type = kwargs.get("ble_apple_type", 0) or 0
            dev.ble_ad_type_count = kwargs.get("ble_ad_type_count", 0) or 0
            dev.ble_payload_len = kwargs.get("ble_payload_len", 0) or 0
            # Use company ID for better manufacturer lookup
            from app.services.ble_company_lookup import lookup_company
            company_name, category = lookup_company(cid)
            if company_name != "Unknown" and dev.manufacturer in ("Unknown", ""):
                dev.manufacturer = company_name
        # Store JA3 hash
        ja3 = kwargs.get("ble_ja3") or ""
        if ja3 and not dev.ble_ja3:
            dev.ble_ja3 = ja3

        # Apple auth tag entity resolution
        auth_tag = kwargs.get("ble_apple_auth") or ""
        if auth_tag and auth_tag != "000000":
            existing_fp = self._auth_tag_cache.get(auth_tag)
            if existing_fp and existing_fp != fp:
                # Same auth tag, different fingerprint → MAC rotation detected!
                # Link these two fingerprints as the same physical device
                if fp not in self._auth_tag_links:
                    self._auth_tag_links[fp] = set()
                if existing_fp not in self._auth_tag_links:
                    self._auth_tag_links[existing_fp] = set()
                self._auth_tag_links[fp].add(existing_fp)
                self._auth_tag_links[existing_fp].add(fp)
                logger.debug("Auth tag link: %s ↔ %s (tag=%s)", fp, existing_fp, auth_tag)
            self._auth_tag_cache[auth_tag] = fp

        # Store advertisement interval
        adv_interval = kwargs.get("ble_adv_interval")
        if adv_interval and adv_interval > 0:
            dev.ble_adv_interval = adv_interval

        # Store service UUIDs and Apple data-flags byte
        if kwargs.get("ble_name"):
            dev.ble_name = kwargs["ble_name"]
        if kwargs.get("class_reason"):
            dev.class_reason = kwargs["class_reason"]
        svc = kwargs.get("ble_svc_uuids")
        if svc and not dev.ble_svc_uuids:
            dev.ble_svc_uuids = svc
        flags = kwargs.get("ble_apple_flags")
        if flags and flags > 0:
            dev.ble_apple_flags = flags
        activity = kwargs.get("ble_activity")
        if activity is not None:
            try:
                dev.ble_activity = int(activity)
            except (TypeError, ValueError):
                pass

    # Apple Continuity sub-type → descriptive label.
    # NOTE 2026-04-16: Per the furiousMAC reverse-engineering of Nearby Info
    # (0x10) and Nearby Action (0x0F), Apple does NOT broadcast iPhone/iPad/
    # Mac. Defaulting those to "iPhone" gave us a 22-phantom-iPhones problem
    # on a fleet where the user actually has iPads + Macs. We now label them
    # "Apple Device" and let the flag decoder + Handoff/AirPlay paths refine.
    _APPLE_CONTINUITY_TYPES = {
        0x02: "iBeacon",
        0x05: "AirDrop",
        0x06: "HomeKit",
        0x07: "AirPods",
        0x08: "Hey Siri",
        0x09: "AirPlay",        # → refined to "Apple Mac/TV"
        0x0B: "Apple Watch",
        0x0C: "Handoff",        # → refined to "Apple Mac/TV"
        0x0D: "WiFi Settings",
        0x0E: "Instant Hotspot",
        0x0F: "Apple Device",   # Nearby Info — was "iPhone"
        0x10: "Apple Device",   # Nearby Action — was "iPhone"
        0x12: "FindMy",
    }

    # Apple Continuity state decoding — what each type tells us about the device
    _APPLE_STATE_INFO = {
        0x02: {"activity": "ibeacon", "device_class": "beacon"},
        0x05: {"activity": "airdrop", "likely_unlocked": True, "device_class": "apple"},
        0x06: {"activity": "homekit", "device_class": "homekit_accessory"},
        0x07: {"activity": "audio_connected", "device_class": "airpods"},
        0x08: {"activity": "hey_siri", "likely_unlocked": True, "device_class": "apple"},
        0x09: {"activity": "airplay_target", "device_class": "mac_or_tv"},
        0x0B: {"activity": "watch_nearby", "device_class": "watch"},
        0x0C: {"activity": "handoff", "likely_unlocked": True, "device_class": "mac_or_tv"},
        0x0D: {"activity": "wifi_sharing", "likely_unlocked": True, "device_class": "apple"},
        0x0E: {"activity": "hotspot", "device_class": "apple"},                 # iPhones can hotspot; so can iPads
        0x0F: {"activity": "nearby_info", "likely_unlocked": False, "device_class": "apple"},
        0x10: {"activity": "nearby_action", "likely_unlocked": True, "device_class": "apple"},
        0x12: {"activity": "findmy_beacon", "device_class": "airtag"},
    }

    # Nearby Info / Nearby Action "data flags" byte — known
    # bit semantics from furiousMAC reverse engineering. We decode as hints,
    # not certainties, since Apple keeps re-assigning these across iOS
    # versions. Used to enrich the label ("Apple (AirPods in)") rather than
    # pretend we can distinguish iPhone from iPad.
    @staticmethod
    def _decode_apple_nearby_flags(apple_info: int | None) -> dict:
        """Return a dict of human-readable booleans gleaned from the
        data-flags byte. All None/unknown if byte isn't available."""
        if not apple_info:
            return {}
        return {
            "airpods_connected": bool(apple_info & 0x01),
            "wifi_on":           bool(apple_info & 0x02),
            "watch_paired":      bool(apple_info & 0x04),
            "primary_icloud":    bool(apple_info & 0x08),
            "auth_tag_present":  bool(apple_info & 0x10),
            "screen_on":         bool(apple_info & 0x20),
        }

    def _decode_apple_state(self, dev: 'EnrichedDevice') -> dict | None:
        """Decode Apple Continuity state from the advertisement type."""
        if not dev.ble_apple_type:
            return None
        state = self._APPLE_STATE_INFO.get(dev.ble_apple_type)
        if not state:
            return None
        result = dict(state)  # Return a copy
        if dev.ble_activity:
            result["activity_hint"] = {1: "audio", 2: "phone", 3: "video"}.get(dev.ble_activity)
        return result

    def _infer_device_category(self, dev: EnrichedDevice) -> str:
        """Infer device category from behavior when type is Unknown.

        Uses MAC rotation frequency, RSSI stability, manufacturer,
        detection duration, and observation patterns.
        """
        if dev.device_type != "Unknown":
            return dev.device_type

        # Scanner-assigned device types come through manufacturer field
        dt = dev.device_type
        mfr_raw = dev.manufacturer
        if mfr_raw == "Meta Glasses":
            return "Meta Smart Glasses"
        if mfr_raw == "Meta Device":
            return "Meta VR/Device"
        # Luxottica is Meta's frame manufacturer for Ray-Ban Meta and Oakley
        # Meta glasses. Any BLE advertisement with CID 0x0D53 is one of those.
        if mfr_raw.startswith("Luxottica"):
            return "Meta Smart Glasses"
        if mfr_raw == "Flipper Zero":
            return "Flipper Zero"
        if mfr_raw == "Venue Beacon":
            return "Venue Beacon"
        if mfr_raw == "Event Badge":
            return "Event Badge"
        if mfr_raw == "Mobile Key Lock":
            return "Mobile Key Lock"
        if mfr_raw == "BLE HID":
            return "BLE HID"
        if mfr_raw == "Auracast":
            return "Auracast"
        if mfr_raw in ("Vehicle", "Vehicle (Tesla)"):
            return mfr_raw
        if mfr_raw == "Camera":
            return "Action Camera"
        if mfr_raw == "E-Scooter":
            return "E-Scooter"
        if mfr_raw == "Gaming":
            return "Gaming Device"
        if mfr_raw == "Medical":
            return "Medical Device"

        # Apple Continuity sub-type gives a starting label. Most types are
        # precise (AirPods, AirTag, Watch). But 0x10/0x0F/0x05 are generic
        # "some Apple device" and get enriched below with the data-flags
        # byte. Handoff (0x0C) and AirPlay (0x09) reliably imply Mac/TV.
        if dev.ble_apple_type:
            at = dev.ble_apple_type
            if at in (0x0C, 0x09):
                # Handoff / AirPlay source → Mac or Apple TV
                return "Apple Mac/TV"
            if at in (0x10, 0x0F, 0x05, 0x08, 0x0D, 0x0E):
                # Generic "Apple device" signals — refine with data flags
                flags = self._decode_apple_nearby_flags(dev.ble_apple_flags)
                bits = []
                if flags.get("airpods_connected"): bits.append("AirPods in")
                if flags.get("watch_paired"):      bits.append("Watch paired")
                if flags.get("screen_on"):         bits.append("active")
                if bits:
                    return "Apple (" + ", ".join(bits) + ")"
                return "Apple Device"
            # Everything else (AirPods, Watch, AirTag, HomeKit, iBeacon) is
            # already precise — use the static table.
            if at in self._APPLE_CONTINUITY_TYPES:
                return self._APPLE_CONTINUITY_TYPES[at]

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
            # Honest default — "Apple Device" until a stronger signal
            # (Handoff/AirPlay/AirPods/AirTag) refines it. Previously we
            # defaulted high-rotation Apple to "iPhone/Watch", which turned
            # every Mac and iPad into a phantom iPhone.
            return "Apple Device"
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
        if mfr in ("August", "Yale"):
            return "Mobile Key Lock"
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
        if mfr == "Pebblebee":
            return "Pebblebee Tracker"
        if mfr == "Meta" or mfr.startswith("Luxottica"):
            return "Meta Device"
        if mfr == "Snap":
            return "Smart Glasses"
        if mfr == "GoPro":
            return "Action Camera"
        if mfr == "Segway":
            return "E-Scooter"
        if mfr == "Logitech":
            return "Peripheral"
        if mfr == "eufy":
            return "Security Cam"
        if mfr == "Dexcom":
            return "Medical Device (CGM)"
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
                         trackers_only: bool = False,
                         group_by_ja3: bool = True,
                         min_readings: int = 2) -> list[dict]:
        """Return enriched device list for dashboard display.

        When group_by_ja3=True, devices sharing the same BLE-JA3 hash
        are merged into a single entry (MAC rotation → same physical device).
        Devices with fewer than min_readings at weak RSSI are filtered out
        (one-off distant blips that add noise).
        """
        now = time.time()
        raw = []

        for fp, dev in self.devices.items():
            idle = now - dev.last_seen
            if active_only and idle > self.STALE_TIMEOUT_S:
                continue
            if min_confidence > 0 and dev.confidence < min_confidence:
                continue
            if trackers_only and not dev.is_tracker:
                continue
            # Skip weak one-off blips (1 reading, RSSI < -85)
            if dev.rssi_readings < min_readings and dev.max_rssi < -85:
                continue

            inferred_type = self._infer_device_category(dev)
            # Learn from successful classifications
            if inferred_type != "Unknown":
                self._learn_profile(dev, inferred_type)

            entry = {
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
                "ble_ja3": dev.ble_ja3 or None,
                "ble_company_id": dev.ble_company_id or None,
                "ble_apple_type": dev.ble_apple_type or None,
                "ble_addr_type": dev.ble_addr_type,
                "ble_name": dev.ble_name or None,
                "class_reason": dev.class_reason or None,
                "ble_svc_uuids": dev.ble_svc_uuids or None,
                "ble_apple_flags": dev.ble_apple_flags or None,
                "ble_activity": dev.ble_activity or None,
                "apple_state": self._decode_apple_state(dev),
            }

            # Add dwell info for trackers
            if entry["is_tracker"]:
                age = now - dev.first_seen
                dwell_min = int(age / 60)
                entry["dwell_minutes"] = dwell_min
                if dwell_min >= 480:
                    entry["dwell_alert"] = "stalker"
                elif dwell_min >= 120:
                    entry["dwell_alert"] = "lingering"
                elif dwell_min >= 30:
                    entry["dwell_alert"] = "watching"
                else:
                    entry["dwell_alert"] = "new"

            raw.append(entry)

        # JA3 super-grouping: merge devices with same JA3 hash
        if group_by_ja3:
            raw = self._merge_by_ja3(raw, now)

        # Sort: trackers first, then by RSSI (strongest first)
        raw.sort(key=lambda d: (not d["is_tracker"], d["current_rssi"]))
        return raw

    @staticmethod
    def _merge_by_ja3(devices: list[dict], now: float) -> list[dict]:
        """Merge devices sharing the same BLE-JA3 hash into single entries.

        Devices with rotating MACs but identical advertisement structure
        (same JA3) are likely the same physical device. Merge them:
        combine sensors, pick best RSSI, earliest first_seen, etc.
        """
        ja3_groups: dict[str, list[dict]] = defaultdict(list)
        no_ja3 = []

        for d in devices:
            ja3 = d.get("ble_ja3")
            if ja3:
                ja3_groups[ja3].append(d)
            else:
                no_ja3.append(d)

        merged = list(no_ja3)
        for ja3, group in ja3_groups.items():
            if len(group) == 1:
                merged.append(group[0])
                continue

            # Merge: pick the entry with strongest RSSI as base
            group.sort(key=lambda x: x["current_rssi"], reverse=True)
            base = dict(group[0])

            # Combine data from all entries
            all_sensors = set()
            all_fps = []
            total_readings = 0
            earliest = base["first_seen"]
            latest = base["last_seen"]

            for d in group:
                all_sensors.update(d["seen_by"])
                all_fps.append(d["fingerprint"])
                total_readings += d["rssi_readings"]
                if d["first_seen"] < earliest:
                    earliest = d["first_seen"]
                if d["last_seen"] > latest:
                    latest = d["last_seen"]
                if d["min_rssi"] < base["min_rssi"]:
                    base["min_rssi"] = d["min_rssi"]
                if d["max_rssi"] > base["max_rssi"]:
                    base["max_rssi"] = d["max_rssi"]
                # Prefer non-Unknown type
                if base["device_type"] == "Unknown" and d["device_type"] != "Unknown":
                    base["device_type"] = d["device_type"]
                if base["manufacturer"] == "Unknown" and d["manufacturer"] != "Unknown":
                    base["manufacturer"] = d["manufacturer"]

            base["seen_by"] = list(all_sensors)
            base["sensor_count"] = len(all_sensors)
            base["rssi_readings"] = total_readings
            base["first_seen"] = earliest
            base["last_seen"] = latest
            base["idle_s"] = round(now - latest, 1)
            base["age_s"] = round(now - earliest, 1)
            base["mac_rotations"] = len(group)  # Each entry = different MAC
            base["merged_fingerprints"] = all_fps
            base["merged_count"] = len(group)

            merged.append(base)

        return merged

    def get_summary(self) -> dict:
        """Quick stats for the dashboard header."""
        now = time.time()
        active = [d for d in self.devices.values()
                  if (now - d.last_seen) < 30]
        trackers = [d for d in active if d.is_tracker]
        classified = [d for d in active if self._infer_device_category(d) != "Unknown"]

        # Count unique JA3 hashes among active devices (physical device estimate)
        ja3_hashes = set()
        no_ja3_count = 0
        for d in active:
            if d.ble_ja3:
                ja3_hashes.add(d.ble_ja3)
            else:
                no_ja3_count += 1
        physical_estimate = len(ja3_hashes) + no_ja3_count

        # New devices: first_seen within last hour
        new_devices = [d for d in active
                       if (now - d.first_seen) < 3600]

        return {
            "total_tracked": len(self.devices),
            "active": len(active),
            "physical_devices": physical_estimate,
            "trackers_active": len(trackers),
            "classified": len(classified),
            "unclassified": len(active) - len(classified),
            "new_last_hour": len(new_devices),
            "learned_profiles": len(self._learned_profiles),
        }

    def prune_stale(self):
        """Remove devices not seen for a long time."""
        now = time.time()
        stale = [fp for fp, dev in self.devices.items()
                 if (now - dev.last_seen) > 600]
        for fp in stale:
            del self.devices[fp]

    # ── Entity Resolution (cross-device association) ────────────────────

    def _association_score(self, dev_a: EnrichedDevice, dev_b: EnrichedDevice) -> float:
        """Score how likely two devices belong to the same person. 0-1."""
        score = 0.0

        # 1. Shared sensors = same physical area
        shared = dev_a.seen_by & dev_b.seen_by
        if shared:
            score += 0.2 * min(len(shared) / 2.0, 1.0)

        # 2. RSSI similarity = similar distance from sensors
        rssi_diff = abs(dev_a.avg_rssi - dev_b.avg_rssi)
        if rssi_diff < 10:
            score += 0.25 * (1.0 - rssi_diff / 10.0)

        # 3. Temporal overlap = appeared at similar times
        overlap = min(dev_a.last_seen, dev_b.last_seen) - max(dev_a.first_seen, dev_b.first_seen)
        if overlap > 0:
            lifespan = max(dev_a.last_seen - dev_a.first_seen,
                           dev_b.last_seen - dev_b.first_seen, 1)
            score += 0.25 * min(overlap / lifespan, 1.0)

        # 4. Same manufacturer = likely same ecosystem
        if dev_a.manufacturer == dev_b.manufacturer and dev_a.manufacturer != "Unknown":
            score += 0.1

        # 5. Complementary device types (phone + watch, phone + earbuds)
        types = frozenset([self._infer_device_category(dev_a),
                           self._infer_device_category(dev_b)])
        complementary = [
            frozenset(["iPhone", "Apple Watch"]),
            frozenset(["iPhone", "AirPods"]),
            frozenset(["iPhone", "FindMy"]),
            frozenset(["Samsung Phone", "Samsung Device"]),
            frozenset(["iPhone", "Apple Device"]),
        ]
        if any(types == p for p in complementary):
            score += 0.2

        return min(score, 1.0)

    def find_associated_devices(self, target_fp: str, max_results: int = 5) -> list[dict]:
        """Find devices likely carried by the same person as target_fp."""
        target = self.devices.get(target_fp)
        if not target:
            return []

        now = time.time()
        candidates = []
        for fp, dev in self.devices.items():
            if fp == target_fp:
                continue
            if (now - dev.last_seen) > self.STALE_TIMEOUT_S:
                continue
            score = self._association_score(target, dev)
            if score > 0.5:
                candidates.append({
                    "fingerprint": fp,
                    "device_type": self._infer_device_category(dev),
                    "manufacturer": dev.manufacturer,
                    "score": round(score, 2),
                    "rssi": dev.current_rssi,
                    "seen_by": list(dev.seen_by),
                })

        return sorted(candidates, key=lambda x: x["score"], reverse=True)[:max_results]

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
                        source="ble_fingerprint",
                        confidence=0.5,
                        is_tracker=kd.is_tracker,
                    )
                    loaded += 1

            logger.info("Loaded %d BLE records from DB (%d profiles, %d devices)",
                        loaded, len(self._learned_profiles), len(self.devices))
        except Exception as e:
            logger.warning("Failed to load BLE data from DB: %s", e)
        return loaded
