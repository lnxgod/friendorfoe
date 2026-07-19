# Android Wi-Fi Drone Identity and AR Cleanup Design

## Problem and Current Evidence

The Android app already recognizes DJI-style Wi-Fi SSIDs, reads Android's
`ScanResult.BSSID`, enriches observations with the local Wi-Fi OUI database,
and shows a BSSID in live drone details. Three gaps prevent that path from
being release-solid:

1. The application starts detection before its startup permission request is
   resolved. `WifiScanCoordinator` checks permission only once and permanently
   closes its flow when permission is absent, while a later grant does not
   restart the active detection session.
2. Wi-Fi detections use only the normalized SSID for their object ID. Two
   transmitters advertising the same SSID can therefore merge, and the Room
   history schema drops SSID, BSSID, signal, frequency, and channel-width data.
3. The AR screen displays a persistent warning to point the camera above the
   horizon. That warning is distracting and is not required for the detector
   or AR tracking to work.

Android can report the observed Wi-Fi transmitter BSSID; it cannot prove that
the address is a permanent aircraft serial identity. DJI aircraft,
controllers, access points, and randomized radios can all be the observed
transmitter. The app must retain and present that evidence without overstating
what it means.

## Scope and Safety Boundary

This change is limited to the Android application, its database migration, and
Android tests. Badge firmware remains read-only: its existing DJI SSID, vendor
information-element, Remote ID, BSSID, and UART paths may be audited and tested,
but no `esp32/` production file will be changed without separate explicit
approval.

The Android collector remains an in-process, lifecycle-independent application
collector. This update does not add a foreground service, background-location
tracking, or a claim that Android has discovered a permanent DJI aircraft MAC.
It also does not change the existing normal Wi-Fi scan rate.

## Permission and Radio Readiness

Wi-Fi collection becomes a recoverable state machine instead of a one-time
permission gate. It distinguishes these states:

- missing runtime permission;
- system location services disabled;
- Wi-Fi disabled;
- ready to scan;
- a transient scan/API error.

The coordinator will not call `startScan()` or read scan results until the
platform requirements for that Android version are satisfied. The manifest and
startup request retain `NEARBY_WIFI_DEVICES` on Android 13 and newer and
`ACCESS_FINE_LOCATION` for scan-result access where Android requires it, along
with the existing Wi-Fi state permissions. The permission callback and
application resume path notify the collector to reevaluate immediately.

While blocked, the collector remains alive and periodically reevaluates its
readiness at a short, low-cost interval so first grant, Wi-Fi enablement, or
location-service enablement recovers without restarting the app. Once ready,
it returns to the current throttled scan cadence. A `SecurityException` or
transient scan failure updates readiness and retries through the same state
machine rather than terminating the flow.

The readiness reason is exposed as data suitable for the existing status/About
UI, allowing the app to say whether permission, Wi-Fi, or location services are
blocking Wi-Fi drone detection. Permission denial remains respected; the app
does not repeatedly launch system permission prompts.

## Observed BSSID Identity

Each Wi-Fi observation canonicalizes its BSSID to six uppercase hexadecimal
octets separated by colons. Malformed addresses, the Android privacy placeholder
`02:00:00:00:00:00`, the broadcast address, all-zero addresses, and multicast
addresses are not accepted as transmitter identities.

When a usable BSSID exists, the live detection ID includes it. This keeps two
radios with the same DJI SSID separate and lets successive observations from
one observed radio update the same object. When no usable BSSID exists, the
existing normalized-SSID ID remains the deterministic fallback.

DJI classification remains evidence-based and additive:

- existing DJI SSID signatures continue to classify matching observations;
- OUI lookup enriches a valid BSSID when a known manufacturer prefix exists;
- an unknown OUI does not suppress an otherwise valid DJI SSID detection;
- a DJI OUI alone may add manufacturer context but is not presented as proof of
  aircraft ownership or model.

The details UI labels the value **Observed BSSID / MAC** and adds concise copy:
“May rotate or belong to the aircraft/controller radio.” Existing SSID, signal,
frequency, and channel information remain visible.

## History Schema and Reconstruction

Room advances from schema version 4 to 5 with nullable columns for:

- SSID;
- BSSID;
- signal strength in dBm;
- frequency in MHz;
- channel width in MHz.

The migration is additive and preserves every existing row. Both current
history-write paths populate the new fields. History-to-domain reconstruction
restores them so a saved DJI observation shows the same observed transmitter
evidence as the live detail view. Older rows naturally retain null values and
continue rendering without fabricated radio data.

## AR Warning Removal

Remove the `showGroundBanner` state/effect and the banner text:
“Camera pointing below horizon — aim higher to detect aircraft.” No replacement
warning is added.

This removal is deliberately narrow. Camera tracking, pitch calculation,
compass behavior, pitch-based ground-clutter suppression, target projection,
and lock-on guidance remain unchanged. The AR screen simply stops nagging the
operator about camera direction.

## Verification Strategy

Implementation follows red-green-refactor with focused regression tests:

1. Permission/readiness tests cover startup before grant, immediate recovery
   after grant, revoke-and-regrant, disabled Wi-Fi, disabled location services,
   transient failure, and `SecurityException` recovery without flow closure.
2. Wi-Fi observation tests cover DJI SSID recognition, canonical BSSID,
   BSSID-based IDs, same-SSID/different-BSSID separation, and rejection of
   placeholder, broadcast, multicast, all-zero, and malformed addresses.
3. Room migration and mapping tests prove version 4-to-5 preservation and a
   round trip of SSID, BSSID, signal, frequency, and channel width through both
   history-write paths and detail reconstruction.
4. UI/source contract tests verify the honest BSSID label and explanatory copy,
   and verify that the AR warning/banner is gone while the detector's
   pitch-dependent behavior remains present.
5. The focused Android suite, full `testDebugUnitTest`, and `assembleDebug` must
   pass.
6. Badge validation is read-only: the native ESP32 detector suite must remain
   green, and an optional hardware smoke test may broadcast a DJI-pattern test
   SSID and observe existing UART output. No badge image is rebuilt from changed
   firmware because badge production code is out of scope.

## Success Criteria

- Granting the required permission after app startup enables Wi-Fi detection
  without force-stopping or restarting the app.
- A temporarily blocked or failed Wi-Fi scan path recovers and continues
  emitting observations.
- Valid observed DJI BSSIDs are normalized, displayed honestly, retained in
  history, and used to avoid same-SSID transmitter collisions.
- Invalid or privacy-placeholder addresses are never treated as stable
  transmitter identities.
- The AR camera-direction warning is absent with no regression to tracking or
  detection behavior.
- All Android release gates pass, all read-only badge detector tests pass, and
  the badge firmware diff is unchanged by this work.
