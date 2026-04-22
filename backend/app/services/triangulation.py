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
from statistics import median

from app.services.probe_identity import normalize_probe_identity

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EARTH_RADIUS_M = 6_371_000.0

# RSSI → distance model
# Match the ESP32 firmware defaults until calibration overrides them.
# Mutable calibration values — updated by CalibrationManager.
RSSI_REF = -40          # dBm at 1 meter (firmware default)
PATH_LOSS_EXPONENT = 2.5

# Per-listener residual offset (dB) — captures systematic bias per receiver.
# Set by update_calibration() from the calibration result. Looked up at
# distance-estimation time:
#   corrected_rssi = observed_rssi - PER_LISTENER_OFFSET_DB.get(device_id, 0)
# A listener that consistently over-reports RSSI (sees device closer than it
# really is) gets a positive offset; subtracting flattens the bias.
PER_LISTENER_OFFSET_DB: dict[str, float] = {}

# Per-listener log-distance model (v0.63+, phone-driven calibration). Each
# sensor that yielded enough samples in a walk session gets its own
# (RSSI_REF, exponent) tuple. When present this trumps the global model
# for that sensor — local environment (brick wall, height, antenna gain)
# dominates accuracy more than per-band differences. Sensors not in the
# table fall back to the global RSSI_REF/PATH_LOSS_EXPONENT.
PER_LISTENER_MODEL: dict[str, tuple[float, float]] = {}


def update_calibration(rssi_ref: float, path_loss: float,
                        per_listener_offset_db: dict | None = None,
                        per_listener_model: dict | None = None):
    """Update the RSSI model with calibrated values.

    `per_listener_offset_db` (v0.62 inter-node calibration) and
    `per_listener_model` (v0.63 phone-walk calibration) are independent
    knobs — passing one does not clear the other. Pass an explicit empty
    dict to clear.
    """
    global RSSI_REF, PATH_LOSS_EXPONENT, PATH_LOSS_OUTDOOR
    global PER_LISTENER_OFFSET_DB, PER_LISTENER_MODEL
    RSSI_REF = rssi_ref
    PATH_LOSS_EXPONENT = path_loss
    PATH_LOSS_OUTDOOR = path_loss
    if per_listener_offset_db is not None:
        PER_LISTENER_OFFSET_DB = dict(per_listener_offset_db)
    if per_listener_model is not None:
        PER_LISTENER_MODEL = {k: tuple(v) for k, v in per_listener_model.items()}
    import logging
    logging.getLogger(__name__).info(
        "Calibration applied: RSSI_REF=%.1f PATH_LOSS=%.2f per_listener_offset=%s per_listener_model=%d nodes",
        rssi_ref, path_loss, PER_LISTENER_OFFSET_DB, len(PER_LISTENER_MODEL))

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

# Per-class observation TTL — how old an RSSI sample can be before the
# trilateration solver stops weighting it. Stationary trackers tolerate
# long gaps (they don't move, the last RSSI is still valid); moving
# threats have much shorter TTL so the solve reflects the CURRENT geometry
# rather than where the drone / person / glasses was a minute ago.
OBS_TTL_STATIONARY_S = 300.0   # AirTag / Tile / Pebblebee / Chipolo / SmartTag
OBS_TTL_MOVING_S     = 60.0    # Meta / Ray-Ban / Oakley / Quest / drones
OBS_TTL_DEFAULT_S    = 120.0   # everything else — historical SENSOR_TTL_SEC

# Stationary privacy targets such as WiFi cameras, doorbells, and AP-like
# devices are scanned at much slower cadences than moving drones. Aggregate
# over a longer window and use a robust per-sensor summary instead of the
# latest sample so staggered 25-30 s scans can still localize together.
STATIONARY_AGG_WINDOW_S = 60.0
STATIONARY_HISTORY_MAX = 32
STATIONARY_INTERSECTION_MAX_ACCURACY_M = 35.0

