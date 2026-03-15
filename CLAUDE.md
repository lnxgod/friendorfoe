# Friend or Foe

## Project Overview
Android AR app for real-time aircraft & drone identification using ADS-B, FAA Remote ID, and WiFi detection. Point your phone at the sky — floating labels identify what's up there.

## Architecture
- `android/` — Kotlin + Jetpack Compose, Clean Architecture (data/domain/presentation/sensor/detection)
- `backend/` — Python FastAPI, multi-source ADS-B aggregation + aircraft enrichment
- `macos/` — Swift companion app (early stage)

## Build
- Android: `cd android && ./gradlew assembleDebug`
- Backend: `cd backend && pip install -r requirements.txt && uvicorn app.main:app`
- Backend (Docker): `cd backend && docker compose up`

## Key Patterns
- Hilt dependency injection, Room database, Retrofit HTTP, CameraX, ARCore
- Bayesian sensor fusion for multi-source confidence scoring
- Repository pattern with fallback chains (adsb.fi → airplanes.live → OpenSky)
- Flow-based reactive streams throughout
- ARCore + compass-math hybrid (ARCore for feature-rich scenes, compass fallback for open sky)

## Conventions
- Domain models in `domain/model/`, ViewModels use StateFlow
- Category colors defined in `presentation/util/CategoryColors.kt`
- All ADS-B sources abstracted behind `AircraftRepository`
- Detection sources in `detection/` package, each emitting Flow<List<SkyObject>>
- Vector drawable silhouettes in `res/drawable/ic_silhouette_*.xml`, mapped via `AircraftSilhouettes.kt`

## Testing
- Android: `cd android && ./gradlew test`
- Backend: `cd backend && pytest`
