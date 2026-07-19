# Uplink Automatic Dual-Scanner UART Updater Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let one laptop connection flash the badge uplink and stage one verified badge-scanner image, after which the uplink automatically and safely upgrades its BLE scanner and Wi-Fi scanner in sequence whenever that embedded image is strictly newer.

**Architecture:** The laptop validates both local ESP images, flashes only the uplink over USB/UART, and uploads the scanner image once with SHA-256. The uplink atomically validates and stores that image, creates a durable generation, prompts both attached scanners, and uses one coordinator worker to evaluate, transfer, retry, and prove post-reboot convergence per slot. A shared numeric-version policy prevents equal-version rewrites, downgrades, unknown comparisons, and cross-target flashes.

**Tech Stack:** ESP-IDF C, PlatformIO/Unity native tests, NVS, ESP SHA-256 and OTA APIs, newline-delimited JSON plus binary UART frames, Python 3, unittest/pytest, FastAPI firmware catalog.

## Global Constraints

- Default physical chain is `laptop USB/UART -> badge uplink -> BLE scanner, then Wi-Fi scanner`.
- The laptop stages once and never issues normal per-slot relay commands.
- Automatic eligibility is exact embedded target match and numeric candidate version strictly greater than the scanner's numeric version.
- Equal, older candidate, newer scanner, unknown/malformed version, or unordered suffix variants are never automatically flashed.
- Same-version/direct relay remains an explicit operator recovery mode and is excluded from the default flash command.
- SHA-256 validates whole-image integrity end-to-end; CRC32 remains only for cheap transport/chunk diagnostics. SHA-256 alone is not described as publisher authentication.
- Only one scanner transfer runs at a time, in deterministic BLE-then-Wi-Fi order.
- A completed write is not success. Success requires a fresh post-reboot report of exact staged version/target, healthy running app, and cleared rollback-pending state.
- Staged-image and coordinator state survive uplink power loss safely. Interrupted work resumes; partially staged data is never offered.
- Maximum automatic attempts are three per slot per staged generation with bounded backoff and a visible final error.

---

### Task 1: Pure Numeric Firmware Update Policy

**Files:**
- Create: `esp32/shared/firmware_update_policy.h`
- Create: `esp32/shared/firmware_update_policy.c`
- Create: `esp32/test/test_firmware_update_policy.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Produces: strict parser/comparator and `firmware_update_policy_evaluate` with explicit eligible/refusal reasons.
- Consumes: candidate target/version and observed scanner target/version only.

- [ ] **Step 1: Write failing parser, ordering, and eligibility tests**

```c
void test_update_policy_only_accepts_strictly_newer_numeric_version(void)
{
    firmware_update_decision_t d = firmware_update_policy_evaluate(
        "scanner-s3-combo-fof_badge", "0.64.69",
        "scanner-s3-combo-fof_badge", "0.64.68");
    TEST_ASSERT_EQUAL(FIRMWARE_UPDATE_ELIGIBLE, d.reason);

    d = firmware_update_policy_evaluate(
        "scanner-s3-combo-fof_badge", "0.64.69",
        "scanner-s3-combo-fof_badge", "0.64.69");
    TEST_ASSERT_EQUAL(FIRMWARE_UPDATE_EQUAL, d.reason);
}
```

Cover optional leading `v`, leading zeros, malformed/missing components, overflow, whitespace, `0.64.10 > 0.64.9`, candidate older, scanner newer, target mismatch, empty/unknown scanner version, exact suffix equality, and same numeric tuple with different suffixes returning unordered/refused.

- [ ] **Step 2: Run native tests to verify RED**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
```

Expected: compile failure because the policy module does not exist.

- [ ] **Step 3: Implement bounded numeric parsing and explicit decisions**

