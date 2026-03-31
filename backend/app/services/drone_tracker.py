"""Automated Remote ID Drone Tracking Orchestrator.

When a confirmed drone (BLE RID, WiFi Beacon RID, DJI IE) is detected,
automatically assigns ONE sensor node to lock on and track it.
Other nodes continue scanning for triangulation + spoof detection.

Lifecycle: 45s lock-on → 10s cooldown → re-evaluate → repeat
Only 1 node tracks at a time per drone.
"""

import logging
import time
from dataclasses import dataclass, field
from math import radians, sin, cos, sqrt, atan2

logger = logging.getLogger(__name__)

# Sources that indicate a real Remote ID drone
CONFIRMED_DRONE_SOURCES = {"ble_rid", "wifi_beacon_rid", "wifi_dji_ie"}

TRACK_DURATION_S = 45
COOLDOWN_S = 10


@dataclass
class TrackingSession:
    drone_id: str
    source: str                          # ble_rid, wifi_beacon_rid, wifi_dji_ie
    assigned_node: str                   # "uplink_4"
    channel: int = 0                     # WiFi channel (0 = BLE)
    bssid: str = ""                      # WiFi BSSID or BLE MAC
    started_at: float = 0.0
    duration_s: int = TRACK_DURATION_S
    cooldown_until: float = 0.0
    drone_lat: float = 0.0
    drone_lon: float = 0.0
    drone_alt: float = 0.0
    rssi_by_node: dict = field(default_factory=dict)
    detection_count: int = 0
    spoof_score: float = 0.0            # 0 = legit, 1 = likely spoofed
    spoof_reason: str = ""


