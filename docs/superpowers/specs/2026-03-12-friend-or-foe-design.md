# Friend or Foe - Aircraft & Drone Identification App

## Overview

Android app with a live AR viewfinder that identifies aircraft and drones in real-time. Point your phone at the sky to see floating labels on aircraft (via ADS-B data) and drones (via FAA Remote ID + WiFi detection). Hybrid backend enriches data with aircraft photos, routes, and details.

## Architecture

### Android App (Kotlin + Jetpack Compose)

```
┌─────────────────────────────────────────┐
│  CameraX Preview + ARCore Hybrid Layer  │
│  (ARCore when features visible,         │
│   compass-math fallback for open sky)   │
├─────────────────────────────────────────┤
│  Sensor Fusion Engine                   │
│  • Compass/accel/gyro → device orient.  │
│  • Camera FOV calculation               │
│  • Sky-position ↔ screen-position map   │
│  • Aircraft-in-view filtering           │
│  • Label placement (overlap avoidance)  │
├─────────────────────────────────────────┤
│  Detection Sources (parallel)           │
│  1. ADS-B via backend API               │
│  2. BLE Remote ID (OpenDroneID)         │
│  3. WiFi SSID pattern matching          │
├─────────────────────────────────────────┤
│  AR Overlay (Compose Canvas)            │
│  • Floating color-coded labels          │
│  • Tap → detail card slide-up           │
│  • Status bar (counts + sensor health)  │
├─────────────────────────────────────────┤
│  Navigation: AR View | List | History   │
└─────────────────────────────────────────┘
```

### Backend Server (Python/FastAPI)

- Proxies ADS-B API calls (OpenSky Network primary, ADS-B Exchange fallback)
- Redis cache (10s TTL for aircraft positions)
- PostgreSQL for user history and aircraft metadata
- Aircraft photo enrichment (Planespotters.net or similar)
- Hosted on single VPS (DigitalOcean/Railway)

## Detection Pipeline

### 1. ADS-B (Manned Aircraft)
- Poll backend every 5-10s for aircraft within ~50nm of user GPS
- Data: ICAO hex, callsign, lat/lon, altitude, heading, speed, aircraft type
- Primary: OpenSky Network (free, 400 req/day anonymous, 4000 authenticated)
- Fallback: ADS-B Exchange (paid, includes military/blocked aircraft)

### 2. BLE Remote ID (Compliant Drones)
- Continuous BLE advertising scan on-device
- FAA mandate since March 2024 — covers newer DJI, Skydio, etc.
- Data: drone serial #, lat/lon, altitude, heading, speed, operator location
- Reference: OpenDroneID receiver-android library (ASTM F3411)

### 3. WiFi SSID Detection (Older Drones)
- Periodic WiFi scan for known patterns: DJI-*, TELLO-*, SKYDIO-*, etc.
- Signal strength → rough distance estimate
- Lower confidence — labeled "possible drone"
- Android limitation: 4 scans per 2 minutes (foreground)

### Unified Sky Object Model
All sources merge into one list:
- Position (lat/lon/alt)
- Identity (callsign, type, registration, or "unknown")
- Source + confidence (ADS-B=high, RemoteID=high, WiFi=low)
- Calculated screen position from fusion engine

## AR Viewfinder UI

### Floating Labels
Color-coded by type:
- **Green** = Commercial airline
- **Yellow** = General aviation / private
- **Red** = Military
- **Blue** = Drone (Remote ID confirmed)
- **Gray** = Unknown / WiFi-only

Label content: Callsign, aircraft type, altitude. Tap → full detail card.

### Detail Card (slide-up on tap)
- Aircraft photo
- Full identity: airline, registration, aircraft model
- Route: origin → destination
- Current: altitude, speed, heading
- Distance from user
- Detection source and confidence

### Status Bar
Shows: aircraft count, drone count, GPS status, sensor health

### Navigation
- **AR View** (default) — live viewfinder with labels
- **List View** — all detected objects sorted by distance
- **History** — past identifications (stored in Room DB)

## AR Implementation: Hybrid Approach

### ARCore Mode (primary)
- Uses visual-inertial odometry for smooth tracking
- Active when camera sees ground features (buildings, trees, horizon)
- Places labels using ARCore's world coordinate system
- Geospatial API for GPS-anchored positioning when available

### Compass-Math Fallback
- Activates when ARCore loses tracking (open sky, no visual features)
- Uses SensorManager: magnetometer + accelerometer + gyroscope
- Calculates device azimuth and elevation angle
- Maps aircraft lat/lon/alt to screen coordinates via:
  1. Convert aircraft GPS → azimuth/elevation relative to user
  2. Compare to camera pointing direction
  3. Project into camera FOV using focal length + sensor size
- Low-pass filtering on sensor data to reduce jitter

## Tech Stack

### Android
| Component | Technology |
|-----------|-----------|
| Language | Kotlin |
| UI | Jetpack Compose |
| Camera | CameraX |
| AR | ARCore + compass-math fallback |
| Sensors | SensorManager |
| BLE | Android BLE APIs + OpenDroneID ref |
| WiFi | WifiManager |
| Networking | Retrofit + OkHttp |
| DI | Hilt |
| Local DB | Room |
| Architecture | MVVM + Clean Architecture |

### Backend
| Component | Technology |
|-----------|-----------|
| Framework | Python/FastAPI |
| ADS-B | OpenSky Network + ADS-B Exchange |
| Cache | Redis (10s TTL) |
| Database | PostgreSQL |
| Enrichment | Aircraft photo/metadata APIs |
| Hosting | DigitalOcean or Railway |

## Permissions Required

- `CAMERA` — CameraX preview
- `ACCESS_FINE_LOCATION` — GPS position, BLE/WiFi scanning
- `BLUETOOTH_SCAN` — Remote ID reception
- `ACCESS_WIFI_STATE` — WiFi SSID scanning
- `INTERNET` — Backend communication

## v1 Scope

- AR viewfinder with floating aircraft labels (ARCore hybrid)
- ADS-B aircraft identification via backend (OpenSky API)
- BLE Remote ID drone scanning (on-device)
- WiFi SSID drone detection (basic pattern matching)
- Tap-to-detail card (callsign, type, altitude, route, photo)
- Status bar with sensor health indicators
- List view (detected objects sorted by distance)
- Identification history (Room DB)
- FastAPI backend (proxy, cache, enrich)

## Future (v1.5+)

- On-device ML aircraft classification (TFLite, FGVC-Aircraft dataset)
- Map view with all detected objects
- Push notifications for notable aircraft
- Snap & Identify photo mode
- Social features and leaderboards
- ADS-B Exchange for military/blocked aircraft
- Historical flight path visualization
