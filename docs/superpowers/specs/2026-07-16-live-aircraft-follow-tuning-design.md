# Live Aircraft, Follow Tuning, and BLE Parity Design

## Summary

Friend or Foe will make aircraft motion on the Android map feel current without
fabricating new reports, stabilize the map's compass-follow mode, reduce false
BLE follower warnings, and close the remaining Android/ESP behavioral-detector
parity gaps from the `v0.64.67-ble-investigation` review.

The work is intentionally presentation-side for aircraft projection. Raw ADS-B
objects, timestamps, history, and backend data remain authoritative. Follow
classification remains Android-side because fixed ESP scanners do not know the
phone's movement; ESP and badge work stays focused on equivalent BLE evidence,
investigation, and display behavior.

## Goals

1. Move aircraft markers smoothly between real ADS-B reports.
2. Make map compass-follow stable while preserving deliberate turns.
3. Require sustained, location-separated evidence before calling a BLE device
   a follower.
4. Keep low-confidence lingering evidence informational rather than alarming.
5. Align Android and ESP pairing-spam/serial-skimmer behavior and evidence.
6. Preserve badge button-2 investigation and all read-only safety guarantees.
7. Publish verified Android and firmware artifacts through the tag-driven
   release path.

## Non-Goals

1. Do not increase ADS-B provider polling beyond the existing balanced cadence.
2. Do not write predicted aircraft positions to Room, history, or the backend.
3. Do not use a fixed ESP node to classify a device as following a moving user.
4. Do not automatically pair, write, subscribe, or try credentials during BLE
   investigation.
5. Do not classify `0xFFE0` or `0xFFF0` as malicious without combined evidence.
6. Do not change AR orientation responsiveness; map stabilization is map-only.

## Confirmed Current Behavior

- `AdsbPoller` starts a new multi-provider request after a five-second delay.
- `TrajectoryPredictor` already projects AR objects for at most 12 seconds and
  decays position confidence to zero at 30 seconds.
- `MapViewScreen` renders reported positions directly and recreates every map
  overlay whenever observed map state changes.
- Map compass-follow consumes the shared sensor heading directly, so normal
  heading noise can trigger frequent map redraws.
- `BleTracker` can label a device as following after three sightings, two
  minutes, more than 50 meters of global user movement, and two rounded
  locations. It does not receive bonded state or GPS accuracy.
- The privacy screen renders every stalker result as a danger strip, including
  threat level 1.
- Android can consume only the first behavioral signal from one advertisement,
  maps serial-skimmer RSSI to `-100`, and does not consistently honor a stable
  ignored behavioral entity.
- Android and scanner firmware use different definitions of a trusted identity
  for serial-skimmer suppression.

## Architecture

### 1. Presentation-Only Aircraft Frames

`MapTrackProjector` owns one `TrajectoryPredictor` and produces
`MapTrackFrame` values from the latest repository list and a monotonic frame
clock. It runs at 250 ms intervals while the map is subscribed.

Each frame contains the original `SkyObject`, predicted position, data age,
confidence, extrapolation flag, and predicted heading. The map uses predicted
position and heading only for drawing. Detail views and repository consumers
continue receiving the original object.

Projection rules remain the existing predictor contract:

- extrapolate aircraft only when heading and speed are valid;
- stop forward motion after 12 seconds;
- decay confidence to zero at 30 seconds;
- freeze and visibly fade older objects until repository stale pruning removes
  them at its existing threshold;
- blend corrections when a new report arrives.

### 2. Stable Map Overlay Ownership

`MapOverlayController` retains user, aircraft, sensor, and remote-search
markers by stable key. An update changes existing marker position, rotation,
alpha, title, and snippet in place; it adds missing keys and removes departed
keys. Distance rings and map-event overlays are not recreated on every heading
sample.

Aircraft marker alpha is derived from data confidence with a visible floor for
stale-but-retained reports. Marker click behavior continues to select the raw
object ID.

### 3. Map-Specific Heading Stabilization

`MapHeadingStabilizer` consumes the already-fused circular heading and emits a
map heading under this contract:

- no more than one output every 100 ms;
- ignore residual circular changes smaller than 3 degrees;
- use a time-aware circular response with a 250 ms time constant;
- take the shortest path across the 0/360 boundary;
- emit the first valid heading immediately and reset when follow mode stops.

The raw `SensorFusionEngine.orientation` flow remains unchanged for AR,
direction finding, and other sensor consumers.