class DroneTracker:
    """Manages automated drone tracking sessions across sensor nodes."""

    def __init__(self):
        # Active tracking sessions: drone_id → TrackingSession
        self.sessions: dict[str, TrackingSession] = {}
        # Per-node lock-on commands (polled by uplinks)
        self.node_commands: dict[str, dict] = {}
        # History of completed sessions for dashboard
        self.history: list[dict] = []
        self._max_history = 100

    def on_detection(self, drone_id: str, source: str, confidence: float,
                     rssi: int, device_id: str, ssid: str = "",
                     bssid: str = "", channel: int = 0,
                     drone_lat: float = 0, drone_lon: float = 0,
                     drone_alt: float = 0, **kwargs):
        """Called on every detection during ingestion. Manages tracking lifecycle."""
        if source not in CONFIRMED_DRONE_SOURCES:
            return

        now = time.time()
        session = self.sessions.get(drone_id)

        if session:
            # Update existing session
            session.rssi_by_node[device_id] = rssi
            session.detection_count += 1
            if drone_lat != 0 and drone_lon != 0:
                session.drone_lat = drone_lat
                session.drone_lon = drone_lon
                session.drone_alt = drone_alt

            # Check if expired
            elapsed = now - session.started_at
            if elapsed >= session.duration_s:
                if session.cooldown_until == 0:
                    # Enter cooldown
                    session.cooldown_until = now + COOLDOWN_S
                    self._release_node(session)
                    logger.info("Tracking %s: cooldown (was on %s, %d detections)",
                                drone_id, session.assigned_node, session.detection_count)
                elif now >= session.cooldown_until:
                    # Cooldown over → re-evaluate
                    self._complete_session(session)
                    del self.sessions[drone_id]
                    # Will start new session on next detection
                    self._start_session(drone_id, source, rssi, device_id,
                                        bssid, channel, drone_lat, drone_lon, drone_alt)
            else:
                # Active session — run spoof check
                self._check_spoof(session)
        else:
            # No active session — start one
            self._start_session(drone_id, source, rssi, device_id,
                                bssid, channel, drone_lat, drone_lon, drone_alt)

    def _start_session(self, drone_id: str, source: str, rssi: int,
                       device_id: str, bssid: str, channel: int,
                       drone_lat: float, drone_lon: float, drone_alt: float):
        """Start a new tracking session, assigning the best node."""
        # Check if we already have RSSI data from multiple nodes
        # (from the ring buffer / recent detections)
        now = time.time()

        # For now, use the reporting node as the tracker
        # In future: query recent RSSI across all nodes and pick strongest
        best_node = device_id

        session = TrackingSession(
            drone_id=drone_id,
            source=source,
            assigned_node=best_node,
            channel=channel,
            bssid=bssid,
            started_at=now,
            drone_lat=drone_lat,
            drone_lon=drone_lon,
            drone_alt=drone_alt,
            rssi_by_node={device_id: rssi},
        )
        self.sessions[drone_id] = session

        # Issue lock-on to the assigned node
        self._issue_lockon(session)

        logger.warning("AUTO-TRACK started: %s via %s (source=%s ch=%d rssi=%d)",
                        drone_id, best_node, source, channel, rssi)

    def _issue_lockon(self, session: TrackingSession):
        """Issue a per-node lock-on command."""
        is_ble = session.source == "ble_rid"
        cmd = {
            "active": True,
            "drone_id": session.drone_id,
            "channel": session.channel,
            "bssid": session.bssid,
            "duration_s": session.duration_s,
            "issued_at": session.started_at,
            "source": session.source,
            "type": "ble" if is_ble else "wifi",
        }
        self.node_commands[session.assigned_node] = cmd

    def _release_node(self, session: TrackingSession):
        """Release the lock-on for a node."""
        node = session.assigned_node
        if node in self.node_commands:
            self.node_commands[node] = {
                "active": False,
                "drone_id": session.drone_id,
                "issued_at": time.time(),
            }

    def _complete_session(self, session: TrackingSession):
        """Archive a completed tracking session."""
        entry = {
            "drone_id": session.drone_id,
            "source": session.source,
            "assigned_node": session.assigned_node,
            "started_at": session.started_at,
            "duration_s": session.duration_s,
            "detection_count": session.detection_count,
            "nodes_seen": list(session.rssi_by_node.keys()),
            "drone_lat": session.drone_lat,
            "drone_lon": session.drone_lon,
            "spoof_score": session.spoof_score,
            "spoof_reason": session.spoof_reason,
            "completed_at": time.time(),
        }
        self.history.append(entry)
        if len(self.history) > self._max_history:
            self.history.pop(0)

    def _check_spoof(self, session: TrackingSession):
        """Cross-check drone-reported GPS against RSSI triangulation estimate.

        If the drone reports being at position X,Y but our RSSI from multiple
        nodes estimates it at A,B, and the distance is too large → likely spoofed.
        """
        if session.drone_lat == 0 or session.drone_lon == 0:
            return
        if len(session.rssi_by_node) < 2:
            return  # Need at least 2 nodes for meaningful cross-check

        # TODO: integrate with position_filter.py EKF for actual triangulated position
        # For now, flag if we have multiple nodes seeing it (basic sanity)
        # A real spoof check would compare RID-reported GPS vs EKF-estimated position
        session.spoof_score = 0.0

    def get_node_command(self, device_id: str) -> dict:
        """Get the lock-on command for a specific node (polled by uplinks)."""
        cmd = self.node_commands.get(device_id)
        if not cmd:
            return {"active": False}

        # Auto-expire
        if cmd.get("active") and cmd.get("issued_at"):
            elapsed = time.time() - cmd["issued_at"]
            duration = cmd.get("duration_s", TRACK_DURATION_S)
            if elapsed > duration + COOLDOWN_S:
                cmd["active"] = False

        return cmd

    def get_status(self) -> dict:
        """Return current tracking status for dashboard."""
        now = time.time()
        active = []
        for drone_id, s in self.sessions.items():
            elapsed = now - s.started_at
            remaining = max(0, s.duration_s - elapsed)
            in_cooldown = s.cooldown_until > 0 and now < s.cooldown_until
            active.append({
                "drone_id": drone_id,
                "source": s.source,
                "assigned_node": s.assigned_node,
                "elapsed_s": round(elapsed, 1),
                "remaining_s": round(remaining, 1),
                "in_cooldown": in_cooldown,
                "detection_count": s.detection_count,
                "nodes_reporting": list(s.rssi_by_node.keys()),
                "drone_lat": s.drone_lat,
                "drone_lon": s.drone_lon,
                "spoof_score": s.spoof_score,
            })
        return {
            "active_sessions": active,
            "total_active": len(active),
            "recent_history": self.history[-10:],
        }

    @staticmethod
    def _haversine_m(lat1, lon1, lat2, lon2) -> float:
        """Distance in meters between two GPS points."""
        R = 6371000
        dlat = radians(lat2 - lat1)
        dlon = radians(lon2 - lon1)
        a = sin(dlat / 2) ** 2 + cos(radians(lat1)) * cos(radians(lat2)) * sin(dlon / 2) ** 2
        return R * 2 * atan2(sqrt(a), sqrt(1 - a))
