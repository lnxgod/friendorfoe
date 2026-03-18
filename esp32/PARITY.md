# Detection Parity: Android vs ESP32

Detailed comparison of detection capabilities between the Friend or Foe Android app and the ESP32 dual-board hardware edition, based on actual source code analysis.

## Detection Module Comparison

| Detection Module | Android | ESP32 | Parity |
|---|---|---|---|
| **OpenDroneID Parser** (7 msg types: Basic ID, Location, Auth, Self-ID, System, Operator ID, Message Pack) | `OpenDroneIdParser.kt` (388 lines) | `open_drone_id_parser.c` (461 lines) | COMPLETE — byte-for-byte port |
| **DJI DroneID IE** (OUI 26:37:12, GPS/altitude/speed/heading/operator) | `DjiDroneIdParser.kt` (208 lines) | `dji_drone_id_parser.c` (212 lines) | COMPLETE |
| **WiFi Beacon RID** (ASTM F3411, OUI FA:0B:BC, type 0x0D) | `WifiBeaconRemoteIdParser.kt` (112 lines) | `wifi_beacon_rid_parser.c` (117 lines) | COMPLETE |
| **SSID Pattern Matching** | 110 patterns (`WifiDroneScanner.kt`) | 104 patterns (`wifi_ssid_patterns.c`) | 95% — 6 patterns behind |
| **OUI Database** | 29 entries (`WifiOuiDatabase.kt`) | 29 entries (`wifi_oui_database.c`) | PERFECT |
| **Bayesian Sensor Fusion** (log-odds, 30s half-life) | 5 source types (`BayesianFusionEngine.kt`, 275 lines) | 5 source types + DJI IE/OUI LRs (`bayesian_fusion.c`, 268 lines) | COMPLETE+ |
| **BLE Remote ID** (UUID 0xFFFA, ASTM F3411) | `RemoteIdScanner.kt` (177 lines) | `ble_remote_id.c` (386 lines) | COMPLETE |

## Platform-Exclusive Features

### Android-Only

| Feature | Why Android-Only |
|---|---|
| **WiFi NaN (Neighbor Awareness Networking)** | Android OS API — no equivalent on bare-metal ESP32 |
| **Visual Detection** (CameraX + ML silhouette classification) | Requires camera hardware and ML inference |
| **ADS-B Polling** (backend API queries to adsb.fi / airplanes.live / OpenSky) | Requires always-on internet; ESP32 scanner has no WiFi STA (radio dedicated to promiscuous mode) |
| **AR Overlay** (ARCore + compass-math hybrid) | Requires phone sensors, display, and ARCore SDK |

### ESP32-Only

| Feature | Why ESP32-Only |
|---|---|
| **Promiscuous WiFi Capture** (raw 802.11 frames) | Android OS blocks raw frame access; ESP32 gets every frame on every channel |
| **Unlimited WiFi Scanning** | Android throttles to 4 scans per 2 minutes; ESP32 hops channels continuously (100ms dwell) |
| **24/7 Unattended Operation** | No screen, no battery drain, deploys as permanent sensor node |
| **Backend Upload** (HTTP POST to `/detections/drones`) | Designed as a sensor node feeding a central server |
| **Multi-Node Sensor Network** | Multiple ESP32 units report to one backend for area coverage |
| **Probe Request Detection** (future) | Can detect drone controller phones via WiFi probe requests |

## Detection Quality Comparison

### WiFi Detection

The ESP32 has a significant advantage for WiFi-based detection:

- **Channel coverage**: ESP32 hops all 13 channels (2.4 GHz) with 100ms dwell time, completing a full sweep in ~1.3 seconds. Android relies on OS-scheduled scans, throttled to 4 per 2 minutes.
- **Frame types**: ESP32 sees all 802.11 frame types (management, control, data) in promiscuous mode. Android only sees beacon summaries via `WifiManager.getScanResults()`.
- **DJI DroneID IE**: ESP32 extracts vendor IEs directly from raw beacon frames. Android can access IEs from `ScanResult.informationElements` (API 30+), but with less reliability.

### BLE Detection

Roughly equivalent. Both use BLE scan for UUID 0xFFFA service data (ASTM F3411 Remote ID):

- Android uses system BLE scanner with duty-cycle managed by the OS.
- ESP32 uses NimBLE with continuous scanning on a dedicated core.

### Bayesian Fusion

Same algorithm and constants, ported from Kotlin to C. Both use log-odds evidence combination with 30-second half-life time decay. ESP32 adds likelihood ratios for DJI IE presence and OUI match as additional evidence sources.

## SSID Pattern Gap

6 patterns present in Android but missing from ESP32:

The Android app has 110 `DronePattern` entries in `WifiDroneScanner.kt` vs 104 entries in the ESP32 `wifi_ssid_patterns.c`. The gap consists of recently-added patterns that haven't been backported yet. Run a diff to identify the specific missing entries:

```bash
# Extract Android patterns
grep 'DronePattern("' android/app/src/main/java/com/friendorfoe/detection/WifiDroneScanner.kt \
  | sed 's/.*DronePattern("\([^"]*\)".*/\1/' | sort > /tmp/android_patterns.txt

# Extract ESP32 patterns
grep '"' esp32/scanner/main/detection/wifi_ssid_patterns.c \
  | sed 's/.*"\([^"]*\)".*/\1/' | sort > /tmp/esp32_patterns.txt

# Show what's missing
diff /tmp/android_patterns.txt /tmp/esp32_patterns.txt
```

## Summary

| Metric | Android | ESP32 |
|---|---|---|
| Core detection parsers | 5/5 | 5/5 |
| SSID patterns | 110 | 104 (95%) |
| OUI entries | 29 | 29 (100%) |
| Bayesian fusion | Same algorithm | Same + extras |
| WiFi capture quality | OS-throttled | Raw promiscuous |
| BLE Remote ID | OS-managed | Continuous NimBLE |
| Deployment model | Phone app, interactive | Sensor node, unattended |
| Unique capabilities | AR overlay, ADS-B, visual detection, WiFi NaN | Promiscuous WiFi, 24/7 operation, multi-node, backend upload |

The two platforms are **complementary**: Android excels at interactive, visual, multi-source identification (camera + ADS-B + AR), while ESP32 excels at continuous, unattended WiFi/BLE monitoring with raw frame access.