```c
typedef enum {
    FIRMWARE_UPDATE_ELIGIBLE = 0,
    FIRMWARE_UPDATE_TARGET_MISMATCH,
    FIRMWARE_UPDATE_UNKNOWN_VERSION,
    FIRMWARE_UPDATE_UNORDERED_VERSION,
    FIRMWARE_UPDATE_EQUAL,
    FIRMWARE_UPDATE_CANDIDATE_OLDER,
} firmware_update_reason_t;
```

Require exactly three decimal components before an optional suffix. Compare numeric triples; exact full strings may be equal. If numeric triples match but suffixes do not, return unordered. Do not allocate or call locale-sensitive conversion.

- [ ] **Step 4: Run the native suite to verify GREEN**

Run Step 2. Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add esp32/shared/firmware_update_policy.c esp32/shared/firmware_update_policy.h esp32/test/test_firmware_update_policy.c esp32/test/test_runner.c esp32/platformio.ini
git commit -m "firmware: refuse downgrades and equal auto updates"
```

---

### Task 2: Target-Verified Atomic SHA-256 Staging

**Files:**
- Modify: `esp32/uplink/main/network/fw_store.h`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/network/fw_auto_check.c`
- Modify: `scripts/fof_badge_flash.py`
- Modify: `scripts/fof_badge_debug_bridge.py`
- Modify: `scripts/test_fof_badge_flash.py`
- Modify: `backend/app/services/firmware_manager.py`
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/tests/test_firmware_catalog.py`
- Modify: `backend/tests/test_firmware_auto_endpoints.py`

**Interfaces:**
- Consumes: scanner image bytes plus caller SHA-256 for early transport error reporting.
- Produces: a `READY` staged record whose size, SHA-256, embedded project, target, and embedded version have all been verified by the uplink itself.

- [ ] **Step 1: Write failing laptop and backend validation tests**

```python
def test_stage_command_uses_embedded_version_target_and_sha256(fake_image):
    request = scanner_stage_request(fake_image)
    assert request["target"] == "scanner-s3-combo-fof_badge"
    assert request["version"] == "0.64.69"
    assert request["sha256"] == hashlib.sha256(fake_image).hexdigest()

def test_custom_catalog_reports_embedded_version_not_custom(fake_image):
    manager.set_custom_firmware("scanner-s3-combo-fof_badge", fake_image)
    assert await manager.get_firmware_version("scanner-s3-combo-fof_badge") == "0.64.69"
```

Add rejection tests for wrong app magic, cross-target project, supplied/embedded version mismatch, bad SHA, truncated image, oversized image, and custom-upload name/project mismatch.

- [ ] **Step 2: Run focused tests to verify RED**

```bash
python3 -m unittest scripts.test_fof_badge_flash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_firmware_catalog.py backend/tests/test_firmware_auto_endpoints.py -v
```

Expected: staging lacks SHA/embedded identity enforcement and backend still returns the `custom` sentinel.

- [ ] **Step 3: Make laptop and backend metadata authoritative from image bytes**

Before any flash or upload, parse uplink and scanner app descriptors and require badge projects/targets. Compute the scanner SHA-256 locally. `fw_upload_begin` sends target, embedded version, byte size, CRC32, and lowercase 64-character SHA-256. The backend custom-upload endpoint performs the same descriptor/name validation and reports embedded version/SHA instead of `custom`.

- [ ] **Step 4: Implement atomic staging state transitions**

Extend `fw_store_info_t` with target/project, `sha256[65]`, and state `EMPTY|STAGING|READY|INVALID`. Before erase, commit `STAGING` and clear prior ready metadata. Stream SHA-256 while writing; after final size/CRC/SHA checks, read and validate the ESP app descriptor at offset `0x20`. Persist complete metadata first, then commit `READY` last.

On boot, `STAGING`, malformed, or incomplete records become `INVALID` and are not offered. Before the first relay after reboot, recompute partition SHA-256 once and invalidate the generation on mismatch.

- [ ] **Step 5: Run focused tests and a badge-uplink build**

```bash
python3 -m unittest scripts.test_fof_badge_flash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_firmware_catalog.py backend/tests/test_firmware_auto_endpoints.py -v
cd esp32/uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: all pass; upload rejection errors identify the failed integrity/identity field.

