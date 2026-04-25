"""Phone-walk calibration: OLS fit + session lifecycle + alpha-beta smoother.

Synthetic-data tests so we can run them headless without nodes or a phone.
"""

import math
import time
import uuid
from collections import deque

import pytest
from httpx import AsyncClient, ASGITransport

from app.routers import detections
from app.services.database import create_tables
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
from app.services.triangulation import SensorTracker


async def _fake_start_node_mode(node, session):
    return {
        "ok": True,
        "session_id": session.session_id,
        "scan_mode": "calibration",
        "device_id": node["device_id"],
    }


async def _fake_stop_node_mode(node, session_id, reason):
    return {
        "ok": True,
        "session_id": session_id,
        "reason": reason,
        "device_id": node["device_id"],
    }


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


def test_checkpoint_without_recent_hearing_is_failed_anchor():
    mgr = PhoneCalibrationManager()
    s = mgr.start("silent", None)

    res = mgr.add_checkpoint(
        session_id=s.session_id,
        sensor_id="sensor_a",
        sensor_lat=37.0,
        sensor_lon=-122.0,
        phone_lat=37.0,
        phone_lon=-122.0,
        phone_accuracy_m=2.0,
        ts_s=1_700_000_010.0,
    )

    assert res["ok"] is False
    assert res["accepted_into_fit"] is False
    assert res["severity"] == "error"
    assert any("no_rssi_heard" in w for w in res["warnings"])


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
async def test_walk_lifecycle_with_token(monkeypatch):
    from app.main import app
    from app.routers.detections import _CAL_TOKEN
    await create_tables()
    device_id = f"uplink-walk-{uuid.uuid4().hex[:8]}"
    detections._phone_cal_mgr.sessions.clear()
    detections._calibration_mode.active_session_id = None
    detections._calibration_mode.fleet_mode_state = "inactive"
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            device_id: {
                "device_id": device_id,
                "last_seen": time.time(),
                "lat": 37.200001,
                "lon": -122.200001,
                "ip": "192.168.42.201",
                "detection_count": 0,
                "total_batches": 1,
                "total_detections": 0,
                "source_fixups_total": 0,
                "source_fixups_by_rule": {},
            }
        },
    )
    monkeypatch.setattr(detections._calibration_mode, "_start_node_mode", _fake_start_node_mode)
    monkeypatch.setattr(detections._calibration_mode, "_stop_node_mode", _fake_stop_node_mode)
    transport = ASGITransport(app=app)
    headers = {"X-Cal-Token": _CAL_TOKEN}
    async with AsyncClient(transport=transport, base_url="http://testserver",
                            headers=headers) as c:
        create_resp = await c.post(
            "/nodes",
            json={
                "device_id": device_id,
                "name": "Pool",
                "lat": 37.200001,
                "lon": -122.200001,
                "alt": 0.0,
                "position_mode": "active",
            },
        )
        assert create_resp.status_code == 201, create_resp.text
        r = await c.post("/detections/calibrate/walk/start",
                         json={"operator_label": "test"})
        assert r.status_code == 200, r.text
        body = r.json()
        sid = body["session_id"]
        assert body["advertise_uuid"].startswith("cafe")
        assert body["target_sensor_count"] == 1
        assert body["mode_state"] == "active"
        # Push a couple trace points
        r = await c.post("/detections/calibrate/walk/sample",
                         json={"session_id": sid, "lat": 37.0, "lon": -122.0})
        assert r.status_code == 200
        # Feedback returns the session shape even with no samples
        r = await c.get(f"/detections/calibrate/walk/feedback?session_id={sid}")
        assert r.status_code == 200
        feedback = r.json()
        assert feedback["trace_points"] >= 1
        assert feedback["eligible_sensor_count"] == 1
        assert feedback["heard_sensor_count"] == 0
        assert feedback["target_sensor_ids"] == [device_id]
        assert feedback["fleet_mode_state"] == "active"
        r = await c.get(f"/detections/calibrate/walk/my-position?session_id={sid}")
        assert r.status_code == 200
        my_pos = r.json()
        assert my_pos["eligible_sensor_count"] == 1
        assert my_pos["heard_sensor_count"] == 0
        assert my_pos["target_sensor_ids"] == [device_id]
        assert my_pos["fleet_mode_state"] == "active"
        assert my_pos["status"] == "no_sensors_hearing_you_yet"
        # End — no sensor samples → fit fails cleanly, applied=False
        r = await c.post("/detections/calibrate/walk/end",
                         json={"session_id": sid})
        assert r.status_code == 200
        body = r.json()
        assert body["applied"] is False
        assert body["verified_fit"]["ok"] is False

        delete_resp = await c.delete(f"/nodes/{device_id}")
        assert delete_resp.status_code == 204


