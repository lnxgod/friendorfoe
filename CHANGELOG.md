# Changelog

All notable changes to Friend or Foe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.23.0-beta] - 2026-03-25

### Added
- **Deep BLE packet parsing** — New `BlePacketParser` extracts rich details from every BLE advertisement:
  - Apple AirTag/FindMy: battery level (Full/Medium/Low/Critical), separated-from-owner flag
  - Apple AirPods/Beats: device family, left/right/case battery percentages
  - Samsung SmartTag: state (offline/connected/overmature), aging counter (how long separated), battery
  - iBeacon: proximity UUID, major/minor IDs, TX power → distance
  - Eddystone-URL: decoded broadcast URL
  - Eddystone-TLM: beacon battery voltage, temperature, uptime
  - Google Fast Pair: 24-bit model ID
  - TX Power + RSSI → estimated distance in meters
  - BLE address type (Public vs Random, API 33+ native, heuristic fallback)
- **Details in privacy alerts** — Detected devices now show parsed info inline (battery, separated status, distance, beacon data)

## [0.22.0-beta] - 2026-03-25

### Added
- **WiFi evil twin / rogue AP detection** — Detects same SSID on multiple BSSIDs with mixed security (classic evil twin), different vendor OUIs (rogue AP), and single BSSID broadcasting many SSIDs (karma/Pineapple attack)
- **Ultrasonic beacon detection** — FFT analysis of microphone input at 48 kHz to detect inaudible 18-22 kHz tracking beacons (SilverPush, Lisnr, Shopkick, Signal360). Off by default — requires microphone permission
- **EMF sweep detector** — Magnetometer-based electromagnetic field detector for finding hidden electronics at close range (1-5cm). Real-time magnitude gauge with color-coded threat levels
- **IR camera detector** — Front camera analysis for detecting night-vision IR LEDs from hidden cameras in dark rooms. Detects bright saturated clusters persisting across frames
- **30+ new WiFi SSID patterns** — V380 variants, Tuya/SmartLife, iCam365, XMeye, CareCam, BVCAM, P2PCam, ThroughTek, TinyCam/SpyGear, AI-Thinker, DEPSTECH/Jetion endoscopes, Tactacam trail cameras, ez Share WiFi SD cards, DDPai/Garmin dashcams
- **8 new BLE signatures** — Tuya IoT (CID 0x07D0), Xiaomi (UUID 0xFD2E), AB Shutter3 camera remotes, DEPSTECH endoscopes, Roborock/iRobot/Ecovacs robot vacuums
- **Settings toggles** for WiFi evil twin detection and ultrasonic beacon detection

## [0.21.0-beta] - 2026-03-25

### Added
- **Per-source detection toggles** — Settings screen with on/off switches for: ADS-B Aircraft, BLE Remote ID (Drones), WiFi Detection (Drones), Privacy Scanner, and Stalker/Follower Detection. All default to ON. Changes take effect on next app launch.
- **BLE stalker/follower detection** — Tracks all detected BLE devices over time. Alerts when a device follows you across multiple locations (2+ min, 50+ meters movement) or lingers near you while stationary. Threat levels: Low (tracking), Medium (persistent), High (camera device following 5+ min). Red "STALKER ALERT" banner in list screen.
- **BLE direction finder engine** — Lock onto any detected BLE device and rotate 360° to map RSSI to compass bearing. Estimates the direction to the device with confidence scoring. Uses top 20% of RSSI samples with circular mean for accuracy.
- **DetectionPrefs** — Centralized settings class replacing GlassesDetectionPrefs. Controls all detection source toggles via SharedPreferences.
- **BleTracker** — New tracking engine that records BLE sightings with location context, calculates user movement, detects followers, and provides direction-finding scan API.

## [0.20.0-beta] - 2026-03-24

### Changed
- **Privacy detection ON by default** — Smart glasses, trackers, hidden cameras, and all privacy device scanning now enabled out of the box. Toggle in About > Settings.
- **README rebrand** — Friend or Foe is now positioned as a "Privacy Awareness & Airspace Detection" platform. README leads with privacy detection capabilities, documents all 60+ device signatures.

### Added
- **Expanded privacy database** — Added Apple AirTag/FindMy, Samsung SmartTag, Tile, DULT unwanted tracker protocol, Google Find My Device, iBeacon, Eddystone, Tesla Sentry Mode, Flipper Zero, Motorola body cameras, hidden camera BLE names, and 25 WiFi SSID patterns (hidden cameras, action cams, dash cams, attack tools, doorbell cameras)
- **WiFi privacy scanning** — WiFi scan results now checked against 25 suspicious SSID patterns for hidden cameras, spy cams, dash cams, action cameras, body cameras, and attack tools

