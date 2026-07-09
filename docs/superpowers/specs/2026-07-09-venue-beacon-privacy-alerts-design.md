# Venue Beacon Privacy Alerts Design

## Summary

Friend or Foe already detects several venue beacon primitives, but they are treated more like background context than a clear privacy signal. The new behavior makes venue-style Bluetooth beacons visible in the app, backend, and badge when the evidence is protocol-specific enough to be trusted.

The goal is to tell the user when a venue, store, event, public space, or similar operator appears to be broadcasting proximity/location beacons, and to expose as much passive metadata as the radio frame safely provides.

## Goals

1. Alert on venue beacon protocols with explicit passive evidence.
2. Preserve rich metadata for inspection: protocol, frame type, UUID, major, minor, URL, telemetry, TX power, RSSI, and estimated distance when available.
3. Keep badge rows useful and compact while allowing the app/backend to show deeper details.
4. Add tracker-family detections only when the passive signature is stable enough to defend.
5. Avoid broad vendor/OUI guesses that would recreate the noisy privacy behavior recently fixed.

## Non-Goals

1. Do not identify a venue, store, or owner from an arbitrary beacon UUID without a trusted mapping.
2. Do not infer cameras, listening, or tracking intent from a generic beacon alone.
3. Do not alert on generic Apple, Samsung, Google, ESP, Realtek, or Bluetooth company identifiers by themselves.
4. Do not broaden Flock beyond the known `B4:1E:52` OUI unless stronger field evidence is added later.
5. Do not ship firmware or release artifacts as part of this design-only step.

## Detection Classes

### Venue Beacon

Protocol-specific BLE beacons used for proximity, venue location, retail/event experiences, wayfinding, or telemetry.

Accepted evidence:

1. iBeacon manufacturer payload: Apple company ID `0x004C`, iBeacon type `0x02`, length `0x15`.
2. Eddystone service UUID `0xFEAA` with frame types:
   - UID `0x00`
   - URL `0x10`
   - TLM `0x20`
   - EID `0x30`
3. Existing named beacon hardware patterns when already supported by the repo, such as Estimote, Kontakt, Gimbal, Beaconstac, RetailNext, or VergeSense. These stay lower-confidence unless paired with protocol evidence.

Alert behavior:

1. Badge display class: `Venue Beacon` or `Beacon Area`.
2. App/backend category: `VENUE_BEACON`.
3. Severity: low to medium by default.
4. Escalation: medium when multiple venue beacons are seen nearby, when RSSI is very close, or when the same beacon persists across time.

### Bluetooth Tracker

Item trackers and compatible unwanted-tracking devices such as AirTag/Find My accessories, Samsung SmartTag, Chipolo, Pebblebee, Motorola moto tag, Tile, and Google Find Hub tags.

Accepted evidence:

1. Existing Apple Find My / AirTag Continuity evidence already parsed by the app/backend/firmware.
2. Samsung SmartTag service data already parsed in Android using service UUID `0xFD5A`.
3. Google Fast Pair tracker-compatible evidence only when model/service evidence maps to a known tracker device.
4. Vendor-specific tracker detections only when backed by stable passive BLE evidence, not brand OUI or company ID alone.

Alert behavior:

1. If the device class is confirmed, show the specific tracker family.
2. If only the network/protocol family is confirmed, show a generic tracker label such as `Find Hub tracker nearby`.
3. Escalate only with persistence, movement-with-user evidence, very close RSSI, or known unwanted-tracker state.

### Public Surveillance and ALPR

Existing camera, body camera, dash camera, doorbell, hidden camera, ALPR, and attack-tool SSID/OUI detections remain in scope for parity review but are not broadened by this design.

Rules:

1. Keep Flock on the known `B4:1E:52` OUI path.
2. Keep ELSAG and existing camera SSID signatures.
3. Do not add Rekor, Genetec, Vigilant, Neology, or similar ALPR brands without direct passive RF evidence.

## Data Model

Venue beacon detections should preserve a details structure equivalent to:

```json
{
  "category": "VENUE_BEACON",
  "protocol": "ibeacon | eddystone | named_beacon",
  "frame_type": "uid | url | tlm | eid | unknown",
  "uuid": "optional",
  "major": 0,
  "minor": 0,
  "url": "optional",
  "telemetry": {
    "battery_mv": 3000,
    "temperature_c": 22.5,
    "pdu_count": 1234,
    "uptime_s": 5678
  },
  "tx_power": -59,
  "rssi": -70,
  "estimated_distance_m": 4.2,
  "evidence": "svc 0xFEAA frame URL"
}
```

Each layer can store or render this differently, but the same evidence should survive:

1. Android parser and privacy presentation.
2. Backend privacy device service and API output.
3. ESP32 BLE fingerprint and badge threat policy.
4. Badge compact row details.

## User-Facing Behavior

### Badge

The badge should show venue beacons as visible alerts. Details should be short:

1. `Eddystone URL -62dB`
2. `iBeacon major/minor -70dB`
3. `Beacon area 3 nearby`

The badge should not imply spying or recording from beacon evidence alone.

### Android App

The app should show richer metadata:

1. Protocol and frame type.
2. UUID/major/minor for iBeacon.
3. Decoded URL for Eddystone URL.
4. Battery, temperature, packet count, and uptime for Eddystone TLM.
5. RSSI and distance estimate when available.
6. A clear caveat that venue beacons can support proximity/location experiences but do not prove camera, microphone, or identity tracking by themselves.

### Backend

The backend should keep normalized fields for grouping, API display, and future localization. It should not collapse all beacons into a string-only detail when structured fields are available.

## False Positive Policy

Allowed alert triggers:

1. Protocol-specific BLE beacon payload.
2. Stable tracker service/model evidence.
3. Existing narrow Wi-Fi SSID or OUI evidence.

Suppressed or context-only signals:

1. Generic Bluetooth company IDs.
2. Generic vendor OUIs.
3. Generic Apple Continuity without a more specific subtype.
4. Generic ESP/Realtek Wi-Fi OUIs.
5. Broad SSID substrings like `cam`, `portal`, `evil`, `twin`, `flock`, or `alpr`.

## Test Plan

Write failing tests before production edits.

Android:

1. iBeacon payload produces a visible `VENUE_BEACON` privacy detection with UUID, major, minor, and TX power.
2. Eddystone URL produces a visible `VENUE_BEACON` privacy detection with decoded URL.
3. Eddystone TLM preserves telemetry fields.
4. Generic Apple manufacturer data does not become a venue alert.
5. Samsung SmartTag parser remains tracker-specific and does not get mislabeled as venue.

Backend:

1. `0xFEAA` service UUID maps to `VENUE_BEACON`.
2. iBeacon-style Apple subtype maps to venue beacon details, not generic Apple.
3. Venue beacon density is counted in grouped privacy summaries.
4. Generic vendor OUI/company IDs remain suppressed.

ESP32:

1. Eddystone `0xFEAA` fingerprints become badge-visible venue beacon events.
2. iBeacon fingerprints become badge-visible venue beacon events.
3. Multiple venue beacons can summarize as a beacon area.
4. Generic Apple or Flock-like BLE names do not create Flock/ALPR or listening events.
5. Existing Flock OUI tests still pass.

## Verification

Minimum verification before calling implementation complete:

1. `backend/.venv/bin/pytest tests/test_privacy_devices.py tests/test_rf_identity.py -q`
2. `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.*`
3. `cd esp32 && ./.venv312/bin/pio test -e test`
4. Targeted badge threat policy tests covering venue beacon rows.

If release or deployment is requested later, the firmware update path must be verified separately before claiming the badge or sensor updates are shipped.
