"""Particle filter for RSSI-based position tracking (v0.62+).

Alternative to the Extended Kalman Filter in position_filter.py. Literature
(Leverege 2023, ResearchGate Particle Filtering-Based Indoor Positioning 2020)
finds particle filters outperform EKF in dynamic environments where the target
is moving and multipath varies — the nonlinear RSSI→distance relationship maps
to a non-Gaussian posterior that EKF linearization handles poorly.

Trade-off vs EKF:
  + Handles multi-modal posteriors (two equally-likely positions when
    geometry is ambiguous — EKF collapses to the mean, particle filter
    keeps both hypotheses until a disambiguating measurement arrives)
  + Robust to RSSI outliers (one bad measurement degrades only a few
    particle weights, not the whole state estimate)
  + No Jacobian required — any motion/measurement model works
  - 100× more memory per track (100 particles × 2 floats vs EKF's 4×4 P)
  - Resampling can collapse diversity if done too aggressively

Usage:
  pfm = ParticleFilterManager()
  pfm.set_origin(avg_lat, avg_lon)
  pfm.update(drone_id, sensor_lat, sensor_lon, measured_distance, timestamp)
  x, y = pfm.get_position(drone_id)          # local meters
  acc = pfm.get_accuracy(drone_id)           # 1σ spread, meters
"""

from __future__ import annotations

import math
import random
import time

from app.services.position_filter import gps_to_local, local_to_gps


class ParticleFilter:
    """Bootstrap particle filter for a single tracked entity.

    State = (x, y) position in local ENU meters from a shared origin.
    Motion model = random walk with per-step σ scaled by elapsed dt.
    Measurement model = Gaussian likelihood on |particle - sensor| - measured.
    """

    # Number of particles per tracked entity. 100 is a conservative sweet spot:
    # enough for multi-modal posteriors in a 40 m yard, small enough that
    # tracking 50 entities × 100 particles × 16 bytes = 80 KB — negligible
    # vs the PSRAM budget we gave the uplink's detection queue.
    NUM_PARTICLES = 100

    # Resampling trigger: effective sample size threshold. When ESS drops
    # below half the particle count, weights have collapsed to a few and
    # we resample with replacement to restore diversity. Lower ESS_THRESH
    # → less resampling (more diversity preserved, slower convergence).
    ESS_THRESH = 0.5

    # Motion-model process noise. Assume targets in a 40 m yard move at
    # up to ~3 m/s (walking pace). Per-second σ = MOTION_SIGMA_MPS; actual
    # σ per prediction step scales with dt to keep behavior consistent
    # regardless of observation cadence.
    MOTION_SIGMA_MPS = 1.5

    # Measurement-model noise. The RSSI→distance estimate has σ roughly
    # linear in distance (30 + 2*d m) per our path-loss model. Used to
    # weight each particle by Gaussian likelihood.
    @staticmethod
    def _meas_sigma(d_est: float) -> float:
        return max(3.0, 0.3 * d_est + 2.0)

    def __init__(self) -> None:
        # Particles: two parallel arrays for cache-friendliness over list-of-tuple
        self.xs: list[float] = []
        self.ys: list[float] = []
        # Log-weights so we can accumulate many updates without underflow
        self.log_w: list[float] = []
        self.last_update_s: float = 0.0
        self.update_count: int = 0
        # Seed spread — re-initialized on first sensor observation using a
        # coarse centroid-based guess so we don't waste steps converging from
        # (0, 0). See ParticleFilterManager.update().
        self._initialized: bool = False

    def _init_uniform(self, cx: float, cy: float, radius_m: float) -> None:
        """Uniform circular seed around (cx, cy)."""
        self.xs = []
        self.ys = []
        self.log_w = []
        for _ in range(self.NUM_PARTICLES):
            # Sample uniformly in a disc (sqrt(U) for radial to preserve density)
            r = radius_m * math.sqrt(random.random())
            theta = 2 * math.pi * random.random()
            self.xs.append(cx + r * math.cos(theta))
            self.ys.append(cy + r * math.sin(theta))
            self.log_w.append(0.0)
        self._initialized = True

    def predict(self, dt: float) -> None:
        """Propagate particles via random-walk motion with dt-scaled noise."""
        if dt <= 0.0:
            return
        sigma = self.MOTION_SIGMA_MPS * math.sqrt(max(dt, 0.1))
        for i in range(len(self.xs)):
            self.xs[i] += random.gauss(0.0, sigma)
            self.ys[i] += random.gauss(0.0, sigma)

    def update_with_range(self, sx: float, sy: float, d_measured: float) -> None:
        """Weight each particle by Gaussian likelihood of its distance to the
        sensor matching the measured distance. Does NOT resample — that's
        decided after weighting to preserve information across short bursts
        of rapid observations from different sensors."""
        sigma = self._meas_sigma(d_measured)
        inv_2sig2 = 1.0 / (2.0 * sigma * sigma)
        for i in range(len(self.xs)):
            dx = self.xs[i] - sx
            dy = self.ys[i] - sy
            d_particle = math.sqrt(dx * dx + dy * dy)
            residual = d_particle - d_measured
            self.log_w[i] += -residual * residual * inv_2sig2
        self.update_count += 1

    def _effective_sample_size(self) -> float:
        """ESS = 1 / Σw² — low ESS means one particle is dominating. Compute
        in normalized-weight space (exp(log_w) / Σ exp(log_w))."""
        if not self.log_w:
            return 0.0
        m = max(self.log_w)
        ws = [math.exp(lw - m) for lw in self.log_w]
        s = sum(ws)
        if s <= 0:
            return 0.0
        ess = (s * s) / sum(w * w for w in ws)
        return ess

    def maybe_resample(self) -> None:
        """Resample with replacement when ESS drops below threshold. Uses
        the systematic-resampling algorithm (Kitagawa 1996) which preserves
        particle diversity better than multinomial."""
        n = len(self.xs)
        if n == 0:
            return
        ess = self._effective_sample_size()
        if ess >= self.ESS_THRESH * n:
            return
        # Normalize weights (working in log-space)
        m = max(self.log_w)
        ws = [math.exp(lw - m) for lw in self.log_w]
        s = sum(ws)
        if s <= 0:
            # Degenerate — reset weights uniformly, keep positions
            self.log_w = [0.0] * n
            return
        ws = [w / s for w in ws]
        # Systematic resampling
        step = 1.0 / n
        u0 = random.random() * step
        cum = 0.0
        j = 0
        new_x: list[float] = []
        new_y: list[float] = []
        for i in range(n):
            u = u0 + i * step
            while cum + ws[j] < u and j < n - 1:
                cum += ws[j]
                j += 1
            new_x.append(self.xs[j])
            new_y.append(self.ys[j])
        self.xs = new_x
        self.ys = new_y
        self.log_w = [0.0] * n

    def get_position(self) -> tuple[float, float]:
        """Weighted mean. Returns (0, 0) if uninitialized."""
        if not self.xs:
            return (0.0, 0.0)
        m = max(self.log_w)
        ws = [math.exp(lw - m) for lw in self.log_w]
        s = sum(ws)
        if s <= 0:
            # Fall back to unweighted mean
            return (sum(self.xs) / len(self.xs), sum(self.ys) / len(self.ys))
        mx = sum(x * w for x, w in zip(self.xs, ws)) / s
        my = sum(y * w for y, w in zip(self.ys, ws)) / s
        return (mx, my)

    def get_accuracy(self) -> float:
        """1-σ spread around the weighted mean — used as accuracy_m."""
        if len(self.xs) < 2:
            return 100.0
        mx, my = self.get_position()
        var = 0.0
        for x, y in zip(self.xs, self.ys):
            dx = x - mx
            dy = y - my
            var += dx * dx + dy * dy
        var /= len(self.xs)
        return math.sqrt(var)


