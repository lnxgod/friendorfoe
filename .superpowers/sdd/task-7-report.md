# Task 7 Report: Badge Scanner Passive Capture and NimBLE GATT Investigation

## Summary

Implemented Task 7 from base `d67fa66099b7da063796d930bf75d064982b740b`:

- Added a pure event-driven BLE investigation core with a hard 12,000 ms deadline, busy rejection, bounded evidence, one-shot results, and explicit scan-resume intent on every terminal state.
- Added passive capture that leaves discovery running and counts qualifying Apple, Fast Pair, and Swift Pair prompt-family advertisements.
- Added a fixed-storage NimBLE client adapter for connect, primary-service discovery, characteristic discovery, and sequential reads only.
- Added strict UART request parsing and mutual exclusion with calibration and OTA.
- Added guarded scan cancel/resume without stopping or deinitializing NimBLE.
- Added canonical display-MAC to NimBLE-byte-order conversion and a recent advertiser address-type cache.

## Files

- `esp32/scanner/main/detection/ble_investigator.h`
- `esp32/scanner/main/detection/ble_investigator.c`
- `esp32/test/test_ble_investigator_state.c`
- `esp32/scanner/main/CMakeLists.txt`
- `esp32/platformio.ini`
- `esp32/scanner/main/detection/ble_remote_id.h`
- `esp32/scanner/main/detection/ble_remote_id.c`
- `esp32/scanner/main/main.c`
- `esp32/test/test_runner.c`
- `.superpowers/sdd/task-7-report.md`

## Red Evidence

1. Required seven state-core tests:
   - Command: `/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test`
   - First sandboxed run: blocked by `Operation not permitted: '/Users/billh/.platformio/platforms.lock'`.
   - Approved cache-access rerun: failed compiling `test_ble_investigator_state.c` with `fatal error: 'ble_investigator.h' file not found`.
2. Review-discovered MAC byte-order regression:
   - Command: `/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test`
   - Result before implementation: failed because `ble_investigator_parse_target_mac` was undeclared.
   - The regression requires `AA:BB:CC:DD:EE:FF` to become NimBLE `addr.val` bytes `FF EE DD CC BB AA` and rejects malformed or overlong addresses.

## Green Evidence

1. Pure core before NimBLE integration:
   - Full native suite passed 295/295 after the seven required tests and portable core were implemented.
2. Final full native suite:
   - Command: `/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test`
   - Result: `PASSED`; 296/296 Unity tests passed, including the seven required state cases and the MAC-order regression.
3. Badge scanner build:
   - Command from `esp32/scanner`: `/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge`
   - Result: `SUCCESS` in 2.83 seconds; GATT client symbols linked.
   - Reported usage: 47.8% RAM and 55.7% flash.
4. Diff validation:
   - Command: `git diff --check`
   - Result: no whitespace errors.

## State And Cleanup Audit

- `ble_investigator_start` rejects an active or unconsumed request without mutation.
- Deadlines default to and are capped at 12,000 ms.
- GATT success ends `COMPLETE`; connect/procedure/disconnect errors, authentication requirements, and timeout end `FAILED`; explicit cancel ends `CANCELLED`.
- Every core terminal transition goes through `finish`, which sets `resume_scan_required`, clears `busy`, and makes exactly one result pending.
- Firmware terminal handling goes through `runtime_cleanup`: emit one bounded End record, cancel pending initiation or terminate any valid connection handle, request scan restoration, consume the result, then reopen the runtime for another request.
- A cleanup-in-progress gate prevents a new request from overwriting terminal evidence.
- A connect event arriving at the deadline is still terminated because cleanup keys off the valid runtime connection handle, not only the core's connected flag.
- Scan restoration remains pending if NimBLE is still busy after asynchronous cancel/terminate. The connection failure/disconnect callback retries restoration, and existing profile maintenance can also retry after the GATT guard is cleared.
- `ble_remote_id_start`, internal scan start, and Meta reacquire all suppress restart while GATT is active.
- Passive capture never calls the scan-pause hook; terminal cleanup still explicitly calls the common resume hook, which is a no-op while discovery is already active.

## Command Parser Audit

Before any scan cancel or connection attempt, `main.c` validates:

- non-empty printable `request_id` shorter than `BLE_INV_REQUEST_ID_LEN`;
- exact `gatt` or `passive_capture` mode;
- integer timeout from 1 through 12,000 ms, defaulting to 12,000;
- exact six-octet colon-delimited GATT target MAC;
- no target field for passive capture;
- calibration inactive;
- OTA and badge firmware quiet window inactive;
- no active investigation.

`ble_investigate_cancel` requires a valid request ID matching the active request. Calibration start and OTA begin are rejected while an investigation is active.

## NimBLE API Safety Scan

Allowed investigation operations found:

- `ble_gap_disc_cancel`
- `ble_gap_connect`
- `ble_gattc_disc_all_svcs`
- `ble_gattc_disc_all_chrs`
- `ble_gattc_read`
- cleanup-only `ble_gap_conn_cancel` and `ble_gap_terminate`

Forbidden API search covered GATT writes, MTU exchange, subscription, security initiation, SM calls, passkey/encryption events, pairing/credential actions, and NimBLE stop/deinit. It returned no matches in `ble_investigator.c` or `ble_investigator.h`.

The adapter does not write, subscribe, pair, initiate security, exchange credentials, stop the host, or deinitialize NimBLE. ATT insufficient authentication, authorization, key-size, and encryption statuses map to `authentication_required` evidence.

## Bounds And Read Policy

- Services: 16.
- Characteristics: 32.
- Reads: 8.
- Value: 64 bytes encoded into 128 hexadecimal characters plus terminator.
- Read queue: readable characteristics only, limited to GAP (`1800`), Device Information (`180A`), `FFE1`, and `FFF1`.
- Runtime core, service handles, read queue, recent-peer cache, and completed result are fixed static storage; no investigation result is placed on firmware task stacks.
- Begin, Progress, Service, Characteristic, Read, and End records use the Task 5 bounded serializer and `uart_tx_send_raw_json`.

## Review Fixes

A read-only focused review found and this task fixed:

- reversed human/NimBLE MAC byte order and resulting address-type cache misses;
- a connection-complete-at-deadline race that could leave a live link open;
- scan restoration attempted before asynchronous cancellation or termination completed;
- a cleanup/new-request race that could overwrite terminal evidence.

## Concerns And Hardware Gap

- No badge was flashed, as required.
- No live peripheral connection, ATT authentication failure, cancellation race, or post-disconnect scan restoration was exercised on hardware. Native tests prove the pure state core and MAC conversion; the ESP-IDF build proves API and link compatibility.
- GATT uses the address type from a bounded recent-advertiser cache. If a target has aged out or was never observed by this scanner, the adapter falls back to a public address type; a random-address target may fail to connect, then emits a bounded failure and restores scanning.
- PlatformIO requires access to the existing `/Users/billh/.platformio` cache. The sandboxed RED attempt captured the lock denial; approved cache-access runs completed both final commands.
