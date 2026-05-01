"""Phone-driven RSSI calibration.

The Android app walks the property emitting a known BLE advertisement at a
fixed TX power. While walking it streams (lat, lon, ts_ms) at ~1 Hz to
`/detections/calibrate/sample`. The fleet's sensors hear the same
advertisement and ingest it through the normal detection pipeline. After
the walk ends, this module joins the phone GPS trace × sensor sightings
by timestamp and fits a per-listener log-distance path-loss model:

    RSSI_i = RSSI_REF_i  -  10·n_i · log10(distance_m)

…where each sensor i gets its own (RSSI_REF_i, n_i) pair. Per-listener
models capture the dominant accuracy killer in this kind of fleet:
local environment (brick wall, foliage, antenna height) varies by node
far more than it varies between (BLE vs WiFi) bands.

Why not WiFi here? Modern Android throttles `WifiManager.startScan()`
and gates `LocalOnlyHotspot` behind permissions/UX that vary by OEM. BLE
peripheral mode is uniform across vendors, the TX power table is well
documented, and one band is enough to dial in the global path-loss model
that the WiFi pipeline shares. (Per-band offsets are a v2 improvement.)
"""

from __future__ import annotations

import math
import time
import uuid
from dataclasses import dataclass, field
from statistics import median

# Reference RSSI at 1 m for ADVERTISE_TX_POWER_HIGH on a typical
# Android peripheral (Pixel/Samsung). Reading TX power from the
# advertisement is unreliable across vendors — we hardcode this so the
# physics calculation has a known anchor. Phones can override per-build
# via the `tx_power_dbm` field on /calibrate/walk/start.
DEFAULT_PHONE_TX_DBM = -59.0

# How long after the walk ends to keep the session in memory before
# garbage-collecting. Long enough for the operator to inspect results
# in the app.
SESSION_TTL_S = 3600.0

# Minimum sensor-side samples required before we'll fit a per-listener
# model. Below this we leave the listener on the global model — under-
# constrained per-listener fits produce wild (n, RSSI_REF) values that
# make distance estimates worse, not better.
MIN_SAMPLES_PER_LISTENER = 20

# Maximum / minimum sane path-loss exponent. Free-space is 2.0; dense
# urban indoor multi-floor tops out around 4.5. Reject fits outside this
# band as bad data (often: phone GPS jumped, or sensor heard a different
# device with the same UUID).
MIN_PATH_LOSS = 1.8
MAX_PATH_LOSS = 5.0

# Apply-quality gates. These are intentionally stricter than "can we fit a
# line?" because a demo/operator model must produce stable positions, not just
# mathematically non-empty coefficients.
APPLY_MIN_GLOBAL_R2 = 0.55
APPLY_MIN_OK_LISTENERS = 3
APPLY_MIN_TARGET_COVERAGE = 0.75
APPLY_MAX_CHECKPOINT_DRIFT_M = 15.0

# During a calibration walk a nearby node can hear many copies of the same
# phone advertisement in a second. Collapse those packet bursts to median RSSI
# before fitting so one "screaming" receiver does not overweight the model.
FIT_PACKET_BUCKET_S = 1.0


@dataclass
class _TracePoint:
    lat: float
    lon: float
    ts_s: float           # epoch seconds (server-corrected if needed)
    accuracy_m: float | None = None


@dataclass
class _CalSample:
    """One sensor heard the phone advertisement at this RSSI."""
    sensor_id: str
    sensor_lat: float
    sensor_lon: float
    rssi: int
    ts_s: float
    # Checkpoint samples carry extra weight in the OLS fit because they
    # come with a ground-truth d≈1 m anchor — operator stood at the
    # sensor and tapped the button. Default 1 = ordinary walk sample.
    weight: int = 1
    scanner_slots_seen: int | None = None


@dataclass
class _Checkpoint:
    """The operator stood next to a sensor and pressed 'I'm here'.
    Anchors the per-listener RSSI_REF and surfaces sensor-placement
    issues (wrong DB coords, swapped labels)."""
    sensor_id: str
    claimed_sensor_lat: float       # registered position from DB / heartbeat
    claimed_sensor_lon: float
    phone_lat: float
    phone_lon: float
    phone_accuracy_m: float | None
    rssi_at_touch: int | None       # strongest observed RSSI for that sensor near touch
    strongest_sensor_at_touch: str | None  # sensor_id with the loudest RSSI overall
    ts_s: float
    anchor_source: str = "phone_gps"