- [ ] **Step 6: Commit**

```bash
git add esp32/uplink/main/network/fw_store.* esp32/uplink/main/core/serial_config.c esp32/uplink/main/network/fw_auto_check.c scripts/fof_badge_flash.py scripts/fof_badge_debug_bridge.py scripts/test_fof_badge_flash.py backend/app/services/firmware_manager.py backend/app/routers/nodes.py backend/tests/test_firmware_catalog.py backend/tests/test_firmware_auto_endpoints.py
git commit -m "badge: verify scanner image before staging"
```

---

### Task 3: End-to-End SHA-256 Scanner Transfer

**Files:**
- Modify: `esp32/shared/uart_protocol.h`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/scanner/main/main.c`
- Modify: `esp32/scanner/main/comms/uart_ota.h`
- Modify: `esp32/scanner/main/comms/uart_ota.c`
- Create: `esp32/test/test_firmware_update_protocol.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: staged SHA-256 in `fw_offer`, echoed in `fw_ready`, and required in `ota_begin`.
- Produces: scanner OTA finalization only after SHA-256 over received PSRAM bytes matches the offer.

- [ ] **Step 1: Write failing protocol and digest tests**

Test valid 64-hex parsing, uppercase/short/nonhex rejection, offer/ready identity binding, a matching buffer digest, one-byte corruption failure, and stale `fw_ready` SHA rejection.

- [ ] **Step 2: Run native tests to verify RED**

Run the Task 1 native command. Expected: protocol digest contracts are absent.

- [ ] **Step 3: Bind negotiation and transfer to one staged image**

Add target, version, generation, and SHA-256 to `fw_offer`; require scanner `fw_ready` to echo them. Require the same fields in `ota_begin`. Increase fixed JSON command buffers based on a documented maximum serialized size and assert truncation checks; never silently truncate.

- [ ] **Step 4: Verify scanner SHA before touching flash**

Extend `uart_ota_begin` to accept the expected digest. At `ota_end`, hash exactly the received PSRAM bytes and compare in constant time before `esp_ota_begin`. Preserve existing size/CRC checks and abort/reset safely on any digest error.

