# Changelog

All notable changes to Friend or Foe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.8.0-beta] - 2026-03-19

### Added

#### Android — New ADS-B & Enrichment Sources
- **ADSB One** — 5th ADS-B source added to parallel query (ADSBx v2 format, no API key)
- **hexdb.io enrichment** — real aircraft database lookup replaces heuristic registration/type guessing. Returns actual registration, ICAO type code, manufacturer, type, and registered owner
- hexdb.io route lookup — resolves callsigns to origin/destination airport pairs
- Aircraft detail now populated from hexdb.io instead of returning failure when backend is unavailable

#### Android — Filtering & Search
- **Filter bar** on List and History screens — search by callsign/ICAO/registration, filter by category, detection source, and object type
- Advanced filter options: max distance, altitude range
- `FilterEngine` — stateless filter engine supporting all filter dimensions
- `FilterState` domain model for reactive filter UI state

#### Android — Auto-Capture & Threat Assessment
- **Auto-capture engine** — correlates ML Kit visual detections with radio-tracked sky objects to automatically photograph nearby aircraft/drones
- Auto-capture toggle button on AR screen with visual indicator
- **Drone threat assessment** on detail cards — risk level (Benign→Restricted) and threat classification (Civilian, Military ISR, Loitering Munition, FPV Combat, etc.) from OSINT drone database
- Autonomy level display (Manual, Semi-Autonomous, Fully Autonomous)

#### Android — Drone Database Expansion
- Drone database expanded with risk levels, threat classifications, and autonomy levels for all entries
- New enum types: `RiskLevel`, `ThreatClassification`, `AutonomyLevel`
- ~1,100 new lines of drone reference data

#### Backend — New ADS-B Sources (fixes bug B8: missing adsb.lol)
- **ADSB One** added to backend fallback chain
- **adsb.lol** added to backend fallback chain (was in Android but missing from backend)
- Backend ADS-B chain now: adsb.fi → airplanes.live → ADSB One → adsb.lol → OpenSky

#### Backend — Enhanced Enrichment
- **hexdb.io** — primary aircraft data source for detail endpoint (real registration, type, manufacturer, owner)
- **hexdb.io route API** — resolves callsigns to origin→destination routes
- **airport-data.com** — added as photo fallback source
- **hexdb.io thumbnails** — added as tertiary photo fallback
- Photo chain now: planespotters.net → airport-data.com → hexdb thumbnail → placeholder
- Concurrent fetching of hexdb.io data and photos for faster detail responses

#### ESP32 — Uplink Enhancements
- **WiFi AP mode** — uplink now runs AP + STA concurrently, creates `FoF-XXXX` hotspot for field configuration
- **HTTP status page** — embedded web server on `http://192.168.4.1` shows live device status, drone count, GPS, WiFi, battery
- Post-flash Web Serial configuration for WiFi credentials and backend URL
- Improved UART RX buffering and NVS configuration persistence

### Changed
- ADS-B fallback chain expanded from 4 to 5 sources (Android) and 3 to 5 sources (backend)
- `AircraftRepository.getAircraftDetail()` now queries hexdb.io instead of returning failure
- `DataSource` enum extended with `ADSB_ONE`

## [0.8.0-alpha] - 2026-03-18

### Added
- **ESP32 Hardware Edition** — dual-board drone detector (ESP32-S3 scanner + ESP32-C3 uplink)
- BLE Remote ID + WiFi promiscuous scanning ported from Android to ESP-IDF C firmware
- 104 SSID patterns, 29 OUI entries, Bayesian fusion engine — full detection parity with Android app
- UART inter-board protocol at 921,600 baud with newline-delimited JSON
- SSD1306 OLED display, GPS NMEA parser, battery monitor, status LED on uplink board
- HTTP batch upload to backend with offline ring buffer (100 batches)
- `POST /detections/drones` and `GET /detections/drones/recent` backend endpoints
- 27 native unit tests for parsers and fusion engine
- See `esp32/INSTALL.md` for hardware setup and `esp32/CHANGELOG.md` for full details

## [0.7.0-beta] - 2026-03-17

### Added
- Always-visible floating zoom +/- buttons on AR screen right edge
- Take Photo button captures full-resolution image to gallery
- Share confirmation bar with Android share sheet integration
- capturePhotoToGallery now returns saved URI for sharing
- ~75 new drone reference photos

### Fixed
- BLE scanner not stopped in RemoteIdScanner.stopScanning()
- Bounds checks in OpenDroneIdParser and DjiDroneIdParser
- ImageProxy not closed on exception in VisualDetectionAnalyzer
- Thread-unsafe LinkedList replaced with ConcurrentLinkedDeque in WifiDroneScanner
- Coroutine scope leaks in AdsbPoller and SkyObjectRepository
- Missing synchronized blocks for DronePartialState mutations
- ListViewModel.locationStarted race condition (now AtomicBoolean)
- DAO providers missing @Singleton in DatabaseModule
- BayesianFusionEngine beliefStates unbounded growth (capped at 500)

## [0.6.0-beta] - 2026-03-16