@dataclass
class WalkSession:
    session_id: str
    operator_label: str               # display name from the phone (e.g., "Bill's Pixel")
    expected_uuid: str                # BLE service UUID the phone is advertising
    tx_power_dbm: float               # phone-reported TX power at 1 m
    started_at: float
    ended_at: float | None = None
    trace: list[_TracePoint] = field(default_factory=list)
    samples: list[_CalSample] = field(default_factory=list)
    checkpoints: list[_Checkpoint] = field(default_factory=list)
    fit_result: dict | None = None    # populated by .fit()
    target_sensor_ids: list[str] = field(default_factory=list)
    target_nodes: list[dict] = field(default_factory=list)
    mode_state: str = "inactive"
    mode_started_at: float | None = None
    mode_stopped_at: float | None = None
    abort_reason: str | None = None
    provisional_fit: dict | None = None
    verified_fit: dict | None = None
    apply_requested: bool = False
    applied: bool = False
    apply_reason: str | None = None


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    R = 6_371_000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R * math.asin(math.sqrt(a))


def _interpolate_position(trace: list[_TracePoint], ts_s: float) -> tuple[float, float] | None:
    """Linear interpolate the phone's position at a given timestamp."""
    if not trace:
        return None
    if ts_s <= trace[0].ts_s:
        return (trace[0].lat, trace[0].lon)
    if ts_s >= trace[-1].ts_s:
        return (trace[-1].lat, trace[-1].lon)
    # Binary-search-light: the trace is short (~300 points typical) so a
    # linear scan is fine. Switch to bisect if a session ever exceeds 5k.
    for i in range(len(trace) - 1):
        a, b = trace[i], trace[i + 1]
        if a.ts_s <= ts_s <= b.ts_s:
            span = b.ts_s - a.ts_s
            if span <= 0:
                return (a.lat, a.lon)
            t = (ts_s - a.ts_s) / span
            return (a.lat + t * (b.lat - a.lat),
                    a.lon + t * (b.lon - a.lon))
    return None


def _ols_fit(distances_m: list[float], rssis: list[float]) -> tuple[float, float, float] | None:
    """Ordinary least-squares fit of  rssi = RSSI_REF + slope * log10(d).

    Returns (rssi_ref, path_loss_exponent, r_squared) or None if data is
    too thin or the fit produces an out-of-band exponent.

    `path_loss_exponent` is recovered as -slope/10 (the textbook log-distance
    model RSSI = RSSI_REF - 10·n·log10(d)).
    """
    n_pts = len(distances_m)
    if n_pts < MIN_SAMPLES_PER_LISTENER:
        return None
    xs = [math.log10(max(d, 0.5)) for d in distances_m]
    ys = list(rssis)
    mx = sum(xs) / n_pts
    my = sum(ys) / n_pts
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx <= 1e-9:
        return None  # degenerate — all distances identical
    slope = sxy / sxx
    intercept = my - slope * mx
    # R² = 1 - SS_res / SS_tot
    sst = sum((y - my) ** 2 for y in ys)
    if sst <= 1e-9:
        return None
    ssr = sum((y - (intercept + slope * x)) ** 2 for x, y in zip(xs, ys))
    r2 = max(0.0, 1.0 - ssr / sst)

    rssi_ref = intercept           # value of y at x=0  →  log10(d)=0  →  d=1m
    path_loss = -slope / 10.0      # slope = -10·n  →  n = -slope/10

    if not (MIN_PATH_LOSS <= path_loss <= MAX_PATH_LOSS):
        return None
    return (rssi_ref, path_loss, r2)


def _percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    k = (len(ordered) - 1) * pct / 100.0
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return float(ordered[int(k)])
    return float(ordered[lo] * (hi - k) + ordered[hi] * (k - lo))


def _error_stats(values: list[float]) -> dict:
    if not values:
        return {
            "count": 0,
            "mean_m": None,
            "median_m": None,
            "p90_m": None,
            "max_m": None,
        }
    return {
        "count": len(values),
        "mean_m": round(sum(values) / len(values), 2),
        "median_m": round(float(median(values)), 2),
        "p90_m": round(_percentile(values, 90) or 0.0, 2),
        "max_m": round(max(values), 2),
    }


def _distance_from_model(rssi: float, rssi_ref: float, path_loss: float) -> float:
    exponent = (rssi_ref - rssi) / (10.0 * path_loss)
    return max(1.0, min(200.0, 10.0 ** exponent))


