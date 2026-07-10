# BLE Threat Detection and Investigation Design

## Summary

Friend or Foe will add two behavioral BLE detectors and a user-initiated,
read-only investigation workflow on Android and the FoF badge:

1. Detect pairing-prompt spam that rotates through many BLE addresses while
   emitting Apple Continuity, Google Fast Pair, or Microsoft Swift Pair
   advertisements.
2. Detect possible serial-service skimmers using combined evidence around
   `0xFFE0` or `0xFFF0`, rather than treating either UUID as malicious alone.
3. Let Android investigate a selected device directly through the phone's BLE
   radio or through a connected badge.
4. Let a standalone badge investigate its selected BLE alert by holding button
   2, while preserving the existing button navigation and phone-pairing flow.

The active workflow is strictly read-only toward the investigated target. It
may connect, discover GATT services and characteristics, read explicitly
readable values, and report authentication or encryption requirements. It must
never write or subscribe on the target, guess credentials, or initiate pairing
automatically.

## Goals

1. Detect high-rate, rotating-address BLE pairing spam without turning a normal
   crowd of phones and accessories into an alert.
2. Surface a possible serial skimmer only after multiple independent passive
   signals agree.
3. Preserve equivalent detector behavior and evidence on Android and badge
   scanner firmware.
4. Provide one normalized investigation result across direct Android GATT and
   badge-assisted GATT.
5. Keep the badge useful without a phone attached.
6. Make every active operation explicit, bounded, cancellable, and recoverable.

## Non-Goals

1. Do not identify a skimmer from `0xFFE0` or `0xFFF0` alone.
2. Do not claim a device is password-protected from advertisements alone.
3. Do not pair automatically, try default PINs, brute-force credentials, write
   characteristics, enable notifications, or invoke vendor commands.
4. Do not keep a persistent connection to an investigated device.
5. Do not replace existing Android direction finding or badge BLE focus mode.
6. Do not require the backend for local Android or standalone badge behavior.

## Architecture

### Shared Behavioral Contract

Android and scanner firmware implement the same pure detector contract with
platform-specific code:

- Android: a Kotlin analyzer consuming `BleAdvertisement` observations.
- Badge scanner: a C analyzer consuming `ble_fingerprint_t`, MAC, address type,
  RSSI, advertisement properties, and monotonic time.

Both implementations use the same constants, state transitions, reason codes,
and fixture scenarios. Detector state is bounded and uses monotonic timestamps.
The detector returns normalized evidence; presentation code does not recreate
the detection rules.

### Investigation Engines

Two engines implement the same read-only result schema:

- Android direct engine based on `BluetoothGatt`.
- Badge scanner engine based on the existing NimBLE host.

Android chooses a route through an `Auto`, `Phone`, or `Badge` control. `Auto`
uses the source that observed the selected device when possible: local Android
detections use the phone, while badge detections use the badge when its USB-C
or bonded BLE command path is available. A user may override the route when
both radios have a usable target address.

### Badge Command Path

The existing command path is extended rather than replaced:

1. Android sends `ble_investigate` through `FOF_CTL` over USB-C or through the
   encrypted badge BLE control characteristic.
2. Badge uplink validates the request and forwards a scanner command over the
   BLE-scanner UART.
3. Badge scanner performs passive focus or read-only GATT investigation.
4. Scanner emits progress and a bounded result over UART.
5. Badge uplink stores the latest result, updates the LCD, and exposes the
   result in USB/BLE status for Android.

## Pairing-Spam Detector

### Qualifying Advertisement Families

Only prompt-capable protocol evidence enters the pairing-spam window:

- Apple company ID `0x004C` with a recognized Continuity pairing/proximity
  subtype and structurally valid payload.
- Google Fast Pair service data or advertised service UUID `0xFE2C`.
- Microsoft company ID `0x0006` with Swift Pair advertising-beacon type `0x03`.

Generic Apple manufacturer traffic, arbitrary Microsoft manufacturer data, and
an isolated `0xFE2C` advertisement remain non-alerting.

### Sliding Window

The initial balanced profile uses an 8-second window. An alert requires all of
the following:

1. At least 12 unique addresses and 24 deduplicated qualifying observations.
2. At least 75 percent of addresses are first seen for that signature within
   the active window, proving rapid address churn rather than ordinary repeat
   advertisements from stable devices.
3. Observations share one structural fingerprint, or a mixed-family burst has
   at least two qualifying families with a shared tight RSSI cluster.
4. RSSI spread is no more than 20 dB and the interquartile spread is no more
   than 12 dB, consistent with one nearby transmitter rather than a crowd.
