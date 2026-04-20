"""Phone-walk calibration: OLS fit + session lifecycle + alpha-beta smoother.

Synthetic-data tests so we can run them headless without nodes or a phone.
"""

import math
import time

import pytest
from httpx import AsyncClient, ASGITransport

from app.services.phone_calibration import (
    PhoneCalibrationManager,
    _ols_fit,
    _haversine_m,
    _interpolate_position,
    _TracePoint,
    DEFAULT_PHONE_TX_DBM,
    MIN_SAMPLES_PER_LISTENER,
)
from app.services.position_filter import AlphaBetaTracker, AlphaBetaManager


# ──────────────────────────────────────────────────────────────────────
# OLS fit math
# ──────────────────────────────────────────────────────────────────────

def test_ols_fit_recovers_synthetic_path_loss_model():
    """Generate a clean log-distance dataset and confirm the fit recovers
    the (RSSI_REF, n) we baked in."""
    true_ref = -55.0
    true_n = 2.7
    distances = [d * 1.0 for d in range(1, 31)] * 2  # 60 samples, 1-30 m
    rssis = [true_ref - 10 * true_n * math.log10(d) for d in distances]
    fit = _ols_fit(distances, rssis)
    assert fit is not None
    rref, n, r2 = fit
    assert abs(rref - true_ref) < 0.5, f"rref recovered as {rref}"
    assert abs(n - true_n) < 0.05, f"n recovered as {n}"
    assert r2 > 0.99


def test_ols_fit_rejects_thin_data():
    distances = [1.0, 2.0, 3.0]
    rssis = [-50.0, -60.0, -65.0]
    assert _ols_fit(distances, rssis) is None


def test_ols_fit_rejects_out_of_band_exponent():
    """Garbage data that fits an n outside [1.8, 5.0] must be rejected
    rather than silently broadcasting a bad model."""
    distances = [1.0 + i * 0.1 for i in range(40)]
    rssis = [-50.0 - 100 * math.log10(d) for d in distances]  # n=10, way out of band
    assert _ols_fit(distances, rssis) is None


# ──────────────────────────────────────────────────────────────────────
# Trace interpolation
# ──────────────────────────────────────────────────────────────────────

def test_trace_interpolation_linear_between_points():
    trace = [
        _TracePoint(lat=37.0, lon=-122.0, ts_s=1000.0),
        _TracePoint(lat=37.001, lon=-122.0, ts_s=2000.0),
    ]
    p = _interpolate_position(trace, 1500.0)
    assert p is not None
    lat, lon = p
    assert abs(lat - 37.0005) < 1e-6
    assert abs(lon + 122.0) < 1e-6


def test_trace_interpolation_clamps_to_endpoints():
    trace = [_TracePoint(lat=37.0, lon=-122.0, ts_s=1000.0)]
    assert _interpolate_position(trace, 999.0) == (37.0, -122.0)
    assert _interpolate_position(trace, 9999.0) == (37.0, -122.0)


# ──────────────────────────────────────────────────────────────────────
# Session lifecycle
# ──────────────────────────────────────────────────────────────────────