# Manufacturer/model substrings that classify a target's mobility profile.
# Drives per-class EKF measurement noise (position_filter.DeviceEKF.update)
# and per-class observation TTL (above). Keep synced with the Android
# GlassesDetector.computeFingerprintKey rotating-device list and the
# EntityTracker HIGH_RISK_KEYWORDS for consistent cross-layer behaviour.
_MOTION_MOVING_SUBSTRS = (
    "meta", "ray-ban", "rayban", "oakley", "luxottica",
    "quest", "smart glasses",
)
_MOTION_STATIONARY_SUBSTRS = (
    "airtag", "tile", "smarttag", "chipolo", "pebblebee",
    "findmy", "find my",
)

_DIAGNOSTIC_SOURCES = frozenset({
    "wifi_assoc",
    "ble_fingerprint",
    "wifi_oui",
})
_DRONE_LIKE_CLASSIFICATIONS = frozenset({
    "confirmed_drone",
    "likely_drone",
    "test_drone",
    "possible_drone",
})


def _motion_class_for(source: str | None,
                      manufacturer: str | None,
                      model: str | None) -> str:
    """Classify a detection's mobility profile for filter tuning.

    Returns one of: "moving", "stationary", "default".

    Cheap substring match on manufacturer + model + source. The caller
    (SensorTracker.ingest) invokes this once per observation; `in` on a
    short lowercased string is ~O(1) for our purposes.

    Priority order:
      1. Explicit stationary class in manufacturer/model (AirTag, Tile, etc.)
         — an AirTag is stationary even if it somehow arrives on a BLE
         feed, and we never want its tight noise profile overridden.
      2. Explicit moving class in manufacturer/model (Meta, Ray-Ban, Quest).
      3. Drone-specific protocols (wifi_beacon_rid, wifi_dji_ie) → moving.
         We deliberately do NOT treat BLE alone as implicit drone. `ble_rid`
         and `ble_fingerprint` must stay distinct from the WiFi RID-only
         paths because both can carry benign nearby devices.
      4. Generic WiFi AP beacons (wifi_ssid, wifi_oui) → stationary.
         Regular APs don't move and deserve the tight noise profile;
         rogue/evil-twin APs are also physically fixed once they're up.
      5. Fallback: "default" (historical noise profile).
    """
    haystack = f"{manufacturer or ''} {model or ''}".lower()
    for kw in _MOTION_STATIONARY_SUBSTRS:
        if kw in haystack:
            return "stationary"
    for kw in _MOTION_MOVING_SUBSTRS:
        if kw in haystack:
            return "moving"
    s = (source or "").lower()
    if s in ("wifi_beacon_rid", "wifi_dji_ie"):
        return "moving"
    if s in ("wifi_ssid", "wifi_oui"):
        return "stationary"
    return "default"


def _obs_ttl_for(motion_class: str) -> float:
    """Observation freshness window per mobility class."""
    if motion_class == "moving":
        return OBS_TTL_MOVING_S
    if motion_class == "stationary":
        return OBS_TTL_STATIONARY_S
    return OBS_TTL_DEFAULT_S


def _source_policy_for_observation(obs: "DroneObservation") -> str:
    """Classify whether an observation is map-grade or diagnostic-only."""
    source = (getattr(obs, "source", "") or "").lower()
    if source in _DIAGNOSTIC_SOURCES:
        return "diagnostic"
    if source == "wifi_probe_request":
        classification = (getattr(obs, "classification", "") or "").lower()
        return "drone_grade" if classification in _DRONE_LIKE_CLASSIFICATIONS else "diagnostic"
    return "drone_grade"


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
    scanner_estimated_distance_m: float | None = None
    backend_estimated_distance_m: float | None = None
    distance_source: str | None = None
    range_model: str | None = None
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
    classification: str | None = None
    ssid: str | None = None
    bssid: str | None = None
    ie_hash: str | None = None
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
PATH_LOSS_OUTDOOR = PATH_LOSS_EXPONENT   # Open air / outdoor sensors
PATH_LOSS_INDOOR = 3.5    # Through walls / indoor sensors


def _normalize_distance_m(distance_m: float | None) -> float | None:
    """Drop missing/invalid scanner distances before they enter the solver."""
    if distance_m is None:
        return None
    if not math.isfinite(distance_m) or distance_m <= 0.0:
        return None
    return float(distance_m)


