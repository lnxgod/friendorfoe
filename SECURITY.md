# Security Policy

## Reporting a Vulnerability

Please report security issues by email to **lnxgod@gmail.com**.

- Expect an acknowledgement within **7 days**.
- We will coordinate public disclosure with you after a fix lands. Please do
  not file public GitHub issues for security-sensitive reports.
- Include reproduction steps, affected component (Android / backend / ESP32
  firmware), and the version (`FOF_VERSION` in `esp32/shared/version.h` or
  the git commit you tested against).

If you find something genuinely critical (e.g. credential exposure, remote
code execution against the backend, RF protocol parser memory corruption),
please mark the email subject `[FoF SECURITY URGENT]` so it doesn't sit in
a triage queue.

## Supported Versions

| Component | Supported |
|---|---|
| ESP32 firmware | latest tagged release + `main` |
| Backend | `main` |
| Android | latest signed release on the [Releases](https://github.com/lnxgod/friendorfoe/releases) page |

Older firmware versions retain ESP-IDF rollback safety nets but receive no
security backports. The auto-OTA system shipped in v0.63.0-svc156 lets an
operator pull the latest firmware from the backend without manual flashing —
keep your fleet current.

## Threat Model & Dual-Use Awareness

Friend or Foe parses public RF traffic and detects anomalies (evil twin,
karma, Pwnagotchi-class behaviours). The detection paths in
`backend/app/services/rf_anomaly.py`,
`backend/app/services/anomaly_detector.py`, and
`android/app/src/main/java/com/friendorfoe/detection/WifiAnomalyDetector.kt`
are **detection only** — there is no active injection, deauthentication,
packet crafting, or evil-twin construction in this codebase, and we will not
accept patches that add those.

Operators are responsible for legal use. Passive RF observation is regulated
in some jurisdictions; check local law before operating sensors at venues
you do not own. The full threat model — what we detect, what we do not, how
confidence is calibrated, and how MAC-randomization is handled — is in
[`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md).

## Production Deployment Notes

A few defaults are intentionally weak for fresh-install developer
convenience and **must** be hardened before public deployment:

- `FOF_CAL_TOKEN` (backend env). The default token is logged at boot with a
  warning and gates `/detections/calibration/*`. Set this to a long random
  string before exposing the backend beyond your LAN.
- WiFi credentials live in `esp32/uplink/main/core/wifi_credentials.h`,
  which is gitignored. Set them via the serial-config window or NVS at
  flash time; never commit a populated file.
- The example fallback backend URL in `esp32/uplink/main/core/config.h`
  (`http://192.168.1.100:8000`) is a placeholder — override via NVS
  (`backend_url`) on first boot; the firmware logs the resolved URL.
