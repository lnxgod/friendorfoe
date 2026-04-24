# Privacy Triangulation & Detection Upgrade PRD

## Summary

Friend or Foe is strongest when a target self-reports position through Remote ID or DJI information elements. It is much weaker for the primary privacy mission: locating nearby cameras, trackers, smart glasses, dashcams, doorbells, and similar devices from BLE and WiFi observations alone.

This PRD defines a phased upgrade to make privacy-device localization materially more useful on real property-scale deployments while also expanding the set of privacy items that the ESP32 fleet can detect and surface through the backend and dashboard.

## Problem

Today, privacy triangulation is limited by four issues:

1. The scanner and backend use different default RSSI-to-distance models, and the backend recomputes distance from RSSI instead of trusting scanner-side estimates.
2. The localization pipeline is optimized for moving drone signals and fresh protocol frames, not slow, stationary privacy devices such as cameras and access points.
3. Rich WiFi privacy detection exists mostly in the standalone `esp32/ble-scanner` path, while the networked `esp32/scanner` path is still primarily drone-oriented.
4. Android has a much broader privacy catalog than the ESP32 fleet, so the phone and sensor network do not share the same practical coverage.

## Goals

1. Improve stationary privacy-device localization to useful property scale.
2. Improve moving BLE privacy-device tracking enough to support approach-and-find workflows.
3. Increase networked privacy detection coverage so ESP32 nodes catch far more of the devices Android already recognizes.
4. Make localization behavior observable and debuggable so bad nodes, bad calibration, and bad solves are easy to identify.

## Non-Goals

1. Centimeter or room-corner precision from commodity ESP32 radios.
2. Full indoor RF tomography or CSI/AoA-based positioning in this phase.
3. Perfect parity with every Android-only sensor mode such as ultrasonic, IR, or magnetometer detection.

## User Value

Primary users are deploying 2 to 4 nodes around a home, building, or yard and want answers to:

1. Is there a privacy-relevant device nearby?
2. What kind of device is it?
3. Is it stationary or moving?
4. Where should I walk to find it?

The upgraded system should answer those questions better than “something might be nearby” and avoid false confidence when the geometry is poor.

## Success Metrics

### Localization

1. Stationary privacy target with 3 or more sensors after calibration: median error <= 10 m, p90 <= 25 m.
2. Stationary privacy target with 2 sensors: system emits a constrained zone or uncertainty radius instead of a misleading point when geometry is ambiguous.
3. Moving privacy BLE target: median jump distance reduced by 50% in tracking diagnostics.

### Coverage

1. ESP32 networked path supports at least the top 80% of Android privacy signatures by observed field utility.
2. Dashboard-visible privacy detections per hour increase materially on known test setups with cameras, trackers, dashcams, and doorbells.

### Operability

1. Tracking diagnostics clearly show source mix, jump statistics, calibration fit quality, and suspect sensors.
2. Operators can tell when a solve is low-confidence or geometry-limited.

## Requirements

### R1. Single Consistent Ranging Pipeline

1. `estimated_distance_m` must become a first-class field from scanner to backend.
2. Backend triangulation must prefer scanner-supplied distance when present.
3. Scanner and backend defaults must not silently disagree.
4. Calibration must update a shared model, not parallel competing models.

### R2. Source-Aware Localization

1. Drone protocol signals with direct GPS continue to use GPS-first behavior.
2. Stationary privacy targets use a longer aggregation window and robust per-sensor RSSI summary.
3. Moving privacy BLE targets use short windows and smoothing, but should not be solved with stale stationary assumptions.
4. Ambiguous 2-sensor solves should return uncertainty-aware output, not overconfident points.

### R3. Networked Privacy WiFi Detection

1. The networked `esp32/scanner` path must detect WiFi privacy items, not only drone SSIDs and OUIs.
2. Privacy WiFi matches must retain manufacturer, category, auth mode, BSSID, RSSI, and timestamps.
3. Detection cadence must support localization of stationary AP-like devices without flooding the queue.

### R4. Expanded Privacy Catalog

1. Port high-value Android privacy signatures into shared or generated data used by Android, ESP32, and backend.
2. Priority classes: hidden cameras, surveillance cameras, ALPR, body cameras, trackers, smart glasses, dashcams, doorbells, baby monitors, thermal cameras, fleet cameras, GPS/OBD trackers.
3. Coverage must be measured by category, not just raw rule count.

### R5. Better Calibration and Diagnostics

1. Keep inter-node calibration and phone-walk calibration, but expose when they disagree.
2. Add per-band awareness as follow-on work: BLE and WiFi should not assume identical propagation.
3. Surface suspect listeners prominently in APIs and dashboards.

## Phased Implementation

### Phase 1: Ranging Consistency and Observability

Scope:

