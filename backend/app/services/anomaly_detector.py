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

    RSSI_SPIKE_DB = 15
    DISAPPEAR_TIMEOUT_S = 60
    DISAPPEAR_MIN_AGE_S = 120
    VELOCITY_THRESHOLD = 3.0
    STRONG_SIGNAL_DB = -30
    MAX_ALERTS = 200

    def __init__(self):
        self.tracks: dict[str, DeviceTrack] = {}
        self.ssid_bssids: dict[str, set] = defaultdict(set)
        self.known_keys: set = set()
        self.alerts: deque[AnomalyAlert] = deque(maxlen=self.MAX_ALERTS)
        self._last_prune = time.time()

        # Whitelist: SSID patterns (glob) and exact BSSIDs
        self.whitelist_ssid_patterns: set = {
            "FoF-*",            # Our own uplink APs
            "CasaChomp*",       # Home mesh
            "Hyrule*",          # Neighbor mesh
            "hyrule*",
            "xfinitywifi",      # ISP hotspot
        }
        self.whitelist_bssids: set = set()

    # ── Whitelist management ─────────────────────────────────────────────

    def add_whitelist(self, pattern: str, wl_type: str = "ssid"):
        if wl_type == "bssid":
            self.whitelist_bssids.add(pattern.upper())
        else:
            self.whitelist_ssid_patterns.add(pattern)

    def remove_whitelist(self, pattern: str, wl_type: str = "ssid"):
        if wl_type == "bssid":
            self.whitelist_bssids.discard(pattern.upper())
        else:
            self.whitelist_ssid_patterns.discard(pattern)

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

            # New device alert (only after warmup, skip BLE unknowns)
            if key not in self.known_keys and len(self.known_keys) > 10:
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
        if ssid:
            track.ssid = ssid
            self.ssid_bssids[ssid].add(bssid)

        # ── Anomaly checks (skip low-confidence noise) ───────────────

        if confidence < 0.05:
            return  # Don't generate anomalies for background noise

        # 1. RSSI spike
        delta = abs(rssi - prev_rssi)
        if delta >= self.RSSI_SPIKE_DB and len(track.readings) > 3 and now > track.alert_suppressed_until:
            direction = "jumped up" if rssi > prev_rssi else "dropped"
            sev = "warning" if delta >= 20 else "info"
            self._alert("rssi_spike", sev, key, track.ssid,
                        f"RSSI {direction} by {delta}dB: {prev_rssi}->{rssi} ({track.ssid})",
                        now, {"prev_rssi": prev_rssi, "new_rssi": rssi, "delta": delta})
            track.alert_suppressed_until = now + 10

        # 2. RSSI velocity
        vel = track.rssi_velocity
        if abs(vel) >= self.VELOCITY_THRESHOLD and now > track.alert_suppressed_until:
            direction = "APPROACHING" if vel > 0 else "DEPARTING"
            sev = "warning" if abs(vel) >= 5.0 else "info"
            self._alert("velocity", sev, key, track.ssid,
                        f"{track.ssid} {direction} at {vel:+.1f} dBm/s",
                        now, {"velocity": round(vel, 2), "rssi": rssi})
            track.alert_suppressed_until = now + 15

        # 3. Signal strength anomaly
        if rssi >= self.STRONG_SIGNAL_DB and now > track.alert_suppressed_until:
            self._alert("signal_anomaly", "warning", key, track.ssid,
                        f"Very strong signal: {track.ssid} RSSI={rssi}",
                        now, {"rssi": rssi})
            track.alert_suppressed_until = now + 60

        # 4. Spoofing detection — MESH AWARE
        if ssid and source != "ble_rid":
            bssid_set = self.ssid_bssids.get(ssid, set())
            if len(bssid_set) >= 3 and not self._same_oui(bssid_set):
                existing = [a for a in self.alerts if a.alert_type == "spoofing" and a.ssid == ssid]
                if not existing or (now - existing[-1].timestamp) > 120:
                    self._alert("spoofing", "critical", key, ssid,
                                f"Spoofing: '{ssid}' from {len(bssid_set)} different-vendor BSSIDs",
                                now, {"bssid_count": len(bssid_set),
                                      "bssids": list(bssid_set)[:5]})

        # 5. BLE MAC rotation tracking (info, not warning)
        if source == "ble_rid" and len(track.bssids_seen) > 5:
            # Many MAC rotations = tracker behavior
            existing = [a for a in self.alerts if a.alert_type == "mac_rotation" and a.device_id == key]
            if not existing or (now - existing[-1].timestamp) > 300:
                self._alert("mac_rotation", "info", key, track.ssid,
                            f"{track.ssid} rotated MAC {len(track.bssids_seen)} times (tracker behavior)",
                            now, {"rotation_count": len(track.bssids_seen)})

        # Periodic prune
        if now - self._last_prune > 30:
            self._prune(now)
            self._last_prune = now

    def _prune(self, now: float):
        stale = []
        for key, track in self.tracks.items():
            idle = now - track.last_seen
            age = now - track.first_seen

            if idle >= self.DISAPPEAR_TIMEOUT_S and age >= self.DISAPPEAR_MIN_AGE_S:
                if now > track.alert_suppressed_until:
                    # Only alert disappearance for interesting devices (not 2% confidence noise)
                    last_reading = track.readings[-1] if track.readings else None
                    if last_reading and track.avg_rssi > -85:  # was reasonably close
                        self._alert("disappearance", "warning", key, track.ssid,
                                    f"Device gone: {track.ssid} (was here {age:.0f}s, silent {idle:.0f}s, last RSSI={last_reading.rssi})",
                                    now, {"age_s": round(age), "idle_s": round(idle)})
                    track.alert_suppressed_until = now + 300

            if idle > 600:
                stale.append(key)

        for key in stale:
            del self.tracks[key]

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
            },
        }