## [0.19.0-beta] - 2026-03-24

### Added
- **Smart glasses / privacy device BLE detection** — New detection module identifies Meta Ray-Ban, Snap Spectacles, Xreal, Vuzix, Google Glass, Bose Frames, Amazon Echo Frames, Brilliant Labs, TCL RayNeo, Rokid, INMO, Even Realities, Solos AirGo, and Axon body cameras via BLE manufacturer Company IDs, service UUIDs, and device name patterns
- **Privacy alert OLED display** — When smart glasses are detected nearby, OLED shows alert with device type, manufacturer, signal strength, camera indicator, and estimated distance
- **Glasses detection JSON output** — New `"type":"glasses"` messages in serial JSON output with device info, confidence, match reason, and camera flag
- **KConfig toggle** — `CONFIG_FOF_GLASSES_DETECTION` (default: ON) enables/disables at build time; NVS runtime toggle via web flasher serial config
- **BLE Scanner web flasher card** — New "BLE Scanner" option on the web flasher page for standalone BLE drone + glasses detection firmware
- **BLE Scanner in CI** — GitHub Actions now builds and deploys BLE Scanner firmware alongside Scanner and Uplink
- **Web flasher manifest** — `manifest-ble-scanner.json` for ESP32-S3 BLE scanner firmware
- **README update** — BLE Scanner with smart glasses detection listed in ESP32 Hardware Edition section

### Detection Database
- 12 manufacturer Company IDs (Meta, Snap, Google, Vuzix, Bose, Axon, Brilliant Labs, TCL, Rokid, Amazon)
- 4 service UUID signatures (Meta 0xFD5F, Bose 0xFDD2, Snap 0xFE45, Amazon 0xFE15)
- 21 device name patterns covering all major smart glasses brands
- GAP Appearance 0x01C0 (eyeglasses) detection as fallback
- Confidence scoring from 0.50 to 0.95 based on match specificity

## [0.18.0-beta] - 2026-03-24

### Changed
- **Material Design 3 UI polish** — Comprehensive UI overhaul based on M3 guidelines audit (Gemini + Codex analysis)
- **Bottom navigation** — Cleaned up from 4 tabs + 2 stray IconButtons to proper 4-tab M3 NavigationBar. Reference Guide and About moved to overflow menu (⋮) in filter bar
- **Back arrows** — Replaced Unicode text arrows with proper `Icons.AutoMirrored.Filled.ArrowBack` + content descriptions across 5 screens
- **Theme system** — Added custom Typography (12 styles with weights, line heights, letter spacing) and Shapes (8/12/16/24dp corners) to MaterialTheme
- **Bottom sheets light theme** — Fixed ~20 hardcoded white/cyan text colors in ZoomViewSheet and SnapPhotoSheet — now readable in both light and dark themes
- **Category colors** — Added dark mode variants with lighter tones for readability
- **Detail screen** — Dynamic title shows callsign/serial instead of generic "Object Detail"
- **Screen transitions** — Added fade+slide animations to all navigation transitions
- **Dark mode map** — Map tiles invert colors in dark theme for consistent dark UI
- **Welcome screen** — Upgraded to ElevatedCard with tonal surface, fixed deprecated Divider
- **Haptic feedback** — Added haptics on AR label tap and long-press interactions
- **Spacing system** — Created Dimens.kt with named constants for consistent spacing

### Fixed
- **ConcurrentModificationException** — SkyObjectRepository.rebuildMergedList no longer modifies list during iteration (Critical bug C2)
- **Database query performance** — Added indices on object_id, object_type, last_seen, timestamp across HistoryEntity and TrackingEntity (DB v3→v4 migration)
- **WiFi permission check** — Added runtime NEARBY_WIFI_DEVICES permission check (Android 13+) before WiFi scanning
- **AutoCaptureEngine cleanup** — Removed pointless screenWidthPx/screenHeightPx normalization (coordinates already 0-1)
- **AR ground banner flicker** — Added pitch hysteresis (show <-10°, hide >-5°) to prevent banner flickering

## [0.17.0-beta] - 2026-03-24

### Fixed
- **Drone stale timeout** — Increased from 60s to 120s, matching ESP32 scanner. Drones now stay visible for 2 full minutes after last BLE update instead of disappearing too quickly.
- **MAC address in drone ID** — Fixed multi-drone-on-same-MAC issue where simulator's 2 drones shared one BLE address. Now keys drone state by serial number when available, preventing MAC-based IDs from appearing.
- **Map auto-centering** — Map no longer snaps back to user position while panning. Detects touch gestures and disables auto-center for 10 seconds after last interaction. Allows free exploration of the map.