- [ ] **Step 5: Run native tests and scanner builds**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo
```

Expected: all pass; corrupted digest never invokes the OTA write path.

- [ ] **Step 6: Commit**

```bash
git add esp32/shared/uart_protocol.h esp32/scanner/main esp32/uplink/main/network/fw_store.c esp32/test/test_firmware_update_protocol.c esp32/test/test_runner.c
git commit -m "badge: enforce scanner firmware SHA end to end"
```

---

### Task 4: Durable Serialized Two-Slot Coordinator

**Files:**
- Create: `esp32/uplink/main/network/fw_update_coordinator.h`
- Create: `esp32/uplink/main/network/fw_update_coordinator.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/uplink/main/network/fw_store.h`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/network/http_status.c`
- Modify: `esp32/scanner/main/main.c`
- Modify: `esp32/scanner/main/comms/uart_tx.c`
- Create: `esp32/test/test_fw_update_coordinator_model.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: successful staged generations, fresh per-slot `fw_check`/scanner-info/health, transfer completion, timeout, and rollback observations.
- Produces: one-worker BLE-then-Wi-Fi queue and status snapshots for USB/HTTP.

- [ ] **Step 1: Write failing pure coordinator-model tests**

Cover new generation queuing both slots, deterministic BLE-first selection, Wi-Fi remaining queued while BLE runs, busy/deferred work remaining queued, strict-policy skips, interrupted-state recovery, retry/backoff at attempts 1 through 3, terminal failure, stale-generation event rejection, and successful post-reboot convergence.

```c
void test_coordinator_serializes_both_slots(void)
{
    fw_update_model_t model = fw_update_model_new_generation(7);
    TEST_ASSERT_EQUAL(FW_SLOT_BLE, fw_update_model_next(&model));
    fw_update_model_note_transfer_started(&model, FW_SLOT_BLE);
    TEST_ASSERT_EQUAL(FW_SLOT_NONE, fw_update_model_next(&model));
    fw_update_model_note_converged(&model, FW_SLOT_BLE);
    TEST_ASSERT_EQUAL(FW_SLOT_WIFI, fw_update_model_next(&model));
}
```

- [ ] **Step 2: Run native tests to verify RED**

Run the Task 1 native command. Expected: coordinator model is absent.

- [ ] **Step 3: Implement model and one worker task**

Use states `UNKNOWN`, `CURRENT`, `QUEUED`, `RELAYING`, `WAIT_REBOOT`, `PENDING_VERIFY`, `CONVERGED`, `NEWER_SKIPPED`, `RETRY_WAIT`, and `FAILED`. A successful stage increments generation, queues BLE and Wi-Fi, and sends `fw_check_now` to both. Exactly one task owns transfer calls and always chooses BLE before Wi-Fi when both are eligible.

- [ ] **Step 4: Persist and recover coordinator state**

Persist generation, staged target/version/SHA identity, per-slot state, attempt
count, and bounded last-error token in a dedicated NVS namespace. Runtime retry
deadlines use monotonic time and are deliberately not persisted. Commit after
every durable transition. On uplink reboot, validate the staged store, wait a
fixed startup grace period, and convert `RELAYING`, `WAIT_REBOOT`,
`PENDING_VERIFY`, or `RETRY_WAIT` back to `QUEUED` within the remaining retry
budget. Never resume a different generation's event.

- [ ] **Step 5: Require post-reboot convergence rather than `ota_done`**

Treat `ota_done` only as `WAIT_REBOOT`. Advance to `PENDING_VERIFY` only from a
scanner report received after transfer completion. Ensure `fw_check` and periodic
scanner status expose target/version, uptime, recovery mode, and
`rollback_pending`, and retain their receipt timestamp in `scanner_info_t`. Mark
`CONVERGED` only after exact target/version, `rollback_pending == false`, normal
recovery mode, and post-mark-valid radio/command health are observed. Requeue a
rollback/old-version report; time out a stuck verification as
`pending_verify_timeout`.

- [ ] **Step 6: Expose compact machine-readable state**

Add `FOF_FW_STATUS` and include the same coordinator snapshot under `/api/fw/info`:

```json
{"generation":7,"staged":{"target":"scanner-s3-combo-fof_badge","version":"0.64.69"},"ble":{"state":"converged","attempts":1},"wifi":{"state":"queued","attempts":0},"complete":false}
```

Keep diagnostic/manual endpoints, but route normal trigger/ready events into the coordinator rather than spawning competing per-ready tasks.

- [ ] **Step 7: Run native tests and badge-uplink build**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: tests pass; exactly one coordinator worker is linked and no new IRAM usage overflows.

- [ ] **Step 8: Commit**

```bash
git add esp32/uplink/main/network/fw_update_coordinator.* esp32/uplink/main esp32/test/test_fw_update_coordinator_model.c esp32/test/test_runner.c
git commit -m "badge: serialize automatic scanner updates"
```

---

### Task 5: One-Stage Laptop Flow and Convergence UX

**Files:**
- Modify: `scripts/fof_badge_flash.py`
- Modify: `scripts/test_fof_badge_flash.py`
- Modify: `esp32/uplink/tools/recover_fof_badge.py`
- Modify: `docs/badge/README.md`

**Interfaces:**
- Consumes: Task 4 `FOF_FW_STATUS` snapshots.
- Produces: default `usb` flow that flashes uplink, stages scanner once, then observes automatic convergence without sending relay commands.

- [ ] **Step 1: Reverse current same-version/manual-relay tests**

```python
def test_usb_flow_stages_once_and_never_relays_slots(serial, images):
    run_usb_flow(serial, images)
    assert serial.commands.count("fw_upload_begin") == 1
    assert not any(cmd in serial.commands for cmd in ("fw_relay", "fw_trigger"))

