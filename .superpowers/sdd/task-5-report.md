# Task 5 Report: Normalized Investigation Contract and Badge Chunk Protocol

## Summary

Implemented the shared BLE investigation contract at base `cd338b5`:

- Added the exact Kotlin request, route, state, result, characteristic, and six chunk models.
- Added a badge chunk assembler with matching-request isolation, per-kind ordered indexes, forward-only progress, fixed service/characteristic/read limits, 64-byte read-value limits, and explicit truncation.
- Added the exact fixed-width C request/result/chunk types, limits, state/mode enums, and standard GATT property masks.
- Added bounded JSON encoders for the investigation request and all six chunk message types. No monolithic result encoder or GATT/transport execution was added.
- Registered the shared C protocol source additively in scanner CMake and native PlatformIO tests.

## Files

- `android/app/src/main/java/com/friendorfoe/detection/BleInvestigationModels.kt`
- `android/app/src/test/java/com/friendorfoe/detection/BleInvestigationModelsTest.kt`
- `esp32/shared/ble_investigation_types.h`
- `esp32/shared/ble_investigation_protocol.h`
- `esp32/shared/ble_investigation_protocol.c`
- `esp32/test/test_ble_investigation_protocol.c`
- `esp32/scanner/main/CMakeLists.txt`
- `esp32/platformio.ini`
- `esp32/test/test_runner.c`
- `.superpowers/sdd/task-5-report.md`

## Red Evidence

1. Android focused test:
   - Command: `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleInvestigationModelsTest`
   - Result: failed in `compileDebugUnitTestKotlin` on missing `BleInvestigationChunkAssembler`, `BleInvestigationChunk`, `BleInvestigationState`, `BleInvestigationRoute`, and `BleInvestigationRequest`.
2. ESP32 native test:
   - Command: `cd esp32 && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test`
   - First sandboxed result: blocked by `Operation not permitted: /Users/billh/.platformio/platforms.lock`.
   - Rerun with approved PlatformIO-home access: failed compiling `test_ble_investigation_protocol.c` because `ble_investigation_protocol.h` did not exist.
3. Boundary TDD discovered during self-review:
   - A 65-byte Kotlin read value failed because it was not capped at 128 hex characters.
   - Mutable characteristic properties failed because the assembler retained the caller's mutable set.

## Green Evidence

1. Android focused test:
   - Command: `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleInvestigationModelsTest`
   - Result: `BUILD SUCCESSFUL`; 7 focused tests passed.
2. ESP32 native suite:
   - Command: `cd esp32 && /Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test`
   - Result: `PASSED`; 282/282 Unity tests passed, including all four Task 5 protocol cases.
3. Focused C warning validation:
   - Command: `clang -std=c11 -Wall -Wextra -Werror -I esp32/shared -c esp32/shared/ble_investigation_protocol.c -o /tmp/ble_investigation_protocol.o`
   - Result: passed with no diagnostics.
4. Diff validation:
   - Command: `git diff --check`
   - Result: no whitespace errors.

## Self-Review

- Limits: Kotlin and C both cap services at 16, characteristics at 32, reads at 8, and readable values at 64 bytes/128 hex characters. Overflow preserves stored data and sets `truncated`.
- JSON escaping: all bounded C strings escape quotes, backslashes, and standard control characters; other non-printable/non-ASCII bytes become `?`, keeping output valid ASCII JSON with no raw newline or carriage return.
- Line sizes: encoders cap their effective destination at `UART_JSON_MAX_SIZE`; every `snprintf`/`vsnprintf` result is checked, and output is accepted only when its length is strictly less than 1024. A maximum quote/backslash expansion case is covered.
- Request mismatch/order: foreign request IDs, pre-begin chunks, skipped indexes, regressing progress states, invalid end states, and post-end chunks are rejected without advancing the active Kotlin result. C accumulation rejects mismatched request IDs and skipped in-range indexes.
- Enum/string parity: Kotlin state names are exactly `IDLE`, `QUEUED`, `SCANNING`, `CONNECTING`, `DISCOVERING`, `READING`, `COMPLETE`, `FAILED`, and `CANCELLED`; C enum order matches and wire strings are the corresponding lowercase JSON names. Modes are `GATT`/`PASSIVE_CAPTURE` and `gatt`/`passive_capture` on the wire.
- Property representation: C uses the standard bit positions `0x01` through `0x80` and serializes named JSON arrays; the tested `READ | WRITE` representation is exactly `["read","write"]`. Kotlin snapshots each `Set<String>` on acceptance.
- Scope: no GATT execution, Bluetooth callbacks, UART routing, badge transport routing, or monolithic result record was implemented.

## Concerns

- PlatformIO requires access to `/Users/billh/.platformio`; the sandboxed attempt could not acquire its lock, but the approved rerun completed the full 282-test suite.
- Gradle reports the existing warning that Android Gradle Plugin 8.2.2 was tested through compile SDK 34 while this project uses compile SDK 35. It did not affect the focused test result.