1. Add `estimated_distance_m` to payloads and schemas.
2. Update backend ingest to pass scanner-computed distance through to `SensorTracker.ingest`.
3. Change `SensorTracker.ingest` to use supplied distance as preferred input, with backend RSSI recomputation only as fallback.
4. Align default ranging constants between firmware and backend until calibration overrides them.
5. Extend diagnostics endpoints with raw-vs-used distance, calibration source, and solve mode.

Expected files:

1. `esp32/uplink/main/comms/http_upload.c`
2. `backend/app/models/schemas.py`
3. `backend/app/routers/detections.py`
4. `backend/app/services/triangulation.py`
5. `esp32/shared/constants.h`

Acceptance:

1. A detection emitted by the scanner arrives at the backend with identical `estimated_distance_m`.
2. Tracking diagnostics show whether a position used GPS, supplied range, or backend-derived range.

### Phase 2: Stationary Privacy Localization Path

Scope:

1. Add a separate stationary-privacy solve path for `wifi_ssid`, `wifi_oui`, and privacy AP classes.
2. Replace the current strict `2s` co-observation gate for stationary privacy targets with a longer aggregation window such as `30s` to `90s`.
3. Use robust summaries per sensor such as median RSSI or trimmed mean instead of a single latest sample.
4. Emit uncertainty-aware outputs:
   - point when geometry is good
   - circle/zone when geometry is weak
   - range-only when only one sensor is credible

Expected files:

1. `backend/app/services/triangulation.py`
2. `backend/app/services/position_filter.py`
3. `backend/app/routers/detections.py`

Acceptance:

1. A stationary camera or AP seen by staggered scans from multiple nodes still produces a stable localized result.
2. Large jump counts fall in diagnostics for stationary classes.

### Phase 3: Networked WiFi Privacy Detection

Scope:

1. Port or share the privacy WiFi pattern database currently living in `esp32/ble-scanner/main/detection/wifi_privacy_scanner.c`.
2. Integrate those patterns into the networked `esp32/scanner` pipeline.
3. Add privacy categories so backend classification can distinguish drones from cameras, dashcams, doorbells, and attack tools.
4. Tune rate limiting separately for stationary privacy APs so localization receives enough data.

Expected files:

1. `esp32/scanner/main/detection/wifi_scanner.c`
2. `esp32/scanner/main/detection/wifi_ssid_patterns.c`
3. `backend/app/services/classifier.py`
4. `backend/app/routers/detections.py`

Acceptance:

1. Known privacy APs appear through the networked scanner path and are not labeled as generic drone WiFi.
2. Detection volume remains stable without overwhelming queue or uplink bandwidth.

### Phase 4: Privacy Catalog Unification

Scope:

1. Define a shared signature source of truth, preferably generated JSON or CSV feeding Kotlin, C, and Python.
2. Port the highest-value Android-only signatures into the ESP32 fleet.
3. Restore tracker coverage on ESP32 where practical, including Find My / AirTag-class identifiers when confidence is acceptable.

Expected files:

1. `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
2. `esp32/scanner/main/detection/glasses_detector.c`
3. `backend/app/services/enrichment_ble.py`
4. new shared data file(s) under `backend/app/data/` or `esp32/shared/`

Acceptance:

1. Android and ESP32 report compatible categories for the same device families.
2. Coverage audits can count signatures by category and platform.

### Phase 5: Calibration V2

Scope:

1. Add BLE-vs-WiFi calibration separation or per-band offsets.
2. Use phone-walk sessions plus inter-node residuals to flag bad node placement, wrong coordinates, or damaged hardware.
3. Add operator-facing feedback on calibration quality before enabling new model parameters.

Expected files:

1. `backend/app/services/calibration.py`
2. `backend/app/services/phone_calibration.py`
3. `backend/app/services/triangulation.py`

## Testing Strategy

1. Unit tests for range propagation, solver selection, and stationary-vs-moving TTL logic.
2. Regression fixtures for known privacy devices: camera APs, Meta glasses, AirTags, dashcams, doorbells.
3. Property test runs with fixed ground-truth device placements and saved RSSI traces.
4. Diagnostics validation through `/detections/tracking/diagnostics`.

## Rollout Plan

1. Ship Phase 1 first. Without consistent ranging, later work is hard to trust.
2. Ship Phase 2 next so stationary privacy localization stops inheriting drone assumptions.
3. Ship Phase 3 and 4 together or back-to-back so coverage and localization improve in the same release.
4. Ship Phase 5 after enough field traces exist to justify per-band calibration.

## Risks

1. More frequent privacy observations may increase queue pressure and uplink load.
2. Tracker support can raise false positives if broad signatures are restored carelessly.
3. Overfitting calibration to one property can hurt portability.

## Open Questions

1. Should stationary privacy results be rendered as points, circles, or heatmap tiles in the dashboard?
2. Should low-confidence tracker detections be visible by default or behind an expert-mode toggle?
3. Do we want one shared signature compiler for Android/C/Python now, or a staged port first and unification second?
