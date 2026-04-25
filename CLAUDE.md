# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Friend or Foe is a multi-platform system for real-time aircraft and drone identification. The Android app uses AR to overlay floating labels on the camera view. ESP32 hardware provides continuous unattended detection. A Python backend aggregates and enriches ADS-B data.

## Build & Test Commands

### Android (Kotlin, JDK 17, compileSdk 35, minSdk 26)
```bash
cd android && ./gradlew assembleDebug          # Debug build
cd android && ./gradlew assembleRelease        # Release (needs keystore env vars)
cd android && ./gradlew test                   # Unit tests
cd android && ./gradlew test --tests "*.SkyPositionMapperTest"  # Single test class
adb install app/build/outputs/apk/debug/app-debug.apk          # Install on device
```

### Backend (Python 3.11+, FastAPI)
```bash
cd backend
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000   # Run server
pytest                                              # All tests
pytest tests/test_api.py -v                         # Verbose
pytest tests/test_api.py::test_health_check -v      # Single test
docker compose up                                   # Full stack (API + Redis + PostgreSQL)
```

### ESP32 (PlatformIO + ESP-IDF, requires Python 3.12 venv — 3.14 breaks pydantic-core)
```bash
cd esp32/scanner && pio run -e scanner-s3-combo       # Build production scanner
cd esp32/scanner && pio run -e scanner-s3-combo-seed  # Build seed scanner
cd esp32/uplink && pio run -e uplink-s3               # Build production uplink
cd esp32/rid-simulator && pio run              # Remote ID simulator
cd esp32 && pio test -e test                   # Run unit tests (native, no hardware needed)
cd esp32/web-flasher && python3 -m http.server 8080  # Local web flasher
```

Current release firmware support is S3-only: `scanner-s3-combo`, `scanner-s3-combo-seed`, and `uplink-s3`.

## Architecture

### Platforms
- **`android/`** — Kotlin + Jetpack Compose, Clean Architecture. AR viewfinder with multi-source detection.
- **`backend/`** — Python FastAPI. ADS-B aggregation, drone detection ingest from sensor nodes, multi-sensor triangulation, entity tracking, RF anomaly alerts, web dashboard, OTA firmware hosting.
- **`esp32/`** — ESP-IDF firmware for the current ESP32-S3 scanner, seed scanner, and uplink fleet, plus shared native tests and RID simulator utilities. Scanner↔Uplink communicate via UART at 921,600 baud with newline-delimited JSON. Unified version in `esp32/shared/version.h`.
- **`macos/`** — Swift companion app (early stage, not production).

### Android Clean Architecture Layers
- **`data/`** — Room database (HistoryEntity, TrackingEntity), Retrofit API services (separate clients for adsb.fi, airplanes.live, adsb.lol, OpenSky, Open-Meteo), repositories with fallback chains
- **`domain/model/`** — `SkyObject` sealed class (`Aircraft`, `Drone`), `Position`, `DetectionSource` enum, `ObjectCategory` enum
- **`domain/usecase/`** — `GetNearbyAircraftUseCase`, `SaveDetectionUseCase`
- **`detection/`** — `AdsbPoller` (5s interval), `RemoteIdScanner` (BLE), `WifiDroneScanner` (SSID patterns + DJI IE + WiFi Beacon RID), `BayesianFusionEngine`, `MilitaryClassifier`, `VisualDetectionAnalyzer` (ML Kit)
- **`sensor/`** — `SensorFusionEngine` (accel/mag/gyro with low-pass filtering), `SkyPositionMapper` (haversine + screen projection)
- **`presentation/`** — Compose screens (AR, Map, List, Detail, History, DroneGuide, Welcome), ViewModels with StateFlow
- **`di/`** — Hilt modules: `NetworkModule` (named Retrofit instances per API), `DatabaseModule`, `SensorModule`