def test_session_full_loop_produces_sane_per_listener_models():
    """Walk a synthetic property: phone walks a 100-m path that passes
    within ~3 m of each sensor (mirroring the real "walk near every
    sensor" calibration recommendation). Each sensor's per-listener fit
    should recover the (RSSI_REF, n) we baked in."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("test", tx_power_dbm=-59.0)

    sensor_a = (37.000_00, -122.000_00)
    sensor_b = (37.000_45, -122.000_45)  # ~58 m NE of sensor_a
    true_ref_a = -55.0
    true_ref_b = -52.0
    true_n = 2.5

    # Path: 100 steps walking from south-west of sensor_a to north-east
    # of sensor_b. Crosses near-zero distance to both. ~1 m per step in
    # both lat and lon (1 deg lat ≈ 111 km; 1e-5 deg ≈ 1.11 m).
    for i in range(100):
        ts = 1_700_000_000 + i  # epoch seconds
        lat = 36.999_95 + i * 0.000_005   # ~0.55 m/step north
        lon = -122.000_05 + i * 0.000_005  # ~0.55 m/step east
        mgr.add_trace_point(s.session_id, lat, lon, ts_s=float(ts))
        for sensor_id, (slat, slon), rref in [
            ("sensor_a", sensor_a, true_ref_a),
            ("sensor_b", sensor_b, true_ref_b),
        ]:
            d = _haversine_m(lat, lon, slat, slon)
            rssi = rref - 10 * true_n * math.log10(max(d, 1.0))
            mgr.add_sensor_sample(s.session_id, sensor_id, slat, slon,
                                  int(round(rssi)), ts_s=float(ts))

    ended = mgr.end(s.session_id)
    fit = ended.fit_result
    assert fit["ok"] is True, fit
    assert fit["global_r_squared"] > 0.9
    assert abs(fit["global_path_loss_exponent"] - true_n) < 0.3
    pl = fit["per_listener"]
    assert pl["sensor_a"]["ok"] is True
    assert pl["sensor_b"]["ok"] is True
    # Per-listener fits should distinguish the two sensors' references
    assert abs(pl["sensor_a"]["rssi_ref"] - true_ref_a) < 2.0
    assert abs(pl["sensor_b"]["rssi_ref"] - true_ref_b) < 2.0


def test_session_with_no_samples_returns_clear_failure():
    mgr = PhoneCalibrationManager()
    s = mgr.start("empty", None)
    mgr.add_trace_point(s.session_id, 37.0, -122.0)
    ended = mgr.end(s.session_id)
    assert ended.fit_result["ok"] is False
    assert "no_sensor_samples" in ended.fit_result["reason"]


def test_find_active_for_uuid():
    mgr = PhoneCalibrationManager()
    s = mgr.start("u", None)
    found = mgr.find_active_for_uuid(s.expected_uuid.upper())  # case-insensitive
    assert found is not None and found.session_id == s.session_id
    mgr.end(s.session_id)
    assert mgr.find_active_for_uuid(s.expected_uuid) is None


# ──────────────────────────────────────────────────────────────────────
# Alpha-beta smoother
# ──────────────────────────────────────────────────────────────────────

def test_alpha_beta_dampens_jitter():
    """Feed a noisy stationary signal — smoother output variance should
    be much smaller than input variance."""
    import random
    random.seed(42)
    t = AlphaBetaTracker(alpha=0.2, beta=0.05)
    inputs = [(random.gauss(0, 5.0), random.gauss(0, 5.0), float(i)) for i in range(50)]
    outputs = [t.update(x, y, ts) for x, y, ts in inputs]
    in_var = sum(x*x + y*y for x, y, _ in inputs) / len(inputs)
    # Skip the first 5 outputs (warmup), compute variance of remainder
    out_var = sum(x*x + y*y for x, y in outputs[10:]) / len(outputs[10:])
    assert out_var < in_var * 0.5, f"smoother failed to dampen jitter: {in_var} vs {out_var}"


def test_alpha_beta_tracks_constant_velocity():
    """Linear motion at 1 m/s — smoother should converge to track it."""
    t = AlphaBetaTracker(alpha=0.5, beta=0.15)
    for i in range(40):
        t.update(float(i), 0.0, float(i))
    # After 40 steps of constant velocity, vx should be near 1.0 m/s
    assert 0.7 < t.vx < 1.3, f"velocity tracking failed: vx={t.vx}"


def test_alpha_beta_manager_per_class_gains():
    """Verify per-class gain selection picks tighter alpha for stationary."""
    mgr = AlphaBetaManager()
    # Same noisy series, different motion classes — stationary smoother
    # should produce a tighter (lower-variance) output trajectory.
    import random
    random.seed(1)
    pts = [(random.gauss(0, 3.0), random.gauss(0, 3.0), float(i)) for i in range(40)]
    out_stat = []
    out_move = []
    for x, y, ts in pts:
        out_stat.append(mgr.smooth("a", x, y, "stationary", now=ts))
        out_move.append(mgr.smooth("b", x, y, "moving", now=ts))
    var_stat = sum(x*x + y*y for x, y in out_stat[10:]) / len(out_stat[10:])
    var_move = sum(x*x + y*y for x, y in out_move[10:]) / len(out_move[10:])
    assert var_stat < var_move, (
        f"stationary should smooth more aggressively: "
        f"stationary_var={var_stat} moving_var={var_move}")


# ──────────────────────────────────────────────────────────────────────
# Endpoint integration (auth, lifecycle)
# ──────────────────────────────────────────────────────────────────────

# ──────────────────────────────────────────────────────────────────────
# Checkpoint anchoring + sensor-placement sanity
# ──────────────────────────────────────────────────────────────────────

def test_checkpoint_anchors_rssi_ref_when_walk_geometry_is_thin():
    """The exact failure mode that bit us in the original synthetic test:
    phone walks at 50–80 m offset from a sensor, RSSI_REF intercept drifts
    several dB from truth. A single checkpoint at d≈1 m should pin it."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("anchor", tx_power_dbm=-59.0)

    sensor = (37.0, -122.0)
    true_ref = -55.0
    true_n = 2.5

    # Walk far from the sensor — every sample is ~70 m away.
    for i in range(80):
        ts = float(1_700_000_000 + i)
        lat = 37.0006   # ~67 m north of sensor
        lon = -122.0001 + i * 0.0000089  # crawls east, ~1 m/step
        mgr.add_trace_point(s.session_id, lat, lon, ts_s=ts)
        d = _haversine_m(lat, lon, *sensor)
        rssi = true_ref - 10 * true_n * math.log10(max(d, 1.0))
        mgr.add_sensor_sample(s.session_id, "sensor_a", sensor[0], sensor[1],
                              int(round(rssi)), ts_s=ts)

    # Touch: operator walks up and presses "I'm here" (provide its own
    # sample so the loudest-at-touch lookup has data).
    touch_ts = float(1_700_000_080)
    mgr.add_sensor_sample(s.session_id, "sensor_a", sensor[0], sensor[1],
                          int(round(true_ref)), ts_s=touch_ts)
    res = mgr.add_checkpoint(
        session_id=s.session_id, sensor_id="sensor_a",
        sensor_lat=sensor[0], sensor_lon=sensor[1],
        phone_lat=sensor[0] + 0.0000045, phone_lon=sensor[1],  # ~0.5 m off
        phone_accuracy_m=2.0, ts_s=touch_ts,
    )
    assert res["ok"]
    assert res["severity"] == "ok"
    assert res["rssi_at_touch"] == int(round(true_ref))

    ended = mgr.end(s.session_id)
    fit = ended.fit_result
    assert fit["ok"], fit
    pl = fit["per_listener"]["sensor_a"]
    # Without the checkpoint anchor this drifted ~5 dB. With the anchor
    # repeated 10× by CHECKPOINT_OLS_WEIGHT, recover to ≤ 1 dB error.
    assert abs(pl["rssi_ref"] - true_ref) < 1.5, pl
    assert pl["rssi_at_touch"] == int(round(true_ref))
    assert pl["label_match"] is True
    assert pl["warnings"] == []
    assert pl["gps_drift_m"] < 1.0


