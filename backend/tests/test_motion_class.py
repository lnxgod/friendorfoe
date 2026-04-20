"""Per-class motion classification for triangulation.

The helper `_motion_class_for(source, manufacturer, model)` decides which
RSSI noise profile and observation TTL to apply to a detection. Getting
this right is the difference between:

  - Meta Glasses body-worn RSSI swings producing a jittery EKF track
    (default R too tight), vs. a smoothly tracked head position
  - An AirTag on a table being swamped by a distant sensor's stale reading
    (default TTL too long), vs. keeping the confident short-range lock
"""

from app.services.triangulation import (
    _motion_class_for,
    _obs_ttl_for,
    OBS_TTL_MOVING_S,
    OBS_TTL_STATIONARY_S,
    OBS_TTL_DEFAULT_S,
)


def test_meta_glasses_is_moving():
    assert _motion_class_for("ble_rid", "Meta", "Ray-Ban Meta") == "moving"
    assert _motion_class_for("ble_rid", "Meta Glasses", "") == "moving"


def test_luxottica_brand_is_moving():
    assert _motion_class_for("ble_rid", "Luxottica", "Oakley HSTN") == "moving"


def test_meta_quest_is_moving():
    assert _motion_class_for("ble_rid", "Meta", "Meta Quest 3") == "moving"


def test_drone_specific_protocols_are_moving_even_without_class_hint():
    """wifi_beacon_rid / wifi_dji_ie are drone-only protocols — airborne
    by definition. ble_rid is intentionally NOT here: it's the generic
    BLE scanner source and covers all kinds of benign devices too."""
    assert _motion_class_for("wifi_beacon_rid", "", "") == "moving"
    assert _motion_class_for("wifi_dji_ie", "", "") == "moving"


def test_ble_rid_without_class_hint_is_default():
    """ble_rid alone doesn't imply drone — don't apply the wide EKF noise."""
    assert _motion_class_for("ble_rid", "", "") == "default"
    assert _motion_class_for("ble_rid", "Unknown", "") == "default"


def test_wifi_ap_beacons_are_stationary():
    """Regular WiFi APs (wifi_ssid / wifi_oui sources) don't move. They
    deserve the tight noise profile + long TTL + heavy RSSI smoothing,
    not the default "unknown mobility" profile that wiggles positions
    around for what is physically a fixed box on a wall."""
    assert _motion_class_for("wifi_ssid", "", "") == "stationary"
    assert _motion_class_for("wifi_oui", "Ubiquiti", "") == "stationary"
    # Even rogue/evil-twin APs are physically fixed once they're up.
    assert _motion_class_for("wifi_ssid", "Unknown", "FreeWiFi") == "stationary"


def test_drone_wifi_sources_still_moving_after_ap_rule():
    """The new 'WiFi AP → stationary' rule must not swallow drone WiFi
    protocols. Regression guard."""
    assert _motion_class_for("wifi_beacon_rid", "", "") == "moving"
    assert _motion_class_for("wifi_dji_ie", "", "") == "moving"


def test_airtag_is_stationary():
    assert _motion_class_for("ble_rid", "Apple", "AirTag") == "stationary"
    assert _motion_class_for("ble_rid", "Apple", "AirTag (Separated)") == "stationary"


def test_tile_and_smarttag_are_stationary():
    assert _motion_class_for("ble_rid", "Tile", "Tile Tracker") == "stationary"
    assert _motion_class_for("ble_rid", "Samsung", "Galaxy SmartTag") == "stationary"


def test_generic_ble_is_default():
    assert _motion_class_for("ble_rid", "Unknown", "") == "default"
    assert _motion_class_for("wifi_probe_request", "", "") == "default"


def test_obs_ttl_per_class():
    assert _obs_ttl_for("moving")     == OBS_TTL_MOVING_S
    assert _obs_ttl_for("stationary") == OBS_TTL_STATIONARY_S
    assert _obs_ttl_for("default")    == OBS_TTL_DEFAULT_S
    assert _obs_ttl_for(None or "anything_else") == OBS_TTL_DEFAULT_S


def test_case_insensitive():
    assert _motion_class_for("ble_rid", "META", "RAY-BAN META") == "moving"
    assert _motion_class_for("ble_rid", "apple", "airtag") == "stationary"


def test_ekf_noise_uses_class():
    """Sanity: DeviceEKF.update branches on motion_class and produces
    different innovation covariances. Regression guard against a future
    refactor that silently drops the class awareness."""
    from app.services.position_filter import DeviceEKF

    # Place target 50m from sensor, measure 50m — zero innovation, so the
    # position won't actually move, but P will shrink less for "moving"
    # (wide R → less trust in the measurement → smaller correction).
    def run(motion_class):
        ekf = DeviceEKF(50.0, 0.0)
        ekf.motion_class = motion_class
        p_before = ekf.P00
        ekf.update(0.0, 0.0, 50.0)
        return p_before - ekf.P00

    shrink_stat = run("stationary")
    shrink_def  = run("default")
    shrink_move = run("moving")

    # Stationary trusts the measurement most → biggest P shrink
    # Moving trusts it least → smallest P shrink
    assert shrink_stat > shrink_def > shrink_move, (
        f"Expected shrink order stationary > default > moving, "
        f"got {shrink_stat:.3f} > {shrink_def:.3f} > {shrink_move:.3f}")


