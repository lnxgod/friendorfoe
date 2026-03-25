🌍 **[English](README.md)** | [עברית](README.he.md) | [Українська](README.uk.md) | [العربية](README.ar.md) | [Türkçe](README.tr.md) | [Azərbaycan](README.az.md) | [Türkmen](README.tk.md) | [پښتو](README.ps.md) | [اردو](README.ur.md) | [Kurdî](README.ku.md) | [Հայերեն](README.hy.md) | [ქართული](README.ka.md) | [فارسی](README.fa.md)

# Friend or Foe — Real-Time Aircraft & Drone Identification

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)
[![ESP32 Build](https://github.com/lnxgod/friendorfoe/actions/workflows/esp32-web-flasher.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/esp32-web-flasher.yml)

**Point your phone at the sky. Know what's up there.**

Friend or Foe is an open-source, **install-and-go** Android app that identifies aircraft and drones in real time using augmented reality. It combines ADS-B transponder data, FAA Remote ID drone broadcasts, WiFi signal analysis, and on-device visual detection to overlay floating labels on the camera view — telling you what's flying overhead, who operates it, where it's going, and whether it's friend or foe. No accounts, no signups, no API keys required — just install and go.

This project was **built with AI** — not just one, but all of them. Claude wrote the code, Grok shaped the design, Codex reviewed the security, and Gemini helped pick the tech stack. Released by [GAMECHANGERSai](https://gamechangersai.org) to showcase what's possible when AI meets human creativity. See the [CHANGELOG](CHANGELOG.md) for version history.

### The 72-Hour Speed Run

> **From zero to confirmed aircraft and drone detections on a real device — in under 72 hours.**
>
> On March 12, 2025, the first commit was made that evening, 13 commits and **8,500+ lines of code** had been written in ~2 hours of AI pair-programming with Claude — producing a build-ready APK with AR viewfinder, multi-source detection, Bayesian sensor fusion, and map view. By March 14, the app was open-sourced with aircraft silhouettes, styled map markers, and polish — totaling **22,000+ lines** across Kotlin, Python, and XML.
>
> The app is **install-and-go** — install the APK, grant permissions, and start identifying aircraft. It connects directly to free public ADS-B APIs with no signup, no keys, no accounts. The optional Python backend only adds additional enrichment (airline names, route info).

---

## Two Ways to Detect

### Android App — Point and Identify

Install the APK, point at the sky — ADS-B, BLE Remote ID, WiFi, and visual ML detection all running on your phone. No accounts, no API keys.

**Download:** [GitHub Releases](https://github.com/lnxgod/friendorfoe/releases)

### ESP32 Hardware Edition — Deploy and Walk Away

Always-on, unattended drone detection. Two-board system you can build for ~$25-40:

- **Scanner** (ESP32-S3 or ESP32-C5) — BLE Remote ID + WiFi promiscuous frame capture, Bayesian fusion, JSON output over UART
- **BLE Scanner** (ESP32-S3/ESP32) — Standalone BLE detector with OLED display. Detects drones via Remote ID AND smart glasses / privacy devices (Meta Ray-Ban, Snap Spectacles, Xreal, etc.)
- **Uplink** (ESP32-C3) — GPS, OLED status display, WiFi backhaul to backend
- **ESP32-C5 variant** — dual-band WiFi 6 scans 2.4 GHz AND 5 GHz (38 channels), catches modern drones that hide on 5 GHz
- **Flash from your browser** — no toolchain needed: [**ESP32 Web Flasher**](https://lnxgod.github.io/friendorfoe/)
- Hardware setup: [INSTALL.md](esp32/INSTALL.md)

---

## What It Does

- **AR Viewfinder** — Live camera view with color-coded floating labels on detected aircraft and drones. Labels show callsign, aircraft type, altitude, and distance.
- **Multi-Source Detection** — Combines four independent detection methods for comprehensive sky awareness:
  - ADS-B transponder data (commercial flights, general aviation)
  - FAA Remote ID via Bluetooth LE (compliant drones)
  - WiFi SSID pattern matching (DJI, Skydio, Parrot, 100+ manufacturers)
  - Visual detection with ML Kit (camera-based object recognition)
- **Smart Classification** — Categorizes everything into 10 types: Commercial, General Aviation, Military, Helicopter, Government, Emergency, Cargo, Drone, Ground Vehicle, and Unknown. Military detection uses callsign patterns, squawk codes, and operator databases.
- **Bayesian Sensor Fusion** — When multiple sensors detect the same object, confidence scores are combined using Bayesian log-odds — not just "pick the highest." Two weak signals agreeing can outweigh one strong signal.
- **Aircraft Silhouettes & Photos** — 120+ ICAO type codes mapped to 10 vector silhouette categories (narrowbody, widebody, regional, turboprop, bizjet, helicopter, fighter, cargo, lightplane, drone) plus 134 bundled aircraft photos for instant visual recognition — no network needed.
- **2D Map View** — OpenStreetMap with distinct marker shapes per category, distance rings, compass-follow mode, and FOV cone overlay.
- **Detail Cards** — Tap any object for full details: registration, operator, route (origin/destination), altitude, speed, heading, squawk code, detection source, and confidence level.
- **History Log** — Persistent database of everything you've identified, searchable and sortable.
- **Drone Reference Guide** — Built-in database of 30+ drone types with photos, specs, and descriptions — from DJI consumer drones to military UCAVs like the Bayraktar TB2, MQ-9 Reaper, and Shahed-136. Browse by category, search by name, or jump straight from an unknown drone detection to identify what you're seeing.
- **Backend Enrichment (Optional)** — An optional Python API server can add airline names, registration numbers, and route information beyond what's already bundled. The app works fully without it — aircraft photos and drone references are included in the APK.

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
