"""Multi-sensor drone triangulation and multilateration engine.

Tracks drone observations from multiple ESP32 sensor nodes and computes
estimated drone positions using:

- **3+ sensors**: Nonlinear least-squares multilateration (iterative Gauss-Newton)
- **2 sensors**: Circle-circle intersection, pick the solution closest to the
  sensor midpoint
- **1 sensor**: Sensor position + RSSI-estimated range (range circle only)

All coordinates are WGS84 decimal degrees. Distance calculations use the
haversine formula. RSSI-to-distance uses the log-distance path loss model
with the same parameters as the ESP32 firmware.
"""

import logging
import math
import time
from collections import Counter, defaultdict, deque
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EARTH_RADIUS_M = 6_371_000.0

# RSSI → distance model
# ESP32 firmware uses 2.5 (open air). Backend uses 3.0 for mixed
# indoor/outdoor environment (walls, obstacles attenuate more).
# Mutable calibration values — updated by CalibrationManager
RSSI_REF = -50          # dBm at 1 meter (ESP32 PCB antenna realistic reference)
PATH_LOSS_EXPONENT = 3.0

def update_calibration(rssi_ref: float, path_loss: float):
    """Update the RSSI model with calibrated values."""
    global RSSI_REF, PATH_LOSS_EXPONENT, PATH_LOSS_OUTDOOR
    RSSI_REF = rssi_ref
    PATH_LOSS_EXPONENT = path_loss
    PATH_LOSS_OUTDOOR = path_loss
    import logging
    logging.getLogger(__name__).info(
        "Calibration applied: RSSI_REF=%.1f PATH_LOSS=%.2f", rssi_ref, path_loss)

# Observation staleness: must be long enough for all sensors to report
# the same device. WiFi active scans run every 2-3s but some SSIDs only
# appear every 30-60s. BLE scan cycles can take 10-30s per sensor.
# 5 minutes keeps SSIDs visible even if they skip a few scan cycles.
OBSERVATION_TTL_SEC = 300.0

# Drone-protocol observations (ASTM Remote ID / DJI IE / Beacon RID) carry
# the drone's own GPS fix. Unlike RSSI-derived distance, GPS only makes
# sense when it's FRESH — a 30 s old Location message may have the drone
# hundreds of meters from its current position. When averaging GPS across
# sensors, drop samples older than this window so a sensor that briefly
# heard the drone minutes ago doesn't anchor the aggregated position.
# Tune upward if sensors routinely take longer than this to deliver a
# batch; downward if the map feels laggy. 5 s balances typical scanner
# latency (~1-3 s end-to-end) with responsiveness to real drone motion.
RID_GPS_MAX_AGE_SEC = 5.0

# Sensor staleness: consider a sensor offline after this
SENSOR_TTL_SEC = 120.0

# Maximum iterations for Gauss-Newton solver
MAX_ITERATIONS = 20
CONVERGENCE_M = 1.0  # stop when update < 1 meter

# Maximum distance from sensor centroid for a valid position (meters)
# Rejects garbage trilateration results that land on the other side of the planet
MAX_RESULT_DISTANCE_M = 10000.0  # 10 km


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class SensorInfo:
    """Registered ESP32 sensor with its last-known GPS position."""
    device_id: str
    lat: float
    lon: float
    alt: float | None = None
    last_seen: float = 0.0  # epoch seconds
    sensor_type: str = "outdoor"  # "indoor" or "outdoor"


@dataclass
class DroneObservation:
    """A single sensor's observation of a drone."""
    device_id: str
    sensor_lat: float
    sensor_lon: float
    rssi: int | None = None
    estimated_distance_m: float | None = None
    # If the drone itself reported GPS (Remote ID / DJI IE)
    drone_lat: float | None = None
    drone_lon: float | None = None
    drone_alt: float | None = None
    heading_deg: float | None = None
    speed_mps: float | None = None
    confidence: float = 0.0
    source: str = ""
    manufacturer: str | None = None
    model: str | None = None
    ssid: str | None = None
    bssid: str | None = None
    operator_lat: float | None = None
    operator_lon: float | None = None
    operator_id: str | None = None
    timestamp: float = 0.0  # epoch seconds


