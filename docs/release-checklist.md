# GitHub Release Checklist

Use this before publishing or tagging a release branch.

## Repository Hygiene

- Run `git status --short` and separate intentional source changes from local runtime files.
- Keep `.env`, `local.properties`, database files, generated firmware credentials, build outputs, logs, and OS metadata out of commits.
- Do not commit generated ESP32 Wi-Fi headers such as `esp32/uplink/main/core/wifi_credentials.h`.
- Keep `backend/app/data/rf_reference.json` and `backend/app/data/rf_overrides.yaml` under review; other `backend/app/data` files are runtime calibration or local reference artifacts.

## Security Defaults

- Set `FOF_CAL_TOKEN` for shared or production deployments. If it is unset, the backend intentionally falls back to the Android dev token and logs a warning.
- Start backend deployments from `backend/.env.example`, then keep filled `.env` files local.
- Verify OpenSky or other external service credentials are configured through environment variables only.

## Verification

- Backend: `cd backend && pytest tests -v`
- Android: `cd android && ./gradlew clean assembleDebug`
- ESP32 scanner build: `cd esp32/scanner && pio run -e scanner-s3-combo`
- ESP32 native tests: `cd esp32 && pio test -e test`
- For dashboard changes, load the backend locally and exercise the Map, Probes, Mobile, and calibration screens.