@pytest.mark.asyncio
async def test_walk_abort_endpoint_stops_fleet_mode(monkeypatch):
    from app.main import app
    from app.routers.detections import _CAL_TOKEN

    await create_tables()
    device_id = f"uplink-abort-{uuid.uuid4().hex[:8]}"
    detections._phone_cal_mgr.sessions.clear()
    detections._calibration_mode.active_session_id = None
    detections._calibration_mode.fleet_mode_state = "inactive"
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            device_id: {
                "device_id": device_id,
                "last_seen": time.time(),
                "lat": 37.230001,
                "lon": -122.230001,
                "ip": "192.168.42.211",
            }
        },
    )
    monkeypatch.setattr(detections._calibration_mode, "_start_node_mode", _fake_start_node_mode)
    monkeypatch.setattr(detections._calibration_mode, "_stop_node_mode", _fake_stop_node_mode)

    transport = ASGITransport(app=app)
    headers = {"X-Cal-Token": _CAL_TOKEN}
    async with AsyncClient(transport=transport, base_url="http://testserver", headers=headers) as c:
        create_resp = await c.post(
            "/nodes",
            json={
                "device_id": device_id,
                "name": "AbortNode",
                "lat": 37.230001,
                "lon": -122.230001,
                "alt": 0.0,
                "position_mode": "active",
            },
        )
        assert create_resp.status_code == 201, create_resp.text

        start_resp = await c.post("/detections/calibrate/walk/start", json={"operator_label": "abort test"})
        assert start_resp.status_code == 200, start_resp.text
        sid = start_resp.json()["session_id"]
        assert detections._calibration_mode.fleet_mode_state == "active"

        abort_resp = await c.post(
            "/detections/calibrate/walk/abort",
            json={"session_id": sid, "reason": "test_abort"},
        )
        assert abort_resp.status_code == 200, abort_resp.text
        payload = abort_resp.json()
        assert payload["ok"] is True
        assert payload["mode_state"] == "inactive"
        assert payload["abort_reason"] == "test_abort"
        assert detections._calibration_mode.fleet_mode_state == "inactive"

        delete_resp = await c.delete(f"/nodes/{device_id}")
        assert delete_resp.status_code == 204


@pytest.mark.asyncio
async def test_walk_sensors_only_include_live_active_geometry_nodes(monkeypatch):
    from app.main import app
    from app.routers.detections import _CAL_TOKEN

    await create_tables()
    transport = ASGITransport(app=app)
    headers = {"X-Cal-Token": _CAL_TOKEN}

    live_frontyard_id = f"uplink-live-{uuid.uuid4().hex[:8]}"
    pool_id = f"uplink-pool-{uuid.uuid4().hex[:8]}"
    stale_frontyard_id = f"uplink-stale-{uuid.uuid4().hex[:8]}"
    gate_id = f"uplink-gate-{uuid.uuid4().hex[:8]}"
    created_ids: list[str] = []

    async with AsyncClient(transport=transport, base_url="http://testserver",
                           headers=headers) as c:
        for payload in [
            {
                "device_id": live_frontyard_id,
                "name": "FrontYard",
                "lat": 37.100001,
                "lon": -122.100001,
                "alt": 0.0,
                "position_mode": "active",
            },
            {
                "device_id": pool_id,
                "name": "Pool",
                "lat": 37.100101,
                "lon": -122.100101,
                "alt": 0.0,
                "position_mode": "active",
            },
            {
                "device_id": stale_frontyard_id,
                "name": "FrontYard",
                "lat": 37.199999,
                "lon": -122.199999,
                "alt": 0.0,
                "position_mode": "active",
            },
            {
                "device_id": gate_id,
                "name": "Gate",
                "lat": 37.300001,
                "lon": -122.300001,
                "alt": 0.0,
                "position_mode": "excluded",
            },
        ]:
            resp = await c.post("/nodes", json=payload)
            assert resp.status_code == 201, resp.text
            created_ids.append(payload["device_id"])

        now = time.time()
        monkeypatch.setattr(
            detections,
            "_node_heartbeats",
            {
                live_frontyard_id: {
                    "device_id": live_frontyard_id,
                    "last_seen": now,
                    "lat": 37.100001,
                    "lon": -122.100001,
                    "ip": "192.168.42.101",
                },
                pool_id: {
                    "device_id": pool_id,
                    "last_seen": now,
                    "lat": 37.100101,
                    "lon": -122.100101,
                    "ip": "192.168.42.102",
                },
                gate_id: {
                    "device_id": gate_id,
                    "last_seen": now,
                    "lat": 37.300001,
                    "lon": -122.300001,
                    "ip": "192.168.42.202",
                },
            },
        )

        resp = await c.get("/detections/calibrate/walk/sensors")
        assert resp.status_code == 200, resp.text
        payload = resp.json()
        sensor_ids = {sensor["device_id"] for sensor in payload["sensors"]}

        assert payload["count"] == 2
        assert live_frontyard_id in sensor_ids
        assert pool_id in sensor_ids
        assert stale_frontyard_id not in sensor_ids
        assert gate_id not in sensor_ids
    async with AsyncClient(transport=transport, base_url="http://testserver") as cleanup:
        for device_id in created_ids:
            await cleanup.delete(f"/nodes/{device_id}")


