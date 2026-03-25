🌍 **[English](README.md)** | [עברית](README.he.md) | [Українська](README.uk.md) | [العربية](README.ar.md) | [Türkçe](README.tr.md) | [Azərbaycan](README.az.md) | [Türkmen](README.tk.md) | [پښتو](README.ps.md) | [اردو](README.ur.md) | [Kurdî](README.ku.md) | [Հայերեն](README.hy.md) | [ქართული](README.ka.md) | [فارسی](README.fa.md)

# Friend or Foe — Privacy Awareness & Airspace Detection

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)
[![ESP32 Build](https://github.com/lnxgod/friendorfoe/actions/workflows/esp32-web-flasher.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/esp32-web-flasher.yml)

**Know what's watching you. Know what's flying above you.**

Friend or Foe is an open-source **privacy awareness** and **airspace detection** platform for Android and ESP32. It passively scans Bluetooth and WiFi signals around you to detect surveillance devices, tracking beacons, hidden cameras, smart glasses, and drones — then identifies every aircraft overhead using augmented reality. No accounts, no signups, no API keys. Install and go.

### What It Detects

| Category | Examples | Method |
|----------|----------|--------|
| **Smart Glasses** | Meta Ray-Ban, Snap Spectacles, Xreal, Vuzix, Google Glass, Bose Frames | BLE manufacturer ID + service UUID |
| **BLE Trackers** | Apple AirTag, Samsung SmartTag, Tile, Google Find My | BLE service UUID + manufacturer data |
| **Hidden Cameras** | WiFi spy cams, IP cameras, nanny cams, charger cams | WiFi SSID patterns + BLE names |
| **Body Cameras** | Axon, Motorola VISTA | BLE + WiFi signatures |
| **Attack Tools** | Flipper Zero, WiFi Pineapple | BLE name + WiFi SSID |
| **Vehicle Cameras** | Tesla Sentry Mode | BLE advertisement |
| **Retail Trackers** | iBeacon, Eddystone | BLE beacon signatures |
| **Action Cameras** | GoPro, Insta360, DJI Action | WiFi SSID patterns |
| **Dash Cameras** | BlackVue, Viofo, 70mai, Nextbase, Thinkware, DDPai | WiFi SSID patterns |
| **Endoscope Cameras** | DEPSTECH, Jetion (peeping tools) | WiFi SSID + BLE |
| **Evil Twin / Rogue APs** | WiFi Pineapple, karma attacks, SSID spoofing | WiFi anomaly analysis |
| **Ultrasonic Beacons** | SilverPush, Lisnr, Shopkick tracking tones | Microphone FFT (18-22 kHz) |
| **Hidden Electronics** | Cameras behind walls, concealed recorders | Magnetometer EMF sweep |
| **Night Vision Cameras** | IR LED arrays from hidden cameras | Front camera IR detection |
| **Drones** | DJI, Skydio, Parrot, Autel + 190 SSID patterns | BLE Remote ID + WiFi + visual ML |
| **Aircraft** | Commercial, military, GA, helicopter, cargo, emergency | ADS-B transponder + AR overlay |

This project was **built with AI** — Claude, Codex, Gemini, and Grok working together. Released by [GAMECHANGERSai](https://gamechangersai.org). See the [CHANGELOG](CHANGELOG.md) for version history.

---

## Two Ways to Detect

### Android App — Point and Identify

Install the APK, grant permissions — privacy scanning and aircraft detection start immediately. BLE scans for smart glasses, trackers, and hidden cameras nearby. WiFi scans for spy camera SSIDs. ADS-B identifies aircraft overhead with AR labels. No accounts, no API keys.

**Download:** [GitHub Releases](https://github.com/lnxgod/friendorfoe/releases)

### ESP32 Hardware Edition — Deploy and Walk Away

Always-on, unattended detection. Build for ~$25-40:

- **Scanner** (ESP32-S3 or ESP32-C5) — BLE Remote ID + WiFi promiscuous frame capture, Bayesian fusion
- **BLE Scanner** (ESP32-S3/ESP32) — Standalone BLE detector with OLED display. Detects drones AND smart glasses / privacy devices with on-screen alerts
- **Uplink** (ESP32-C3) — GPS, OLED status display, WiFi backhaul to backend
- **ESP32-C5 variant** — dual-band WiFi 6 scans 2.4 + 5 GHz (38 channels)
- **Flash from your browser** — no toolchain needed: [**ESP32 Web Flasher**](https://lnxgod.github.io/friendorfoe/)
- Hardware setup: [INSTALL.md](esp32/INSTALL.md)

---

## Privacy Detection

Friend or Foe passively monitors BLE advertisements and WiFi networks around you, matching against a database of **60+ known surveillance device signatures**:

### BLE Detection (Android + ESP32)
- **13 smart glasses brands** — Meta Ray-Ban, Snap Spectacles, Google Glass, Vuzix, Bose Frames, Amazon Echo Frames, Xreal, Brilliant Labs, TCL RayNeo, Rokid, INMO, Even Realities, Solos
- **7 tracker/stalkerware signatures** — Apple AirTag (FindMy type 0x12), Samsung SmartTag (UUID 0xFD5A), Tile (UUID 0xFEED), DULT protocol (UUID 0xFCB2), Google Find My Device (UUID 0xFE2C)
- **3 body camera brands** — Axon, Motorola VISTA
- **7 hidden camera BLE name patterns** — V380, IPC, CLOUDCAM, HIDVCAM, HDWiFiCam, LookCam, Camera
- **Vehicle cameras** — Tesla Sentry Mode (BLE name)
- **Attack tools** — Flipper Zero (BLE name)
- **Retail tracking** — iBeacon (Apple type 0x02), Eddystone (UUID 0xFEAA)

### WiFi Detection (Android)
- **9 hidden camera SSID patterns** — MV*, YDXJ_*, IPC-*, IP_CAM_*, HDWiFiCam*, CLOUDCAM*, HIDVCAM*, GW_AP*, JXLCAM*
- **3 action camera SSIDs** — GoPro*, Insta360*, OsmoAction*
- **7 dash camera SSIDs** — BlackVue*, DR900*, DR750*, VIOFO_*, 70mai_*, Nextbase*, Thinkware*
- **2 body camera WiFi patterns** — Axon Body*, WGVISTA*
- **Attack tools** — Pineapple*
- **Doorbell cameras** — Ring Setup*

Privacy detection is **on by default** and can be toggled in Settings (About screen).

---

## Airspace Detection

- **AR Viewfinder** — Live camera view with color-coded floating labels on aircraft and drones. Labels show callsign, type, altitude, and distance.
- **Multi-Source Detection** — Four independent methods:
  - ADS-B transponder data (commercial flights, general aviation)
  - FAA Remote ID via Bluetooth LE (compliant drones)
  - WiFi SSID pattern matching (190+ drone manufacturer patterns)
  - Visual detection with ML Kit (camera-based object recognition)
- **Smart Classification** — 10 categories: Commercial, GA, Military, Helicopter, Government, Emergency, Cargo, Drone, Ground Vehicle, Unknown
- **Bayesian Sensor Fusion** — Log-odds evidence combination across all sensors
- **Aircraft Database** — 193 type codes, 135 airlines, 62 countries, 134 bundled photos, 10 vector silhouette categories
- **2D Map View** — OpenStreetMap with category markers, distance rings, dark mode support
- **Detail Cards** — Full aircraft/drone details with external lookup links
- **History Log** — Persistent detection database, searchable and filterable
- **Drone Reference Guide** — 30+ drone types from consumer to military UCAVs

---

## How It Works

### Detection Sources

| Source | What It Detects | Range | Confidence |
|--------|----------------|-------|-----------|
| **ADS-B** | Commercial & GA aircraft with transponders | ~250 NM | Very High (0.95) |
| **Remote ID (BLE)** | FAA-compliant drones (250g+) | ~300m | High (0.85) |
| **WiFi SSID** | DJI, Skydio, Parrot, budget drones | ~100m | Moderate (0.3-0.85) |
| **Visual (ML Kit)** | Anything visible in camera | Line of sight | Variable |

### Sensor Fusion & AR

The app fuses accelerometer, magnetometer, and gyroscope data to determine exactly where the phone is pointing. Aircraft positions (lat/lon/altitude) are projected onto the camera view using haversine distance calculations and camera FOV geometry.

**ARCore + Compass Hybrid**: ARCore provides excellent tracking when the camera sees ground features, but struggles when pointed at featureless sky — exactly when you need it most. The app automatically falls back to compass-math orientation when ARCore loses tracking, providing seamless labels in all conditions.

### Architecture

```
┌──────────────────────────────────────────────────┐
│  Android App (Kotlin + Jetpack Compose)          │
│                                                  │
│  ┌─────────┐ ┌─────────┐ ┌──────────┐          │
│  │ AR View │ │   Map   │ │   List   │  ...UI   │
│  └────┬────┘ └────┬────┘ └────┬─────┘          │
│       │           │           │                  │
│  ┌────┴───────────┴───────────┴─────┐           │
│  │         ViewModels (MVVM)        │           │
│  └────┬─────────────────────┬───────┘           │
│       │                     │                    │
│  ┌────┴────┐          ┌────┴──────┐             │
│  │ Sensor  │          │ Detection │             │
│  │ Fusion  │          │  Sources  │             │
│  │ Engine  │          │           │             │
│  │         │          │ • ADS-B   │             │
│  │ • ARCore│          │ • BLE RID │             │
│  │ • Accel │          │ • WiFi    │             │
│  │ • Gyro  │          │ • Visual  │             │
│  │ • Mag   │          │           │             │
│  └─────────┘          └─────┬─────┘             │
│                             │                    │
│                    ┌────────┴────────┐           │
│                    │ Bayesian Fusion │           │
│                    │   Engine        │           │
│                    └────────┬────────┘           │
│                             │                    │
└─────────────────────────────┼────────────────────┘
                              │ HTTP
                    ┌─────────┴─────────┐
                    │  Backend (FastAPI) │
                    │  • Aircraft fetch  │
                    │  • Enrichment      │
                    │  • Caching (Redis) │
                    └─────────┬─────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
         adsb.fi     airplanes.live     OpenSky
        (primary)     (fallback)      (fallback)

┌──────────────────────────────────────────────────┐
│  ESP32 Hardware Edition                          │
│                                                  │
│  ┌──────────────────┐    UART     ┌───────────┐ │
│  │  Scanner (S3/C5) │───921600───→│  Uplink   │ │
│  │  • BLE Remote ID │   baud      │  (C3)     │ │
│  │  • WiFi Promisc  │            │  • GPS    │ │
│  │  • Bayesian      │            │  • OLED   │ │
│  │    Fusion        │            │  • WiFi   │ │
│  └──────────────────┘            └─────┬─────┘ │
│                                        │        │
└────────────────────────────────────────┼────────┘
                                         │ HTTP POST
                               ┌─────────┴─────────┐
                               │  Backend (FastAPI) │
                               └───────────────────┘
```

---

## Getting Started

### Prerequisites

- **Android Studio** Hedgehog (2023.1.1) or later
- **JDK 17**
- **Android SDK 35** (compileSdk) with minSdk 26 (Android 8.0+)
- An Android device with GPS, compass, and camera (emulator works for list/map views but not AR)

> **No accounts or signups needed.** The app connects directly to free public ADS-B APIs — no backend setup, no API keys, no accounts.

### Quick Start — Android Only

The app connects directly to free public ADS-B APIs — no backend setup or accounts needed. Aircraft photos and the drone reference guide are bundled in the APK. To just build and run:

```bash
cd android
./gradlew assembleDebug
# Install on connected device:
adb install app/build/outputs/apk/debug/app-debug.apk
```

Or download the latest pre-built APK from [**GitHub Releases**](https://github.com/lnxgod/friendorfoe/releases).

### Backend Setup (Optional — enables enrichment)

The backend adds aircraft photos, airline names, registration lookups, and route information. **Requires Python 3.11+ and optionally Redis for caching.**

```bash
cd backend

# Create virtual environment
python -m venv .venv
source .venv/bin/activate  # or .venv\Scripts\activate on Windows

# Install dependencies
pip install -r requirements.txt

# Copy environment config
cp .env.example .env
# Edit .env if you want to add OpenSky credentials (optional, increases rate limits)

# Run the server
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

**With Docker** (includes Redis + PostgreSQL):

```bash
cd backend
docker compose up
```

The API will be available at `http://localhost:8000`. Health check: `http://localhost:8000/health`

### Connecting the App to Your Backend

Update the backend URL in the Android app's network configuration to point to your server's IP address.

---

## Tech Stack

### Android App

| Component | Library | Version |
|-----------|---------|---------|
| Language | Kotlin | 1.9.22 |
| UI | Jetpack Compose + Material 3 | 2024.02.00 |
| AR | ARCore | 1.41.0 |
| Camera | CameraX | 1.3.1 |
| ML | ML Kit Object Detection | 17.0.1 |
| DI | Hilt | 2.50 |
| HTTP | Retrofit + OkHttp | 2.9.0 / 4.12.0 |
| Database | Room | 2.6.1 |
| Images | Coil | 2.5.0 |
| Maps | OSMDroid | 6.1.18 |
| Async | Coroutines + Flow | 1.7.3 |

### Backend

| Component | Library | Version |
|-----------|---------|---------|
| Framework | FastAPI | 0.115.6 |
| Server | Uvicorn | 0.34.0 |
| HTTP Client | httpx | 0.28.1 |
| Cache | Redis | 5.2.1 |
| Database | SQLAlchemy + asyncpg | 2.0.36 |
| Validation | Pydantic | 2.10.4 |

---

## Project Structure

```
friendorfoe/
├── android/                           # Android app
│   └── app/src/main/java/com/friendorfoe/
│       ├── data/
│       │   ├── local/                 # Room database (history, tracking)
│       │   ├── remote/                # Retrofit API services & DTOs
│       │   └── repository/            # Data repositories
│       ├── detection/                 # Detection engines
│       │   ├── AdsbPoller.kt          # ADS-B data polling
│       │   ├── RemoteIdScanner.kt     # BLE Remote ID scanning
│       │   ├── WifiDroneScanner.kt    # WiFi SSID detection
│       │   ├── BayesianFusionEngine.kt # Multi-sensor confidence fusion
│       │   ├── MilitaryClassifier.kt  # Military aircraft identification
│       │   └── VisualDetection*.kt    # ML Kit visual detection
│       ├── domain/model/              # Domain models (Aircraft, Drone, SkyObject)
│       ├── di/                        # Hilt dependency injection
│       ├── presentation/
│       │   ├── ar/                    # AR viewfinder screen
│       │   ├── map/                   # OpenStreetMap view
│       │   ├── list/                  # Sortable object list
│       │   ├── detail/                # Full object detail cards
│       │   ├── drones/                # Drone reference guide browser
│       │   ├── history/               # Detection history
│       │   ├── welcome/               # Welcome/launch screen
│       │   ├── about/                 # App info
│       │   └── util/                  # AircraftPhotos, CategoryColors, DroneDatabase
│       └── sensor/                    # Sensor fusion & positioning
├── backend/                           # Python FastAPI server
│   └── app/
│       ├── main.py                    # FastAPI entry point
│       ├── config.py                  # Environment configuration
│       ├── routers/aircraft.py        # API endpoints
│       └── services/
│           ├── adsb.py                # Multi-source ADS-B fetching
│           └── enrichment.py          # Aircraft data enrichment
├── esp32/                             # ESP32 hardware edition
│   ├── scanner/                       # Scanner firmware (S3 + C5)
│   ├── uplink/                        # Uplink firmware (C3)
│   ├── shared/                        # Shared UART protocol, types
│   └── web-flasher/                   # Browser-based firmware flasher
├── images/                            # Aircraft reference photos (Wikimedia CC)
├── scripts/                           # Utility scripts (photo downloader)
├── macos/                             # macOS companion (early stage)
└── docs/                              # Design documents
```

---

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/health` | Health check (Redis, DB status) |
| `GET` | `/aircraft/nearby?lat=&lon=&radius_nm=` | Aircraft near a position |
| `GET` | `/aircraft/{icao_hex}/detail` | Enriched aircraft details |

---

## Contributing

Contributions are welcome! Whether it's bug fixes, new detection methods, UI improvements, or documentation — we'd love your help.

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Ideas for Contributors

- Additional aircraft type code mappings
- Improved WiFi drone manufacturer fingerprints
- Night mode / dark theme for the AR overlay
- Flight path prediction and trajectory visualization
- Social features (share sightings, community reports)
- iOS port using the macOS Swift foundation
- Integration with additional ADS-B data sources

---

## Built With AI

This project wasn't built with just one AI — it was built with **all of them**. We tested every major AI platform and used each one where it was strongest. The git history tells the story:

| Date | What Happened |
|------|--------------|
| **March 12** |  13 commits had landed — **8,500+ lines of code** written in ~2 hours of AI pair-programming. The result: a build-ready APK with AR viewfinder, four detection sources, Bayesian sensor fusion, map view, and history tracking. |
| **March 14** | Open-source release. Added 120+ aircraft silhouettes, styled map markers with category shapes, permission handling polish, and security review. Later: bundled 134 aircraft photos as offline assets, added welcome screen with update checker, and Coil image loading. |
| **Total** | **23,000+ lines** of Kotlin, Python, and XML. Confirmed real-world detections of commercial aircraft and drones on device. |

### AI Roles

| AI | Role |
|----|------|
| **[Claude](https://claude.ai)** (Anthropic) | Primary coding agent — architecture, implementation, pair-programming |
| **[Grok](https://grok.com)** (xAI) | Design direction and research |
| **[Codex](https://chatgpt.com)** (OpenAI) | Security review and consulting |
| **[Gemini](https://gemini.google.com)** (Google) | Tech stack research — evaluating libraries, frameworks, and approaches |
| **[ML Kit](https://developers.google.com/ml-kit)** (Google) | On-device visual object detection (runs directly on the phone) |

**What is vibe coding?** It's a collaborative, conversational approach to software development where a human and AI build together in real time. Instead of writing every line by hand, you describe what you want, iterate on ideas, debug together, and let the AI handle the boilerplate while you focus on the vision and architecture. It's programming by vibes — and it works.

Friend or Foe was built with multiple AIs, each contributing where they excel. Claude served as the primary coding partner — from initial architecture through sensor fusion algorithms, Bayesian math, and vector drawable artwork. Grok helped shape the design direction. Codex performed the security audit before open-sourcing. Gemini researched which technologies and frameworks to use. And ML Kit runs on-device, powering visual detection without any cloud dependency. Every file in this repo was shaped by human-AI collaboration.

**We believe the future of software development is using the right AI for the right job**, and we're open-sourcing this project so you can see what that looks like in practice.

---

## About GAMECHANGERSai

**[GAMECHANGERSai](https://gamechangersai.org)** is a 501(c)(3) nonprofit dedicated to creating hands-on, AI-powered learning experiences for kids and families.

> *"Playful AI games that teach every learner to build and remix."*

Through our platform, learners develop real, transferable skills:

- **Creative Coding** — Building functional programs with AI as a co-pilot
- **Systems Thinking** — Understanding how complex pieces interact and how to debug them
- **Resource Management** — Strategic planning and optimization
- **Ethical Reasoning** — Learning to ask "should we?" alongside "can we?" when using AI

**Friend or Foe is proof that these skills are real.** The same creative coding, systems thinking, and AI collaboration that kids learn on our platform were used to build this fully functional aircraft identification system. What starts as a game becomes a tool. What starts as play becomes expertise.

We're committed to giving back to the open-source community. By releasing Friend or Foe publicly, we hope to:

- **Inspire** developers and learners to explore what's possible with AI-assisted development
- **Demonstrate** that AI is a creative partner, not a replacement — every decision in this app was guided by human judgment
- **Show** that the skills our kids are learning aren't just for games — they're the foundation for building real, useful technology

Visit us at **[gamechangersai.org](https://gamechangersai.org)** to learn more about our mission.

---

## Data Sources & Attribution

Friend or Foe is 100% free to use — every data source below is open and requires **no API keys or paid accounts**.

### ADS-B Aircraft Position Data

The app uses a three-tier fallback chain so you always get aircraft data even if one source is down:

| Priority | Source | API Endpoint | What It Provides | Auth |
|----------|--------|-------------|------------------|------|
| 1st | **[adsb.fi](https://adsb.fi)** | `opendata.adsb.fi/api` | Real-time aircraft positions, callsigns, altitude, speed, heading, squawk codes | Free, no key |
| 2nd | **[airplanes.live](https://airplanes.live)** | `api.airplanes.live` | Same data as adsb.fi (compatible ADSBx v2 format) | Free, no key |
| 3rd | **[OpenSky Network](https://opensky-network.org)** | `opensky-network.org/api` | ICAO state vectors — position, velocity, heading, vertical rate | Free, no key (optional account for higher rate limits) |

All three sources provide live ADS-B transponder data from worldwide receiver networks. The app queries by GPS coordinates and radius, and automatically falls through to the next source on failure or timeout.

### Enrichment & Supporting Data

| Source | What It Provides | Auth |
|--------|------------------|------|
| **[Planespotters.net](https://planespotters.net)** | Aircraft photos by ICAO hex code — shown on detail cards | Free, no key |
| **[Open-Meteo](https://open-meteo.com)** | Current weather (cloud cover, wind speed, conditions) — used to adjust detection parameters | Free, no key |
| **[OpenStreetMap](https://openstreetmap.org)** | Map tiles via OSMDroid for the 2D map view | Free, no key |

### On-Device Detection (No External APIs)

These detection methods run entirely on the phone with no network calls:

- **FAA Remote ID (Bluetooth LE)** — Scans for compliant drone broadcasts within ~300m
- **WiFi SSID Matching** — Identifies DJI, Skydio, Parrot, and 100+ other drone manufacturers by WiFi patterns
- **Visual Detection (ML Kit)** — Camera-based object recognition running on-device
- **Military Classification** — Callsign patterns, squawk codes, and operator database (all bundled locally)

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

You are free to use, modify, and distribute this software for any purpose. We just ask that you keep the copyright notice and give credit where it's due.

---

*Made with curiosity, code, and every AI we could get our hands on.*
*Released with love by [GAMECHANGERSai](https://gamechangersai.org).*