@dataclass
class EmitRecord:
    """Diagnostic snapshot of a single position emit.

    Captured by SensorTracker.get_located_drones() for every LocatedDrone
    returned. Used by GET /detections/tracking/diagnostics to let us see
    why a marker appeared at a given place: which position_source fired,
    how many sensors contributed, and how far it moved from the previous
    emit. Not used to drive any behavior — pure observability.
    """
    timestamp: float
    position_source: str
    sensor_count: int
    accuracy_m: float | None
    lat: float
    lon: float
    jump_m: float
    used_sensors: list[str] = field(default_factory=list)


@dataclass
class LocatedDrone:
    """A drone with an estimated or known position, ready for map display."""
    drone_id: str
    lat: float
    lon: float
    alt: float | None = None
    heading_deg: float | None = None
    speed_mps: float | None = None
    position_source: str = "unknown"  # "gps", "trilateration", "intersection", "range_only"
    accuracy_m: float | None = None   # estimated position accuracy
    range_m: float | None = None      # for single-sensor: estimated range circle radius
    sensor_count: int = 0
    observations: list[DroneObservation] = field(default_factory=list)
    confidence: float = 0.0
    manufacturer: str | None = None
    model: str | None = None
    operator_lat: float | None = None
    operator_lon: float | None = None
    operator_id: str | None = None


# ---------------------------------------------------------------------------
# Haversine helpers
# ---------------------------------------------------------------------------