### 4. Evidence-Based BLE Follower Policy

Only unbonded, non-ignored detections in plausible mobile privacy categories
enter follower tracking. The initial category set is `FINDMY`, `BLE_TRACKER`,
`GPS_TRACKER`, `OBD_TRACKER`, `SMART_GLASSES`, `ACTION_CAMERA`,
`DASH_CAMERA`, `VEHICLE_CAMERA`, and `BODY_CAMERA`.

Every sighting carries location accuracy. A following alert requires all of:

1. At least 5 minutes between first and last qualifying sightings.
2. At least 6 qualifying sightings.
3. Location accuracy no worse than 50 meters for contributing sightings.
4. At least 3 sequential location clusters separated by 75 meters.
5. At least 150 meters of cumulative movement between those clusters.
6. Sightings in the first, middle, and final thirds of the observation span.

Movement is calculated from the locations attached to that device's sightings,
not unrelated global movement before or after the device appeared. Invalid or
poor-accuracy coordinates do not contribute.

A qualifying follower starts at threat level 2. A camera-capable candidate may
reach level 3 after at least 10 minutes with a strongest RSSI of at least
`-70 dBm`. Repeated checks return stable evidence but notification cooldown
continues to be enforced by `PrivacyAlertPolicy`.

Lingering remains a separate reason. It requires at least 10 minutes, at least
10 qualifying sightings, no more than 25 meters of clustered movement, and a
strongest RSSI of at least `-70 dBm`. It remains threat level 1 and is rendered
with a neutral status tone and a `Nearby device` title, never `Follower alert`.

Bonded state, category, accuracy, cluster count, movement, duration, and
temporal coverage are retained as inspectable evidence for tests and detail UI.

### 5. Behavioral BLE Parity

Android and ESP continue using the detector thresholds established by the BLE
investigation release. This update changes only edge behavior:

1. Pairing spam has priority when one observation completes both detectors,
   but the serial track is not consumed; the next qualifying observation can
   emit the serial candidate. This matches scanner firmware behavior.
2. Android `SerialSkimmer` carries and displays the analyzer's strongest RSSI.
3. A trusted serial identity means a coherent, recognized non-serial product
   identity, or an Android-bonded device. Public address type alone is not
   trust. Generic UART identities, unknown fingerprints, and serial-skimmer
   signatures are never trusted.
4. Ignoring `ble:pairing-spam` or another behavioral entity suppresses future
   rows by stable entity key as well as current MAC.

The Kotlin and Unity fixtures cover prompt/serial collision, strongest RSSI,
recognized-product suppression, generic UART non-suppression, PKOC suppression,
mixed-service negatives, cooldown, and ignored stable entities where Android
owns persistence.

### 6. Badge and ESP Behavior

Scanner firmware emits the same normalized behavioral evidence fields already
carried in `drone_detection_t`. Badge threat policy continues mapping pairing
spam and serial skimmer to their existing evidence-rich rows. Button-2 hold
continues to investigate the selected alert and restore scanning afterward.

The release verifies production scanner, badge scanner, production uplink, and
badge uplink builds. Existing scanner sequence, status age, and investigation
transport fields remain the source of freshness; this change does not invent a
second telemetry protocol.

## Error and Lifecycle Behavior

- A failed ADS-B request leaves the last real report available. Projection
  coasts only to 12 seconds, then freezes and fades; it never runs indefinitely.
- Leaving the map cancels its frame clock and heading flow through
  `WhileSubscribed`, avoiding background animation work.
- Turning compass-follow off resets map stabilization and norths the map.
- Missing location permission or poor GPS accuracy suppresses follower
  classification rather than treating uncertain movement as evidence.
- Stopping privacy detection clears follower state so old evidence cannot leak
  into a later scan session.
- BLE detector state remains bounded and monotonic-time based.

## Verification and Release Gates

1. Focused JVM tests for map heading, map frames, follower policy, behavioral
   collision, RSSI, trust, and ignore behavior.
2. Full `./gradlew testDebugUnitTest` and `./gradlew assembleDebug`.
3. Full native ESP test suite plus scanner and uplink builds for production and
   badge environments.
4. `git diff --check`, secret-pattern scan, and version/release manifest audit.
5. Connected Android/badge checks when hardware is available; otherwise every
   unrun physical check is recorded explicitly.
6. Push the reviewed branch, merge to `main`, push a fresh `v*` tag, wait for
   Android and firmware release workflows, verify assets, then locally inspect
   APK version, digest, and signature.

