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
from dataclasses import dataclass, field

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EARTH_RADIUS_M = 6_371_000.0

# RSSI → distance model (matches ESP32 firmware constants.h)
RSSI_REF = -40          # dBm at 1 meter
PATH_LOSS_EXPONENT = 2.5

# Observation staleness: discard observations older than this
OBSERVATION_TTL_SEC = 30.0

# Sensor staleness: consider a sensor offline after this
SENSOR_TTL_SEC = 120.0

# Maximum iterations for Gauss-Newton solver
MAX_ITERATIONS = 20
CONVERGENCE_M = 1.0  # stop when update < 1 meter


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

def rssi_to_distance_m(rssi: int) -> float:
    """Estimate distance from RSSI using log-distance path loss model."""
    exponent = (RSSI_REF - rssi) / (10.0 * PATH_LOSS_EXPONENT)
    d = 10.0 ** exponent
    return max(0.5, min(d, 5000.0))


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

    def __init__(self) -> None:
        self.sensors: dict[str, SensorInfo] = {}
        # drone_id -> {device_id -> DroneObservation}
        self.observations: dict[str, dict[str, DroneObservation]] = {}

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
            )

        # Compute distance from RSSI if not provided
        dist = estimated_distance_m
        if dist is None and rssi is not None:
            dist = rssi_to_distance_m(rssi)

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

        if drone_id not in self.observations:
            self.observations[drone_id] = {}
        self.observations[drone_id][device_id] = obs

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

        # Prune old sensors
        expired_sensors = [sid for sid, s in self.sensors.items() if now - s.last_seen > SENSOR_TTL_SEC]
        for sid in expired_sensors:
            del self.sensors[sid]

    def get_active_sensors(self) -> list[SensorInfo]:
        """Return currently active sensors."""
        self.prune()
        return list(self.sensors.values())

    def get_located_drones(self) -> list[LocatedDrone]:
        """
        Compute positions for all currently-tracked drones.

        Priority:
        1. If ANY observation has drone GPS (Remote ID / DJI IE) → use it directly
        2. If 3+ sensors → multilaterate from RSSI distances
        3. If 2 sensors → circle intersection
        4. If 1 sensor → sensor position + range circle
        """
        self.prune()
        results: list[LocatedDrone] = []

        for drone_id, obs_map in self.observations.items():
            observations = list(obs_map.values())
            if not observations:
                continue

            # Pick best metadata from highest-confidence observation
            best_obs = max(observations, key=lambda o: o.confidence)

            # --- Priority 1: Direct GPS from drone ---
            gps_obs = [o for o in observations if o.drone_lat and o.drone_lon
                       and o.drone_lat != 0.0 and o.drone_lon != 0.0]
            if gps_obs:
                best_gps = max(gps_obs, key=lambda o: o.timestamp)
                results.append(LocatedDrone(
                    drone_id=drone_id,
                    lat=best_gps.drone_lat,
                    lon=best_gps.drone_lon,
                    alt=best_gps.drone_alt,
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
                continue

            # --- Collect sensors with valid positions and distance estimates ---
            usable = [
                o for o in observations
                if o.sensor_lat != 0.0 and o.sensor_lon != 0.0 and o.estimated_distance_m is not None
            ]

            if len(usable) >= 3:
                # --- Priority 2: Trilateration (3+ sensors) ---
                sensors_pos = [(o.sensor_lat, o.sensor_lon) for o in usable]
                distances = [o.estimated_distance_m for o in usable]
                result = _trilaterate(sensors_pos, distances)
                if result:
                    lat, lon, accuracy = result
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

        return results