## [0.16.0-beta] - 2026-03-24

### Added
- **Visual engines wired to UI** — ROI confirmation, motion detector, and SORT tracker now produce visible effects instead of being orphaned dead code
- **SORT label persistence** — Labels stay on screen during ML Kit dropouts via velocity-predicted synthetic detections
- **ROI confidence boost** — Triple-confirmed aircraft (radio + visual + ROI) get 20% brighter labels
- **Motion blob alerting** — Radio-silent moving sky objects trigger DarkTargetScorer "UNKNOWN FLYING" alerts
- **Multi-drone RID simulator** — 2 simulated drones: FOF-SIM-001 (80m/15m/s CW) + FOF-SIM-002 (120m/10m/s CCW)

### Fixed
- **SORT duplicate prevention** — Coasted tracks no longer duplicate ML Kit detections in same frame
- **ROI recycled bitmap guard** — Prevents crash when bitmap is recycled during ROI analysis
- **Removed USB OTG detector** — Not a contactless detection method

## [0.15.0-beta] - 2026-03-24

### Added
- **ROI-based visual confirmation** — Crops regions around ADS-B predicted positions, analyzes brightness/edge patterns at 5Hz to confirm aircraft visually present
- **Motion detection for radio-silent objects** — IMU-stabilized temporal differencing detects moving sky objects without radio signals (catches drones with no transponder)
- **SORT visual tracker** — Keeps AR labels alive between ML Kit frames using velocity prediction (up to 10 frame coast)
- **ESP32 probe request sniffing** — WiFi scanner now catches drone controllers probing for their drone's SSID, detecting operators even when the drone isn't broadcasting
- **193 aircraft reference entries** (+35 new: Su-57, FC-31, Y-20, H-6, Z-10, MQ-1, RQ-4, PC-24, SF50, AW169, Bell 505, CH-53, NH90, Mi-26, B-1B, B-52H, F-4, F-5, Kfir, and more)
- **135 airline callsigns** (+105 new: IndiGo, Korean Air, LATAM, Qatar Airways, Swiss, Iberia, Wizz Air, FedEx, UPS, and 95 more)
- **62 ICAO country hex ranges** (+43 new: Turkey, Saudi Arabia, UAE, Indonesia, Thailand, Poland, Sweden, Argentina, and 35 more)
- **195 drone WiFi SSID patterns** (+91 from baseline: DJI newer models, FPV systems, underwater drones, military datalinks, budget brands, counter-UAS)
- **52 drone OUI entries** (+23 from baseline: DJI variants, Autel, Skydio, Shield AI, AeroVironment, PowerVision, Silvus Technologies)

### Fixed
- **BLE drone GPS coordinates** — 2-byte offset in OpenDroneID service data parsing
- **BLE drone duplicates** — Wait for Basic ID serial before emitting drone
- **Autel OUI misattribution** — 78:8C:B5 was TP-Link, not Autel (removed)

### Improved
- **Chinese/Russian military classification** — Added Su-57, FC-31, Y-20, TU16 (H-6), WZ10 (Z-10), SU25, AN72 type codes
- **ESP32 scanner patterns synced** — 191 patterns matching Android's 195

## [0.14.0-beta] - 2026-03-23

### Added
- **ROI-based visual confirmation** — Crops small regions around ADS-B predicted positions and analyzes brightness/edge patterns to confirm aircraft are visually present. Runs at 5Hz on background thread, independent of ML Kit. Reduces false positives by focusing on where planes should be.
- **Motion detection for radio-silent objects** — IMU-stabilized temporal differencing detects moving objects in the sky even without ADS-B or Remote ID. Finds drones without transponders by detecting pixel changes between camera frames, compensating for phone movement using gyroscope data.
- **SORT visual tracker** — Simple Online Realtime Tracker keeps AR labels alive between ML Kit detection frames using velocity prediction. A plane detected in frame 1 stays tracked through frames 2-10 even if ML Kit temporarily misses it. Assigns stable tracking IDs across frames.

## [0.13.0-beta] - 2026-03-23

