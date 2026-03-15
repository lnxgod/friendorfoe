# Changelog

All notable changes to Friend or Foe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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

[0.3.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.3.0-beta
[0.2.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.2.0-beta
[0.1.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.1.0-beta
