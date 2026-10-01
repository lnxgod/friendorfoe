# Wi-Fi probe activity

Open **Privacy → Wi-Fi probes** or **More → Wi-Fi probes**. Enable **Sensor backend** in App settings, configure the backend URL, and select the scanner physically near you. Results belong to that scanner; they are not labeled as being near the phone.

This feature requires a Friend or Foe ESP32 Wi-Fi scanner uploading to the backend. Ordinary Android Wi-Fi scan results list access points and cannot supply other clients' probe requests. A USB-only badge connection does not supply this feed. Deploy the updated backend and scanner/uplink firmware together for complete sampled reporting. Older scanner firmware intentionally omits ordinary directed requests and most wildcard requests; the app cannot reconstruct those missing frames.

The view shows observed transmitter MAC addresses, requested network names (SSIDs), explicit wildcard searches, current signal/channel, report age, and sampled report count in the last five minutes. Search by network or MAC, choose newest/strongest sorting, filter named searches, or set a minimum received signal of −80/−65 dBm. Signal strength is measured at the scanner, not a distance estimate. **Alert me here** provides vibration and a banner for new addresses or network names while this screen is open. It baselines the first poll and does not announce the same observation every five seconds. This is a foreground feature; it does not add background notifications.

Probe requests are ordinary network discovery, not proof of an attack or an established connection. A MAC can change, and multiple transmitters can share a capability fingerprint. The activity feed groups only by observed MAC within the selected scanner, without fingerprint-based identity inference. It does not record packet payloads or credentials.

## Implementation

- Android: dedicated Compose screen with lifecycle-gated five-second polling, scanner selection retained across recreation, search/filter controls, source changes that clear retained rows, retry/setup states, and monotonic aging even during outages.
- Backend: additive `GET /detections/probes/activity?sensor_id=…&max_age_s=300`. An unselected request returns the observer list with no transmitter rows. Projection uses scanner timestamps when synchronized, scopes signal and SSIDs before aggregation, and preserves exact primary names containing commas. Empty metadata is “name not captured” unless there is affirmative wildcard evidence.
- Scanner: validate SSID IEs and strip FCS before parsing; distinguish named, wildcard, and binary names. Ordinary probe telemetry is limited to three reports per second globally and one per MAC/name per 30 seconds, with queue pressure shedding. Existing priority observations retain their cadence. Probe telemetry bypasses LCD display filters but routine probes do not become badge threat alerts.
- Shared scanner/uplink deduplication: separate transmitter MACs and target names even when capability hashes match. Existing JSON protocol remains compatible.
- Capture remains limited to supported bands, the currently monitored channel, channel hopping, RF conditions, and sampling. No claim of exhaustive detection is made.

## Verification

Regression tests cover scope isolation, same-fingerprint transmitters, comma-containing SSIDs, wildcard versus missing names, invalid/malformed frames, delayed uploads, five-minute expiry, signal sorting/filtering, foreground lifecycle cancellation, source switching, initial alert baselines, dense-traffic budgets, and badge threat suppression. Emulator screenshots use synthetic observations, not real nearby devices.

A physical RF capture was not performed. Hardware acceptance: use a scanner beside your own test client, have it search for a known network, and confirm the exact SSID plus a separate general scan when the client sends one. Verify backend reporting and observer identity before interpreting an empty list.

Primary platform references: [Android Wi-Fi scan results](https://developer.android.com/develop/connectivity/wifi/wifi-scan), [ESP-IDF Wi-Fi capture metadata](https://docs.espressif.com/projects/esp-idf/en/v5.2.2/esp32s3/api-reference/network/esp_wifi.html).

## Verified locally

- Android JVM: 1,150 tests passed; `lintDebug`, debug APK, and instrumentation APK builds passed.
- Android emulator: 156 tests passed (fresh-launch test plus 155 interaction/storage tests). The new probe screen tests cover setup, scanner selection, named/wildcard rendering, search, and navigation back to More.
- Backend: 636 tests passed.
- ESP32 native: 946 tests passed with AddressSanitizer enabled.
- Standard and badge scanner firmware compiled; the badge artifact verifier passed. Probe channels use received-packet metadata, including channel 14 handling.
- Badge uplink firmware compiled and passed the immutable artifact verifier using the repository’s example Wi-Fi configuration.

## Emulator previews

![Probe activity with synthetic observations](activity.png)
![Scanner setup](setup.png)