### Added
- **Trajectory prediction** — Dead-reckoning extrapolates aircraft positions between 5s ADS-B polls using heading, speed, and vertical rate. Labels now track smoothly instead of jumping.
- **Compass bias auto-correction** — Visual-radio match residuals used to estimate and correct compass drift in real-time
- **Triple photo capture** — Each capture saves clean photo + annotated (AR overlay + info panel) + AI-aimed zoomed shot
- **One-tap smart spotter** — Tap AR label to auto-lock, track 2s, then capture all 3 photos automatically
- **Trajectory direction arrows** — Cyan arrows on AR labels showing aircraft heading
- **Confidence-based label styling** — Labels fade from solid (fresh) to yellow (coasting) to gray (stale)
- **ESP32 BLE Scanner** — Standalone BLE Remote ID scanner with OLED drone list display (ESP32 original support)
- **OLED drone list** — Clean display showing drone ID, lat/lon, altitude, speed with yellow/blue split layout

### Fixed
- **BLE Remote ID scan filter** — Android was filtering by Service UUID (AD type 0x03) instead of Service Data (AD type 0x16), preventing detection of BLE Remote ID drones
- **BLE drone GPS coordinates** — 2-byte offset in service data parsing caused wrong lat/lon
- **BLE drone duplicates** — Location messages arriving before Basic ID created MAC-based duplicate entries
- **Camera FOV swap** — Horizontal/vertical FOV defaults were swapped (45/60 instead of 60/45), causing incorrect AR label placement. All 51 tests now pass.
- **ADS-B timestamp merge bug** — All aircraft got `Instant.now()` instead of actual API timestamps, making merge logic pick random winners
- **Scanner raw AD walker** — BLE parser now always tries raw AD structure walking even when NimBLE structured parse fails
- **MapViewModel thread safety** — Location listener uses AtomicBoolean to prevent double registration

### Improved
- **Adaptive visual-radio match threshold** — Tighter when data is fresh, looser when stale/extrapolated; match candidates increased from 2 to 5
- **DJI DroneID GPS validation** — Rejects near-null-island coordinates and physically implausible altitude/speed values
- **Faster compass convergence** — Adaptive P-controller gain (5s convergence vs 20s)
- **Aircraft images** — Replaced 10 wrong photos (H60, UH60, U2, R44, E135, SF34, BE76, C5, C5M, R22)
- **Type code mappings** — Added H47 (Chinook) + 12 new codes (SF50, PC24, RQ4, MQ1, etc.)
- **Simulator orbit** — 150m radius at 15 m/s for better map tracking visibility

## [0.11.0-beta] - 2026-03-22

### Added
- Scanner OLED display improvements
- Uplink enhancements and KConfig pin configuration

## [0.10.0-beta] - 2026-03-21

### Added
- **Scanner status LED** — 6 blink patterns on Scanner board (boot, idle, scanning, detection, UART heartbeat, error) using GPIO48 (S3) / GPIO27 (C5)
- **Uplink scanner-disconnect LED** — alternating long/short blink pattern when Scanner UART link is lost (5s timeout)
- **UART connection handshake** — Uplink flashes solid 2s "connected!" when Scanner's first status message arrives
- Version sync: all ESP32 firmware and web flasher versions now match project version

## [0.9.0-beta] - 2026-03-20

### Added
- **ESP32-C5 dual-band scanner** — 2.4 + 5 GHz WiFi 6 channel hopping (38 channels interleaved), RISC-V single-core support
- **French DRI parser** — "Signalement Electronique a Distance" beacon parsing (Android + ESP32)
- **Acoustic drone detector** — microphone-based drone detection using frequency analysis (Android)
- **Dark target scorer** — visual detection scoring for non-transmitting objects (Android)
- **Trajectory classifier** — flight path analysis for drone vs bird/aircraft discrimination (Android)
- **Sensor map API service** — crowd-sourced detection map endpoint (Android)
- **Backend triangulation service** — multi-sensor position triangulation
- **ESP32 web flasher C5 variant** — flash ESP32-C5 boards from browser
- ESP32-C5 sdkconfig with WiFi 6 (802.11ac/ax), BLE 5.0, single-core FreeRTOS

### Changed
- Scanner firmware builds for both ESP32-S3 and ESP32-C5 from shared source tree
- CI builds and packages both scanner variants; C5 firmware attached to releases
- Web flasher updated with C5 flash card and wiring diagram

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

[0.10.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.10.0-beta
[0.9.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.9.0-beta
[0.8.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.8.0-beta
[0.7.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.7.0-beta
[0.6.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.6.0-beta
[0.5.1-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.1-beta
[0.5.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.0-beta
[0.4.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.4.0-beta
[0.3.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.3.0-beta
[0.2.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.2.0-beta
[0.1.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.1.0-beta