def test_checkpoint_warns_on_gps_drift():
    """Phone GPS 30 m from registered sensor coords → fit & response
    flag the discrepancy so the operator can fix the DB row."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("drift", None)
    mgr.add_sensor_sample(s.session_id, "sensor_a", 37.0, -122.0, -50,
                          ts_s=1_700_000_010.0)
    # Phone is ~33 m west of where the sensor is registered.
    res = mgr.add_checkpoint(
        session_id=s.session_id, sensor_id="sensor_a",
        sensor_lat=37.0, sensor_lon=-122.0,
        phone_lat=37.0, phone_lon=-122.0004,
        phone_accuracy_m=3.0, ts_s=1_700_000_010.0,
    )
    assert res["ok"]
    assert res["gps_drift_m"] > 25
    assert any("gps_drift" in w for w in res["warnings"])
    assert res["severity"] in ("warn", "error")


def test_readiness_is_not_ready_initially():
    """Fresh session with no samples → no sensor ready, session not ready."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("readiness", None)
    fb = mgr.feedback(s.session_id)
    assert fb["session_readiness"]["sensors_ready"] == 0
    assert fb["session_readiness"]["ready_overall"] is False


def test_readiness_ready_after_enough_samples_plus_checkpoint():
    """A sensor that has heard enough samples across a useful distance
    range AND been touched should flip to ready."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("readiness2", None)
    sensor = (37.0, -122.0)
    # Walk ~30 steps across ~30 m to give the sensor samples spanning
    # multiple distances.
    for i in range(30):
        ts = float(1_700_000_000 + i)
        lat = 37.0 + i * 0.00001        # ~1.1 m/step
        lon = -122.0 + i * 0.00001
        mgr.add_trace_point(s.session_id, lat, lon, ts_s=ts)
        d = _haversine_m(lat, lon, *sensor)
        mgr.add_sensor_sample(s.session_id, "sensor_a", *sensor,
                              int(-55 - 25 * (d / 10)), ts_s=ts)
    # Before checkpoint — not ready (no d≈0 anchor)
    fb = mgr.feedback(s.session_id)
    item = next(it for it in fb["sensors"] if it["sensor_id"] == "sensor_a")
    # Walk has enough samples + enough range, but no checkpoint yet, so
    # the stricter 50-sample threshold applies — not ready.
    assert item["readiness"]["has_checkpoint"] is False
    # Touch the sensor — drops threshold to 20, should now be ready.
    mgr.add_checkpoint(
        session_id=s.session_id, sensor_id="sensor_a",
        sensor_lat=sensor[0], sensor_lon=sensor[1],
        phone_lat=sensor[0], phone_lon=sensor[1],
        phone_accuracy_m=2.0,
    )
    fb = mgr.feedback(s.session_id)
    item = next(it for it in fb["sensors"] if it["sensor_id"] == "sensor_a")
    assert item["readiness"]["has_checkpoint"] is True
    assert item["readiness"]["ready"] is True
    assert item["readiness"]["samples_count"] >= 20


def test_readiness_hints_guide_the_operator():
    """A sensor short on samples / range / checkpoint should return
    actionable hints so the phone UI can tell the operator what to do."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("hints", None)
    sensor = (37.0, -122.0)
    # Only 3 samples at almost the same distance
    for i in range(3):
        ts = float(1_700_000_000 + i)
        mgr.add_trace_point(s.session_id, 37.0005, -122.0005, ts_s=ts)
        mgr.add_sensor_sample(s.session_id, "sensor_a", *sensor, -80, ts_s=ts)
    fb = mgr.feedback(s.session_id)
    item = next(it for it in fb["sensors"] if it["sensor_id"] == "sensor_a")
    hints = item["readiness"]["hints"]
    # All three hint categories should fire
    assert any("walk_up_and_press_im_here" in h for h in hints)
    assert any("walk_farther" in h or "widen_range" in h for h in hints)
    assert any("more_samples" in h for h in hints)


