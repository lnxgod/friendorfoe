# BLE Threat Investigation Verification

Verification snapshot: 2026-07-10 for implementation commit `bcecea1` and
release candidate `0.64.67-ble-investigation` on
`codex/ble-threat-investigation`.

## Result

The Android and FoF badge software paths pass their complete automated test and
build matrix. Source inspection confirms that target-device investigation is
read-only. Live phone, USB-C badge, bonded badge BLE, physical button/display,
and protected-characteristic checks were not run because no physical Android
device or FoF badge was attached.

This feature does not require the backend for local Android or standalone badge
operation, and this branch makes no backend code changes.

## Verification Matrix

| Surface | Command | Result |
| --- | --- | --- |
| Android focused protocol/routing | `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.data.badge.BadgeInvestigationProtocolTest --tests com.friendorfoe.presentation.privacy.BleInvestigationRoutingTest` | PASS, 33 tests |
| Android full JVM suite | `cd android && ./gradlew testDebugUnitTest --rerun-tasks` | PASS, 40 suites / 308 tests / 0 skipped / 0 failures / 0 errors |
| Android debug APK | `cd android && ./gradlew assembleDebug --rerun-tasks` | PASS |
| Android APK metadata | `/Users/billh/Library/Android/sdk/cmdline-tools/latest/bin/apkanalyzer manifest version-name app/build/outputs/apk/debug/app-debug.apk`; same command with `version-code` | PASS, `0.64.67-ble-investigation` / `109` |
| Backend full suite | `cd backend && /Users/billh/gai/friendorfoe/backend/.venv/bin/pytest tests -q` | PASS, 272 tests |
| ESP32 native suite | `cd esp32 && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test` | PASS, 369 tests |
| Production scanner firmware | `cd esp32/scanner && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo -e scanner-s3-combo-seed` | PASS, 2 targets |
| Badge scanner firmware | `cd esp32/scanner && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge` | PASS, RAM 47.9%, flash 56.1% |
| Production uplink firmware | `cd esp32/uplink && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3` | PASS, RAM 20.1%, flash 56.3% |
| Badge uplink firmware | `cd esp32/uplink && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge` | PASS, RAM 44.0%, flash 69.5% |
| Branch whitespace | `git diff --check eec77a6..HEAD` | PASS, no output |
| Target read-only scan | `rg -n "createBond|ble_gattc_write|setCharacteristicNotification|requestMtu|writeCharacteristic|writeDescriptor" android/app/src/main/java/com/friendorfoe/detection esp32/scanner/main/detection esp32/uplink/main/core` | PASS, no matches |

The Android build reports the repository's existing AGP 8.2.2 versus
`compileSdk = 35` warning, Room schema-export warning, and existing deprecation
warnings. It reports no feature-specific compiler error or test failure.

## Detector Parity

Android `BleThreatAnalyzerTest` and native
`test_ble_threat_detector.c` exercise the same defaults: 8-second prompt
window, 12 unique MACs, 24 observations, 75% churn, 20 dB RSSI span, 12 dB
RSSI IQR, 60-second cooldown, 20-second clear, three serial observations over
five seconds, -70 dBm close threshold, and a minimum of two supporting serial
signals.

| Behavior | Android evidence | ESP32 evidence | Result |
| --- | --- | --- | --- |
| Swift Pair rotating flood | `twelve rotating Swift Pair addresses in eight seconds emit one flood` | `test_ble_threat_swift_pair_rotating_flood_alerts_once` | PARITY |
| Apple/Fast Pair isolated traffic | `isolated Fast Pair and generic Apple traffic do not alert` | prompt-family qualification plus mixed-family tests | PARITY |
| Mixed Apple/Fast/Swift flood | `mixed Apple Fast Pair and Swift Pair burst alerts once` | `test_ble_threat_mixed_prompt_families_alert` | PARITY |
| Varied crowd negative | `varied crowd does not alert when RSSI spread exceeds twenty dB` | `test_ble_threat_varied_crowd_does_not_alert` | PARITY |
| Cooldown and quiet clear | `sustained flood respects sixty second cooldown`; exact 20-second clear tests | `test_ble_threat_cooldown_and_clear` | PARITY |
| Persistent FFE0 candidate | `persistent close sparse FFE0 device emits possible serial skimmer` | `test_ble_threat_persistent_sparse_ffe0_alerts` | PARITY |
| FFE0 alone negative | `FFE0 alone never alerts` | `test_ble_threat_ffe0_only_does_not_alert` | PARITY |
| Exact combined evidence gate | `persistent sparse FFE0 candidate with exactly two supporting signals alerts` | `test_ble_threat_exact_two_supporting_signals_alert` | PARITY |
| Multi-service negative | `multi service device suppresses sparse profile evidence` | `test_ble_threat_multi_service_profile_does_not_alert` | PARITY |
| Trusted identity negative | `trusted product suppresses serial heuristic` | `test_ble_threat_trusted_product_suppresses_serial_candidate` | PARITY |
| FFF0/PKOC negative | `PKOC identity suppresses FFF0 heuristic` | `test_ble_threat_pkoc_fff0_is_suppressed` | PARITY |