def test_particle_filter_sigma_uses_class():
    from app.services.particle_filter import ParticleFilter
    # At 50m, stationary sigma should be smaller than moving sigma
    sig_stat = ParticleFilter._meas_sigma(50.0, "stationary")
    sig_def  = ParticleFilter._meas_sigma(50.0, "default")
    sig_move = ParticleFilter._meas_sigma(50.0, "moving")
    assert sig_stat < sig_def < sig_move


def test_alpha_beta_peek_does_not_corrupt_velocity():
    """Calling peek() repeatedly must not change the tracker's velocity
    estimate. This is what makes predict-forward display safe between
    real observations."""
    from app.services.position_filter import AlphaBetaTracker

    t = AlphaBetaTracker(alpha=0.5, beta=0.15)
    # Establish a real velocity from observations
    for i in range(20):
        t.update(float(i), 0.0, float(i))
    vx_before = t.vx
    vy_before = t.vy
    x_before = t.x
    y_before = t.y
    last_t_before = t.last_t

    # Peek 50 times at advancing wall-clock — no state mutation
    for ts in range(20, 70):
        peeked = t.peek(float(ts))
        # Peek's returned position should advance with velocity, saturated
        # at 5 s so a long observation gap doesn't fly off the property.
        capped_dt = min(5.0, float(ts) - last_t_before)
        expected_x = x_before + vx_before * capped_dt
        assert abs(peeked[0] - expected_x) < 1e-6

    # Tracker internals identical to before peek loop
    assert t.vx == vx_before
    assert t.vy == vy_before
    assert t.x == x_before
    assert t.y == y_before
    assert t.last_t == last_t_before


def test_alpha_beta_peek_saturates_long_gaps():
    """Long gap shouldn't extrapolate the marker into the next county
    on a momentary velocity spike."""
    from app.services.position_filter import AlphaBetaTracker

    t = AlphaBetaTracker(alpha=0.5, beta=0.15)
    for i in range(20):
        t.update(float(i), 0.0, float(i))
    # 60 s gap (well beyond the 5 s saturation cap)
    far = t.peek(80.0)
    near = t.peek(24.0)
    # Both should equal "saturated" projection — peek caps dt at 5 s
    expected_x = t.x + t.vx * 5.0
    assert abs(far[0] - expected_x) < 1e-6
    assert abs(near[0] - expected_x) < 1e-6


def test_alpha_beta_peek_returns_none_for_uninitialised_tracker():
    """A tracker that has never seen an observation should not pretend
    to know where the target is."""
    from app.services.position_filter import AlphaBetaManager

    mgr = AlphaBetaManager()
    assert mgr.peek("ghost") is None


def test_smooth_if_fresh_only_folds_in_new_observations():
    """The dashboard polls at arbitrary cadence; we must NOT re-feed
    the same EKF position into the smoother on every poll, or the
    velocity estimate gets corrupted by zero-innovation updates that
    aren't real measurements."""
    from app.services.position_filter import AlphaBetaManager

    mgr = AlphaBetaManager()
    # Real observation at t=10
    mgr.smooth_if_fresh("a", observation_time=10.0,
                        x=5.0, y=0.0, motion_class="moving", now=10.0)
    # Real observation at t=12
    mgr.smooth_if_fresh("a", observation_time=12.0,
                        x=10.0, y=0.0, motion_class="moving", now=12.0)
    vx_after_real_obs = mgr.trackers["a"].vx
    # Now the dashboard polls 5 times at the same EKF observation_time
    # (no new packet arrived between polls). vx must NOT change.
    for _ in range(5):
        mgr.smooth_if_fresh("a", observation_time=12.0,
                            x=10.0, y=0.0, motion_class="moving", now=14.0)
    assert mgr.trackers["a"].vx == vx_after_real_obs


def test_smooth_if_fresh_returns_extrapolated_position_between_observations():
    """Between real observations, smooth_if_fresh extrapolates by
    velocity — this is what stops the dashboard marker from freezing
    on the last sample, instead of jumping when the next packet lands."""
    from app.services.position_filter import AlphaBetaManager

    mgr = AlphaBetaManager()
    # Several constant-velocity real observations
    for i in range(15):
        mgr.smooth_if_fresh("a", observation_time=float(i),
                            x=float(i), y=0.0, motion_class="moving", now=float(i))
    # Now poll the same observation_time — should extrapolate forward
    p_now = mgr.smooth_if_fresh("a", observation_time=14.0,
                                 x=14.0, y=0.0, motion_class="moving", now=14.0)
    p_later = mgr.smooth_if_fresh("a", observation_time=14.0,
                                   x=14.0, y=0.0, motion_class="moving", now=16.0)
    # 2 s of additional extrapolation should advance position by ~2*vx
    assert p_later[0] > p_now[0]