@pytest.mark.asyncio
async def test_active_calibration_beacon_creates_walk_sample_without_map_clutter(monkeypatch):
    from app.main import app
    from app.routers.detections import _CAL_TOKEN

    await create_tables()
    transport = ASGITransport(app=app)
    headers = {"X-Cal-Token": _CAL_TOKEN}

    tracker = SensorTracker()
    recent = deque(maxlen=50000)
    monkeypatch.setattr(detections, "_sensor_tracker", tracker)
    monkeypatch.setattr(detections, "_recent_detections", recent)
    monkeypatch.setattr(detections._calibration_mode, "_start_node_mode", _fake_start_node_mode)
    monkeypatch.setattr(detections._calibration_mode, "_stop_node_mode", _fake_stop_node_mode)
    detections._phone_cal_mgr.sessions.clear()

    device_id = f"uplink-cal-{uuid.uuid4().hex[:8]}"
    async with AsyncClient(transport=transport, base_url="http://testserver",
                           headers=headers) as c:
        create_resp = await c.post(
            "/nodes",
            json={
                "device_id": device_id,
                "name": "Pool",
                "lat": 37.410001,
                "lon": -122.410001,
                "alt": 0.0,
                "position_mode": "active",
            },
        )
        assert create_resp.status_code == 201, create_resp.text

        now = time.time()
        monkeypatch.setattr(
            detections,
            "_node_heartbeats",
            {
                device_id: {
                    "device_id": device_id,
                    "last_seen": now,
                    "lat": 37.410001,
                    "lon": -122.410001,
                    "ip": "192.168.42.201",
                    "detection_count": 0,
                    "total_batches": 1,
                    "total_detections": 0,
                    "source_fixups_total": 0,
                    "source_fixups_by_rule": {},
                }
            },
        )

        start_resp = await c.post(
            "/detections/calibrate/walk/start",
            json={"operator_label": "Test phone"},
        )
        assert start_resp.status_code == 200, start_resp.text
        start_body = start_resp.json()
        sid = start_body["session_id"]
        adv_uuid = start_body["advertise_uuid"]

        trace_resp = await c.post(
            "/detections/calibrate/walk/sample",
            json={
                "session_id": sid,
                "lat": 37.410050,
                "lon": -122.410050,
                "accuracy_m": 3.0,
            },
        )
        assert trace_resp.status_code == 200, trace_resp.text

        ingest_resp = await c.post(
            "/detections/drones",
            json={
                "device_id": device_id,
                "device_lat": 37.410001,
                "device_lon": -122.410001,
                "device_alt": 0.0,
                "timestamp": int(time.time()),
                "detections": [
                    {
                        "drone_id": "BLE:12345678:Calibration Beacon",
                        "source": "ble_fingerprint",
                        "confidence": 0.85,
                        "timestamp": int(time.time() * 1000),
                        "rssi": -57,
                        "manufacturer": "Calibration Beacon",
                        "model": "FP:12345678",
                        "bssid": "AA:BB:CC:DD:EE:FF",
                        "ble_svc_uuids": adv_uuid,
                    }
                ],
            },
        )
        assert ingest_resp.status_code == 200, ingest_resp.text
        assert ingest_resp.json()["accepted"] == 1

        session = detections._phone_cal_mgr.get(sid)
        assert session is not None
        assert len(session.samples) == 1
        assert len(recent) == 0
        obs = tracker.observations[f"FP:CAL-{sid}"][device_id]
        assert obs.distance_source == "backend_rssi"
        assert obs.scanner_estimated_distance_m is None

        feedback_resp = await c.get(f"/detections/calibrate/walk/feedback?session_id={sid}")
        assert feedback_resp.status_code == 200, feedback_resp.text
        feedback = feedback_resp.json()
        assert feedback["eligible_sensor_count"] == 1
        assert feedback["eligible_sensor_ids"] == [device_id]
        assert feedback["heard_sensor_count"] == 1
        assert feedback["heard_sensor_ids"] == [device_id]

        my_pos_resp = await c.get(f"/detections/calibrate/walk/my-position?session_id={sid}")
        assert my_pos_resp.status_code == 200, my_pos_resp.text
        my_pos = my_pos_resp.json()
        assert my_pos["eligible_sensor_count"] == 1
        assert my_pos["heard_sensor_count"] == 1
        assert my_pos["status"] == "only_1_sensor_need_3"

        map_resp = await c.get("/detections/drones/map", params={"exclude_known": "false"})
        assert map_resp.status_code == 200, map_resp.text
        assert map_resp.json()["drone_count"] == 0

    async with AsyncClient(transport=transport, base_url="http://testserver") as cleanup:
        await cleanup.delete(f"/nodes/{device_id}")


