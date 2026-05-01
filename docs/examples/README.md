# Friend or Foe — Live Examples

This folder contains redacted screenshots and JSON samples from a live Friend or Foe deployment running firmware **v0.63.0-svc156** with the backend on **v0.63.20-controlpath-recovery**. Use these as reference for what the platform produces in normal operation.

## Privacy posture

Everything here was captured from a real, operational sensor and then run through layered redaction:

- **GPS coordinates** — fuzzed to the nearest 0.1° (~11 km).
- **SSIDs** — drone, surveillance, and IoT brand prefixes (DJI, Skydio, Ring, Nest, Wyze, Verkada, etc.) are kept unchanged because that's the diagnostic payload reviewers care about. Every other SSID — including the operator's home networks and probe-request leaks from passing devices — is replaced with `[REDACTED-SSID-N]`.
- **BSSIDs / MACs** — first 3 octets (OUI) preserved so manufacturer attribution remains visible; last 3 octets replaced with `XX:XX:XX`.
- **RFC1918 IP addresses** — collapsed to `10.0.0.X`.
- **Satellite tile imagery** of operator property — Gaussian-blurred.

Redaction was applied two ways:

- **JSON samples** (`api-samples/`) — captured via `curl` from the live API, then run through a JSON-walking redaction script that policies each field by name (`ssid`, `bssid`, `lat`/`lon`, `probed_ssids`, `connected_ap_ssid`, `drone_id`, `ip`, etc.).
- **Dashboard screenshots** (`screenshots/`) — captured via Playwright with an in-browser DOM redactor that runs on a 700 ms interval, walking text nodes plus column-aware table cells, plus an explicit deny-list for known operator SSIDs.

## What each screenshot shows

| File | What it demonstrates |
| --- | --- |
| `01-overview.png` | Top-level dashboard. Backend health, sensor count, calibration state, firmware drift warnings, recent activity feed with the new **Connected AP** column for `wifi_assoc` rows (svc156). |
| `02-map.png` | Map tab — multi-source filtering (drones / trackers / mobile hotspots / WiFi devices / known APs / unknown), triangulation status panel (Gauss-Newton + circle-circle + RSSI range circle). |
| `03-alerts.png` | Alert tab — active drone alerts, AirTag stalker tracking, BLE device fingerprints, WiFi probe activity by stable identity, **`wifi_deauth_flood`** warnings, alert history with `mac_rotation` events. |
| `04-diagnostics.png` | System readiness, ingest path, detection-explanation breakdown (WiFi fingerprint / Apple Continuity BLE / Known infrastructure / Randomized probe identity). |
| `05-wifi-aps.png` | Empty-state WiFi devices view (no APs in current scan window — the scanner is BLE-primary in this snapshot). |
| `06-ble.png` | BLE devices tab — AirTag stalker classification, IoT/ESP32 detection, JA3 fingerprints, MAC-rotation count per fingerprint. |
| `07-probes.png` | Probe-request fingerprinting — 24 stable probe identities tracked across MAC randomization (probe_le_hash + IEH + OUI). "Most Searched Networks" sidebar. |
| `08-mobile-hotspots.png` | Mobile devices grouped by stable probe identity, with all observed randomized MACs as evidence. Demonstrates MAC-randomization defeat in the wild. |
| `09-all-detections.png` | Unified All Detections view — every signal class (WiFi Assoc / Probe / BLE FP / AirTag / Apple Continuity), each tagged with full evidence chain. |
| `10-sensors.png` | Sensors tab — GPS position summary, scanner firmware fleet rollout state, RF calibration parameters (RSSI reference, path loss exponent), per-scanner diagnostics. |
| `11-entities.png` | Entity tracker — physical devices correlated across BLE fingerprints, WiFi probes, and AP associations. Marked "gone" only when all sensors lose them. |
| `12-anomalies.png` | RF anomaly detection — alert breakdown (new device / mac_rotation / wifi_deauth_flood / lingering tracker), tracked devices by RSSI. |

## What each JSON sample shows

Captured from the running backend at `http://fof-server.local:8000`:

| File | Endpoint | What it shows |
| --- | --- | --- |
| `api-samples/firmware-latest-uplink-s3.json` | `GET /nodes/firmware/latest/uplink-s3` | New svc156 firmware-host endpoint. Returns `{name, version, size, sha256, download_url, board, description}`. Uplinks poll this every 30 min for self-update. |
| `api-samples/firmware-latest-scanner-s3-combo-seed.json` | `GET /nodes/firmware/latest/scanner-s3-combo-seed` | Same endpoint, scanner variant. The uplink stages this on its `fw_store` partition so connected scanners can pull it via the existing UART OTA chain. |
| `api-samples/nodes-status.json` | `GET /detections/nodes/status` | Per-node health: scanner versions per UART slot, calibration state, RSSI envelope, geometry status, firmware fleet rollout target. |
| `api-samples/detections-grouped.json` | `GET /detections/grouped` | The unified detection feed: 6 device groups in this snapshot — 2 AirTag BLE fingerprints, 3 `wifi_assoc` rows (with redacted STA→AP MAC pairs), 1 `wifi_probe_request`. Note: `connected_ap` is `None` in this capture because the BSSID-to-AP cache (svc156) had not been populated for these specific BSSIDs yet — under normal operation it surfaces as the SSID the station is associated with. |
| `api-samples/wifi-ap-inventory.json` | `GET /detections/wifi/ap-inventory` | The AP inventory feeding the BSSID→SSID lookup for `wifi_assoc` enrichment. Empty in this capture window (no fresh beacons in the last interval). |

## Redaction script

The exact redaction logic used for the JSON samples is preserved in [fof_redact.py](./fof_redact.py) so reviewers can verify the policy is symmetric: anything redacted in the JSON would also be redacted in the screenshots, and vice versa.

## Reproducing

These captures were taken from the operator's live deployment. To reproduce against your own deployment:

```bash
# JSON samples
curl -s http://YOUR_BACKEND:8000/detections/grouped \
  | python3 docs/examples/fof_redact.py \
  > docs/examples/api-samples/detections-grouped.json

# Dashboard screenshots
# Open http://YOUR_BACKEND:8000/static/dashboard.html in Playwright,
# inject the redactor (see commit history of this folder), and screenshot
# each tab via fullPage:true.
```