class ParticleFilterManager:
    """Owns one ParticleFilter per tracking_id, plus shared ENU origin.

    Drop-in alternative to PositionFilterManager. Separate instance so the
    two can run side-by-side for A/B comparison or be switched via config."""

    def __init__(self) -> None:
        self.filters: dict[str, ParticleFilter] = {}
        self.origin_lat: float = 0.0
        self.origin_lon: float = 0.0
        self._origin_set: bool = False
        self._last_prune = time.time()

    def set_origin(self, lat: float, lon: float) -> None:
        if lat == 0 and lon == 0:
            return
        self.origin_lat = lat
        self.origin_lon = lon
        self._origin_set = True

    def update(self, device_id: str, sensor_lat: float, sensor_lon: float,
               measured_distance: float,
               timestamp: float = 0.0) -> tuple[float, float, float] | None:
        """Feed one range observation. Mirrors PositionFilterManager.update()
        signature so callers can swap without changing call sites.

        Returns (lat, lon, accuracy_m) or None if not yet localized."""
        if not self._origin_set or sensor_lat == 0 or sensor_lon == 0:
            return None
        sx, sy = gps_to_local(sensor_lat, sensor_lon, self.origin_lat, self.origin_lon)
        now = timestamp if timestamp > 0 else time.time()

        pf = self.filters.get(device_id)
        if pf is None:
            pf = ParticleFilter()
            self.filters[device_id] = pf

        if not pf._initialized:
            # Seed with a uniform disc around the sensor at the measured distance
            # — the target is *somewhere on a ring at radius d*, so bootstrap
            # particles across a larger disc that covers it.
            pf._init_uniform(sx, sy, radius_m=max(measured_distance * 1.5, 10.0))
            pf.last_update_s = now

        # Predict-step: propagate particles by motion-model noise since last update
        dt = max(0.0, now - pf.last_update_s) if pf.last_update_s else 0.0
        if dt > 0.0 and dt < 60.0:  # skip huge gaps (entity was gone)
            pf.predict(dt)
        elif dt >= 60.0:
            # Long gap — re-seed around sensor to recover without waiting for
            # diversity to recover through noise alone
            pf._init_uniform(sx, sy, radius_m=max(measured_distance * 1.5, 10.0))

        pf.update_with_range(sx, sy, measured_distance)
        pf.maybe_resample()
        pf.last_update_s = now

        # Haven't converged yet — need a few observations before returning
        if pf.update_count < 3:
            return None

        x, y = pf.get_position()
        acc = pf.get_accuracy()
        lat, lon = local_to_gps(x, y, self.origin_lat, self.origin_lon)
        return (lat, lon, acc)

    def get_position(self, device_id: str) -> tuple[float, float] | None:
        pf = self.filters.get(device_id)
        if pf is None or not pf._initialized:
            return None
        return pf.get_position()

    def get_accuracy(self, device_id: str) -> float | None:
        pf = self.filters.get(device_id)
        if pf is None:
            return None
        return pf.get_accuracy()

    def prune(self, stale_timeout_s: float = 300.0) -> None:
        """Drop filters that haven't been updated recently."""
        now = time.time()
        if now - self._last_prune < 30.0:
            return
        self._last_prune = now
        stale = [k for k, pf in self.filters.items()
                 if now - pf.last_update_s > stale_timeout_s]
        for k in stale:
            del self.filters[k]

    def get_stats(self) -> dict:
        return {
            "filter_type": "particle",
            "tracked_count": len(self.filters),
            "particles_per_filter": ParticleFilter.NUM_PARTICLES,
            "origin_set": self._origin_set,
        }