### Added
- 49 new drones across consumer 2024-25, budget/toy, enterprise, and military/defense categories
- 12 new WiFi SSID patterns for SIMREX, Neheme, AOVO, TENSSENX, Freefly, senseFly, Wingcopter, Flyability, DJI Flip/Neo
- Visual shape classification using bounding box aspect ratio and motion history (quad vs fixed-wing hints)
- WiFi beacon Remote ID parser
- Airline lookup and ICAO country code display
- WiFi channel utility for drone frequency detection

### Changed
- Visual detection tags now show shape hints ("DRONE? quad", "DRONE? fw")
- Mark CI releases as full releases instead of prerelease

## [0.5.1-beta] - 2026-03-15

### Fixed
- ADS-B silent failure in release builds: ProGuard was stripping Gson/Retrofit response classes, causing null deserialization and 0 aircraft displayed

### Added
- Combined Reference Guide screen with tabbed Aircraft (138) and Drones (52) view, replacing drone-only entry point

## [0.5.0-beta] - 2026-03-15

### Added
- Snap-to photo capture: tap any AR label to lock on, zoom, and capture full-resolution photos
- Aircraft Reference Guide with 138 searchable entries covering all bundled aircraft types
- 20 new drone database entries (DJI Neo, Mini 3, Agras T40, Bayraktar TB3, MQ-25 Stingray, XQ-58 Valkyrie, and more)
- 20 new drone photos sourced from Wikimedia Commons
- Zoom slider spans ultrawide to telephoto (all forward-facing lenses via CameraX)
- Zoom preset buttons (0.5x, 1x, 2x, 5x, Max) in snap photo sheet
- "View in Aircraft Guide" button on aircraft detail cards
- AircraftGuide navigation route with type code deep linking
- ImageCapture use case added to CameraX pipeline for full-resolution photo capture

### Changed
- AR label tap behavior: now opens snap-to photo sheet instead of detail bottom sheet
- Camera zoom range now tracks minZoomRatio for ultrawide lens access

## [0.4.0-beta] - 2026-03-15

### Added
- Drone reference guide with searchable database of common drone types
- 68 new bundled aircraft photos (AC-130, AH-64, SR-71, U-2, UH-1, and more)
- 9 government/law enforcement callsign patterns (Secret Service, US Marshals, DEA, ICE, State Police, and more)
- 8 additional military type codes (E-4B, C-17A, C-130J, AC-130, F-117, AV-8B, EA-18G, C-12)
- 12 README translations (Hebrew, Ukrainian, Arabic, Turkish, Azerbaijani, Turkmen, Pashto, Urdu, Kurdish, Armenian, Georgian, Persian)
- Drone photo download script (`scripts/download_drone_photos.py`)

### Changed
- Government aircraft silhouette fallback changed from BIZJET to NARROWBODY for better accuracy when type code is unavailable

### Fixed
- Government aircraft without type codes no longer show business jet silhouette

## [0.3.0-beta] - 2026-03-14

### Added
- Welcome/launch screen with app info, version display, and quick links to GameChangers and GitHub
- In-app "Check for Updates" button — queries GitHub Releases API for newer versions
- 134 bundled aircraft photos as APK assets (offline, no network needed)
- Photo download script (`scripts/download_aircraft_photos.py`) for sourcing photos from Wikimedia Commons
- Coil image loading library for efficient photo display
- Aircraft photo display on detail cards with bundled asset fallback
- GitHub Actions CI workflow for automated builds on push and PR
- CHANGELOG.md

### Changed
- Detail screen now shows aircraft photos from bundled assets instead of requiring network
- Navigation updated to support welcome screen as start destination

## [0.2.0-beta] - 2025-03-14

### Added
- 120+ aircraft silhouettes mapped to ICAO type codes (10 vector drawable categories)
- Styled map markers with distinct shapes per aircraft category
- OpenStreetMap 2D map view with distance rings, compass-follow, and FOV cone
- Aircraft detail cards with full metadata (registration, operator, route, squawk)
- Detection history with persistent Room database storage
- MIT License and open-source release

### Changed
- Improved permission handling and onboarding flow
- Security review and hardening

## [0.1.0-beta] - 2025-03-12

### Added
- Initial release — AR viewfinder with floating labels
- Multi-source detection: ADS-B, FAA Remote ID (BLE), WiFi SSID, ML Kit visual
- Bayesian sensor fusion engine for multi-sensor confidence scoring
- Military aircraft classification (callsign patterns, squawk codes, operator DB)
- ARCore + compass-math hybrid orientation tracking
- Three-tier ADS-B fallback chain (adsb.fi → airplanes.live → OpenSky)
- Optional Python FastAPI backend for aircraft enrichment
- Hilt dependency injection, Room database, Retrofit networking
- List view with sortable columns
- Bottom navigation (AR, Map, List, History, About)

[0.8.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.8.0-beta
[0.7.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.7.0-beta
[0.6.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.6.0-beta
[0.5.1-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.1-beta
[0.5.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.0-beta
[0.4.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.4.0-beta
[0.3.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.3.0-beta
[0.2.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.2.0-beta
[0.1.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.1.0-beta
