# Threat Model

Friend or Foe is a passive RF + visual detection system for aircraft and
drones. This document spells out what it is and is not designed to do, and
the calibration we use so users can interpret detections honestly.

## What Friend or Foe Detects

| Source | Confidence (likelihood ratio) | Notes |
|---|---|---|
| ADS-B transponder | 100 | 4-source fallback chain, no API keys |
| BLE Remote ID (FAA / EU) | 50 | OpenDroneID parser, byte-for-byte parity Android ↔ ESP32 |
| WiFi Beacon RID (NAN) | 50 | Frame-level parse, same parity |
| DJI DroneID IE | 30 | Vendor-specific WiFi IE, includes operator GPS |
| OUI manufacturer match | 5 | 29-entry curated drone OUI table |
| SSID pattern match | 3 | 104 prefixes (DJI-, Tello-, Skydio-, etc.) |
| BLE tracker fingerprint | — | AirTag, Tile, etc., separate "tracker" classification |
| Smart-glasses fingerprint | — | Meta CID, Ray-Ban, Oakley, with rotating-RPA grouping |
| RF anomaly | — | Evil twin (mixed open/WPA), karma (one BSSID, many SSIDs), Pwnagotchi BSSID |

Each detection carries a likelihood ratio that combines via Bayesian
log-odds in `BayesianFusionEngine` (Android) and `bayesian_fusion.c`
(ESP32). Evidence decays with a 30-second half-life and the log-odds are
clamped to [-7, +7] so a single noisy frame can't dominate.

## What Friend or Foe Does Not Detect

- **Drones operating without Remote ID.** Required by the FAA Part 89 rule
  (US) and EU Delegated Regulation 2019/945, but non-compliance happens.
  We have no way to see a drone that isn't broadcasting.
- **Wired or fiber-only devices.** This is an RF + optical system.
- **Fully randomized devices.** A device that randomizes its MAC, IE hash,
  service UUIDs, and manufacturer data simultaneously is invisible to
  fingerprint-based grouping.
- **Spoofed ADS-B from rogue ground stations.** ADS-B has no integrity
  protection; we trust the network. Cross-corroboration with sensor-network
  RSSI / triangulation helps, but a determined adversary can inject.
- **Visual identification beyond ML Kit's general-object class.** The AR
  viewer overlays *labels* derived from RF; the camera ML is not a
  drone-vs-bird classifier.

## Confidence Calibration

The README and dashboard show a single confidence number per device, but
under the hood the value is a Bayesian posterior, not a heuristic. SSID-only
detections are explicitly low-confidence (typically 0.30–0.40) and labelled
`possible_drone` rather than `confirmed_drone`. Multi-source detections
(BLE Remote ID + DJI IE + ADS-B) saturate the log-odds clamp and produce
near-certainty very quickly.

Per-source ratios live in `esp32/shared/constants.h` and the Android
`BayesianFusionEngine`. Math walk-through is in
[`BAYESIAN_FUSION.md`](BAYESIAN_FUSION.md).

## MAC Randomization Handling

Modern phones, glasses, and drones rotate their BLE Random Private
Addresses every 15 minutes by default. Naive MAC-keyed tracking creates a
new "device" every rotation and floods the operator with false novelty.
FoF fingerprints on:

- **BLE-JA3** — a hash of the BLE advertising payload structure (length,
  type, ordering of TLVs). Stable across RPA rotation.
- **Service UUID set + manufacturer data hash** — for devices that
  advertise consistent app/firmware identifiers.
- **WiFi probe IE-hash** — for probe-request grouping; stable across
  randomized MAC-randomized 802.11 stacks.

See `backend/app/services/entity_tracker.py` for the correlation rules and
`android/app/src/main/java/com/friendorfoe/detection/BleJa3.kt` for the
fingerprint computation.

## Privacy

- **No remote telemetry.** The Android app runs locally and uploads only
  what the user explicitly chooses to send to a backend.
- **Backend stores what the operator's own sensor nodes upload.** There is
  no central, multi-tenant cloud.
- **Venue opt-out.** The `WhitelistedSSID` table (and a future allowlist
  UI) lets venue operators mark networks that should be ignored.
- **No accounts, no API keys.** The backend has no auth on most read
  endpoints by design; operators are expected to firewall their dashboard
  appropriately.

## Known False-Positive Sources

- Action cameras (GoPro, Insta360) advertising on WiFi can match drone OUI
  patterns — confidence is intentionally bounded for OUI-only detections.
- WiFi mesh extenders and base-stations with mismatched OUI used to trigger
  evil-twin alerts on the Android app; suppressed in v0.63.0-svc156.
- Pre-svc150 ESP32 nodes had heap-pressure restarts that surfaced as
  spurious uplink-down anomalies; the auto-OTA path resolves this fleet-wide.

## Out of Scope

- Active interference (jamming, deauth, packet crafting) is **not in this
  codebase** and patches that add it will not be accepted. See
  [`../SECURITY.md`](../SECURITY.md) for the dual-use posture.
- Spectrum analysis below 2.4 GHz / above 5 GHz is not supported on the
  current hardware (ESP32-S3 + phone radios).
