# Badge USB Uplink OTA Task5C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route the isolated uplink OTA adapter through a dedicated, credit-fenced USB command and binary path in normal and startup recovery-only modes.

**Architecture:** A host-testable uplink coordinator produces typed actions and bounded result frames. The existing stream gains uplink-only two-phase peek/commit and nonmutating timeout primitives, while scanner feed behavior is left untouched. `serial_config` performs strict cJSON extraction and both transport modes invoke one dedicated begin handler.

**Tech Stack:** ESP-IDF C, cJSON, PlatformIO native Unity/ASan, Python source-contract tests.

## Global Constraints

- Do not implement Task5D HTTP/non-USB mutation lockdown or status.
- Do not change laptop/Android code, flash hardware, push, tag, or bump versions.
- Do not touch `.camera-before-zoom.jpg`.
- Preserve the scanner legacy upload path and uncredited scanner begin behavior.
- Keep startup firmware-operation token ordering unchanged.

---

### Task 1: Transactional stream and coordinator policy

**Files:**
- Create: `esp32/shared/badge_usb_uplink_ota.h`
- Create: `esp32/shared/badge_usb_uplink_ota.c`
- Create: `esp32/test/test_badge_usb_uplink_ota.c`
- Modify: `esp32/shared/badge_usb_stream.h`
- Modify: `esp32/shared/badge_usb_stream.c`
- Modify: `esp32/test/test_badge_usb_stream.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Produces a `badge_usb_uplink_action_t` state machine with `CONTINUE`,
  `RETRY_PENDING`, `WAIT_RECEIPT`, `FINISH`, `ABORT_DROP`, and
  `COMMITTED_RESTART`.
- Produces bounded manifest validation, result rendering, cleanup/terminal
  latches, and injected committed emit/drain/restart execution.
- Produces `badge_usb_stream_peek_binary`, `badge_usb_stream_commit_binary`,
  `badge_usb_stream_binary_timed_out`, and `badge_usb_stream_clear_binary`.

- [x] Write Unity tests for 4095+1, 5000, exact multiples, extra-byte/drop,
  complete-plus-line, busy pending, receipt failure, timeout ordering, terminal
  latches, bounded frames, and committed restart despite output failures.
- [x] Run `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test` and
  confirm failures are caused by missing interfaces.
- [x] Implement only the pure policy and stream APIs needed by those tests.
- [x] Re-run the native suite and require all cases to pass.

### Task 2: Dedicated command parsing and mode routing

**Files:**
- Modify: `esp32/uplink/main/core/serial_config.h`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/core/badge_usb_transport.h`
- Modify: `esp32/uplink/main/core/badge_usb_transport.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Produces exact recognition/classification for `uplink_ota_begin`.
- Produces a bounded parser that requires string identity/version/SHA/flow
  fields, exact uint32 size/CRC, and a boolean recovery rewrite field.
- Both normal and recovery paths call `badge_usb_transport_handle_uplink_ota_begin`.

- [x] Add focused source contracts proving the dedicated command, unchanged
  `ota` alias, strict manifest extraction, shared handler, and no recovery full
  dispatcher.
- [x] Run the focused pytest and confirm RED.
- [x] Implement the classifier/parser and shared transport begin handler.
- [x] Re-run focused pytest and native manifest-validation tests to GREEN.

### Task 3: Transactional binary integration and terminal lifecycle

**Files:**
- Modify: `esp32/uplink/main/core/badge_usb_transport.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Consumes the Task 1 coordinator/actions and Task 2 dedicated handler.
- Uses adapter begin/write/finish/abort directly and emits only
  `FOF_UPLINK_OTA:` frames for the dedicated route.

- [x] Add source contracts for READY-before-parser, <=512 writes, cumulative
  transport counts, credit receipt/drain fencing, pending retry/no-read,
  nonmutating timeout cleanup order, one terminal, and no double abort.
- [x] Run the focused contract and confirm RED.
- [x] Implement the dedicated binary action loop while leaving scanner code on
  the existing `badge_usb_stream_feed` and scanner backend calls.
- [x] Re-run focused contracts and the full native suite to GREEN.

### Task 4: Irreversible commit and rollback-return recovery

**Files:**
- Modify: `esp32/uplink/main/core/badge_runtime.h`
- Modify: `esp32/uplink/main/core/badge_runtime.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Produces `badge_runtime_clear_expected_reboot()` which clears RTC magic,
  persisted expected reason, and the in-memory reason.
- Consumes the injected committed runner and calls the nonreturning app restart
  after attempted emit and drain.

- [x] Add native committed-runner tests and source contracts for emit failure,
  drain failure, unconditional restart, no post-commit cleanup, and rollback
  API unexpected return.
- [x] Run native/focused tests and confirm RED.
- [x] Implement the minimal restart seam and rollback clear/failure response.
- [x] Re-run native/focused tests to GREEN.

### Task 5: Release gates and focused commit

**Files:**
- Verify all Task5C files only.

- [x] Run native ASan and require every case to pass.
- [x] Run focused and full backend pytest suites.
- [x] Clean-build `uplink-s3-fof_badge` and `uplink-s3`.
- [x] Run strict badge manifest verification and record SHA-256.
- [x] Run `git diff --check`, inspect exact scope, and verify the camera file is
  the only unrelated untracked item.
- [ ] Commit the focused Task5C slice locally without push/tag/flash/version.
