# Badge USB OTA Lifecycle Follow-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the remaining startup-token deadlock and OTA cleanup race while preserving committed OTA state and binary-session ownership.

**Architecture:** Put the runtime UART startup claim/start/release lifecycle in a small host-testable policy with an exact result enum and same-token bounded release retries. Keep OTA mutation serialized, publish status through the existing bounded snapshot, and make cleanup distinguish retryable release failures from the one terminal cleanup receipt.

**Tech Stack:** ESP-IDF C, PlatformIO native Unity tests with AddressSanitizer, Python pytest source contracts, PlatformIO ESP32-S3 firmware builds.

## Global Constraints

- Strict RED-first for every production behavior.
- One implementation agent; no subagent delegation.
- Do not wire the USB command route.
- Do not flash hardware, push, tag, or change firmware versions.
- Do not touch `.camera-before-zoom.jpg`.
- Produce one concise focused commit only after all requested gates pass.

---

### Task 1: Exact Runtime UART Startup Lifecycle

**Files:**
- Create: `esp32/uplink/main/core/uart_startup_gate.h`
- Create: `esp32/uplink/main/core/uart_startup_gate.c`
- Modify: `esp32/platformio.ini`
- Create: `esp32/test/test_uart_startup_gate.c`
- Modify: `esp32/test/test_runner.c`
- Modify: `esp32/uplink/main/main.c`
- Test: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Produces: `uart_startup_gate_result_t uart_startup_gate_run(const uart_startup_gate_hooks_t *, uint32_t, uint32_t, uint32_t, uint32_t)`.
- Result values: `UART_STARTUP_GATE_OK`, `UART_STARTUP_GATE_CLAIM_TIMEOUT`, `UART_STARTUP_GATE_START_FAILED`, and `UART_STARTUP_GATE_RELEASE_FAILED`.

- [ ] **Step 1: Write failing native and source-contract tests**

  Add callbacks that prove `start` reached task-entry success, force every release attempt to fail, and assert every attempt receives the exact claimed token. Add a source contract requiring three 10 ms release attempts and a release-failure branch ordered as safe-mode, recovery-only, one-shot USB recovery, bounded USB drain, and direct `badge_usb_recovery_restart(BADGE_USB_RESET_APP, "uart_start_token_release")`, with no idle-reservation helper.

- [ ] **Step 2: Run RED**

  Run `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test -f test_uart_startup_gate` and the focused pytest contract. Expect missing lifecycle API/result and direct recovery branch failures.

- [ ] **Step 3: Implement the minimal lifecycle**

  Claim with the existing bounded startup loop, call the real `uart_rx_start`, then call `fw_store_operation_end` with the same token up to three times with 10 ms waits between attempts. Return release failure ahead of start failure because the unreleased global token is the dominant recovery condition. Route only release failure through the direct recovery primitive; retain idle-reserved automatic restart for claim/start failure after successful release.

- [ ] **Step 4: Run GREEN**

  Re-run the focused native and pytest tests and require all pass.

### Task 2: Ordered Cleanup and Exact Release Retry Receipt

**Files:**
- Modify: `esp32/uplink/main/core/uplink_usb_ota.c`
- Modify: `esp32/uplink/main/core/uplink_usb_ota.h`
- Modify: `esp32/test/test_uplink_usb_ota.c`
- Modify: `esp32/test/test_runner.c`
- Test: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Produces: retryable nonterminal result metadata and one terminal `UPLINK_USB_OTA_PHASE_ABORTED` cleanup receipt after exact token release succeeds.

- [ ] **Step 1: Write failing adversarial cleanup tests**

  Make the fake operation hook use the real `fw_operation_state_t`. During `operation_end`, try a scanner-staging claim before release and prove it fails, assert scanner 1, scanner 0, and HTTP already resumed in order, release the exact token, then prove a competing claim can succeed. Force one release failure and assert workers resume once, repeated abort retries only release, success emits one required terminal receipt, and later replay emits none.

- [ ] **Step 2: Run RED**

  Run the focused native OTA tests. Expect failures because current cleanup releases before resumes and loses the terminal transition.

- [ ] **Step 3: Implement ordered idempotent cleanup**

  Abort integrity and OTA once, resume owned scanner 1, scanner 0, and HTTP once, then call exact `operation_end` last. On failure retain the token and explicit release-pending state. On a later successful retry, clear ownership, set cleanup complete, and emit exactly one required terminal receipt.

- [ ] **Step 4: Run GREEN**

  Re-run focused native OTA tests and source contracts.

### Task 3: Nonterminal Contention and Unknown Remaining Sentinel

**Files:**
- Modify: `esp32/uplink/main/core/uplink_usb_ota.c`
- Modify: `esp32/uplink/main/core/uplink_usb_ota.h`
- Modify: `esp32/test/test_uplink_usb_ota.c`
- Test: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Produces: `retryable=true` with `UPLINK_USB_OTA_PHASE_NONE` for adapter contention.
- Produces: `UPLINK_USB_OTA_REMAINING_UNKNOWN` equal to `UINT32_MAX` when the bounded status snapshot is unavailable.

- [ ] **Step 1: Write failing contention and status-busy tests**

  Begin a real receiving session, force mutator contention, assert the result is retryable and nonterminal and the published session remains receiving, then release contention and prove the next write succeeds. Force status writer busy while receiving and assert remaining returns the named unknown sentinel rather than zero.

- [ ] **Step 2: Run RED**

  Run focused native and source-contract tests. Expect terminal ABORTED and zero-remaining mismatches.

- [ ] **Step 3: Implement minimal result and sentinel changes**

  Add `retryable` to the result, make busy responses use phase NONE, and return the named unknown sentinel only when the three-attempt status read fails.

- [ ] **Step 4: Run GREEN and committed-state regression**

  Re-run all OTA native tests, including post-setboot committed/reboot-required/no-cleanup assertions.

### Task 4: Full Verification and Focused Commit

**Files:**
- Verify all modified files only.

- [ ] **Step 1: Run complete gates**

  Run the ASan native suite, focused and full backend pytest suites, clean badge and non-badge builds, and strict badge manifest verification.

- [ ] **Step 2: Audit scope**

  Run `git diff --check`, inspect `git diff --name-status` and `git diff --stat`, and verify `.camera-before-zoom.jpg` remains untracked and untouched.

- [ ] **Step 3: Commit**

  Stage only the focused plan, tests, lifecycle module, adapter, header, build filter, runner, contracts, and `main.c`; run `git diff --cached --check`; then commit with a concise badge USB OTA lifecycle subject.