class PhoneCalibrationManager:
    """In-memory store of walk sessions + per-listener OLS fitter.

    Keeping sessions in process memory is fine: a session is at most a
    few thousand small tuples (~100 KB), and we only need them long
    enough for the operator to inspect the result. Persisted *fits* go
    to disk via the existing CalibrationManager.last_result path.
    """

    def __init__(self) -> None:
        self.sessions: dict[str, WalkSession] = {}
        self._last_prune = time.time()

    def start(self, operator_label: str, tx_power_dbm: float | None) -> WalkSession:
        sid = uuid.uuid4().hex[:12]
        # The expected_uuid ties phone advertisements to this session. The
        # phone reads it back from the start response and sets it as the
        # service UUID on its BLE advertiser. The 'CAFE' prefix is a
        # human-recognisable marker so operators eyeballing wireshark can
        # tell at a glance "that ad came from a calibration walk".
        expected_uuid = f"cafe{sid[:4]}-0000-1000-8000-{sid[:12]}"
        s = WalkSession(
            session_id=sid,
            operator_label=operator_label or "phone",
            expected_uuid=expected_uuid,
            tx_power_dbm=float(tx_power_dbm) if tx_power_dbm is not None else DEFAULT_PHONE_TX_DBM,
            started_at=time.time(),
        )
        self.sessions[sid] = s
        return s

    def add_trace_point(self, session_id: str, lat: float, lon: float,
                        ts_s: float | None = None,
                        accuracy_m: float | None = None) -> bool:
        s = self.sessions.get(session_id)
        if s is None or s.ended_at is not None:
            return False
        s.trace.append(_TracePoint(
            lat=lat, lon=lon,
            ts_s=ts_s if ts_s and ts_s > 1e9 else time.time(),
            accuracy_m=accuracy_m,
        ))
        return True

    def add_sensor_sample(self, session_id: str, sensor_id: str,
                          sensor_lat: float, sensor_lon: float,
                          rssi: int, ts_s: float | None = None,
                          scanner_slots_seen: int | None = None) -> bool:
        """Called by the detection ingest path when a sensor hears a
        BLE advertisement matching an active session's expected UUID."""
        s = self.sessions.get(session_id)
        if s is None or s.ended_at is not None:
            return False
        s.samples.append(_CalSample(
            sensor_id=sensor_id,
            sensor_lat=sensor_lat,
            sensor_lon=sensor_lon,
            rssi=int(rssi),
            ts_s=ts_s if ts_s and ts_s > 1e9 else time.time(),
            scanner_slots_seen=scanner_slots_seen,
        ))
        return True

    # ── Checkpoint constants ───────────────────────────────────────
    # Window in which to look for sensor samples around a touch event.
    CHECKPOINT_WINDOW_S = 10.0
    # GPS-drift thresholds: how far phone GPS can deviate from the
    # registered sensor position before we surface a warning. Below 5 m
    # is "matches expected"; 5–15 m is "borderline, watch for repeat";
    # above 15 m is "the sensor's recorded coordinates are likely wrong".
    GPS_DRIFT_OK_M = 5.0
    GPS_DRIFT_WARN_M = 15.0
    # Repeat factor for checkpoint samples in OLS — 10× the data weight
    # so the intercept (RSSI_REF) is anchored by the d≈1 m anchor rather
    # than extrapolated from distant samples. Cheap; doesn't change the
    # math, just data balance.
    CHECKPOINT_OLS_WEIGHT = 10
    # Dominance margin (dB) before we call a sensor the "strongest" at
    # touch. 6 dB ≈ 2× signal power — safely above RSSI noise floor.
    LABEL_MATCH_MARGIN_DB = 6

    def add_checkpoint(self, session_id: str, sensor_id: str,
                       sensor_lat: float, sensor_lon: float,
                       phone_lat: float, phone_lon: float,
                       phone_accuracy_m: float | None = None,
                       ts_s: float | None = None,
                       anchor_source: str = "phone_gps") -> dict:
        """Record an "I'm standing next to <sensor>" event.

        Returns immediate sanity result for the phone UI:
          {ok, gps_drift_m, rssi_at_touch, strongest_sensor_at_touch,
           warnings: [...], severity: "ok"|"warn"|"error"}
        """
        s = self.sessions.get(session_id)
        if s is None or s.ended_at is not None:
            return {"ok": False, "reason": "unknown_or_ended_session"}
        now = ts_s if ts_s and ts_s > 1e9 else time.time()

        # Look at all samples in the window around the touch event,
        # bucket by sensor, take the loudest per sensor.
        cutoff_lo = now - self.CHECKPOINT_WINDOW_S
        cutoff_hi = now + self.CHECKPOINT_WINDOW_S
        loudest_per_sensor: dict[str, _CalSample] = {}
        for c in s.samples:
            if c.ts_s < cutoff_lo or c.ts_s > cutoff_hi:
                continue
            cur = loudest_per_sensor.get(c.sensor_id)
            if cur is None or c.rssi > cur.rssi:
                loudest_per_sensor[c.sensor_id] = c

        rssi_at_touch = loudest_per_sensor.get(sensor_id)
        rssi_val = rssi_at_touch.rssi if rssi_at_touch else None

        # Identify the overall loudest sensor — used to detect label swaps.
        strongest_id: str | None = None
        strongest_rssi: int = -127
        for sid, cs in loudest_per_sensor.items():
            if cs.rssi > strongest_rssi:
                strongest_rssi = cs.rssi
                strongest_id = sid

        # Warnings + severity ladder
        warnings: list[str] = []
        gps_drift_m = _haversine_m(phone_lat, phone_lon, sensor_lat, sensor_lon)
        if gps_drift_m > self.GPS_DRIFT_WARN_M:
            warnings.append(
                f"gps_drift_{gps_drift_m:.0f}m_likely_wrong_coords"
            )
        if rssi_val is None:
            warnings.append("no_rssi_heard_at_touch_check_uuid")
        if anchor_source == "sensor_position_fallback":
            warnings.append("anchor_used_saved_sensor_coordinates")
        elif strongest_id and strongest_id != sensor_id and \
             strongest_rssi - (rssi_val or -127) > self.LABEL_MATCH_MARGIN_DB:
            warnings.append(
                f"claimed_sensor_not_strongest_loudest_was_{strongest_id}_likely_swapped"
            )

        if any("likely_swapped" in w for w in warnings) or \
           any("no_rssi_heard" in w for w in warnings):
            severity = "error"
        elif warnings or gps_drift_m > self.GPS_DRIFT_OK_M:
            severity = "warn"
        else:
            severity = "ok"

        # Synthesize a high-weight d≈1 m sample so the OLS fit gets an
        # RSSI_REF anchor for this listener. The phone is "right next
        # to" the sensor, so we treat it as 1 m for the log-distance
        # math (avoids log10(0) blowup; 1 m is well within human reach).
        if rssi_val is not None:
            s.samples.append(_CalSample(
                sensor_id=sensor_id,
                sensor_lat=sensor_lat,
                sensor_lon=sensor_lon,
                rssi=rssi_val,
                ts_s=now,
                weight=self.CHECKPOINT_OLS_WEIGHT,
            ))
            # Force the trace point too — gives the fit a phone position
            # exactly co-located with the sensor at this timestamp.
            s.trace.append(_TracePoint(
                lat=sensor_lat, lon=sensor_lon, ts_s=now,
                accuracy_m=phone_accuracy_m,
            ))

        s.checkpoints.append(_Checkpoint(
            sensor_id=sensor_id,
            claimed_sensor_lat=sensor_lat,
            claimed_sensor_lon=sensor_lon,
            phone_lat=phone_lat,
            phone_lon=phone_lon,
            phone_accuracy_m=phone_accuracy_m,
            rssi_at_touch=rssi_val,
            strongest_sensor_at_touch=strongest_id,
            ts_s=now,
            anchor_source=anchor_source,
        ))
        return {
            "ok": rssi_val is not None,
            "sensor_id": sensor_id,
            "gps_drift_m": round(gps_drift_m, 1),
            "rssi_at_touch": rssi_val,
            "strongest_sensor_at_touch": strongest_id,
            "warnings": warnings,
            "severity": severity,
            "accepted_into_fit": rssi_val is not None,
            "anchor_source": anchor_source,
        }

    def find_active_for_uuid(self, advertised_uuid: str) -> WalkSession | None:
        """Lookup helper used by the detection-ingest hook to attach
        incoming sensor samples to the right session by its UUID."""
        u = (advertised_uuid or "").lower()
        for s in self.sessions.values():
            if s.ended_at is None and s.expected_uuid.lower() == u:
                return s
        return None

    def end(self, session_id: str) -> WalkSession | None:
        s = self.sessions.get(session_id)
        if s is None:
            return None
        s.ended_at = time.time()
        s.fit_result = self.fit(s)
        return s

    def get(self, session_id: str) -> WalkSession | None:
        return self.sessions.get(session_id)

    # ── Readiness constants ────────────────────────────────────────
    # Closed-loop readiness tells the phone UI when a sensor has
    # enough data to fit cleanly, so the operator knows when to stop
    # standing at a given spot vs. keep walking. These thresholds
    # were picked to match what the OLS fitter actually needs:
    # ≥ 20 samples so the regression isn't under-determined,
    # ≥ 5 m distance span so log10(d) has dynamic range, and a
    # checkpoint so RSSI_REF is anchored at d≈1 m. Without a
    # checkpoint the sample count requirement goes up to 50.
    READY_MIN_SAMPLES_WITH_CHECKPOINT = 20
    READY_MIN_SAMPLES_NO_CHECKPOINT  = 50
    READY_MIN_DISTANCE_RANGE_M       = 5.0
    # How many sensors must be ready before we call the whole session
    # ready. 4/6 = 2/3 of the deployed fleet — meaningful coverage
    # without requiring the operator to visit every last corner.
    SESSION_READY_MIN_SENSORS = 4

    def _sensor_readiness(self, session: WalkSession, sensor_id: str) -> dict:
        """Return readiness diagnostics for one sensor in this walk."""
        # Distance statistics over ALL accepted samples (not just recent
        # window) — a sensor that heard us 10 min ago at close range
        # still counts toward the fit's distance span.
        distances: list[float] = []
        for c in session.samples:
            if c.sensor_id != sensor_id:
                continue
            pos = _interpolate_position(session.trace, c.ts_s)
            if pos is None:
                continue
            d = _haversine_m(pos[0], pos[1], c.sensor_lat, c.sensor_lon)
            if 0.5 <= d <= 200.0:
                distances.append(d)
        has_checkpoint = any(cp.sensor_id == sensor_id for cp in session.checkpoints)
        span = (max(distances) - min(distances)) if distances else 0.0
        needed = (self.READY_MIN_SAMPLES_WITH_CHECKPOINT if has_checkpoint
                  else self.READY_MIN_SAMPLES_NO_CHECKPOINT)
        ready = (
            len(distances) >= needed and
            span >= self.READY_MIN_DISTANCE_RANGE_M
        )
        hints: list[str] = []
        if not has_checkpoint:
            hints.append("walk_up_and_press_im_here")
        if span < self.READY_MIN_DISTANCE_RANGE_M:
            hints.append("walk_farther_from_sensor_to_widen_range")
        if len(distances) < needed:
            hints.append(f"need_{needed - len(distances)}_more_samples")
        return {
            "samples_count": len(distances),
            "samples_needed": needed,
            "distance_range_m": round(span, 1),
            "distance_min_m": round(min(distances), 1) if distances else None,
            "distance_max_m": round(max(distances), 1) if distances else None,
            "has_checkpoint": has_checkpoint,
            "ready": ready,
            "hints": hints,
        }

    def feedback(self,
                 session_id: str,
                 window_s: float = 10.0,
                 eligible_sensor_ids: list[str] | set[str] | tuple[str, ...] | None = None) -> dict:
        """Live "what does the fleet hear right now" snapshot for the
        Android UI. Group recent sensor samples by sensor_id, return
        latest RSSI + sample count + interpolated distance + per-sensor
        readiness so the operator knows when to stop standing at a
        given spot vs. keep walking. Also returns an overall session-
        readiness summary that drives a "you can stop now" banner."""
        s = self.sessions.get(session_id)
        if s is None:
            return {"error": "unknown_session"}
        now = time.time()
        cutoff = now - window_s
        recent = [c for c in s.samples if c.ts_s >= cutoff]
        per_sensor_recent: dict[str, list[_CalSample]] = {}
        for c in recent:
            per_sensor_recent.setdefault(c.sensor_id, []).append(c)
        eligible_ids = sorted({
            str(sensor_id)
            for sensor_id in (eligible_sensor_ids or [])
            if sensor_id
        })
        eligible_set = set(eligible_ids)
        # Union of every sensor we've ever heard from this session +
        # every sensor the operator touched. Gives the UI a stable row
        # per sensor instead of appearing/disappearing with silence.
        if eligible_set:
            all_sensor_ids = set(eligible_set)
        else:
            all_sensor_ids = set(per_sensor_recent.keys()) | {c.sensor_id for c in s.samples} | {
                cp.sensor_id for cp in s.checkpoints
            }
        last_pos = (s.trace[-1].lat, s.trace[-1].lon) if s.trace else None
        items = []
        for sid in all_sensor_ids:
            lst = per_sensor_recent.get(sid, [])
            latest = max(lst, key=lambda c: c.ts_s) if lst else None
            # Fallback to the most recent all-time sample for sensor_lat
            # / sensor_lon when the recent window is empty (so we can
            # still compute distance to phone GPS and draw the row).
            fallback = None
            if latest is None:
                for c in reversed(s.samples):
                    if c.sensor_id == sid:
                        fallback = c
                        break
            src = latest or fallback
            d_m = None
            if src and last_pos:
                d_m = _haversine_m(last_pos[0], last_pos[1],
                                   src.sensor_lat, src.sensor_lon)
            readiness = self._sensor_readiness(s, sid)
            cp = max((c for c in s.checkpoints if c.sensor_id == sid),
                     key=lambda c: c.ts_s, default=None)
            checkpoint_status = "none"
            if cp is not None:
                checkpoint_status = "error" if cp.rssi_at_touch is None else (
                    "warn"
                    if _haversine_m(cp.phone_lat, cp.phone_lon,
                                    cp.claimed_sensor_lat, cp.claimed_sensor_lon) > self.GPS_DRIFT_OK_M
                    else "ok"
                )
            last_all_time = latest or fallback
            last_heard_age_s = (
                round(now - last_all_time.ts_s, 1)
                if last_all_time is not None else None
            )
            items.append({
                "sensor_id": sid,
                "current_rssi": latest.rssi if latest else None,
                "last_rssi": last_all_time.rssi if last_all_time else None,
                "last_heard_age_s": last_heard_age_s,
                "samples_in_window": len(lst),
                "sample_count_total": readiness["samples_count"],
                "scanner_slots_seen": last_all_time.scanner_slots_seen if last_all_time else None,
                "accepted_into_fit": readiness["samples_count"] > 0,
                "checkpoint_status": checkpoint_status,
                "anchor_source": cp.anchor_source if cp else None,
                "distance_m_estimated_from_phone_gps": round(d_m, 1) if d_m is not None else None,
                "readiness": readiness,
            })
        items.sort(key=lambda r: (not r["readiness"]["ready"],
                                   -(r["current_rssi"] or -127)))
        sensors_ready = sum(1 for it in items if it["readiness"]["ready"])
        heard_sensor_ids = sorted([
            sid for sid, lst in per_sensor_recent.items()
            if lst and (not eligible_set or sid in eligible_set)
        ])
        sensors_total = len(eligible_ids) if eligible_ids else len(items)
        return {
            "session_id": session_id,
            "trace_points": len(s.trace),
            "samples_total": len(s.samples),
            "samples_recent": len(recent),
            "phone_lat": last_pos[0] if last_pos else None,
            "phone_lon": last_pos[1] if last_pos else None,
            "eligible_sensor_count": sensors_total,
            "eligible_sensor_ids": eligible_ids if eligible_ids else sorted(all_sensor_ids),
            "heard_sensor_count": len(heard_sensor_ids),
            "heard_sensor_ids": heard_sensor_ids,
            "sensors": items,
            "session_readiness": {
                "sensors_ready": sensors_ready,
                "sensors_total": sensors_total,
                "ready_overall": sensors_ready >= self.SESSION_READY_MIN_SENSORS,
                "min_required": self.SESSION_READY_MIN_SENSORS,
            },
        }

    def fit(self, session: WalkSession) -> dict:
        """Per-listener OLS path-loss fit. Returns a dict suitable for
        UI display and for handing to triangulation.update_calibration().
        """
        if not session.trace:
            return {"ok": False, "reason": "no_trace_points"}
        if not session.samples:
            return {"ok": False, "reason": "no_sensor_samples"}

        # Bucket samples by sensor and join with interpolated phone GPS.
        # Normal walk packets are collapsed into 1 s median-RSSI buckets so
        # repeated packets from a loud receiver improve confidence without
        # dominating the regression. Checkpoint samples remain weighted because
        # they deliberately anchor the d≈1 m intercept.
        per_sensor: dict[str, list[tuple[float, float]]] = {}  # sensor_id → [(d_m, rssi), ...]
        validation_pairs: dict[str, list[tuple[float, float]]] = {}
        packet_buckets: dict[str, dict[int, list[tuple[float, float]]]] = {}
        raw_samples_by_sensor: dict[str, int] = {}
        checkpoint_fit_samples = 0
        for c in session.samples:
            raw_samples_by_sensor[c.sensor_id] = raw_samples_by_sensor.get(c.sensor_id, 0) + 1
            if c.weight > 1:
                d_m = 1.0   # checkpoint anchor — operator next to sensor
                entry = (d_m, float(c.rssi))
                bucket = per_sensor.setdefault(c.sensor_id, [])
                for _ in range(max(1, c.weight)):
                    bucket.append(entry)
                    checkpoint_fit_samples += 1
                validation_pairs.setdefault(c.sensor_id, []).append(entry)
            else:
                pos = _interpolate_position(session.trace, c.ts_s)
                if pos is None:
                    continue
                d_m = _haversine_m(pos[0], pos[1], c.sensor_lat, c.sensor_lon)
                if d_m < 0.5 or d_m > 200.0:
                    # Reject implausible distances — likely GPS noise or
                    # the phone advertised before/after the trace span.
                    continue
                bucket_id = int(c.ts_s // FIT_PACKET_BUCKET_S)
                packet_buckets.setdefault(c.sensor_id, {}).setdefault(bucket_id, []).append(
                    (d_m, float(c.rssi))
                )

        sample_windows_by_sensor: dict[str, int] = {}
        for sid, buckets in packet_buckets.items():
            sample_windows_by_sensor[sid] = len(buckets)
            for samples in buckets.values():
                d_m = float(median([p[0] for p in samples]))
                rssi = float(median([p[1] for p in samples]))
                entry = (d_m, rssi)
                per_sensor.setdefault(sid, []).append(entry)
                validation_pairs.setdefault(sid, []).append(entry)

        # Pre-index checkpoints per sensor so we can attach sanity fields
        # (gps_drift_m, rssi_at_touch, label_match) to each per-listener
        # entry below. Using the most recent checkpoint per sensor — if
        # the operator touches the same sensor twice, the second reading
        # wins (likely re-tested after a coordinate fix).
        latest_checkpoint: dict[str, _Checkpoint] = {}
        for cp in session.checkpoints:
            cur = latest_checkpoint.get(cp.sensor_id)
            if cur is None or cp.ts_s > cur.ts_s:
                latest_checkpoint[cp.sensor_id] = cp

        per_listener_models: dict[str, dict] = {}
        successful_fits: list[tuple[str, float, float, float, int]] = []  # (sid, rref, n, r2, samples)
        for sid, pairs in per_sensor.items():
            ds = [p[0] for p in pairs]
            rs = [p[1] for p in pairs]
            res = _ols_fit(ds, rs)
            distance_span = (max(ds) - min(ds)) if ds else 0.0
            entry: dict
            if res is None:
                entry = {
                    "samples": len(pairs),
                    "raw_samples": raw_samples_by_sensor.get(sid, 0),
                    "sample_windows": sample_windows_by_sensor.get(sid, 0),
                    "distance_range_m": round(distance_span, 1),
                    "ok": False,
                    "reason": "insufficient_or_out_of_band",
                }
            else:
                rref, n, r2 = res
                entry = {
                    "samples": len(pairs),
                    "raw_samples": raw_samples_by_sensor.get(sid, 0),
                    "sample_windows": sample_windows_by_sensor.get(sid, 0),
                    "distance_range_m": round(distance_span, 1),
                    "rssi_ref": round(rref, 2),
                    "path_loss_exponent": round(n, 3),
                    "r_squared": round(r2, 3),
                    "ok": True,
                }
                successful_fits.append((sid, rref, n, r2, len(pairs)))
            cp = latest_checkpoint.get(sid)
            if cp is not None:
                drift = _haversine_m(cp.phone_lat, cp.phone_lon,
                                     cp.claimed_sensor_lat, cp.claimed_sensor_lon)
                entry["gps_drift_m"] = round(drift, 1)
                entry["rssi_at_touch"] = cp.rssi_at_touch
                entry["anchor_source"] = cp.anchor_source
                entry["label_match"] = (
                    cp.strongest_sensor_at_touch is None or
                    cp.strongest_sensor_at_touch == sid
                )
                w: list[str] = []
                if drift > self.GPS_DRIFT_WARN_M:
                    w.append(f"gps_drift_{drift:.0f}m_likely_wrong_coords")
                if cp.strongest_sensor_at_touch and cp.strongest_sensor_at_touch != sid:
                    w.append(
                        f"loudest_at_touch_was_{cp.strongest_sensor_at_touch}_likely_swapped"
                    )
                if cp.anchor_source == "sensor_position_fallback":
                    w.append("anchor_used_saved_sensor_coordinates")
                entry["warnings"] = w
            per_listener_models[sid] = entry

        # Global model = R²-weighted average of per-listener fits.
        # Pooling raw (d, rssi) pairs across sensors is mathematically wrong
        # when per-listener RSSI_REF differs (the same distance produces
        # different RSSI on two nodes); the pooled regression gets a degraded
        # slope. Averaging successful per-listener fits, weighted by goodness-
        # of-fit, gives a sensible fallback model for new sensors that haven't
        # been individually calibrated yet.
        if not successful_fits:
            return {
                "ok": False,
                "reason": "no_per_listener_fits_succeeded",
                "per_listener": per_listener_models,
                "trace_points": len(session.trace),
                "samples_total": len(session.samples),
            }
        weights = [r2 * samples for _, _, _, r2, samples in successful_fits]
        wsum = sum(weights) or 1.0
        g_ref = sum(w * rref for w, (_, rref, _, _, _) in zip(weights, successful_fits)) / wsum
        g_n = sum(w * n for w, (_, _, n, _, _) in zip(weights, successful_fits)) / wsum
        # Composite r² is the average of per-listener r²s — gives the operator
        # a single fit-quality number that represents the typical sensor.
        g_r2 = sum(r2 for _, _, _, r2, _ in successful_fits) / len(successful_fits)

        accepted_listener_ids: list[str] = []
        all_range_errors: list[float] = []
        for sid, entry in per_listener_models.items():
            pairs = validation_pairs.get(sid, [])
            errors: list[float] = []
            if entry.get("ok"):
                rref = float(entry["rssi_ref"])
                n = float(entry["path_loss_exponent"])
                for d_m, rssi in pairs:
                    pred = _distance_from_model(rssi, rref, n)
                    err = abs(pred - d_m)
                    errors.append(err)
                    all_range_errors.append(err)
            entry["range_error_m"] = _error_stats(errors)

            accepted = bool(entry.get("ok"))
            reject_reasons: list[str] = []
            if not accepted:
                reject_reasons.append(str(entry.get("reason") or "fit_failed"))
            elif float(entry.get("r_squared") or 0.0) < 0.35:
                accepted = False
                reject_reasons.append("listener_r2_below_0_35")
            err_stats = entry.get("range_error_m") or {}
            if accepted and (err_stats.get("median_m") is not None) and float(err_stats["median_m"]) > 20.0:
                accepted = False
                reject_reasons.append("median_range_error_above_20m")
            if accepted and (err_stats.get("p90_m") is not None) and float(err_stats["p90_m"]) > 50.0:
                accepted = False
                reject_reasons.append("p90_range_error_above_50m")
            if accepted and float(entry.get("gps_drift_m") or 0.0) > APPLY_MAX_CHECKPOINT_DRIFT_M:
                accepted = False
                reject_reasons.append("checkpoint_gps_drift_above_15m")
            entry["accepted_for_apply"] = accepted
            entry["apply_reject_reasons"] = reject_reasons
            if accepted:
                accepted_listener_ids.append(sid)

        target_ids = sorted({
            str(sensor_id)
            for sensor_id in (getattr(session, "target_sensor_ids", None) or [])
            if sensor_id
        })
        if not target_ids:
            target_ids = sorted(per_sensor.keys())
        target_count = len(target_ids)
        required_listeners = (
            max(APPLY_MIN_OK_LISTENERS, math.ceil(target_count * APPLY_MIN_TARGET_COVERAGE))
            if target_count >= APPLY_MIN_OK_LISTENERS else target_count
        )
        target_coverage = (
            len([sid for sid in accepted_listener_ids if sid in set(target_ids)]) / target_count
            if target_count else 0.0
        )
        apply_reasons: list[str] = []
        if g_r2 < APPLY_MIN_GLOBAL_R2:
            apply_reasons.append(f"global_r2_below_{APPLY_MIN_GLOBAL_R2:.2f}")
        if len(accepted_listener_ids) < required_listeners:
            apply_reasons.append(
                f"accepted_listeners_{len(accepted_listener_ids)}_below_required_{required_listeners}"
            )
        if target_count and target_coverage < APPLY_MIN_TARGET_COVERAGE:
            apply_reasons.append(
                f"target_coverage_{target_coverage:.2f}_below_{APPLY_MIN_TARGET_COVERAGE:.2f}"
            )
        for sid in target_ids:
            entry = per_listener_models.get(sid)
            if entry and not entry.get("accepted_for_apply"):
                reasons = ",".join(entry.get("apply_reject_reasons") or [entry.get("reason") or "not_accepted"])
                apply_reasons.append(f"listener_{sid}:{reasons}")

        model_validation = {
            "applyable": not apply_reasons,
            "reasons": apply_reasons,
            "global_r2": round(g_r2, 3),
            "min_global_r2": APPLY_MIN_GLOBAL_R2,
            "accepted_listener_count": len(accepted_listener_ids),
            "accepted_listener_ids": accepted_listener_ids,
            "successful_listener_count": len(successful_fits),
            "target_sensor_count": target_count,
            "target_sensor_ids": target_ids,
            "target_coverage": round(target_coverage, 3),
            "min_target_coverage": APPLY_MIN_TARGET_COVERAGE,
            "required_listener_count": required_listeners,
            "range_error_m": _error_stats(all_range_errors),
            "packet_bucket_s": FIT_PACKET_BUCKET_S,
            "raw_samples_total": len(session.samples),
            "fit_sample_windows": sum(sample_windows_by_sensor.values()),
            "checkpoint_fit_samples": checkpoint_fit_samples,
        }

        return {
            "ok": True,
            "global_rssi_ref": round(g_ref, 2),
            "global_path_loss_exponent": round(g_n, 3),
            "global_r_squared": round(g_r2, 3),
            "per_listener": per_listener_models,
            "trace_points": len(session.trace),
            "samples_total": len(session.samples),
            "checkpoints_total": len(session.checkpoints),
            "checkpointed_sensor_count": len(latest_checkpoint),
            "session_id": session.session_id,
            "tx_power_dbm": session.tx_power_dbm,
            "model_validation": model_validation,
        }

    def prune(self) -> None:
        now = time.time()
        if now - self._last_prune < 60.0:
            return
        self._last_prune = now
        stale = [sid for sid, s in self.sessions.items()
                 if s.ended_at and now - s.ended_at > SESSION_TTL_S]
        for sid in stale:
            del self.sessions[sid]