5. The burst rate is at least three qualifying observations per second.

Android scan-overlap duplicates are deduplicated by family, MAC, structural
fingerprint, and a 250 ms time bucket. Firmware uses the equivalent key. BLE
random/private address type is supporting evidence where the scanner exposes
it, but it is not required because Android does not reliably expose controller
address type for every scan result.

The detector emits one synthetic entity keyed by anomaly family and structural
signature. It does not create one row per rotating MAC. The alert contains
family counts, unique-address count, observation count, window duration, RSSI
range, strongest RSSI, and whether the burst is single-family or mixed-family.

A 60-second cooldown prevents repeated alerts from a sustained flood. The
entity clears only after 20 seconds without a qualifying burst. Thresholds are
named constants so field tuning does not change the result schema.

### Pairing-Spam Investigation

Rotating or nonconnectable prompt-spam addresses are not suitable GATT targets.
Investigating this alert starts a bounded passive capture instead. The capture
collects protocol families, structural signatures, rotating addresses, RSSI
distribution, advertisement rate, connectability, names, company IDs, and
service UUIDs for 12 seconds, then returns a summarized result.

## Serial-Service Skimmer Detector

### Passive Candidate Evidence

`0xFFE0` and `0xFFF0` are common on legitimate devices, so the detector uses a
score made from independent evidence:

- Serial service evidence: advertisement includes `0xFFE0` or `0xFFF0`.
- Sparse service profile: it is the only advertised application service.
- Generic UART identity: a known UART-module-style name or a short generic
  serial/Bluetooth name, without a recognized trusted product identity.
- Persistence: at least three observations spanning at least five seconds.
- Proximity: strongest RSSI is at least -70 dBm.
- Address evidence: random/static/private address, with no trusted company ID.
- Connectability: advertisement properties indicate a connectable peripheral.

The passive alert requires serial service, sparse profile, persistence, and at
least two of generic identity, proximity, address evidence, and connectability.
A trusted vendor/product match suppresses the heuristic unless stronger active
evidence later contradicts it. A coherent Public Key Open Credential (PKOC)
identity suppresses the `0xFFF0` heuristic because that UUID also has a
legitimate physical-access use.

The passive label is `Possible Serial Skimmer`, never `Confirmed Skimmer`.
Evidence details distinguish `0xFFE0` from `0xFFF0` and state that the service
is shared by legitimate IoT products.

### Active Confidence Update

Read-only investigation may strengthen or weaken the candidate:

- Strengthen when discovery finds a matching UART characteristic family such
  as `0xFFE1` or `0xFFF1`, a sparse GATT profile, generic device information,
  or an authentication/encryption error on an explicitly readable serial
  characteristic.
- Weaken when standard Device Information identifies a coherent legitimate
  product, when multiple non-serial application services explain the device,
  or when the advertised serial UUID is not present after discovery.

An authentication error is reported as `Authentication required`; it is not
proof of a skimmer and never triggers credential attempts.

## Investigation Result Contract

Each result contains bounded fields suitable for Kotlin, UART JSON, and badge
display:

```json
{
  "request_id": "local-id",
  "transport": "phone|badge_usb|badge_ble|badge_button",
  "mode": "gatt|passive_capture",
  "target_mac": "AA:BB:CC:DD:EE:FF",
  "state": "queued|scanning|connecting|discovering|reading|complete|failed|cancelled",
  "connectable": true,
  "services": ["1800", "1801", "180A", "FFE0"],
  "characteristics": [
    {"service":"FFE0", "uuid":"FFE1", "properties":["read", "write"]}
  ],
  "device_info": {
    "manufacturer": "optional",
    "model": "optional",
    "firmware": "optional"
  },
  "security": {
    "bonded": false,
    "encrypted": false,
    "authentication_required": true
  },
  "passive_evidence": {
    "families": ["swift_pair"],
    "unique_macs": 14,
    "observation_count": 31
  },
  "summary": "UART service found; authentication required",
  "error": null
}
```

Results are capped at 16 services, 32 characteristics, and 64 bytes per
readable value. Binary values are rendered as bounded hexadecimal and sanitized
ASCII. Truncation is explicit.

## Read-Only GATT Procedure

1. Verify the selected address is recent and the app/scanner has permission.
2. Allow only one active investigation per radio.
3. Stop or pause the minimum scanning work needed for the connection.
4. Connect with a 12-second total deadline.
5. Discover primary services and characteristics.
6. Read Generic Access and Device Information values that advertise the read
   property.
7. For a serial-skimmer candidate, attempt a bounded read only when the UART
   characteristic advertises the read property. Record authentication or
   encryption errors without initiating pairing.
