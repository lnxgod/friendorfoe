# Badge Two-Button Reset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the physical nine-second quiet/off chord with a safe ten-second software-reset chord.

**Architecture:** Reuse the existing shared chord state machine, changing its emitted event from a power toggle to a reset at a 10,000 ms threshold. The uplink button task will consume the chord, arm the existing expected-software-reboot marker, and call ESP-IDF `esp_restart()` directly. USB quiet-mode behavior remains untouched.

**Tech Stack:** ESP-IDF C, PlatformIO native Unity tests, pytest source-contract tests.

## Global Constraints

- Both physical buttons held continuously for exactly 10,000 ms trigger reset.
- No physical button chord may toggle quiet/off mode.
- A reset is one-shot until full button release and cannot loop from boot-held buttons.
- Keep USB quiet-mode controls, scanner behavior, OTA/UART updating, themes, versions, and factory bundles unchanged.

---

### Task 1: Specify Reset-Chord State-Machine Behavior

**Files:**
- Modify: `esp32/test/test_badge_power_chord.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `badge_power_chord_init(...)` and `badge_power_chord_update(...)`
- Produces: native tests expecting `BADGE_POWER_CHORD_RESET` at the exact 10-second boundary

- [x] **Step 1: Change the native tests to require reset at 10 seconds**

Update the helper threshold from `9000U` to `10000U`, replace toggle event
expectations with `BADGE_POWER_CHORD_RESET`, and update timing values and test
names to cover the exact boundary, early release, one-shot/full-release,
one-button, boot-held, consumed-press, and uint32 wrap cases.

- [x] **Step 2: Run the focused native test suite and verify RED**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: compilation fails because `BADGE_POWER_CHORD_RESET` is not defined,
proving the tests require the new event.

### Task 2: Implement the Reset Event and Uplink Reboot

**Files:**
- Modify: `esp32/shared/badge_power_chord.h`
- Modify: `esp32/uplink/main/hw/display_st7735.c`

**Interfaces:**
- Consumes: `badge_runtime_arm_expected_reboot(const char *reason)` and ESP-IDF `esp_restart(void)`
- Produces: `BADGE_POWER_CHORD_RESET` and a ten-second physical reset path

- [x] **Step 1: Replace the toggle event with the reset event**

Rename `BADGE_POWER_CHORD_TOGGLE` to `BADGE_POWER_CHORD_RESET`. Keep the state
machine's existing release, boot-held, input-consumption, and wrap-safe behavior.

- [x] **Step 2: Wire the uplink button task to controlled reboot**

Set the chord threshold to `10000`, consume both button releases, cancel the
pending button-two gesture, call
`badge_runtime_arm_expected_reboot("button_chord")`, then call `esp_restart()`.
Remove the chord call to `badge_power_runtime_toggle(...)`.

- [x] **Step 3: Run the native suite and verify GREEN**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: all native tests pass.

### Task 3: Update the Contract and User Documentation

**Files:**
- Modify: `backend/tests/test_badge_quiet_mode_contract.py`
- Modify: `docs/badge/README.md`

**Interfaces:**
- Consumes: badge display source text
- Produces: regression contract that rejects restoration of the physical quiet toggle

- [x] **Step 1: Change the backend source contract**

Require `BADGE_RESET_CHORD_HOLD_MS 10000`, `BADGE_POWER_CHORD_RESET`,
`badge_runtime_arm_expected_reboot("button_chord")`, and `esp_restart()` in the
display runtime. Assert the physical chord no longer calls
`badge_power_runtime_toggle("button_chord")`.

- [x] **Step 2: Update badge documentation**

Document the ten-second two-button software reset and remove the nine-second
quiet/off shortcut statement.

- [x] **Step 3: Run the focused backend contract test**

Run: `cd backend && .venv/bin/pytest tests/test_badge_quiet_mode_contract.py -v`

Expected: all tests in the file pass.

### Task 4: Verify Firmware Compatibility

**Files:**
- No additional source changes expected.

**Interfaces:**
- Consumes: completed reset implementation
- Produces: build artifacts proving scanner and uplink compatibility

- [x] **Step 1: Run the complete native ESP32 suite**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: zero failed tests.

- [x] **Step 2: Build scanner badge firmware**

Run: `cd esp32/scanner && /Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge`

Expected: build exits zero.

- [x] **Step 3: Build uplink badge firmware**

Run: `cd esp32/uplink && /Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge`

Expected: build exits zero and reports memory usage.

- [x] **Step 4: Review the final diff and confirm scope**

Run: `git diff --check && git status --short && git diff --stat`

Expected: no whitespace errors; only the reset chord, its tests, and its docs
are modified. Do not flash, bump versions, update factory bundles, or push in
this task.