Badge policy tests also prove that the UART evidence survives transport,
pairing-spam uses a stable rotating-MAC entity, serial UUID alone remains
hidden, combined serial evidence becomes a `SKIM` row, and both new classes are
eligible for the badge display lanes.

## Investigation Evidence

- Android coordinator tests cover busy rejection, timeout, cancellation,
  terminal-state ownership, authentication-required evidence, fresh-target
  validation, allowlisted readable characteristics, disconnect races, and
  bounded traversal.
- Android badge protocol tests cover all `FOF_INV` chunk types, request and
  generation correlation, ordering, truncation, UTF-8 byte limits, strict UTF-8
  rejection, 12-second total timeout, USB acknowledgement, HTTP terminal
  polling, BLE callback fencing, disconnect/replay, and partial evidence.
- Route tests cover Android-to-phone, Android-to-badge fallback, badge USB-C,
  bonded and encrypted badge BLE, fresh phone fallback, badge-only passive
  capture, stale/invalid targets, unavailable explicit routes, and rapid busy
  replacement.
- Native scanner/uplink tests cover bounded begin/service/characteristic/read/
  end frames, scanner-slot routing, request correlation, timeout/replay,
  encrypted FF03 chunk access, read-only investigator phase fences, scan resume
  on every terminal path, button-2 action selection, copied targets, and bounded
  LCD progress/result pages.

## Built Artifacts

| Artifact | SHA-256 |
| --- | --- |
| `android/app/build/outputs/apk/debug/app-debug.apk` | `90c88da58dbb5d1937dbae132188450156c252136a6823f4bfb2f2e1537f6867` |
| `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin` | `41286239cdf9a73aac2b3a9344aba3ce995bd37277449e122578822bc06e8944` |
| `esp32/uplink/.pio/build/uplink-s3-fof_badge/firmware.bin` | `bbe7f66d6eaa1dee103fdfc6111e3ef9b6ebdd38d07f6b2fa2e97b6b133cc61c` |

These local artifacts are build evidence only. Release artifacts are produced
and signed by the tag workflows.

## Hardware Status

`adb devices -l` returned no attached Android target. macOS
`system_profiler SPUSBHostDataType` showed only the two built-in USB buses and
no FoF badge.

| Physical check | Status |
| --- | --- |
| Direct phone investigation discovers services and exits without pairing | NOT RUN: hardware unavailable |
| USB-C badge returns matching `FOF_INV` begin/end frames | NOT RUN: hardware unavailable |
| Bonded, encrypted badge BLE returns compact status and chunks | NOT RUN: hardware unavailable |
| Button-2 hold investigates the selected alert, displays pages, and resumes scanning | NOT RUN: hardware unavailable |
| Protected readable characteristic reports authentication required without opening a pairing dialog | NOT RUN: hardware unavailable |

An Android 15 emulator smoke check previously installed the feature APK,
launched `MainActivity`, opened the Privacy screen, and produced no crash-buffer
entry. It had no BLE target or badge feed, so it does not replace any physical
check above.

## Remaining Limitations

- Actual BLE controller, OEM Android stack, and bonded badge behavior need the
  five physical checks above.
- Cancelling Android retrieval does not cancel an already accepted scanner
  operation. The UI and result state explicitly report that the badge may still
  be running.
- Investigation is intentionally identification-only: service discovery,
  metadata, and allowlisted readable characteristics. It does not pair, write,
  subscribe, negotiate MTU, or attempt credentials against the target.
