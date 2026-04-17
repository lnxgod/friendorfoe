"""Extended Kalman Filter for device position estimation.

Tracks device positions using RSSI-derived distance measurements from
multiple sensors. Each device gets its own EKF instance that maintains
state [x, y, vx, vy] in local meters relative to the sensor centroid.

Handles asynchronous sensor updates natively — each observation triggers
a predict(dt) + update(sensor, distance) cycle.
"""

import math
import time
import logging

logger = logging.getLogger(__name__)

# Earth radius for coordinate conversion
EARTH_RADIUS_M = 6_371_000.0

# Diagnostic threshold: velocities above this (m/s) for a tracked device
# are treated as "implausibly fast" and logged. 30 m/s ≈ 108 km/h — faster
# than any realistic pedestrian and most consumer drones sustain.
_IMPLAUSIBLE_VELOCITY_MPS = 30.0

# Module-level counters surfaced by GET /detections/tracking/diagnostics.
EKF_HEALTH: dict = {
    "velocity_warnings": 0,
    "covariance_errors": 0,
    "update_rejects": 0,
}


class DeviceEKF:
    """Extended Kalman Filter for a single tracked device.

    State: [x, y, vx, vy] in local meters from origin.
    Measurement: distance from a sensor at known (sx, sy).
    """

    __slots__ = ('x', 'y', 'vx', 'vy',
                 'P00', 'P01', 'P02', 'P03',
                 'P10', 'P11', 'P12', 'P13',
                 'P20', 'P21', 'P22', 'P23',
                 'P30', 'P31', 'P32', 'P33',
                 'q_std', 'last_time', 'update_count',
                 'last_accuracy', 'history')

    def __init__(self, x: float, y: float, q_std: float = 0.05):
        self.x = x
        self.y = y
        self.vx = 0.0
        self.vy = 0.0
        # Covariance matrix (flat, 4x4 symmetric)
        # Start with high uncertainty
        self.P00 = 100.0; self.P01 = 0.0; self.P02 = 0.0; self.P03 = 0.0
        self.P10 = 0.0; self.P11 = 100.0; self.P12 = 0.0; self.P13 = 0.0
        self.P20 = 0.0; self.P21 = 0.0; self.P22 = 10.0; self.P23 = 0.0
        self.P30 = 0.0; self.P31 = 0.0; self.P32 = 0.0; self.P33 = 10.0
        self.q_std = q_std
        self.last_time = time.time()
        self.update_count = 0
        self.last_accuracy = 100.0
        # Position history: list of (timestamp, x, y, accuracy)
        # Only record when position moves > 3m from last recorded point
        self.history: list[tuple[float, float, float, float]] = []

    def predict(self, now: float | None = None) -> None:
        """Predict state forward by elapsed time."""
        if now is None:
            now = time.time()
        dt = now - self.last_time
        if dt <= 0:
            return
        self.last_time = now

        # State prediction: x += vx*dt, y += vy*dt
        self.x += self.vx * dt
        self.y += self.vy * dt

        # Covariance prediction: P = F*P*F' + Q
        # F = [[1,0,dt,0],[0,1,0,dt],[0,0,1,0],[0,0,0,1]]
        # Process noise Q using continuous white noise acceleration model
        q = self.q_std ** 2
        dt2 = dt * dt
        dt3 = dt2 * dt
        dt4 = dt3 * dt

        # F*P*F' (applying state transition to covariance)
        # Row 0: P[0] += dt*P[2], then column 0: P[:,0] += dt*P[:,2]
        # Simplified inline for performance
        p00 = self.P00 + dt * (self.P20 + self.P02) + dt2 * self.P22
        p01 = self.P01 + dt * (self.P21 + self.P03) + dt2 * self.P23
        p02 = self.P02 + dt * self.P22
        p03 = self.P03 + dt * self.P23
        p11 = self.P11 + dt * (self.P31 + self.P13) + dt2 * self.P33
        p12 = self.P12 + dt * self.P32
        p13 = self.P13 + dt * self.P33
        p22 = self.P22
        p23 = self.P23
        p33 = self.P33

        # Add process noise Q
        self.P00 = p00 + q * dt4 / 4
        self.P01 = p01
        self.P02 = p02 + q * dt3 / 2
        self.P03 = p03
        self.P10 = p01
        self.P11 = p11 + q * dt4 / 4
        self.P12 = p12
        self.P13 = p13 + q * dt3 / 2
        self.P20 = p02 + q * dt3 / 2
        self.P21 = p12
        self.P22 = p22 + q * dt2
        self.P23 = p23
        self.P30 = p03
        self.P31 = p13 + q * dt3 / 2
        self.P32 = p23
        self.P33 = p33 + q * dt2

    def update(self, sensor_x: float, sensor_y: float, measured_distance: float) -> None:
        """Update state with a distance measurement from one sensor."""
        dx = self.x - sensor_x
        dy = self.y - sensor_y
        expected_dist = math.sqrt(dx * dx + dy * dy)
        if expected_dist < 0.5:
            expected_dist = 0.5

        # Innovation (measurement residual)
        innov = measured_distance - expected_dist

        # Jacobian H = [dx/d, dy/d, 0, 0]
        h0 = dx / expected_dist
        h1 = dy / expected_dist

        # Measurement noise R — very high because RSSI distance estimates are noisy.
        # A 3dB RSSI fluctuation at 50m can shift distance estimate by 30-50%.
        # Increased from (15+d*0.5)^2 to reduce position wiggling for stationary devices.
        r = (30.0 + measured_distance * 2.0) ** 2  # Aggressive: far readings have very low weight

        # Innovation covariance S = H*P*H' + R
        s = (h0 * (h0 * self.P00 + h1 * self.P01) +
             h1 * (h0 * self.P10 + h1 * self.P11) + r)

        if abs(s) < 1e-10:
            EKF_HEALTH["update_rejects"] += 1
            return

        si = 1.0 / s

        # Kalman gain K = P*H' / S
        k0 = (self.P00 * h0 + self.P01 * h1) * si
        k1 = (self.P10 * h0 + self.P11 * h1) * si
        k2 = (self.P20 * h0 + self.P21 * h1) * si
        k3 = (self.P30 * h0 + self.P31 * h1) * si

        # State update
        self.x += k0 * innov
        self.y += k1 * innov
        self.vx += k2 * innov
        self.vy += k3 * innov

        # Diagnostic: implausibly fast velocity suggests a bad measurement
        # drove vx/vy out of bounds. We only log; we don't clamp yet.
        speed = math.sqrt(self.vx * self.vx + self.vy * self.vy)
        if speed > _IMPLAUSIBLE_VELOCITY_MPS:
            EKF_HEALTH["velocity_warnings"] += 1
            logger.warning(
                "ekf.velocity speed=%.1fm/s innov=%.1f meas=%.1f expected=%.1f vx=%.2f vy=%.2f",
                speed, innov, measured_distance, expected_dist, self.vx, self.vy,
            )

        # Covariance update (Joseph form for stability)
        # P = (I - K*H) * P * (I - K*H)' + K*R*K'
        # Simplified: P = P - K*S*K' (equivalent for scalar measurement)
        self.P00 -= k0 * s * k0
        self.P01 -= k0 * s * k1
        self.P02 -= k0 * s * k2
        self.P03 -= k0 * s * k3
        self.P10 -= k1 * s * k0
        self.P11 -= k1 * s * k1
        self.P12 -= k1 * s * k2
        self.P13 -= k1 * s * k3
        self.P20 -= k2 * s * k0
        self.P21 -= k2 * s * k1
        self.P22 -= k2 * s * k2
        self.P23 -= k2 * s * k3
        self.P30 -= k3 * s * k0
        self.P31 -= k3 * s * k1
        self.P32 -= k3 * s * k2
        self.P33 -= k3 * s * k3

        self.update_count += 1

        # Covariance health: the simplified update (P -= k·s·kᵀ) can drive
        # the diagonal slightly negative under numerical noise. When that
        # happens we silently poisoned the filter — the sqrt below would
        # produce NaN and the caller's `ekf_acc < 5000` guard would drop
        # us into the trilateration/range_only fallback chain.
        if self.P00 < 0 or self.P11 < 0 or self.P22 < 0 or self.P33 < 0:
            EKF_HEALTH["covariance_errors"] += 1
            logger.error(
                "ekf.covariance_negative P00=%.3e P11=%.3e P22=%.3e P33=%.3e — accuracy set to inf",
                self.P00, self.P11, self.P22, self.P33,
            )
            self.last_accuracy = float("inf")
        else:
            self.last_accuracy = math.sqrt(self.P00 + self.P11)

        # Record position history when movement exceeds 3m from last point
        if self.update_count >= 3:
            # Use the filter's own clock (last_time) so history timestamps match
            # the measurement timestamps fed via predict(). The original guard
            # on _last_update_time always fell through to wall time.
            now = self.last_time
            if not self.history:
                self.history.append((now, self.x, self.y, self.last_accuracy))
            else:
                last_t, last_x, last_y, _ = self.history[-1]
                dx = self.x - last_x
                dy = self.y - last_y
                moved = math.sqrt(dx * dx + dy * dy)
                # Record if moved > 10m or > 30s since last record.
                # RSSI-based positioning has 10-30m noise — don't trail jitter.
                if moved > 10.0 or (now - last_t) > 30.0:
                    self.history.append((now, self.x, self.y, self.last_accuracy))
                    # Keep max 500 history points (~80 min at 10s intervals)
                    if len(self.history) > 500:
                        self.history.pop(0)

    def get_position(self) -> tuple[float, float]:
        return (self.x, self.y)

    def get_accuracy(self) -> float:
        return self.last_accuracy