def _median_or_none(values: list[float]) -> float | None:
    """Median helper that tolerates empty input."""
    if not values:
        return None
    return float(median(values))


def rssi_to_distance_m(rssi: int, indoor: bool = False,
                       device_id: str | None = None) -> float:
    """Estimate distance from RSSI using log-distance path loss model.

    Per-listener model (from phone-walk calibration) takes priority when
    available — node-by-node fit captures local environment (brick wall,
    foliage, antenna height) far better than a single global model.

    Indoor sensors fall back to a higher path loss exponent (3.5) because
    walls attenuate signal — same RSSI indoors means closer actual distance.
    """
    if device_id and device_id in PER_LISTENER_MODEL:
        rssi_ref, n = PER_LISTENER_MODEL[device_id]
    else:
        rssi_ref = RSSI_REF
        n = PATH_LOSS_INDOOR if indoor else PATH_LOSS_OUTDOOR
    exponent = (rssi_ref - rssi) / (10.0 * n)
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

    # Gauss-Newton iteration with RSSI-distance variance weighting (v0.62+).
    # Closer measurements are exponentially more reliable than far ones because
    # log-distance path-loss makes RSSI uncertainty translate to bigger meters
    # the further you go. Weight: w_i = 1 / σ²(d_i) where
    #   σ(d) ≈ d * ln(10) / (10·n)  for ±1 dB RSSI noise (n = path loss exp)
    # Cleanly reduces to: w_i ∝ 1 / d_i²  (drop the constant — it factors out).
    # This means a 5 m sensor is 36× more influential than a 30 m sensor, which
    # matches what we want: anchor the solution on the most-trustworthy ranges.
    weights = [1.0 / max(d * d, 1.0) for d in distances]

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
            w = weights[i]

            # Jacobian row: [dx/d_est, dy/d_est]
            jx = dx / d_est
            jy = dy / d_est

            # Accumulate J^T * W * J and J^T * W * r (weighted normal equations)
            jt_j[0][0] += w * jx * jx
            jt_j[0][1] += w * jx * jy
            jt_j[1][0] += w * jy * jx
            jt_j[1][1] += w * jy * jy
            jt_r[0] += w * jx * residual
            jt_r[1] += w * jy * residual

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
        # drone_id -> {device_id -> recent DroneObservation deque}
        self._observation_history: dict[str, dict[str, deque[DroneObservation]]] = defaultdict(
            lambda: defaultdict(lambda: deque(maxlen=STATIONARY_HISTORY_MAX))
        )
        # RSSI smoothing: (drone_id, device_id) -> [rssi_values]
        self._rssi_history: dict[tuple[str, str], list[int]] = {}
        # EKF position filter
        from app.services.position_filter import (
            PositionFilterManager, AlphaBetaManager, gps_to_local, local_to_gps,
        )
        # Stash on the instance so get_located_drones can use them without
        # re-importing every call.
        self._gps_to_local = gps_to_local
        self._local_to_gps = local_to_gps
        self._ekf = PositionFilterManager(stale_timeout=OBSERVATION_TTL_SEC)
        # v0.62 particle filter — runs alongside EKF, handles multi-modal
        # posteriors (ambiguous geometry) where EKF collapses to the mean.
        # Used as a fallback when EKF hasn't yet converged; becomes the
        # primary source when it has more observations than the EKF.
        from app.services.particle_filter import ParticleFilterManager
        self._pf = ParticleFilterManager()
        # v0.63: alpha-beta smoother applied to triangulated positions
        # before they leave get_located_drones() — damps the residual
        # jitter that comes from EKF's distance-scale R, keeps the dash
        # marker from "popping" between sample windows. Per-class gains.
        self._smoother = AlphaBetaManager()

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
        classification: str | None = None,
        ssid: str | None = None,
        bssid: str | None = None,
        ie_hash: str | None = None,
        operator_lat: float | None = None,
        operator_lon: float | None = None,
        operator_id: str | None = None,
        timestamp: float = 0.0,
    ) -> None:
        """Record a single drone observation from a sensor.

        `timestamp` (epoch seconds) should be the scanner's wall-clock
        capture time so multi-node observations of the same RF frame align
        to the same instant for triangulation (v0.60+ time-sync feature).
        Pass 0 / omit to fall back to receive-time."""
        now = timestamp if timestamp > 0 else time.time()

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

        # Pick a tracking_id that's stable across MAC rotation so the EKF
        # accumulates observations for the SAME physical device:
        #   - BLE fingerprints: model starts with "FP:" → use fingerprint
        #   - WiFi probes: prefer ie_hash (FNV1a of IEs, survives MAC rotation)
        #   - WiFi APs (SSID/beacon/OUI): use BSSID — it's the AP's stable ID
        #   - Drones (RID/DJI): use drone_id (already a serial number)
        tracking_id = drone_id
        if model and model.startswith("FP:"):
            tracking_id = model
        elif source == "wifi_probe_request":
            tracking_id = normalize_probe_identity(
                ie_hash=ie_hash,
                drone_id=drone_id,
                bssid=bssid,
            ).identity
        elif source in ("wifi_ssid", "wifi_oui", "wifi_beacon_rid", "wifi_dji_ie") and bssid:
            tracking_id = f"AP:{bssid}"

        # Prefer the scanner's own distance estimate when present so the
        # backend doesn't silently replace it with a different RSSI model.
        scanner_dist = _normalize_distance_m(estimated_distance_m)
        backend_dist = None
        dist = scanner_dist
        distance_source = "scanner" if scanner_dist is not None else None
        range_model = None

        # RSSI smoothing — EMA over recent values per (target, sensor)
        if rssi is not None:
            history_key = (tracking_id, device_id)
            history = self._rssi_history.get(history_key)
            if history is None:
                history = []
                self._rssi_history[history_key] = history
            history.append(rssi)
            if len(history) > 20:
                history.pop(0)
            # EMA — α gates how fast the smoothed value tracks raw RSSI.
            # Per-class so a moving target (drone, body-worn glasses) can
            # actually follow real RSSI shifts while a stationary tracker
            # (AirTag on a desk) gets aggressive smoothing that resists
            # multipath fading. Big enough effect at the property scale
            # to flatten the per-packet jitter that BLE/WiFi best-effort
            # delivery causes when scanners report at staggered cadences.
            mcls_for_alpha = _motion_class_for(source, manufacturer, model)
            alpha = {"moving": 0.20, "stationary": 0.05}.get(mcls_for_alpha, 0.08)
            smoothed = history[0]
            for v in history[1:]:
                smoothed = smoothed * (1 - alpha) + v * alpha
            is_indoor = sensor_type == "indoor"
            # v0.62: apply per-listener offset before distance conversion.
            # Defaults to 0 for listeners not in the calibration table — no
            # behavior change until a calibration with per-listener data lands.
            corrected = smoothed - PER_LISTENER_OFFSET_DB.get(device_id, 0.0)
            backend_dist = rssi_to_distance_m(
                int(round(corrected)),
                indoor=is_indoor,
                device_id=device_id,
            )
            if device_id in PER_LISTENER_MODEL:
                range_model = "per_listener"
            else:
                range_model = "global_indoor" if is_indoor else "global_outdoor"
            if dist is None:
                dist = backend_dist
                distance_source = "backend_rssi"

        obs = DroneObservation(
            device_id=device_id,
            sensor_lat=device_lat or 0.0,
            sensor_lon=device_lon or 0.0,
            rssi=rssi,
            estimated_distance_m=dist,
            scanner_estimated_distance_m=scanner_dist,
            backend_estimated_distance_m=backend_dist,
            distance_source=distance_source,
            range_model=range_model,
            drone_lat=drone_lat,
            drone_lon=drone_lon,
            drone_alt=drone_alt,
            heading_deg=heading_deg,
            speed_mps=speed_mps,
            confidence=confidence,
            source=source,
            manufacturer=manufacturer,
            model=model,
            classification=classification,
            ssid=ssid,
            bssid=bssid,
            ie_hash=ie_hash.upper() if ie_hash else None,
            operator_lat=operator_lat,
            operator_lon=operator_lon,
            operator_id=operator_id,
            timestamp=now,
        )

        if tracking_id not in self.observations:
            self.observations[tracking_id] = {}

        # Always update observation (smoothed RSSI handles history now)
        self.observations[tracking_id][device_id] = obs
        self._observation_history[tracking_id][device_id].append(obs)

        # Feed EKF with smoothed distance + the scan-time timestamp so dt
        # for the state-transition matrix reflects real elapsed time between
        # observations, not arbitrary HTTP arrival jitter.
        if (dist is not None and device_lat and device_lon and device_lat != 0
                and _source_policy_for_observation(obs) == "drone_grade"):
            # Tell the filters what kind of target this is — stationary
            # trackers get tight R, body-worn/airborne targets get wide R,
            # unknowns fall back to the historical default. See
            # position_filter.DeviceEKF.update() for the sigma profiles.
            mcls = _motion_class_for(source, manufacturer, model)
            self._ekf.update(tracking_id, device_lat, device_lon, dist,
                             timestamp=now, motion_class=mcls)
            # Mirror observation into the particle filter. Shares ENU origin
            # with the EKF. Separate state so we can A/B the two filters.
            if not self._pf._origin_set and self._ekf._origin_set:
                self._pf.set_origin(self._ekf.origin_lat, self._ekf.origin_lon)
            self._pf.update(tracking_id, device_lat, device_lon, dist,
                            timestamp=now, motion_class=mcls)

    def prune(self) -> None:
        """Remove stale observations and sensors."""
        now = time.time()

        # Prune old observations with a per-class TTL so moving threats
        # (drones, Meta Glasses, Quest) don't carry stale 2-minute-old
        # readings into the current solve. Stationary trackers keep the
        # longer window since their last RSSI is still valid when the
        # device hasn't moved. Unknown classes use the historical default.
        to_remove = []
        for drone_id, obs_map in self.observations.items():
            expired = []
            for did, obs in obs_map.items():
                mcls = _motion_class_for(
                    getattr(obs, "source", None),
                    getattr(obs, "manufacturer", None),
                    getattr(obs, "model", None),
                )
                if now - obs.timestamp > _obs_ttl_for(mcls):
                    expired.append(did)
            for did in expired:
                del obs_map[did]
            if not obs_map:
                to_remove.append(drone_id)
        for drone_id in to_remove:
            del self.observations[drone_id]

        # Prune per-sensor observation history used by the stationary
        # localization path. Keep only recent samples; older history is
        # not useful once it falls outside the stationary aggregation window.
        stale_tracking_ids = []
        for tracking_id, sensor_histories in self._observation_history.items():
            stale_devices = []
            for did, samples in sensor_histories.items():
                while samples and (now - samples[0].timestamp) > OBSERVATION_TTL_SEC:
                    samples.popleft()
                if not samples:
                    stale_devices.append(did)
            for did in stale_devices:
                del sensor_histories[did]
            if not sensor_histories:
                stale_tracking_ids.append(tracking_id)
        for tracking_id in stale_tracking_ids:
            del self._observation_history[tracking_id]

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

    def _aggregate_stationary_observations(
        self,
        tracking_id: str,
        observations: list[DroneObservation],
    ) -> list[DroneObservation]:
        """Collapse slow stationary scans into one robust sample per sensor."""
        if not observations:
            return []

        newest_ts = max((o.timestamp for o in observations), default=0.0)
        cutoff = newest_ts - STATIONARY_AGG_WINDOW_S
        history_by_sensor = self._observation_history.get(tracking_id, {})
        aggregated: list[DroneObservation] = []

        for latest in observations:
            samples = [
                s for s in history_by_sensor.get(latest.device_id, ())
                if s.timestamp >= cutoff
            ]
            if not samples:
                samples = [latest]

            used_distances = [
                d for d in (_normalize_distance_m(s.estimated_distance_m) for s in samples)
                if d is not None
            ]
            if not used_distances:
                continue

            scanner_distances = [
                d for d in (_normalize_distance_m(s.scanner_estimated_distance_m) for s in samples)
                if d is not None
            ]
            backend_distances = [
                d for d in (_normalize_distance_m(s.backend_estimated_distance_m) for s in samples)
                if d is not None
            ]
            rssi_values = [s.rssi for s in samples if s.rssi is not None]
            confidence_values = [s.confidence for s in samples]
            freshest = max(samples, key=lambda s: s.timestamp)

            scanner_distance = _median_or_none(scanner_distances)
            backend_distance = _median_or_none(backend_distances)
            if scanner_distance is not None:
                used_distance = scanner_distance
                distance_source = "scanner"
            elif backend_distance is not None:
                used_distance = backend_distance
                distance_source = "backend_rssi"
            else:
                used_distance = _median_or_none(used_distances)
                distance_source = freshest.distance_source

            aggregated.append(DroneObservation(
                device_id=freshest.device_id,
                sensor_lat=freshest.sensor_lat,
                sensor_lon=freshest.sensor_lon,
                rssi=int(round(median(rssi_values))) if rssi_values else freshest.rssi,
                estimated_distance_m=used_distance,
                scanner_estimated_distance_m=scanner_distance,
                backend_estimated_distance_m=backend_distance,
                distance_source=distance_source,
                range_model=freshest.range_model,
                drone_lat=freshest.drone_lat,
                drone_lon=freshest.drone_lon,
                drone_alt=freshest.drone_alt,
                heading_deg=freshest.heading_deg,
                speed_mps=freshest.speed_mps,
                confidence=max(confidence_values) if confidence_values else freshest.confidence,
                source=freshest.source,
                manufacturer=freshest.manufacturer,
                model=freshest.model,
                classification=freshest.classification,
                ssid=freshest.ssid,
                bssid=freshest.bssid,
                ie_hash=freshest.ie_hash,
                operator_lat=freshest.operator_lat,
                operator_lon=freshest.operator_lon,
                operator_id=freshest.operator_id,
                timestamp=freshest.timestamp,
            ))

        return aggregated

    @staticmethod
    def _best_range_anchor(observations: list[DroneObservation]) -> DroneObservation | None:
        """Pick the strongest single-sensor anchor for conservative range-only output."""
        usable = [o for o in observations if o.estimated_distance_m is not None]
        if not usable:
            return observations[0] if observations else None
        return min(
            usable,
            key=lambda o: (
                o.estimated_distance_m if o.estimated_distance_m is not None else float("inf"),
                -o.confidence,
                -o.timestamp,
            ),
        )

    def get_located_drones(self,
                           include_probe_diagnostics: bool = False) -> list[LocatedDrone]:
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
            source_policy = _source_policy_for_observation(best_obs)
            motion_class = _motion_class_for(
                best_obs.source,
                best_obs.manufacturer,
                best_obs.model,
            )

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

            # --- Priority 1.25: Stationary privacy solve path ---
            # WiFi cameras, doorbells, and other stationary privacy targets
            # report on slow, staggered cadences. Use a long aggregation
            # window + median per-sensor range instead of the latest sample,
            # and bypass the moving-target EKF/PF preference entirely.
            if motion_class == "stationary" and source_policy == "drone_grade":
                stationary_obs = self._aggregate_stationary_observations(drone_id, observations)
                stationary_usable = [
                    o for o in stationary_obs
                    if o.sensor_lat != 0.0 and o.sensor_lon != 0.0
                    and o.estimated_distance_m is not None
                ]

                if stationary_usable:
                    centroid_lat = sum(o.sensor_lat for o in stationary_usable) / len(stationary_usable)
                    centroid_lon = sum(o.sensor_lon for o in stationary_usable) / len(stationary_usable)

                    def _stationary_result_valid(lat: float, lon: float) -> bool:
                        if lat < -90 or lat > 90 or lon < -180 or lon > 180:
                            return False
                        dist = _haversine_m(centroid_lat, centroid_lon, lat, lon)
                        return dist < MAX_RESULT_DISTANCE_M

                    if len(stationary_usable) >= 3:
                        sensors_pos = [(o.sensor_lat, o.sensor_lon) for o in stationary_usable]
                        distances = [o.estimated_distance_m for o in stationary_usable]
                        result = _trilaterate(sensors_pos, distances)
                        if result:
                            lat, lon, accuracy = result
                            if _stationary_result_valid(lat, lon):
                                results.append(LocatedDrone(
                                    drone_id=drone_id,
                                    lat=lat,
                                    lon=lon,
                                    heading_deg=best_obs.heading_deg,
                                    speed_mps=best_obs.speed_mps,
                                    position_source="stationary_trilateration",
                                    accuracy_m=accuracy,
                                    sensor_count=len(stationary_usable),
                                    observations=stationary_obs,
                                    confidence=best_obs.confidence,
                                    manufacturer=best_obs.manufacturer,
                                    model=best_obs.model,
                                    operator_lat=best_obs.operator_lat,
                                    operator_lon=best_obs.operator_lon,
                                    operator_id=best_obs.operator_id,
                                ))
                                self._record_emit(results[-1])
                                continue

                    if len(stationary_usable) == 2:
                        s1 = (stationary_usable[0].sensor_lat, stationary_usable[0].sensor_lon)
                        s2 = (stationary_usable[1].sensor_lat, stationary_usable[1].sensor_lon)
                        d1 = stationary_usable[0].estimated_distance_m
                        d2 = stationary_usable[1].estimated_distance_m
                        result = _intersect_two(s1, d1, s2, d2)
                        if result:
                            lat, lon, accuracy = result
                            if (
                                _stationary_result_valid(lat, lon)
                                and accuracy <= STATIONARY_INTERSECTION_MAX_ACCURACY_M
                            ):
                                results.append(LocatedDrone(
                                    drone_id=drone_id,
                                    lat=lat,
                                    lon=lon,
                                    heading_deg=best_obs.heading_deg,
                                    speed_mps=best_obs.speed_mps,
                                    position_source="stationary_intersection",
                                    accuracy_m=accuracy,
                                    sensor_count=2,
                                    observations=stationary_obs,
                                    confidence=best_obs.confidence,
                                    manufacturer=best_obs.manufacturer,
                                    model=best_obs.model,
                                    operator_lat=best_obs.operator_lat,
                                    operator_lon=best_obs.operator_lon,
                                    operator_id=best_obs.operator_id,
                                ))
                                self._record_emit(results[-1])
                                continue

                    anchor = self._best_range_anchor(stationary_usable)
                    if anchor is not None:
                        results.append(LocatedDrone(
                            drone_id=drone_id,
                            lat=anchor.sensor_lat,
                            lon=anchor.sensor_lon,
                            heading_deg=anchor.heading_deg,
                            speed_mps=anchor.speed_mps,
                            position_source="stationary_range_only",
                            range_m=anchor.estimated_distance_m,
                            sensor_count=len(stationary_usable),
                            observations=stationary_obs,
                            confidence=best_obs.confidence,
                            manufacturer=best_obs.manufacturer,
                            model=best_obs.model,
                            operator_lat=best_obs.operator_lat,
                            operator_lon=best_obs.operator_lon,
                            operator_id=best_obs.operator_id,
                        ))
                        self._record_emit(results[-1])
                        continue

            if source_policy == "diagnostic":
                if best_obs.source != "wifi_probe_request" or not include_probe_diagnostics:
                    continue

            # --- Priority 1.5: EKF smoothed position ---
            # Pick the tighter of EKF vs particle filter when both converged.
            # The EKF is analytically optimal for Gaussian/linear cases; the
            # particle filter handles multi-modal / nonlinear posteriors.
            ekf_filter = self._ekf.filters.get(drone_id)
            pf_filter  = self._pf.filters.get(drone_id)
            ekf_ok = (ekf_filter is not None and ekf_filter.update_count >= 3)
            pf_ok  = (pf_filter is not None and pf_filter.update_count >= 3 and pf_filter._initialized)
            # If only PF converged (e.g. EKF rejected observations), use it.
            if source_policy == "drone_grade" and pf_ok and not ekf_ok:
                from app.services.position_filter import local_to_gps
                px, py = pf_filter.get_position()
                pf_lat, pf_lon = local_to_gps(px, py, self._pf.origin_lat, self._pf.origin_lon)
                pf_acc = pf_filter.get_accuracy()
                if abs(pf_lat) <= 90 and abs(pf_lon) <= 180 and pf_acc < 5000:
                    results.append(LocatedDrone(
                        drone_id=drone_id, lat=pf_lat, lon=pf_lon,
                        heading_deg=best_obs.heading_deg,
                        speed_mps=best_obs.speed_mps,
                        position_source="particle",
                        accuracy_m=pf_acc,
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
            if source_policy == "drone_grade" and ekf_filter and ekf_filter.update_count >= 3:
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
            # Enforce a tight staleness window against the newest observation so
            # trilateration doesn't mix "30-second-old" sensor reads with fresh
            # ones — that's what was anchoring the EKF to the wrong geometry.
            # 2s matches v0.60 time-sync cadence: observations within 2s are
            # genuinely the same target event, anything older is a ghost pull.
            newest_ts = max((o.timestamp for o in observations), default=0)
            usable = [
                o for o in observations
                if o.sensor_lat != 0.0 and o.sensor_lon != 0.0
                and o.estimated_distance_m is not None
                and (newest_ts == 0 or (newest_ts - o.timestamp) <= 2.0)
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
            if usable and source_policy == "drone_grade":
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
            elif observations and source_policy == "drone_grade":
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

        # Post-EKF alpha-beta smoothing pass — damps the residual jitter
        # that comes from RSSI-distance error so dashboard markers don't
        # "pop" between sample windows. Skip for sources where we already
        # have ground-truth GPS (Remote ID drones report their own pos);
        # only smooth derived positions where smoothing actually helps.
        smoothed_results = []
        now_ts = time.time()
        for d in results:
            # Trust drone-reported GPS; only smooth derived positions.
            if d.position_source in ("gps", "rid_gps", "drone_gps"):
                smoothed_results.append(d)
                continue
            if d.observations and _source_policy_for_observation(d.observations[0]) == "diagnostic":
                smoothed_results.append(d)
                continue
            mcls = _motion_class_for(
                d.observations[0].source if d.observations else None,
                d.manufacturer,
                d.model,
            )
            # Smooth in local meters so the alpha/beta scaling is in
            # physical units rather than degrees-of-arc.
            x_m, y_m = self._gps_to_local(d.lat, d.lon,
                                          self._ekf.origin_lat or d.lat,
                                          self._ekf.origin_lon or d.lon)
            # smooth_if_fresh folds the EKF position into the smoother
            # only when the EKF actually has a new observation since the
            # last call (using ekf.last_time as the freshness signal).
            # Between real RF packets it returns the peek-extrapolated
            # position — velocity carries the marker forward without
            # corrupting the velocity estimate by re-feeding stale data.
            ekf_filter = self._ekf.filters.get(d.drone_id)
            ekf_obs_time = ekf_filter.last_time if ekf_filter else now_ts
            sx, sy = self._smoother.smooth_if_fresh(
                d.drone_id, ekf_obs_time, x_m, y_m, mcls, now=now_ts,
            )
            # Sanity clamp — on the property scale we don't expect any
            # smoothed position farther than 10 km from the EKF origin.
            # Larger values would indicate a degenerate smoother state
            # (which we shouldn't ever produce, but belt-and-suspenders
            # so a future regression can't blast meters-as-degrees onto
            # the dashboard).
            if abs(sx) > 10_000 or abs(sy) > 10_000:
                sx, sy = x_m, y_m
            slat, slon = self._local_to_gps(sx, sy,
                                            self._ekf.origin_lat or d.lat,
                                            self._ekf.origin_lon or d.lon)
            # Replace lat/lon while preserving everything else. Dataclass
            # is mutable so direct assignment is fine and avoids a copy.
            d.lat = slat
            d.lon = slon
            smoothed_results.append(d)
        self._smoother.prune()

        return smoothed_results
