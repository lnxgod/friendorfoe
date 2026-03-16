# Changelog

All notable changes to Friend or Foe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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

[0.5.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.0-beta
[0.4.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.4.0-beta
[0.3.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.3.0-beta
[0.2.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.2.0-beta
[0.1.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.1.0-beta