def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in meters between two WGS84 points."""
    rlat1, rlon1 = math.radians(lat1), math.radians(lon1)
    rlat2, rlon2 = math.radians(lat2), math.radians(lon2)
    dlat = rlat2 - rlat1
    dlon = rlon2 - rlon1
    a = math.sin(dlat / 2) ** 2 + math.cos(rlat1) * math.cos(rlat2) * math.sin(dlon / 2) ** 2
    return EARTH_RADIUS_M * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def _offset_m_to_deg(lat: float, dx_m: float, dy_m: float) -> tuple[float, float]:
    """Convert meter offsets (east, north) to degree offsets at a given latitude."""
    dlat = dy_m / EARTH_RADIUS_M * (180 / math.pi)
    dlon = dx_m / (EARTH_RADIUS_M * math.cos(math.radians(lat))) * (180 / math.pi)
    return dlat, dlon


def _destination_point(lat: float, lon: float, bearing_rad: float, dist_m: float) -> tuple[float, float]:
    """Compute destination point given start, bearing (radians), and distance."""
    rlat = math.radians(lat)
    rlon = math.radians(lon)
    ad = dist_m / EARTH_RADIUS_M
    lat2 = math.asin(math.sin(rlat) * math.cos(ad) + math.cos(rlat) * math.sin(ad) * math.cos(bearing_rad))
    lon2 = rlon + math.atan2(
        math.sin(bearing_rad) * math.sin(ad) * math.cos(rlat),
        math.cos(ad) - math.sin(rlat) * math.sin(lat2),
    )
    return math.degrees(lat2), math.degrees(lon2)


# ---------------------------------------------------------------------------
# RSSI → distance
# ---------------------------------------------------------------------------

# Path loss exponents by environment
PATH_LOSS_OUTDOOR = 3.0   # Open air / outdoor sensors
PATH_LOSS_INDOOR = 3.5    # Through walls / indoor sensors


def rssi_to_distance_m(rssi: int, indoor: bool = False) -> float:
    """Estimate distance from RSSI using log-distance path loss model.

    Indoor sensors use higher path loss exponent (3.5) because walls
    attenuate signal — same RSSI indoors means closer actual distance.
    """
    n = PATH_LOSS_INDOOR if indoor else PATH_LOSS_OUTDOOR
    exponent = (RSSI_REF - rssi) / (10.0 * n)
    d = 10.0 ** exponent
    return max(1.0, min(d, 200.0))  # Clamp to 200m max — property scale


# ---------------------------------------------------------------------------
# Multilateration solver (3+ sensors)
# ---------------------------------------------------------------------------

def _trilaterate(sensors: list[tuple[float, float]], distances: list[float]) -> tuple[float, float, float] | None:
    """
    Estimate position from 3+ sensor positions and estimated distances.

    Uses iterative Gauss-Newton least squares on a local ENU projection.
    Returns (lat, lon, accuracy_m) or None if solver fails.
    """
    n = len(sensors)
    if n < 3:
        return None

    # Use centroid as initial guess
    avg_lat = sum(s[0] for s in sensors) / n
    avg_lon = sum(s[1] for s in sensors) / n

    # Convert sensor positions to local ENU offsets (meters) from centroid
    sx = []
    sy = []
    for s_lat, s_lon in sensors:
        dx = _haversine_m(avg_lat, avg_lon, avg_lat, s_lon)
        if s_lon < avg_lon:
            dx = -dx
        dy = _haversine_m(avg_lat, avg_lon, s_lat, avg_lon)
        if s_lat < avg_lat:
            dy = -dy
        sx.append(dx)
        sy.append(dy)

    # Gauss-Newton iteration
    x, y = 0.0, 0.0  # initial guess = centroid

    for _ in range(MAX_ITERATIONS):
        # Compute residuals and Jacobian
        jt_j = [[0.0, 0.0], [0.0, 0.0]]
        jt_r = [0.0, 0.0]

        for i in range(n):
            dx = x - sx[i]
            dy = y - sy[i]
            d_est = math.sqrt(dx * dx + dy * dy)
            if d_est < 0.01:
                d_est = 0.01

            residual = d_est - distances[i]

            # Jacobian row: [dx/d_est, dy/d_est]
            jx = dx / d_est
            jy = dy / d_est

            # Accumulate J^T * J and J^T * r
            jt_j[0][0] += jx * jx
            jt_j[0][1] += jx * jy
            jt_j[1][0] += jy * jx
            jt_j[1][1] += jy * jy
            jt_r[0] += jx * residual
            jt_r[1] += jy * residual

        # Solve 2x2 system: jt_j * delta = -jt_r
        det = jt_j[0][0] * jt_j[1][1] - jt_j[0][1] * jt_j[1][0]
        if abs(det) < 1e-12:
            break

        delta_x = -(jt_j[1][1] * jt_r[0] - jt_j[0][1] * jt_r[1]) / det
        delta_y = -(-jt_j[1][0] * jt_r[0] + jt_j[0][0] * jt_r[1]) / det

        x += delta_x
        y += delta_y

        if math.sqrt(delta_x ** 2 + delta_y ** 2) < CONVERGENCE_M:
            break

    # Convert back to lat/lon
    dlat, dlon = _offset_m_to_deg(avg_lat, x, y)
    result_lat = avg_lat + dlat
    result_lon = avg_lon + dlon

    # Compute accuracy as RMS residual
    rms = 0.0
    for i in range(n):
        dx = x - sx[i]
        dy = y - sy[i]
        d_est = math.sqrt(dx * dx + dy * dy)
        rms += (d_est - distances[i]) ** 2
    accuracy = math.sqrt(rms / n)

    return result_lat, result_lon, accuracy


# ---------------------------------------------------------------------------
# Two-sensor intersection
# ---------------------------------------------------------------------------

def _intersect_two(
    s1: tuple[float, float], d1: float,
    s2: tuple[float, float], d2: float,
) -> tuple[float, float, float] | None:
    """
    Two-circle intersection: returns the point closest to the midpoint
    of the two sensors, plus accuracy estimate.
    """
    dist_between = _haversine_m(s1[0], s1[1], s2[0], s2[1])
    if dist_between < 1.0:
        return None  # sensors at same location

    # If circles don't overlap, use weighted midpoint
    if dist_between > d1 + d2:
        w1 = 1.0 / max(d1, 1.0)
        w2 = 1.0 / max(d2, 1.0)
        wt = w1 + w2
        mid_lat = s1[0] * (w1 / wt) + s2[0] * (w2 / wt)
        mid_lon = s1[1] * (w1 / wt) + s2[1] * (w2 / wt)
        accuracy = dist_between  # poor accuracy
        return mid_lat, mid_lon, accuracy

    # Circles overlap: compute intersection points
    # Work in local coordinates (meters from s1)
    bearing = math.atan2(
        math.radians(s2[1] - s1[1]) * math.cos(math.radians(s2[0])),
        math.radians(s2[0] - s1[0]),
    )

    # Distance along baseline from s1 to the intersection chord
    a = (d1 * d1 - d2 * d2 + dist_between * dist_between) / (2 * dist_between)

    h_sq = d1 * d1 - a * a
    if h_sq < 0:
        h_sq = 0
    h = math.sqrt(h_sq)

    # Point on baseline at distance a from s1
    mid_lat, mid_lon = _destination_point(s1[0], s1[1], bearing, a)

    # Two intersection points are offset perpendicular by ±h
    perp1 = bearing + math.pi / 2
    perp2 = bearing - math.pi / 2
    p1_lat, p1_lon = _destination_point(mid_lat, mid_lon, perp1, h)
    p2_lat, p2_lon = _destination_point(mid_lat, mid_lon, perp2, h)

    # Pick the one closest to the midpoint of the two sensors
    mid_s_lat = (s1[0] + s2[0]) / 2
    mid_s_lon = (s1[1] + s2[1]) / 2

    d_p1 = _haversine_m(p1_lat, p1_lon, mid_s_lat, mid_s_lon)
    d_p2 = _haversine_m(p2_lat, p2_lon, mid_s_lat, mid_s_lon)

    if d_p1 <= d_p2:
        return p1_lat, p1_lon, h  # accuracy ~ perpendicular offset
    else:
        return p2_lat, p2_lon, h


# ---------------------------------------------------------------------------
# Sensor Tracker (the main stateful service)
# ---------------------------------------------------------------------------

class SensorTracker:
    """
    Stateful service that tracks ESP32 sensors and drone observations.

    Call `ingest()` when a detection batch arrives. Call `get_located_drones()`
    to get the current map view with triangulated positions.
    """

    # How far a position must move between consecutive emits before we log
    # a WARNING-level "jump" — tuned for "did a marker teleport?" triage.
    LARGE_JUMP_THRESHOLD_M = 50.0
    # Emit history depth per drone_id (roughly 3–5 min at 1 Hz polling).
    EMIT_HISTORY_MAX = 200

    def __init__(self) -> None:
        self.sensors: dict[str, SensorInfo] = {}
        # drone_id -> {device_id -> DroneObservation}
        self.observations: dict[str, dict[str, DroneObservation]] = {}
        # RSSI smoothing: (drone_id, device_id) -> [rssi_values]
        self._rssi_history: dict[tuple[str, str], list[int]] = {}
        # EKF position filter
        from app.services.position_filter import PositionFilterManager
        self._ekf = PositionFilterManager(stale_timeout=OBSERVATION_TTL_SEC)

        # ── Diagnostics (populated by get_located_drones via _emit) ─────
        # drone_id -> ring buffer of recent emits
        self._emit_history: dict[str, deque] = defaultdict(
            lambda: deque(maxlen=self.EMIT_HISTORY_MAX)
        )
        # drone_id -> {"prev_source→new_source": count}
        self._source_flip_counts: dict[str, dict[str, int]] = defaultdict(
            lambda: defaultdict(int)
        )
        # (drone_id, position_source) -> emit count
        self._emit_counters: Counter = Counter()
        # Global counter of large-jump WARNINGs for the dashboard summary.
        self._large_jump_count: int = 0

    def _record_emit(self, located: "LocatedDrone") -> None:
        """Record one emit decision for diagnostics. Pure side-effect."""
        drone_id = located.drone_id
        source = located.position_source or "unknown"
        hist = self._emit_history[drone_id]
        prev = hist[-1] if hist else None
        jump_m = 0.0
        if prev is not None:
            jump_m = _haversine_m(prev.lat, prev.lon, located.lat, located.lon)
        used_sensors = [o.device_id for o in located.observations]
        record = EmitRecord(
            timestamp=time.time(),
            position_source=source,
            sensor_count=located.sensor_count,
            accuracy_m=located.accuracy_m,
            lat=located.lat,
            lon=located.lon,
            jump_m=jump_m,
            used_sensors=used_sensors,
        )
        hist.append(record)
        self._emit_counters[(drone_id, source)] += 1
        if prev is not None and prev.position_source != source:
            key = f"{prev.position_source}→{source}"
            self._source_flip_counts[drone_id][key] += 1
        if jump_m >= self.LARGE_JUMP_THRESHOLD_M:
            self._large_jump_count += 1
            prev_source = prev.position_source if prev else "(none)"
            prev_sensors = prev.sensor_count if prev else 0
            logger.warning(
                "tracking.jump drone=%s %s→%s jump=%.0fm sensors=%d→%d acc=%s used=%s",
                drone_id, prev_source, source, jump_m,
                prev_sensors, located.sensor_count,
                f"{located.accuracy_m:.0f}" if located.accuracy_m is not None else "?",
                used_sensors,
            )

    def ingest(
        self,
        device_id: str,
        device_lat: float | None,
        device_lon: float | None,
        device_alt: float | None,
        drone_id: str,
        *,
        rssi: int | None = None,
        estimated_distance_m: float | None = None,
        sensor_type: str = "outdoor",
        drone_lat: float | None = None,
        drone_lon: float | None = None,
        drone_alt: float | None = None,
        heading_deg: float | None = None,
        speed_mps: float | None = None,
        confidence: float = 0.0,
        source: str = "",
        manufacturer: str | None = None,
        model: str | None = None,
        ssid: str | None = None,
        bssid: str | None = None,
        operator_lat: float | None = None,
        operator_lon: float | None = None,
        operator_id: str | None = None,
    ) -> None:
        """Record a single drone observation from a sensor."""
        now = time.time()

        # Update sensor registry
        if device_lat is not None and device_lon is not None:
            self.sensors[device_id] = SensorInfo(
                device_id=device_id,
                lat=device_lat,
                lon=device_lon,
                alt=device_alt,
                last_seen=now,
                sensor_type=sensor_type,
            )
            # Update EKF origin from sensor centroid
            if not self._ekf._origin_set:
                self._ekf.set_origin(device_lat, device_lon)

        # Use fingerprint as grouping key for BLE (handles MAC rotation)
        tracking_id = drone_id
        if model and model.startswith("FP:"):
            tracking_id = model  # Use fingerprint instead of rotating drone_id

        # RSSI smoothing — EMA over last 10 values per (device, sensor)
        dist = estimated_distance_m
        if rssi is not None:
            history_key = (tracking_id, device_id)
            history = self._rssi_history.get(history_key)
            if history is None:
                history = []
                self._rssi_history[history_key] = history
            history.append(rssi)
            if len(history) > 20:
                history.pop(0)
            # EMA: very heavy smoothing to reduce position wiggling
            smoothed = history[0]
            alpha = 0.08
            for v in history[1:]:
                smoothed = smoothed * (1 - alpha) + v * alpha
            is_indoor = sensor_type == "indoor"
            dist = rssi_to_distance_m(int(round(smoothed)), indoor=is_indoor)

        obs = DroneObservation(
            device_id=device_id,
            sensor_lat=device_lat or 0.0,
            sensor_lon=device_lon or 0.0,
            rssi=rssi,
            estimated_distance_m=dist,
            drone_lat=drone_lat,
            drone_lon=drone_lon,
            drone_alt=drone_alt,
            heading_deg=heading_deg,
            speed_mps=speed_mps,
            confidence=confidence,
            source=source,
            manufacturer=manufacturer,
            model=model,
            ssid=ssid,
            bssid=bssid,
            operator_lat=operator_lat,
            operator_lon=operator_lon,
            operator_id=operator_id,
            timestamp=now,
        )

        if tracking_id not in self.observations:
            self.observations[tracking_id] = {}

        # Always update observation (smoothed RSSI handles history now)
        self.observations[tracking_id][device_id] = obs

        # Feed EKF with smoothed distance
        if dist is not None and device_lat and device_lon and device_lat != 0:
            self._ekf.update(tracking_id, device_lat, device_lon, dist)

    def prune(self) -> None:
        """Remove stale observations and sensors."""
        now = time.time()

        # Prune old observations
        to_remove = []
        for drone_id, obs_map in self.observations.items():
            expired = [did for did, obs in obs_map.items() if now - obs.timestamp > OBSERVATION_TTL_SEC]
            for did in expired:
                del obs_map[did]
            if not obs_map:
                to_remove.append(drone_id)
        for drone_id in to_remove:
            del self.observations[drone_id]

        # Drop sensor registry entries after 30 min of silence (matches
        # entity_tracker.STALE_TIMEOUT_S so the two subsystems expire state
        # on the same cadence). Sensors re-register on their next heartbeat.
        dead_sensors = [sid for sid, s in self.sensors.items() if now - s.last_seen > 1800]
        for sid in dead_sensors:
            del self.sensors[sid]

        # Clean RSSI history for pruned drones
        stale_keys = [k for k in self._rssi_history if k[0] not in self.observations]
        for k in stale_keys:
            del self._rssi_history[k]

    def get_active_sensors(self, include_offline: bool = True) -> list[SensorInfo]:
        """Return sensors. If include_offline=True, returns all known sensors."""
        self.prune()
        if include_offline:
            return list(self.sensors.values())
        return [s for s in self.sensors.values() if time.time() - s.last_seen <= SENSOR_TTL_SEC]

    def is_sensor_online(self, device_id: str) -> bool:
        """Check if a sensor has reported within the TTL window."""
        s = self.sensors.get(device_id)
        if not s:
            return False
        return time.time() - s.last_seen <= SENSOR_TTL_SEC

    def get_ekf_stats(self) -> dict:
        """Return EKF position filter statistics."""
        return self._ekf.get_stats()

    def get_located_drones(self) -> list[LocatedDrone]:
        """
        Compute positions for all currently-tracked drones.

        Priority:
        1. If ANY observation has drone GPS (Remote ID / DJI IE) → use it directly
        1.5. If EKF has a converged position (3+ updates) → use smoothed EKF position
        2. If 3+ sensors → multilaterate from RSSI distances (fallback)
        3. If 2 sensors → circle intersection (fallback)
        4. If 1 sensor → sensor position + range circle
        """
        self.prune()
        self._ekf.prune()
        results: list[LocatedDrone] = []

        for drone_id, obs_map in self.observations.items():
            observations = list(obs_map.values())
            if not observations:
                continue

            # Pick best metadata from highest-confidence observation
            best_obs = max(observations, key=lambda o: o.confidence)

            # --- Priority 1: Direct GPS from drone ---
            # Smooth GPS by averaging last N reports (weighted by recency).
            # Only count observations whose drone-reported GPS is fresh —
            # stale per-sensor caches can drag a live drone's position
            # backward toward wherever the drone was when that sensor last
            # heard a Location message.
            now_ts = time.time()
            gps_obs = [
                o for o in observations
                if o.drone_lat and o.drone_lon
                and o.drone_lat != 0.0 and o.drone_lon != 0.0
                and (now_ts - o.timestamp) <= RID_GPS_MAX_AGE_SEC
            ]
            if gps_obs:
                # Sort by time, use exponential weighting for smooth position
                gps_sorted = sorted(gps_obs, key=lambda o: o.timestamp)
                if len(gps_sorted) >= 3:
                    # Weighted average: recent positions count more
                    total_w = 0
                    avg_lat = avg_lon = avg_alt = 0.0
                    for i, g in enumerate(gps_sorted):
                        w = 2.0 ** i  # Exponential: newest gets highest weight
                        avg_lat += g.drone_lat * w
                        avg_lon += g.drone_lon * w
                        avg_alt += (g.drone_alt or 0) * w
                        total_w += w
                    smooth_lat = avg_lat / total_w
                    smooth_lon = avg_lon / total_w
                    smooth_alt = avg_alt / total_w
                else:
                    best_gps = gps_sorted[-1]
                    smooth_lat = best_gps.drone_lat
                    smooth_lon = best_gps.drone_lon
                    smooth_alt = best_gps.drone_alt

                best_gps = gps_sorted[-1]  # Use newest for metadata
                results.append(LocatedDrone(
                    drone_id=drone_id,
                    lat=smooth_lat,
                    lon=smooth_lon,
                    alt=smooth_alt,
                    heading_deg=best_gps.heading_deg,
                    speed_mps=best_gps.speed_mps,
                    position_source="gps",
                    accuracy_m=10.0,  # GPS-reported positions are ~10m accurate
                    sensor_count=len(observations),
                    observations=observations,
                    confidence=best_obs.confidence,
                    manufacturer=best_obs.manufacturer,
                    model=best_obs.model,
                    operator_lat=best_obs.operator_lat,
                    operator_lon=best_obs.operator_lon,
                    operator_id=best_obs.operator_id,
                ))
                self._record_emit(results[-1])
                continue

            # --- Priority 1.5: EKF smoothed position ---
            ekf_filter = self._ekf.filters.get(drone_id)
            if ekf_filter and ekf_filter.update_count >= 3:
                from app.services.position_filter import local_to_gps
                x, y = ekf_filter.get_position()
                ekf_lat, ekf_lon = local_to_gps(x, y, self._ekf.origin_lat, self._ekf.origin_lon)
                ekf_acc = ekf_filter.get_accuracy()
                if abs(ekf_lat) <= 90 and abs(ekf_lon) <= 180 and ekf_acc < 5000:
                    results.append(LocatedDrone(
                        drone_id=drone_id,
                        lat=ekf_lat,
                        lon=ekf_lon,
                        heading_deg=best_obs.heading_deg,
                        speed_mps=best_obs.speed_mps,
                        position_source="kalman",
                        accuracy_m=ekf_acc,
                        sensor_count=len(observations),
                        observations=observations,
                        confidence=best_obs.confidence,
                        manufacturer=best_obs.manufacturer,
                        model=best_obs.model,
                        operator_lat=best_obs.operator_lat,
                        operator_lon=best_obs.operator_lon,
                        operator_id=best_obs.operator_id,
                    ))
                    self._record_emit(results[-1])
                    continue

            # --- Collect sensors with valid positions and distance estimates ---
            usable = [
                o for o in observations
                if o.sensor_lat != 0.0 and o.sensor_lon != 0.0 and o.estimated_distance_m is not None
            ]

            # Compute sensor centroid for result validation
            if usable:
                centroid_lat = sum(o.sensor_lat for o in usable) / len(usable)
                centroid_lon = sum(o.sensor_lon for o in usable) / len(usable)
            else:
                centroid_lat = centroid_lon = 0.0

            def _result_valid(lat: float, lon: float) -> bool:
                """Reject results too far from sensor centroid or outside valid range."""
                if lat < -90 or lat > 90 or lon < -180 or lon > 180:
                    return False
                if centroid_lat == 0 and centroid_lon == 0:
                    return False
                dist = _haversine_m(centroid_lat, centroid_lon, lat, lon)
                return dist < MAX_RESULT_DISTANCE_M

            if len(usable) >= 3:
                # --- Priority 2: Trilateration (3+ sensors) ---
                sensors_pos = [(o.sensor_lat, o.sensor_lon) for o in usable]
                distances = [o.estimated_distance_m for o in usable]
                result = _trilaterate(sensors_pos, distances)
                if result:
                    lat, lon, accuracy = result
                    if _result_valid(lat, lon):
                        results.append(LocatedDrone(
                            drone_id=drone_id,
                            lat=lat,
                            lon=lon,
                            heading_deg=best_obs.heading_deg,
                            speed_mps=best_obs.speed_mps,
                            position_source="trilateration",
                            accuracy_m=accuracy,
                            sensor_count=len(usable),
                            observations=observations,
                            confidence=best_obs.confidence,
                            manufacturer=best_obs.manufacturer,
                            model=best_obs.model,
                            operator_lat=best_obs.operator_lat,
                            operator_lon=best_obs.operator_lon,
                            operator_id=best_obs.operator_id,
                        ))
                        self._record_emit(results[-1])
                        continue

            if len(usable) == 2:
                # --- Priority 3: Two-circle intersection ---
                s1 = (usable[0].sensor_lat, usable[0].sensor_lon)
                s2 = (usable[1].sensor_lat, usable[1].sensor_lon)
                d1 = usable[0].estimated_distance_m
                d2 = usable[1].estimated_distance_m
                result = _intersect_two(s1, d1, s2, d2)
                if result:
                    lat, lon, accuracy = result
                    if not _result_valid(lat, lon):
                        result = None  # fall through to range_only
                if result:
                    lat, lon, accuracy = result
                    results.append(LocatedDrone(
                        drone_id=drone_id,
                        lat=lat,
                        lon=lon,
                        heading_deg=best_obs.heading_deg,
                        speed_mps=best_obs.speed_mps,
                        position_source="intersection",
                        accuracy_m=accuracy,
                        sensor_count=2,
                        observations=observations,
                        confidence=best_obs.confidence,
                        manufacturer=best_obs.manufacturer,
                        model=best_obs.model,
                        operator_lat=best_obs.operator_lat,
                        operator_lon=best_obs.operator_lon,
                        operator_id=best_obs.operator_id,
                    ))
                    self._record_emit(results[-1])
                    continue

            # --- Priority 4: Single sensor with range estimate ---
            if usable:
                obs = usable[0]
                results.append(LocatedDrone(
                    drone_id=drone_id,
                    lat=obs.sensor_lat,
                    lon=obs.sensor_lon,
                    heading_deg=obs.heading_deg,
                    speed_mps=obs.speed_mps,
                    position_source="range_only",
                    range_m=obs.estimated_distance_m,
                    sensor_count=1,
                    observations=observations,
                    confidence=obs.confidence,
                    manufacturer=obs.manufacturer,
                    model=obs.model,
                    operator_lat=obs.operator_lat,
                    operator_lon=obs.operator_lon,
                    operator_id=obs.operator_id,
                ))
                self._record_emit(results[-1])
            elif observations:
                # No distance estimate at all — just show sensor position
                obs = observations[0]
                if obs.sensor_lat != 0.0 and obs.sensor_lon != 0.0:
                    results.append(LocatedDrone(
                        drone_id=drone_id,
                        lat=obs.sensor_lat,
                        lon=obs.sensor_lon,
                        position_source="range_only",
                        sensor_count=1,
                        observations=observations,
                        confidence=obs.confidence,
                        manufacturer=obs.manufacturer,
                        model=obs.model,
                    ))
                    self._record_emit(results[-1])

        return results