def test_checkpoint_warns_on_label_swap():
    """Operator says "I'm at sensor_b" but sensor_a is the loudest
    receiver — labels are swapped or sensor_a is misplaced near sensor_b."""
    mgr = PhoneCalibrationManager()
    s = mgr.start("swap", None)
    sensor_a = (37.0, -122.0)
    sensor_b = (37.0008, -122.0008)  # ~104 m NE of sensor_a
    # Phone is at sensor_b's claimed position. sensor_a is reporting a
    # very strong signal (it's actually the one nearby — labels swapped).
    ts = 1_700_000_010.0
    mgr.add_sensor_sample(s.session_id, "sensor_a", sensor_a[0], sensor_a[1],
                          -45, ts_s=ts)
    mgr.add_sensor_sample(s.session_id, "sensor_b", sensor_b[0], sensor_b[1],
                          -85, ts_s=ts)
    res = mgr.add_checkpoint(
        session_id=s.session_id, sensor_id="sensor_b",
        sensor_lat=sensor_b[0], sensor_lon=sensor_b[1],
        phone_lat=sensor_b[0], phone_lon=sensor_b[1],
        phone_accuracy_m=3.0, ts_s=ts,
    )
    assert res["ok"]
    assert res["severity"] == "error"
    assert res["strongest_sensor_at_touch"] == "sensor_a"
    assert any("likely_swapped" in w for w in res["warnings"]), res["warnings"]


@pytest.mark.asyncio
async def test_walk_endpoints_require_token():
    from app.main import app
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as c:
        r = await c.post("/detections/calibrate/walk/start", json={"operator_label": "x"})
        assert r.status_code == 401
        r = await c.post("/detections/calibrate/walk/sample",
                         json={"session_id": "x", "lat": 0, "lon": 0})
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_walk_lifecycle_with_token():
    from app.main import app
    from app.routers.detections import _CAL_TOKEN
    transport = ASGITransport(app=app)
    headers = {"X-Cal-Token": _CAL_TOKEN}
    async with AsyncClient(transport=transport, base_url="http://testserver",
                            headers=headers) as c:
        r = await c.post("/detections/calibrate/walk/start",
                         json={"operator_label": "test"})
        assert r.status_code == 200, r.text
        body = r.json()
        sid = body["session_id"]
        assert body["advertise_uuid"].startswith("cafe")
        # Push a couple trace points
        r = await c.post("/detections/calibrate/walk/sample",
                         json={"session_id": sid, "lat": 37.0, "lon": -122.0})
        assert r.status_code == 200
        # Feedback returns the session shape even with no samples
        r = await c.get(f"/detections/calibrate/walk/feedback?session_id={sid}")
        assert r.status_code == 200
        assert r.json()["trace_points"] >= 1
        # End — no sensor samples → fit fails cleanly, applied=False
        r = await c.post("/detections/calibrate/walk/end",
                         json={"session_id": sid})
        assert r.status_code == 200
        body = r.json()
        assert body["applied"] is False
        assert body["fit"]["ok"] is False