@pytest.mark.asyncio
async def test_walk_start_fails_if_any_target_node_cannot_enter_calibration_mode(monkeypatch):
    from app.main import app
    from app.routers.detections import _CAL_TOKEN

    await create_tables()
    transport = ASGITransport(app=app)
    headers = {"X-Cal-Token": _CAL_TOKEN}
    detections._phone_cal_mgr.sessions.clear()
    detections._calibration_mode.active_session_id = None
    detections._calibration_mode.fleet_mode_state = "inactive"

    bad_id = f"uplink-bad-{uuid.uuid4().hex[:8]}"
    ok_id = f"uplink-ok-{uuid.uuid4().hex[:8]}"
    late_ok_id = f"uplink-late-ok-{uuid.uuid4().hex[:8]}"
    stopped_ids: list[str] = []

    async def fake_start(node, session):
        if node["device_id"] == bad_id:
            raise RuntimeError("arm failed")
        return {"ok": True, "session_id": session.session_id, "scan_mode": "calibration"}

    async def fake_stop(node, session_id, reason):
        stopped_ids.append(node["device_id"])
        return await _fake_stop_node_mode(node, session_id, reason)

    monkeypatch.setattr(detections._calibration_mode, "_start_node_mode", fake_start)
    monkeypatch.setattr(detections._calibration_mode, "_stop_node_mode", fake_stop)
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            bad_id: {
                "device_id": bad_id,
                "last_seen": time.time(),
                "lat": 37.300101,
                "lon": -122.300101,
                "ip": "192.168.42.102",
            },
            ok_id: {
                "device_id": ok_id,
                "last_seen": time.time(),
                "lat": 37.300001,
                "lon": -122.300001,
                "ip": "192.168.42.101",
            },
            late_ok_id: {
                "device_id": late_ok_id,
                "last_seen": time.time(),
                "lat": 37.300201,
                "lon": -122.300201,
                "ip": "192.168.42.103",
            },
        },
    )

    async with AsyncClient(transport=transport, base_url="http://testserver", headers=headers) as c:
        for device_id, lat, lon in [
            (bad_id, 37.300101, -122.300101),
            (ok_id, 37.300001, -122.300001),
            (late_ok_id, 37.300201, -122.300201),
        ]:
            resp = await c.post(
                "/nodes",
                json={
                    "device_id": device_id,
                    "name": (
                        "a-bad"
                        if device_id == bad_id
                        else ("b-ok" if device_id == ok_id else "c-late-ok")
                    ),
                    "lat": lat,
                    "lon": lon,
                    "alt": 0.0,
                    "position_mode": "active",
                },
            )
            assert resp.status_code == 201, resp.text

        start_resp = await c.post("/detections/calibrate/walk/start", json={"operator_label": "test"})
        assert start_resp.status_code == 503
        assert "failed to arm fleet calibration mode" in start_resp.text
        assert not detections._phone_cal_mgr.sessions
        assert detections._calibration_mode.active_session_id is None
        assert set(stopped_ids) == {ok_id, late_ok_id}

        await c.delete(f"/nodes/{ok_id}")
        await c.delete(f"/nodes/{bad_id}")
        await c.delete(f"/nodes/{late_ok_id}")
