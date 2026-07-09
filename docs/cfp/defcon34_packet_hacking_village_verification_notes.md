# DEF CON 34 PHV CFP Verification Notes

These notes are for the submitters, not part of the email body. They capture what was verified in the repo and where the CFP wording should stay careful.

## Repo-Verified Timeline

- 2026-03-12: initial Android/FastAPI project appears in commit history. `612d2c4` adds ADS-B, BLE Remote ID, and Wi-Fi detection sources.
- 2026-03-18: `c1964a9` adds the ESP32 hardware edition, web flasher, and detection parity docs.
  - `CHANGELOG.md` records BLE Remote ID + Wi-Fi promiscuous scanning ported to ESP-IDF, 104 SSID patterns, 29 OUI entries, Bayesian fusion parity, UART at 921600, and native parser/fusion tests.
- 2026-03-20: `2d7160e` adds the ESP32-C5 dual-band path, including C5 PlatformIO environment and 2.4/5 GHz interleaved scan logic.
- 2026-03-23 to 2026-03-24: troubleshooting-oriented drone support expands:
  - `7050308` fixes Android BLE drone detection, BLE scanner OLED display, and scanner raw AD parsing.
  - `CHANGELOG.md` v0.13 notes the BLE Remote ID scan filter bug: Android was filtering service UUID instead of service data, preventing BLE Remote ID detection.
  - `CHANGELOG.md` v0.15 adds ESP32 probe-request sniffing, expands to 195 drone Wi-Fi SSID patterns and 52 drone OUI entries, and fixes BLE drone GPS offset/duplicates.
  - `CHANGELOG.md` v0.16 adds the multi-drone RID simulator.
- 2026-03-28: `856fee5` records a three-board node path: ESP32-S3 BLE scanner, ESP32-C5 Wi-Fi scanner, and ESP32 OLED uplink.
- 2026-03-30: `44d144b` adds S3 combo firmware and Wi-Fi-only S3 work.
- 2026-04-08: `4c04a21` lands "All-S3 production node."
- 2026-04-18: `da53a02` lands remote UART flash with staged-handshake OTA and calls out the S3-first fleet.
- 2026-04-24: `CHANGELOG.md` states current firmware support is S3-only and release artifacts expose only `scanner-s3-combo`, `scanner-s3-combo-seed`, and `uplink-s3`.
- 2026-04-29: `1acf931` adds FoF badge board variants.
- 2026-05-14 to 2026-05-18: badge privacy, USB-C detail, BLE/theme control, scanner relay recovery, and Android badge console work mature.
- README AI/history anchors:
  - `244b34f` / `7c5f330` present the earliest public README as Claude/vibe-coded, including the first Android architecture and implementation wave.
  - `3d11a07` changes the README to multi-AI collaboration: Claude for coding, Grok for design, Codex for security review/consulting, Gemini for tech-stack research.
  - `da7326c` has committed README wording that Claude helped bootstrap the first pass while Codex had become the day-to-day partner for maintenance, firmware, review, testing, and release prep.
  - The current working tree has stronger Codex-primary docs (`README.md`, `CLAUDE.md`, and `docs/ARCHITECTURE.md`), but those specific lines are not committed yet in this checkout.

## Current Technical Facts

- Current badge hardware is a three-board Seeed XIAO ESP32-S3 badge: one uplink MCU plus BLE-primary and Wi-Fi-primary scanner MCUs.
- The scanner firmware image is shared by both scanner boards; the uplink assigns active roles/profiles at runtime.
- Current badge PlatformIO envs:
  - `uplink-s3-fof_badge`
  - `scanner-s3-combo-fof_badge`
- Current production S3 envs:
  - `uplink-s3`
  - `scanner-s3-combo`
  - `scanner-s3-combo-seed`
- Inter-board scanner/uplink UART is `UART_BAUD_RATE 921600`.
- Badge USB control paths use 115200 in `scripts/fof_badge_flash.py` and debug bridge defaults.
- Recovery tooling tries multiple flash baud rates, including 115200, 460800, and 921600.
- Current scanner `main.c` errors out for non-ESP32-S3 or split-only scanner builds: "Supported FoF scanner firmware is ESP32-S3 combo/seed only."

## Claims To Keep Careful

- C5: the repo proves C5 was explored for dual-band/5 GHz and later retired from current release artifacts. It does not prove one single root cause. Safe wording: "the C5 path was not producing the packet stream we needed reliably enough on the timeline" or "scan cadence, tooling maturity, and firmware stability made S3 the pragmatic path."
- WLED outage: repo contains WLED classification tests/data, but the outage/mapping moment is oral-history context from Will. Treat it as a field story, not a source-code claim. Best framing: Remote ID can self-report GPS when present; the hard localization problem was cheaper/non-compliant drones, controllers, smart glasses, trackers, and other BLE/Wi-Fi devices without self-reported position. The outage helped distinguish "multi-node sensing is broken" from "BLE RSSI/localization is slippery"; Wi-Fi AP-mode packets provided a more stable contrast case.
- Remote ID origin: Will adding Android Remote ID support, failing to see his own DJI drone, chasing DJI Wi-Fi evidence/SSID behavior, using Ubertooth/BLE packet checks, buying later real Remote ID hardware, and capturing other Remote ID drones are submitter-provided origin context. Repo evidence supports BLE Remote ID, Wi-Fi Beacon RID, DJI DroneID parser paths, curated drone SSID references, ESP32 Wi-Fi SSID patterns, and the `esp32/rid-simulator/` debug tool.
- UART wire stinger: no repo artifact found. Treat as a bench anecdote from Will.
- Triforce/ground-plane art: no repo text found. If used, present as Charles's hardware story, not as a repo-verified fact.
- AI workflow: committed README history supports Claude-heavy early coding, a quick multi-AI collaboration phase, and a later Codex day-to-day role. Current working-tree docs support Codex as the primary engineering orchestrator. Charles/Will having separate sessions, parallel agent loops, and bench-specific troubleshooting before consolidation are submitter-provided context, not a repo-verifiable timeline.
- ESP32-P4/RTL-SDR ADS-B: current repo search shows existing ADS-B support through Android/backend API paths and only incidental P4 references in firmware support code. Treat ESP32-P4/RTL-SDR ADS-B decoding as submitter-provided future/testing context and phrase it as "if ready by DEF CON" rather than a current supporting file.
- Speaker work backgrounds, phone numbers, handles, Will's Team Charity Case role, prior DEF CON speaking history, and bot-detection patent work came from the provided CFP brief/user context, not repo verification.

## Useful Source Pointers

- `README.md`: badge overview, S3 trio, badge-to-sensor-platform story, AI workflow, passive posture.
- `docs/badge/README.md`: version matrix, badge boundary, USB-C/AP/BLE controls, Android badge testing.
- `docs/fof_badge_notes.md`: safe USB recovery, FOF_STATUS fields, badge display policy guardrails.
- `docs/ARCHITECTURE.md`: packet pipeline, 921600 UART, backend flow, cross-stack parsing.
- `docs/THREAT_MODEL.md`: passive-only scope, confidence calibration, what is not detected.
- `docs/BAYESIAN_FUSION.md`: log-odds model, source likelihood ratios, decay.
- `docs/TRIANGULATION.md`: path-loss model, calibration, solver limits.
- `docs/examples/README.md`: redaction policy and meaning of screenshots/API samples.
- `CHANGELOG.md`: timeline and rationale for S3-only release, confidence/false-positive cleanup, badge work.
- `esp32/CHANGELOG.md`: firmware OTA/recovery, badge track, UART/firmware details.