### Backend Structure
- **`app/main.py`** — FastAPI with lifespan management (creates Postgres tables if reachable, falls back to no-persistence), request logging middleware, mounts `static/dashboard.html`
- **`app/services/adsb.py`** — Multi-source ADS-B with Redis caching (30s TTL), bounding box queries
- **`app/services/enrichment.py` / `enrichment_ble.py`** — ICAO→country/airline/photo lookup; BLE company-ID + signature enrichment
- **`app/services/triangulation.py`** — Multi-node drone localization: 3+ sensors via Gauss-Newton NLLS, 2 sensors via circle-circle intersection, 1 sensor via RSSI range circle. Log-distance path loss model shared with ESP32 firmware.
- **`app/services/position_filter.py`** — EKF smoothing of triangulated positions (tuning notes in user memory)
- **`app/services/calibration.py`** — Inter-node RSSI calibration: nodes take turns broadcasting AP at max/min power; regression on (distance, RSSI) sets `path_loss_exponent` and `RSSI_REF`. Persisted to disk, manual-trigger only.
- **`app/services/entity_tracker.py`** — Correlates BLE fingerprints, WiFi probes, and hotspots into logical entities. Entity marked "gone" only when ALL sensors lose it.
- **`app/services/anomaly_detector.py` / `rf_anomaly.py`** — RF anomaly alerts with whitelisting, fingerprint-based BLE tracking, mesh-aware spoofing detection
- **`app/services/classifier.py`** — Drone signature classification
- **`app/services/firmware_manager.py`** — Firmware upload/storage for OTA delivery to nodes
- **`app/routers/`** — `aircraft.py` (nearby/detail), `detections.py` (POST from ESP32 uplinks, recent), `nodes.py` (register/update/list/delete sensor nodes + OTA upload)
- **`app/models/schemas.py`** and **`app/models/db_models.py`** — Pydantic v2 schemas; SQLAlchemy models (`SensorNode`, `DroneDetection`)

### ESP32 Detection Pipeline
Scanner captures raw 802.11 frames (channel hopping 1-13) and BLE advertisements, runs 5 parsers (BLE Remote ID, DJI DroneID IE, WiFi Beacon RID, SSID pattern, OUI lookup), fuses via Bayesian engine, serializes to JSON over UART → Uplink batches and POSTs to backend.

### Shared Code (`esp32/shared/`)
- `uart_protocol.h` — Wire format: short JSON keys (`src`, `conf`, `mfr`, etc.)
- `detection_types.h` — `drone_detection_t` struct used by both scanner and uplink
- `constants.h` — Bayesian priors, RSSI model, OpenDroneID encoding constants, DJI/ASTM OUIs

### Detection Parsers — Ported Byte-for-Byte
The same core detection logic exists in both Android (Kotlin) and ESP32 (C): OpenDroneID, DJI DroneID IE, WiFi Beacon RID, SSID patterns (104 prefixes), OUI database (29 entries), Bayesian fusion (log-odds with 30s half-life evidence decay).

## Key Patterns

- **Bayesian sensor fusion** — Log-odds evidence combination, not max-confidence. Likelihood ratios: ADS_B=100, BLE_RID/WiFi_NAN=50, DJI_IE=30, OUI=5, SSID=3. Clamped to [-7, +7].
- **ADS-B fallback chain** — adsb.fi → airplanes.live → adsb.lol → OpenSky. All free, no API keys.
- **Reactive streams** — StateFlow for UI state, SharedFlow for detection emissions, Flow for database queries. `SharingStarted.WhileSubscribed` for lifecycle.
- **Hilt DI** — `@Named` qualifiers distinguish Retrofit instances per API source. Separate OkHttpClient for ADS-B (8s timeout) vs general (15s timeout).
- **ARCore + compass hybrid** — ARCore for feature-rich scenes, compass-math fallback for open sky (exactly when you need it most).
- **Repository pattern** — `AircraftRepository` merges 4 ADS-B sources by ICAO hex, preferring most recently updated. `SkyObjectRepository` merges all detection types with deduplication.

## Conventions

- Domain models in `domain/model/`, sealed `SkyObject` with `Aircraft` and `Drone` subtypes
- ViewModels use `StateFlow`, Compose screens observe via `collectAsStateWithLifecycle`
- Category colors in `presentation/util/CategoryColors.kt`
- Vector silhouettes in `res/drawable/ic_silhouette_*.xml`, mapped via `AircraftSilhouettes.kt`
- Detection sources each emit `Flow<List<SkyObject>>`, collected by `SkyObjectRepository`
- Backend uses `Result<T>` returns; 502 when all upstream ADS-B sources fail
- ESP32 uses FreeRTOS tasks with priority-based scheduling (Scanner: dual-core radio/processing split; Uplink: priority 5=UART down to 1=LED)
- Stale detection pruning: 60s (Android), 120s (ESP32)

## CI/CD

- **Android**: GitHub Actions on push/PR to main. JDK 17, Gradle cache. Debug APK artifact (30-day retention). Release APK on `v*` tags via `softprops/action-gh-release`.
- **ESP32**: GitHub Actions on push to main (esp32/ paths). PlatformIO build, firmware artifacts (90-day retention). Deploys web flasher to GitHub Pages. Firmware tarballs attached to releases.
