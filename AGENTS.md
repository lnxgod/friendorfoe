# Repository Guidelines

## Project Structure & Module Organization
`android/` contains the Kotlin + Jetpack Compose app. Main code lives in `android/app/src/main/java/com/friendorfoe`, bundled assets in `android/app/src/main/assets/{aircraft,drones}`, and JVM unit tests in `android/app/src/test/java`. `backend/` is a FastAPI service: `app/main.py` boots the API, `app/routers/` holds endpoints, `app/services/` contains domain logic, and `backend/tests/` covers API and math helpers. `esp32/` holds PlatformIO/ESP-IDF firmware for `scanner/`, `ble-scanner/`, and `uplink/`, plus shared C code in `shared/`, native tests in `test/`, and the flasher in `web-flasher/`. `scripts/` contains utilities; `docs/` contains setup docs.

## Build, Test, and Development Commands
- `cd android && ./gradlew clean assembleDebug` builds the Android debug APK.
- `cd backend && python -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt` sets up the API locally.
- `cd backend && uvicorn app.main:app --host 0.0.0.0 --port 8000` runs the backend on `localhost:8000`.
- `cd backend && pytest tests -v` runs backend tests.
- `cd backend && docker compose up` starts the API with Redis and PostgreSQL.
- `cd esp32/scanner && pio run -e scanner-s3-combo` builds one scanner firmware target.
- `cd esp32 && pio test -e test` runs native ESP32 unit tests with Unity.

## Coding Style & Naming Conventions
Follow the surrounding style; no repo-wide formatter config is checked in. Use 4-space indentation. Kotlin types use `PascalCase`, packages stay lowercase by layer, and UI/state files follow patterns like `*Screen.kt` and `*ViewModel.kt`. Python modules and functions use `snake_case`; keep async endpoints type-annotated where practical. ESP32 C code uses `snake_case` for functions, `ALL_CAPS` for macros, and short block comments for hardware-specific logic.

## Testing Guidelines
Keep tests close to the module they validate. Android unit tests should mirror package structure and end in `*Test.kt`. Backend tests use `pytest` with `pytest.mark.asyncio`; prefer `httpx.AsyncClient`/`ASGITransport` for API coverage. ESP32 native tests live under `esp32/test/`. Add regression coverage for new detectors, parsers, and boundary-condition math.

## Commit & Pull Request Guidelines
Recent history favors versioned, concise subjects such as `v0.63.6: inter-node calibration` or `v0.63.5-token-heal: self-heal stale token`. Match that style when landing release-facing work: version or scope first, then a short imperative summary. PRs should call out affected surfaces (`android`, `backend`, `esp32`), list verification commands run, include screenshots for Compose/dashboard changes, and note any hardware, credentials, or API assumptions.

## Security & Configuration Tips
Start from `backend/.env.example`; do not commit filled `.env` files, OpenSky credentials, Android signing material, or generated ESP32 Wi-Fi headers such as `esp32/uplink/main/core/wifi_credentials.h`.