8. Disconnect and close the client.
9. Restore scanning in a finally-style cleanup path.

No active investigation runs automatically when an alert is created.

## Android Behavior

The existing privacy detail dialog gains an `Investigate` action. The action
opens a compact investigation surface with route selection, progress, cancel,
summary, services, security, device information, and detector evidence.

Local detector results retain the freshest observed MAC and timestamp needed
for direct connection. Badge-sourced entities retain their BSSID/MAC and badge
entity key. A stale target returns a clear result and offers passive recapture;
it does not silently connect to another address.

Investigation results are attached to the selected detection in view-model
state and are not mixed into backend detections as new devices.

## Badge Button and Display Behavior

Button 2 keeps its current gestures:

- Single press: move to the next focus entry, or next detail page.
- Double press: enter or leave details.
- Long press on an eligible selected BLE entity: start investigation.
- Long press on a pairing-spam entity: start extended passive capture.
- Long press on a non-BLE entity: show the deepest available evidence page.
- Long press while no eligible entity is selected: retain the existing phone
  pairing/QR behavior.

The badge shows `CHECKING`, current phase, and a bounded countdown while work is
active. Result pages show identity/route, services, and security/evidence. A
failed investigation shows the reason and leaves the original alert intact.

The selected snapshot entry already carries the source, entity key, BSSID,
evidence, and detail. The button handler must copy the selected target into a
request before the display model refreshes, so a reordering snapshot cannot
redirect an in-flight investigation.

## Error Handling

- Busy radio: reject the second request with `busy`; do not replace the active
  target silently.
- Stale or rotated address: return `target_stale` and offer passive recapture.
- Nonconnectable advertiser: return passive evidence plus `not_connectable`.
- Connection or discovery timeout: disconnect, restore scanning, and report the
  phase that timed out.
- Authentication/encryption required: report evidence and stop without pairing.
- USB/BLE badge transport loss: badge continues the accepted local operation;
  Android marks transport disconnected and can fetch the latest result after
  reconnection.
- Scanner UART unavailable: badge reports `scanner_unavailable` and does not
  show a false successful investigation.
- Malformed or oversized result: reject or truncate with an explicit flag.

Investigation failures never increase threat confidence by themselves.

## Testing

### Android

1. Pairing-spam fixtures cover Apple, Fast Pair, Swift Pair, mixed-family
   floods, overlapping callback deduplication, cooldown, and decay.
2. Negative fixtures cover a dense but varied crowd, stable legitimate devices,
   generic Apple traffic, isolated Fast Pair, and isolated Swift Pair.
3. Serial-skimmer tests require combined evidence and suppress service-only,
   weak, nonpersistent, trusted-product, and multi-service cases.
4. GATT engine tests use a fake client for success, authentication required,
   timeout, disconnect, stale target, truncation, cancellation, and cleanup.
5. Route-selection tests cover local phone, badge USB-C, badge BLE, unavailable
   badge, and explicit override.
6. View-model and protocol tests prove progress and results remain attached to
   the requested entity.

### ESP32 and Badge

1. Native C tests mirror all detector fixtures and thresholds.
2. Investigation state-machine tests cover passive capture, GATT success,
   authentication required, timeout, busy, disconnect, and scan restoration.
3. UART tests cover request validation, forwarding, progress, result parsing,
   truncation, and unavailable scanner behavior.
4. Button policy tests cover eligible BLE, pairing-spam, non-BLE detail, idle
   pairing fallback, and snapshot reordering.
5. Badge threat-policy tests prove pairing spam and possible serial skimmers are
   visible with stable keys and that UUID-only legitimate devices stay hidden.
6. Badge scanner and uplink firmware targets must build successfully.

## Verification

Minimum software verification:

1. Android targeted unit tests followed by `./gradlew testDebugUnitTest`.
2. Android `./gradlew assembleDebug`.
3. ESP32 native tests through the repo-local PlatformIO environment.
4. Badge scanner build for `scanner-s3-combo-fof_badge`.
5. Badge uplink build for `uplink-s3-fof_badge`.

Hardware verification, when the badge and a safe test peripheral are available:

1. Direct Android investigation discovers services and exits without writes.
2. Android sends an investigation through badge USB-C and receives the result.
3. Android sends the same request through bonded badge BLE control.
4. Holding badge button 2 on a selected BLE test alert starts investigation,
   displays progress/results, and restores scanning afterward.
5. A read-protected test characteristic reports authentication required without
   opening a pairing flow.

Release tagging, APK publication, firmware deployment, and live hardware flash
are separate actions unless explicitly requested after implementation.
