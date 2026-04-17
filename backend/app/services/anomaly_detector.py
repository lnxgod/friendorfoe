"""RF anomaly detection engine for drone surveillance.

Processes detection events and generates alerts for suspicious RF activity.
Noise reduction: whitelisting, fingerprint-based BLE tracking, mesh-aware spoofing.
"""

import fnmatch
import logging
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)


@dataclass
class RSSIReading:
    rssi: int
    timestamp: float
    device_id: str


@dataclass
class DeviceTrack:
    track_key: str          # fingerprint hash (BLE) or BSSID (WiFi)
    ssid: str
    first_seen: float
    last_seen: float
    source: str
    manufacturer: str
    readings: deque = field(default_factory=lambda: deque(maxlen=100))
    seen_by: set = field(default_factory=set)
    bssids_seen: set = field(default_factory=set)   # MAC rotation tracking
    alert_suppressed_until: float = 0.0
    hour_counts: dict = field(default_factory=dict)  # {hour: count} for temporal patterns
    seen_days: set = field(default_factory=set)       # Set of date ordinals

    @property
    def avg_rssi(self) -> float:
        if not self.readings:
            return -100.0
        return sum(r.rssi for r in self.readings) / len(self.readings)

    @property
    def rssi_velocity(self) -> float:
        if len(self.readings) < 3:
            return 0.0
        recent = list(self.readings)[-10:]
        if len(recent) < 2:
            return 0.0
        dt = recent[-1].timestamp - recent[0].timestamp
        if dt < 0.5:
            return 0.0
        return (recent[-1].rssi - recent[0].rssi) / dt


@dataclass
class AnomalyAlert:
    alert_type: str
    severity: str
    device_id: str
    ssid: str
    message: str
    timestamp: float
    details: dict = field(default_factory=dict)