# ── Coordinate conversion ────────────────────────────────────────────────

def gps_to_local(lat: float, lon: float, origin_lat: float, origin_lon: float) -> tuple[float, float]:
    """Convert GPS to local meters using equirectangular projection."""
    cos_lat = math.cos(math.radians(origin_lat))
    x = (lon - origin_lon) * math.radians(1) * EARTH_RADIUS_M * cos_lat
    y = (lat - origin_lat) * math.radians(1) * EARTH_RADIUS_M
    return (x, y)


def local_to_gps(x: float, y: float, origin_lat: float, origin_lon: float) -> tuple[float, float]:
    """Convert local meters back to GPS."""
    cos_lat = math.cos(math.radians(origin_lat))
    lon = origin_lon + x / (math.radians(1) * EARTH_RADIUS_M * cos_lat)
    lat = origin_lat + y / (math.radians(1) * EARTH_RADIUS_M)
    return (lat, lon)


# ── Filter Manager ───────────────────────────────────────────────────────

class PositionFilterManager:
    """Manages per-device EKF instances.

    Handles creation, update, pruning, and coordinate conversion.
    """

    def __init__(self, stale_timeout: float = 300.0):
        self.filters: dict[str, DeviceEKF] = {}
        self.stale_timeout = stale_timeout
        self.origin_lat: float = 0.0
        self.origin_lon: float = 0.0
        self._origin_set = False

    def set_origin(self, lat: float, lon: float) -> None:
        """Set the local coordinate origin (sensor centroid)."""
        if lat == 0 and lon == 0:
            return
        self.origin_lat = lat
        self.origin_lon = lon
        self._origin_set = True

    def update(self, device_id: str, sensor_lat: float, sensor_lon: float,
               measured_distance: float) -> tuple[float, float, float] | None:
        """Feed a new distance observation. Returns (lat, lon, accuracy_m) or None.

        For new devices, delays EKF initialization until we have 2+ sensor
        observations for a better starting position (weighted centroid).
        """
        if not self._origin_set or sensor_lat == 0 or sensor_lon == 0:
            return None

        sx, sy = gps_to_local(sensor_lat, sensor_lon, self.origin_lat, self.origin_lon)
        now = time.time()

        ekf = self.filters.get(device_id)
        if ekf is None:
            # Don't create yet — collect initial observations
            # Store pending observations as a list
            pending_key = "_pending_" + device_id
            pending = self.filters.get(pending_key)
            if pending is None:
                # Abuse the dict to store pending obs (not an EKF)
                self._pending = getattr(self, '_pending', {})
                if device_id not in self._pending:
                    self._pending[device_id] = []
                self._pending[device_id].append((sx, sy, measured_distance, now))
                if len(self._pending[device_id]) < 2:
                    return None
                # Initialize at inverse-variance-weighted centroid of pending
                # observations. Variance of the distance measurement scales as
                # (30 + 2*d)^2 (same model used by update()); weight = 1/variance
                # so sensors with confident short-range readings dominate the
                # initial position instead of distant sensors pulling it off.
                obs_list = self._pending[device_id]
                total_w = 0.0
                cx, cy = 0.0, 0.0
                for ox, oy, od, _ in obs_list:
                    sigma = 30.0 + 2.0 * max(od, 1.0)
                    w = 1.0 / (sigma * sigma)
                    cx += ox * w
                    cy += oy * w
                    total_w += w
                cx /= total_w
                cy /= total_w
                ekf = DeviceEKF(cx, cy)
                self.filters[device_id] = ekf
                # Replay pending observations
                for ox, oy, od, ot in obs_list:
                    ekf.predict(ot)
                    ekf.update(ox, oy, od)
                del self._pending[device_id]
                return None  # Not ready yet, need more convergence

        ekf.predict(now)
        ekf.update(sx, sy, measured_distance)

        # Need at least 3 updates for a meaningful position
        if ekf.update_count < 3:
            return None

        x, y = ekf.get_position()
        lat, lon = local_to_gps(x, y, self.origin_lat, self.origin_lon)
        accuracy = ekf.get_accuracy()

        # Reject garbage positions
        if abs(lat - self.origin_lat) > 0.1 or abs(lon - self.origin_lon) > 0.1:
            return None
        if accuracy > 5000:
            return None

        return (lat, lon, accuracy)

    def prune(self) -> None:
        """Remove stale filters."""
        now = time.time()
        expired = [k for k, v in self.filters.items()
                   if now - v.last_time > self.stale_timeout]
        for k in expired:
            del self.filters[k]

    def get_history(self, device_id: str) -> list[dict]:
        """Return position history trail for a device as GPS coords."""
        ekf = self.filters.get(device_id)
        if not ekf or not ekf.history:
            return []
        return [
            {"t": t, "lat": local_to_gps(x, y, self.origin_lat, self.origin_lon)[0],
             "lon": local_to_gps(x, y, self.origin_lat, self.origin_lon)[1],
             "accuracy": acc}
            for t, x, y, acc in ekf.history
        ]

    def get_all_trails(self) -> dict[str, list[dict]]:
        """Return trails for all devices that have movement history (2+ points)."""
        trails = {}
        for device_id, ekf in self.filters.items():
            if ekf.history and len(ekf.history) >= 2:
                trails[device_id] = self.get_history(device_id)
        return trails

    def get_stats(self) -> dict:
        """Return summary stats."""
        active = [f for f in self.filters.values() if f.update_count >= 3]
        with_trails = sum(1 for f in self.filters.values() if len(f.history) >= 2)
        return {
            "total_tracked": len(self.filters),
            "active_positions": len(active),
            "avg_accuracy": sum(f.last_accuracy for f in active) / max(1, len(active)),
            "avg_updates": sum(f.update_count for f in active) / max(1, len(active)),
            "devices_with_trails": with_trails,
        }