def test_equal_version_is_reported_current_without_rewrite():
    result = evaluate_status(equal_version_status())
    assert result.terminal
    assert result.outcome == "current"
```

Add BLE-then-Wi-Fi progress, newer-scanner skip, malformed-version refusal, retry visibility, terminal failure, disconnect/resume polling, timeout, and explicit recovery-only `--allow-same-version` tests.

- [ ] **Step 2: Run script tests to verify RED**

```bash
python3 -m unittest scripts.test_fof_badge_flash
```

Expected: current USB flow explicitly relays slots and enables same-version rewriting.

- [ ] **Step 3: Make stage-once automatic coordination the default**

After uplink flash/reconnect and one successful scanner upload, poll `FOF_FW_STATUS`. Print compact state transitions for both slots and exit zero only when each is `converged`, `current`, or `newer_skipped`. Exit nonzero on invalid staged image, terminal slot failure, or timeout. A transient USB disconnect reconnects and resumes status polling for the same generation.

Retain direct/manual relay and same-version operations only behind clearly named recovery subcommands/flags with an operator warning. They must not be reachable through defaults.

- [ ] **Step 4: Update recovery tooling and operator docs**

Document one cable, expected BLE-then-Wi-Fi progression, power-cycle behavior, refusal reasons, recovery-only override, and the exact evidence to capture before calling a batch ready.

- [ ] **Step 5: Run Python, native, and clean badge builds**

```bash
python3 -m unittest scripts.test_fof_badge_flash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -t clean -e scanner-s3-combo-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -t clean -e uplink-s3-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

- [ ] **Step 6: Commit**

```bash
git add scripts/fof_badge_flash.py scripts/test_fof_badge_flash.py esp32/uplink/tools/recover_fof_badge.py docs/badge/README.md
git commit -m "badge: stage once and await both scanners"
```

---

### Task 6: Fault Injection, Hardware Gate, and Release Evidence

**Files:**
- Create: `docs/badge/automatic-uart-update-test-matrix.md`
- Modify: `scripts/test_fof_badge_flash.py`
- Modify: `esp32/test/test_fw_update_coordinator_model.c`

**Interfaces:**
- Consumes: the complete updater and two attached badge scanners when hardware is available.
- Produces: reproducible automated fault evidence plus an honestly labeled hardware result.

- [ ] **Step 1: Add automated fault regressions**

Exercise bad SHA, cross-target image, downgrade, equal version, uplink reset during staging, uplink reset between scanners, scanner reset during receive, scanner reset during inactive-partition write, dropped `ota_done`, rollback to old app, stuck pending verify, simultaneous `fw_ready`, and a newer scanner. Each test asserts no downgrade and correct resume/terminal state.

- [ ] **Step 2: Run the full software gate**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
cd android
./gradlew testDebugUnitTest assembleDebug
cd ../esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo
cd ../uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3
cd ../..
git diff --check
```

Record test counts, image sizes, RAM/flash/IRAM usage, binary SHA-256 values, and exact commit.

- [ ] **Step 3: Run the physical two-scanner test when Charles and hardware are present**

From versions one release older, connect only the laptop to the badge uplink. Run the default script. Capture that the uplink is flashed, scanner image is staged once, BLE converges first, Wi-Fi converges second, both report exact embedded version/target with rollback clear, and no manual relay command is sent.

Repeat with equal versions (zero writes), one newer scanner (no downgrade), and one power interruption. Until this is run, label the chain `software-verified; physical dual-scanner verification pending` rather than “field proven.”

- [ ] **Step 4: Commit the evidence matrix**

```bash
git add docs/badge/automatic-uart-update-test-matrix.md scripts/test_fof_badge_flash.py esp32/test/test_fw_update_coordinator_model.c
git commit -m "test: gate automatic dual-scanner updates"
```