class AnomalyDetector:
    """Stateful anomaly detection with whitelist, fingerprint tracking, mesh awareness."""

    RSSI_SPIKE_DB = 25           # 15 was too sensitive — normal WiFi fluctuates 15-20dB
    DISAPPEAR_TIMEOUT_S = 300    # 60s was too fast — phones/devices leave and return
    DISAPPEAR_MIN_AGE_S = 600    # Only alert disappearance for devices seen 10+ min
    VELOCITY_THRESHOLD = 8.0     # 3.0 was noise — need fast approach/depart to alert
    STRONG_SIGNAL_DB = -10       # -15 still triggered on nearby legitimate devices
    STRONG_SIGNAL_FIRST_SEEN_S = 30   # Only alert strong signal on recently-seen devices
    MAX_ALERTS = 200
    # Per-alert-type cooldown so the same track can't re-alert on every batch.
    # Keyed (track_key, alert_type) -> earliest next allowed alert timestamp.
    ALERT_COOLDOWN_S = 600

    # Lingering tracker thresholds (seconds, severity)
    TRACKER_DWELL_THRESHOLDS = [
        (1800, "info"),       # 30 min
        (7200, "warning"),    # 2 hours
        (28800, "critical"),  # 8 hours
    ]
    _TRACKER_KEYWORDS = {"AirTag", "FindMy", "Tile Tracker", "SmartTag",
                         "Google Tracker", "Tracker", "Chipolo", "Pebblebee"}

    def __init__(self):
        self.tracks: dict[str, DeviceTrack] = {}
        self.ssid_bssids: dict[str, set] = defaultdict(set)
        self._tracker_alert_level: dict[str, int] = {}  # key → highest threshold index alerted
        self._known_trackers: set[str] = set()  # fingerprints marked as "mine"
        self.known_keys: set = set()
        self.alerts: deque[AnomalyAlert] = deque(maxlen=self.MAX_ALERTS)
        self._last_prune = time.time()
        self._known_aps: set[str] = set()  # SSIDs of known WiFi APs (learned over time)
        self._ap_first_seen: dict[str, float] = {}  # SSID → first seen timestamp
        # (track_key, alert_type) -> earliest next allowed alert ts
        self._alert_cooldowns: dict[tuple[str, str], float] = {}

        # Whitelist: uses shared classifier whitelist (loaded from DB)
        # Import at use-time to get the live set
        self.whitelist_bssids: set = set()

    def _cooldown_active(self, key: str, alert_type: str, now: float) -> bool:
        exp = self._alert_cooldowns.get((key, alert_type), 0.0)
        return now < exp

    def _mark_cooldown(self, key: str, alert_type: str, now: float,
                       duration: float | None = None) -> None:
        self._alert_cooldowns[(key, alert_type)] = now + (duration or self.ALERT_COOLDOWN_S)

    # ── Whitelist management ─────────────────────────────────────────────

    @property
    def whitelist_ssid_patterns(self):
        """Use the shared classifier whitelist (loaded from DB)."""
        from app.services.classifier import WHITELIST_SSID_PATTERNS
        return WHITELIST_SSID_PATTERNS

    def add_whitelist(self, pattern: str, wl_type: str = "ssid"):
        if wl_type == "bssid":
            self.whitelist_bssids.add(pattern.upper())
        else:
            from app.services.classifier import WHITELIST_SSID_PATTERNS
            WHITELIST_SSID_PATTERNS.add(pattern)

    def remove_whitelist(self, pattern: str, wl_type: str = "ssid"):
        if wl_type == "bssid":
            self.whitelist_bssids.discard(pattern.upper())
        else:
            from app.services.classifier import WHITELIST_SSID_PATTERNS
            WHITELIST_SSID_PATTERNS.discard(pattern)

    def get_whitelist(self) -> dict:
        return {
            "ssid_patterns": sorted(self.whitelist_ssid_patterns),
            "bssids": sorted(self.whitelist_bssids),
        }

    def _is_whitelisted(self, ssid: str, bssid: str) -> bool:
        if bssid and bssid.upper() in self.whitelist_bssids:
            return True
        if ssid:
            for pattern in self.whitelist_ssid_patterns:
                if fnmatch.fnmatch(ssid, pattern):
                    return True
        return False

    # ── Track key: fingerprint for BLE, BSSID for WiFi ───────────────────

    @staticmethod
    def _track_key(source: str, bssid: str, drone_id: str, model: str) -> str:
        """BLE devices: track by fingerprint hash. WiFi: track by BSSID."""
        if source == "ble_rid" and model and model.startswith("FP:"):
            return model  # FP:XXXXXXXX — stable across MAC rotations
        return bssid or drone_id

    # ── Mesh detection ────────────────────────────────────────────────────

    @staticmethod
    def _same_oui(bssids: set) -> bool:
        """True if all BSSIDs share the same OUI prefix (mesh network)."""
        ouis = set()
        for b in bssids:
            parts = b.upper().split(":")
            if len(parts) >= 3:
                ouis.add(":".join(parts[:3]))
        return len(ouis) <= 1

    # ── Main ingestion ────────────────────────────────────────────────────

    def ingest(self, drone_id: str, source: str, confidence: float,
               rssi: int, ssid: str, bssid: str, manufacturer: str,
               device_id: str, received_at: float, model: str = "", **kwargs):
        now = received_at or time.time()

        # Skip whitelisted
        if self._is_whitelisted(ssid, bssid):
            return

        # Skip broadcast probes — always high RSSI, many MACs, not anomalous
        if ssid == "(broadcast)":
            return

        # Rogue AP detection — track known WiFi APs, alert on new ones
        if source == "wifi_oui" and ssid:
            if ssid not in self._known_aps:
                self._ap_first_seen.setdefault(ssid, now)
                # Only alert after warmup (30+ known APs) and if AP persists >10s
                if len(self._known_aps) > 30 and (now - self._ap_first_seen[ssid]) > 10:
                    self._alert("new_ap", "warning", bssid or ssid, ssid,
                                f"New WiFi AP detected: {ssid} (BSSID={bssid or '?'}) RSSI={rssi}",
                                now, {"ssid": ssid, "bssid": bssid, "rssi": rssi})
                    self._known_aps.add(ssid)
            else:
                pass  # Known AP, no alert
            if len(self._known_aps) <= 30:
                self._known_aps.add(ssid)  # Learning phase

        # Skip known non-drone BLE devices — iPhones, MacBooks, etc. disappearing is normal
        _NOISE_BLE_TYPES = {"iphone", "macbook", "ipad", "apple watch", "airpods",
                            "samsung phone", "samsung tablet", "audio device",
                            "smart home", "fitness tracker", "smartwatch", "unknown"}
        if drone_id:
            parts = drone_id.split(":")
            if len(parts) >= 3:
                dev_type = ":".join(parts[2:]).strip().lower()
                if dev_type in _NOISE_BLE_TYPES:
                    return

        # Determine track key
        key = self._track_key(source, bssid, drone_id, model)
        if not key:
            return

        # Get or create track
        if key not in self.tracks:
            track = DeviceTrack(
                track_key=key, ssid=ssid or drone_id,
                first_seen=now, last_seen=now,
                source=source, manufacturer=manufacturer or "",
            )
            self.tracks[key] = track

            # New device alert (only after warmup — need 50+ known devices first)
            if key not in self.known_keys and len(self.known_keys) > 50:
                # Only alert for interesting new devices, not every random BLE
                if confidence >= 0.1 or source != "ble_rid":
                    self._alert("new_device", "info", key, ssid or drone_id,
                                f"New device: {ssid or drone_id} ({manufacturer or 'unknown'}) RSSI={rssi}",
                                now, {"rssi": rssi, "source": source})

            self.known_keys.add(key)
        else:
            track = self.tracks[key]

        # Update track
        prev_rssi = track.readings[-1].rssi if track.readings else rssi
        track.readings.append(RSSIReading(rssi=rssi, timestamp=now, device_id=device_id))
        track.last_seen = now
        track.seen_by.add(device_id)
        if bssid:
            track.bssids_seen.add(bssid)

        # Temporal tracking
        from datetime import datetime
        dt = datetime.fromtimestamp(now)
        current_hour = dt.hour
        current_day = dt.toordinal()

        # Check for unusual time BEFORE incrementing (so we detect first-ever appearance at this hour)
        if (len(track.seen_days) >= 3 and track.hour_counts.get(current_hour, 0) == 0
                and not self._cooldown_active(key, "unusual_time", now)):
            sev = "warning" if 0 <= current_hour <= 5 else "info"
            self._alert("unusual_time", sev, key, track.ssid,
                        f"Device seen at hour {current_hour}:00 for first time "
                        f"(tracked {len(track.seen_days)} days)",
                        now, {"hour": current_hour, "ssid": ssid, "sensor": device_id})
            self._mark_cooldown(key, "unusual_time", now)

        track.hour_counts[current_hour] = track.hour_counts.get(current_hour, 0) + 1
        track.seen_days.add(current_day)
        if ssid:
            track.ssid = ssid
            self.ssid_bssids[ssid].add(bssid)

        # ── Anomaly checks ────────────────────────────────────────────

        if confidence < 0.05:
            return  # Don't generate anomalies for background noise

        # Skip anomaly checks for static WiFi APs — RSSI fluctuation is normal
        if source in ("wifi_oui", "wifi_probe_request"):
            return

        # 1. RSSI spike — compare against previous reading FROM THE SAME SENSOR.
        # Comparing across sensors causes false spikes (sensor A at -40, sensor B at -80 = fake 40dB jump).
        same_sensor_prev = None
        for r in reversed(track.readings):
            if r.device_id == device_id and r.timestamp != now:
                same_sensor_prev = r.rssi
                break
        if same_sensor_prev is not None:
            delta = abs(rssi - same_sensor_prev)
            if (delta >= self.RSSI_SPIKE_DB
                    and len(track.readings) > 3
                    and not self._cooldown_active(key, "rssi_spike", now)):
                direction = "jumped up" if rssi > same_sensor_prev else "dropped"
                sev = "warning" if delta >= 20 else "info"
                self._alert("rssi_spike", sev, key, track.ssid,
                            f"RSSI {direction} by {delta}dB: {same_sensor_prev}->{rssi} on {device_id} ({track.ssid})",
                            now, {"prev_rssi": same_sensor_prev, "new_rssi": rssi, "delta": delta, "sensor": device_id})
                self._mark_cooldown(key, "rssi_spike", now)

        # 2. RSSI velocity — require >=2 sensors to independently agree on direction.
        # A stationary transmitter seen at different distances by different sensors
        # must NOT register as motion; only if multiple sensors each see the same
        # sign of RSSI slope over a real time window is it physical movement.
        by_sensor: dict[str, list] = {}
        for r in track.readings:
            by_sensor.setdefault(r.device_id, []).append(r)

        sensor_velocities: list[tuple[str, float]] = []
        for sid, readings in by_sensor.items():
            if len(readings) < 3:
                continue
            recent = readings[-10:]
            dt = recent[-1].timestamp - recent[0].timestamp
            if dt < 2.0:  # need at least 2 s span per sensor
                continue
            sv = (recent[-1].rssi - recent[0].rssi) / dt
            if abs(sv) >= self.VELOCITY_THRESHOLD:
                sensor_velocities.append((sid, sv))

        # Require at least 2 sensors with same-sign velocity above threshold
        approaching = [v for _, v in sensor_velocities if v > 0]
        departing = [v for _, v in sensor_velocities if v < 0]
        if (len(approaching) >= 2 or len(departing) >= 2) \
                and not self._cooldown_active(key, "velocity", now):
            agreeing = approaching if len(approaching) >= 2 else departing
            vel_mean = sum(agreeing) / len(agreeing)
            direction = "APPROACHING" if vel_mean > 0 else "DEPARTING"
            sev = "warning" if abs(vel_mean) >= 5.0 else "info"
            self._alert("velocity", sev, key, track.ssid,
                        f"{track.ssid} {direction} at {vel_mean:+.1f} dBm/s "
                        f"(confirmed by {len(agreeing)} sensors)",
                        now, {"velocity": round(vel_mean, 2), "rssi": rssi,
                              "confirming_sensors": len(agreeing)})
            self._mark_cooldown(key, "velocity", now)

        # 3. Signal strength anomaly — only alert for recently-seen devices
        # (close encounters with existing devices generate noise otherwise).
        age = now - track.first_seen
        if (rssi >= self.STRONG_SIGNAL_DB
                and age <= self.STRONG_SIGNAL_FIRST_SEEN_S
                and not self._cooldown_active(key, "signal_anomaly", now)):
            self._alert("signal_anomaly", "warning", key, track.ssid,
                        f"Very strong signal on new device: {track.ssid} RSSI={rssi}",
                        now, {"rssi": rssi, "age_s": round(age, 1)})
            self._mark_cooldown(key, "signal_anomaly", now)

        # 4. Spoofing detection — MESH AWARE
        # Residential networks commonly have 3-5 BSSIDs (router + extenders + mesh).
        # Only alert at 8+ different-vendor BSSIDs.
        # Skip broadcast probes — always come from many different MACs.
        if ssid and source != "ble_rid" and ssid != "(broadcast)":
            bssid_set = self.ssid_bssids.get(ssid, set())
            if (len(bssid_set) >= 8 and not self._same_oui(bssid_set)
                    and not self._cooldown_active(ssid, "spoofing", now)):
                sev = "critical" if len(bssid_set) >= 15 else "warning"
                self._alert("spoofing", sev, key, ssid,
                            f"Spoofing: '{ssid}' from {len(bssid_set)} different-vendor BSSIDs",
                            now, {"bssid_count": len(bssid_set),
                                  "bssids": list(bssid_set)[:5]})
                self._mark_cooldown(ssid, "spoofing", now)

        # 5. BLE MAC rotation tracking (info, not warning)
        if source == "ble_rid" and len(track.bssids_seen) > 5:
            if not self._cooldown_active(key, "mac_rotation", now):
                self._alert("mac_rotation", "info", key, track.ssid,
                            f"{track.ssid} rotated MAC {len(track.bssids_seen)} times (tracker behavior)",
                            now, {"rotation_count": len(track.bssids_seen)})
                self._mark_cooldown(key, "mac_rotation", now)

        # 6. Lingering tracker detection
        if self._is_tracker_id(drone_id) and key not in self._known_trackers:
            age = now - track.first_seen
            idle = now - track.last_seen
            if idle < 120:  # still active
                prev_level = self._tracker_alert_level.get(key, -1)
                for i, (threshold_s, sev) in enumerate(self.TRACKER_DWELL_THRESHOLDS):
                    if age >= threshold_s and i > prev_level:
                        mins = int(age / 60)
                        hours = mins // 60
                        time_str = f"{hours}h {mins % 60}m" if hours > 0 else f"{mins}m"
                        self._alert("lingering_tracker", sev, key, track.ssid,
                                    f"Tracker lingering {time_str}: {track.ssid} RSSI={rssi}",
                                    now, {"dwell_s": round(age), "dwell_minutes": mins,
                                          "rssi": rssi, "tracker_type": drone_id.split(":")[-1] if ":" in drone_id else "unknown"})
                        self._tracker_alert_level[key] = i
                        break  # one escalation per ingestion

        # Periodic prune
        if now - self._last_prune > 30:
            self._prune(now)
            self._last_prune = now

    def _prune(self, now: float):
        # NOTE 2026-04-16: the legacy per-track-key disappearance alert was
        # removed. It fired per BSSID/fingerprint *within* this detector,
        # which meant a phone that rotated through 4 MACs generated 4
        # separate "Device gone" alerts — and did so redundantly with the
        # RF anomaly detector's own disappearance path. The authoritative
        # presence-loss signal now lives at the entity level
        # (EntityTracker._prune → EventDetector.emit_departure). This
        # detector still prunes its internal tracks table for memory.
        stale = [key for key, track in self.tracks.items() if (now - track.last_seen) > 600]
        for key in stale:
            del self.tracks[key]
            self._tracker_alert_level.pop(key, None)

    def _is_tracker_id(self, drone_id: str) -> bool:
        """Check if drone_id looks like a tracker (e.g., BLE:HASH:AirTag)."""
        if not drone_id:
            return False
        for kw in self._TRACKER_KEYWORDS:
            if kw in drone_id:
                return True
        return False

    def mark_tracker_known(self, fingerprint: str):
        """Mark a tracker as 'mine' — suppresses lingering alerts."""
        self._known_trackers.add(fingerprint)
        self._tracker_alert_level.pop(fingerprint, None)

    def unmark_tracker_known(self, fingerprint: str):
        """Remove a tracker from the known list."""
        self._known_trackers.discard(fingerprint)

    def _alert(self, alert_type, severity, device_id, ssid, message, timestamp, details=None):
        alert = AnomalyAlert(
            alert_type=alert_type, severity=severity,
            device_id=device_id, ssid=ssid,
            message=message, timestamp=timestamp,
            details=details or {},
        )
        self.alerts.append(alert)
        log_fn = logger.warning if severity in ("warning", "critical") else logger.info
        log_fn("ANOMALY [%s] %s: %s", severity.upper(), alert_type, message)

    def get_alerts(self, limit=50, severity=None, alert_type=None):
        now = time.time()
        items = list(self.alerts)
        if severity:
            items = [a for a in items if a.severity == severity]
        if alert_type:
            items = [a for a in items if a.alert_type == alert_type]
        items = items[-limit:]
        items.reverse()
        return [{
            "alert_type": a.alert_type, "severity": a.severity,
            "device_id": a.device_id, "ssid": a.ssid,
            "message": a.message, "timestamp": a.timestamp,
            "age_s": round(now - a.timestamp, 1), "details": a.details,
        } for a in items]

    def get_tracked_devices(self):
        now = time.time()
        return [{
            "track_key": t.track_key, "ssid": t.ssid, "source": t.source,
            "manufacturer": t.manufacturer, "avg_rssi": round(t.avg_rssi, 1),
            "last_rssi": t.readings[-1].rssi if t.readings else None,
            "rssi_velocity": round(t.rssi_velocity, 2),
            "reading_count": len(t.readings), "age_s": round(now - t.first_seen, 1),
            "idle_s": round(now - t.last_seen, 1),
            "mac_rotations": len(t.bssids_seen),
            "seen_by": list(t.seen_by),
        } for t in self.tracks.values() if (now - t.last_seen) < 300]

    def get_stats(self):
        now = time.time()
        active = [t for t in self.tracks.values() if (now - t.last_seen) < 30]
        return {
            "total_tracked": len(self.tracks),
            "active_devices": len(active),
            "total_alerts": len(self.alerts),
            "alerts_last_5min": sum(1 for a in self.alerts if now - a.timestamp < 300),
            "known_keys": len(self.known_keys),
            "whitelist_ssids": len(self.whitelist_ssid_patterns),
            "whitelist_bssids": len(self.whitelist_bssids),
            "alert_breakdown": {
                "new_device": sum(1 for a in self.alerts if a.alert_type == "new_device"),
                "rssi_spike": sum(1 for a in self.alerts if a.alert_type == "rssi_spike"),
                "velocity": sum(1 for a in self.alerts if a.alert_type == "velocity"),
                "disappearance": sum(1 for a in self.alerts if a.alert_type == "disappearance"),
                "spoofing": sum(1 for a in self.alerts if a.alert_type == "spoofing"),
                "signal_anomaly": sum(1 for a in self.alerts if a.alert_type == "signal_anomaly"),
                "mac_rotation": sum(1 for a in self.alerts if a.alert_type == "mac_rotation"),
                "lingering_tracker": sum(1 for a in self.alerts if a.alert_type == "lingering_tracker"),
            },
            "known_trackers": len(self._known_trackers),
        }
