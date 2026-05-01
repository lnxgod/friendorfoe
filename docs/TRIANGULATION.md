# Multi-Node Triangulation

When two or more sensor nodes hear the same drone, the backend converts
their RSSI readings into a position estimate. Three regimes, picked by
sensor count.

## Path-Loss Model (RSSI → distance)

Log-distance path-loss, identical math on the firmware (used to estimate
range during a single-sensor encounter) and on the backend (used to
multi-laterate):

```
distance_m = 10 ^ ((RSSI_REF - rssi) / (10 * path_loss_exponent))
```

`RSSI_REF` is the measured received power in dBm at 1 m, and
`path_loss_exponent` is the environment factor (≈2.0 in free space, 2.5–3.5
in dense indoor environments). Defaults live in
`backend/app/services/triangulation.py:35-37` (`RSSI_REF = -40`,
`PATH_LOSS_EXPONENT = 2.7`) and are overridden by the inter-node
calibration walk described below.

## Three-Regime Solver

| Sensors with RSSI | Method | Source |
|---|---|---|
| 3+ | Gauss-Newton non-linear least squares | `_trilaterate` (`triangulation.py:419`) |
| 2 | Circle–circle intersection | `_intersect_two` (`triangulation.py:521`) |
| 1 | RSSI range circle (no bearing) | `rssi_to_distance_m` + sensor position |

For 3+ sensors we minimize the squared residual

```
F(x, y) = Σᵢ ( ‖(x, y) − sᵢ‖ − dᵢ )²
```

over (x, y), starting from the centroid of the participating sensors. The
solver also weighs each residual by the listener's recent RSSI variance, so
a sensor in deep multipath fades doesn't dominate the fit.

For two sensors we intersect the two range circles geometrically and pick
the candidate point closer to the inter-sensor midpoint (assumes the drone
is between or near the sensors, which is true for most operational sites).

For a single sensor we report a circle of likely positions rather than a
point. The dashboard renders this as a translucent ring; the Android app
renders it as a directional cone if the phone has a recent compass reading
to bias toward.

## Inter-Node RSSI Calibration

`backend/app/services/calibration.py` implements a controlled-source
calibration: sensor nodes take turns broadcasting an AP at full power then
quarter power. Every other sensor records the observed RSSI; the backend
runs a regression on (known distance, observed RSSI) pairs to recover
`RSSI_REF` and `path_loss_exponent` for the site, and persists them via
`update_calibration` (`triangulation.py:61`). Per-listener offsets capture
antenna-orientation differences between nodes.

The Android-led "calibration walk" in v0.63.0+ folds in operator phone
positions: the operator walks the perimeter of the area while the phone
streams its GPS position, every sensor logs the corresponding RSSI, and
the regression gains tens of thousands of points without manual sample
collection.

Calibration is **manual-only** (button-triggered) and persisted to
`backend/app/data/applied_calibration.json`. Restarts retain the last
applied calibration; there is no auto-recalibration loop.

## Smoothing

Raw triangulated points are noisy. `position_filter.py` runs a constant-
velocity Kalman filter per drone, with process noise tuned for slow
hover drift (default 0.5 m/s² acceleration std). The dashboard shows the
EKF-smoothed position as the operator-facing point and the raw measurements
as faint trail dots.

## Limitations

- **All RSSI-based ranging is fundamentally noisy.** Expect ±10–20 m at
  good SNR, much worse in dense urban settings. Triangulation is for
  "where roughly" not "where exactly."
- **Two-sensor solutions can flip** when the drone moves through the
  inter-sensor baseline. The smoother dampens this; raw two-sensor fixes
  in the dashboard show the ambiguity as a wider EKF uncertainty ellipse.
- **No altitude.** Current model is 2-D. ADS-B detections include altitude
  from the aircraft itself; drone Remote ID detections include height
  above takeoff if the drone broadcasts it. Pure RSSI gives no Z.
- **Per-radio asymmetry.** WiFi RSSI and BLE RSSI are not directly
  comparable; the calibration table is per-source-and-listener.
