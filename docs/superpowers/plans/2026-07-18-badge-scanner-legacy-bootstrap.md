# Badge Scanner 0.64.68 Legacy Bootstrap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bootstrap the exact `0.64.68-badge-live-follow` scanners through the durable automatic updater without weakening strict `.69` manifest receipts.

**Architecture:** A pure authorization helper validates the legacy frame against the cached scanner identity and committed staged manifest. The uplink parser accepts legacy syntax only when all strict-only fields are absent, and the durable coordinator queues it only from exact `offered` state. The relay core uses session-bound legacy acknowledgements, complete progress, and done receipts, then retains the existing staged SHA, CRC, ESP image validation, same-MAC, rollback, and radio convergence gates.

**Tech Stack:** ESP-IDF C, cJSON, NVS coordinator state, UART OTA, PlatformIO/Unity native tests, pytest source-contract tests, Python hardware flasher.

## Global Constraints

- Automatically accept legacy readiness only from exact source version `0.64.68-badge-live-follow`.
- Never treat a partial or malformed strict receipt as legacy.
- Require exact cached target, project, hardware, immutable MAC, board, and version before queueing.
- Require exact committed generation, requested slot, BLE-first gate, and durable `offered` state.
- Preserve strict newer-only ordering, no downgrade, no equal rewrite, staged raw SHA-256 revalidation, bounded retries, and fail-closed NVS behavior.
- Legacy receipt compatibility ends after this one bootstrap; `.69` scanners use strict receipts.
- Hardware success requires BLE-primary convergence before Wi-Fi-primary convergence and exact post-reboot MAC continuity.

---

### Task 1: Pure Legacy Authorization Policy

**Files:**
- Create: `esp32/shared/firmware_legacy_ready.h`
- Create: `esp32/shared/firmware_legacy_ready.c`
- Create: `esp32/test/test_firmware_legacy_ready.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `fof_legacy_ready_view_t`, `fof_legacy_identity_view_t`, and `fof_legacy_manifest_view_t` string/number views.
- Produces: `bool fof_firmware_legacy_ready_authorized(...)` and constant `FOF_LEGACY_READY_BOOTSTRAP_VERSION`.

- [ ] **Step 1: Write the failing authorization tests**

```c
void test_legacy_ready_authorizes_only_exact_06468_identity_and_manifest(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();
    TEST_ASSERT_TRUE(fof_firmware_legacy_ready_authorized(
        &ready, &identity, &manifest));

    ready.strict_fields_absent = false;
    TEST_ASSERT_FALSE(fof_firmware_legacy_ready_authorized(
        &ready, &identity, &manifest));
}
```

Add one-negative-at-a-time cases for wrong legacy source version, missing or
invalid MAC, target/project/hardware mismatch, board mismatch, current-version
mismatch, target-version mismatch, size mismatch, CRC mismatch, empty SHA in the
manifest, candidate not strictly newer, and null views.

- [ ] **Step 2: Run the focused test and observe RED**

Run:

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
```

Expected: compile failure because `firmware_legacy_ready.h` does not exist.

- [ ] **Step 3: Implement the pure policy**

```c
#define FOF_LEGACY_READY_BOOTSTRAP_VERSION \
    "0.64.68-badge-live-follow"

bool fof_firmware_legacy_ready_authorized(
    const fof_legacy_ready_view_t *ready,
    const fof_legacy_identity_view_t *identity,
    const fof_legacy_manifest_view_t *manifest)
{
    return ready && identity && manifest &&
        ready->strict_fields_absent && identity->received &&
        fof_firmware_hardware_id_is_canonical(identity->hardware_id) &&
        strcmp(ready->current_version,
               FOF_LEGACY_READY_BOOTSTRAP_VERSION) == 0 &&
        strcmp(identity->version, ready->current_version) == 0 &&
        strcmp(ready->board, manifest->target) == 0 &&
        strcmp(identity->board, manifest->target) == 0 &&
        strcmp(identity->firmware_name, manifest->target) == 0 &&
        strcmp(identity->project, manifest->project) == 0 &&
        strcmp(identity->hardware, manifest->hardware) == 0 &&
        strcmp(ready->target_version, manifest->version) == 0 &&
        ready->size == manifest->size && ready->crc32 == manifest->crc32 &&
        fof_firmware_sha256_hex_is_valid(manifest->sha256) &&
        fof_firmware_version_compare(manifest->version,
                                     ready->current_version) ==
            FOF_VERSION_NEWER;
}
```

Implement `fof_firmware_hardware_id_is_canonical` locally in the module: exactly
17 lowercase or uppercase hex/colon characters in `xx:xx:xx:xx:xx:xx` form.

- [ ] **Step 4: Run native tests and verify GREEN**

Run the Step 2 command. Expected: all native cases pass.

---

### Task 2: Parser and Durable Coordinator Bridge

**Files:**
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/uplink/main/network/fw_store.h`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Consumes: Task 1 authorization helper and the current `scanner_info_t` cache.
- Produces: `bool fw_store_handle_legacy_scanner_ready(int, const char *, const char *, const char *, uint32_t, uint32_t)`.

- [ ] **Step 1: Write failing integration contracts**

Add source-contract assertions that the UART handler:

```python
assert "strict_receipt_fields_absent" in ready_handler
assert "fw_store_handle_legacy_scanner_ready" in ready_handler
assert "malformed_fw_ready" in ready_handler
```

Add coordinator assertions that legacy queueing requires
`FW_AUTO_SLOT_OFFERED`, exact generation, requested mask, BLE-first gate, and a
successful durable save before worker start.

- [ ] **Step 2: Run focused contracts and observe RED**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_firmware_transport_contract.py -q
```

Expected: the legacy handler and offered-state gate are absent.

- [ ] **Step 3: Parse legacy only when strict extensions are absent**

```c
bool strict_receipt_fields_absent =
    cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_FW_NAME) == NULL &&
    cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_FW_PROJECT) == NULL &&
    cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_FW_HARDWARE) == NULL &&
    cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_FW_SHA256) == NULL &&
    cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_FW_GENERATION) == NULL &&
    cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_FW_ALLOW_SAME) == NULL;
```

Use exact bounded-string and uint32 parsing for the common legacy fields. A
strict frame with any missing or malformed extension remains
`malformed_fw_ready`; it never falls through.

- [ ] **Step 4: Bind authorization to exact durable offered state**

Construct Task 1 views from the committed `fw_store_info_t` and the slot's
cached `scanner_info_t`. After pure authorization succeeds, use an internal
`enqueue_auto_relay_locked(..., require_offered=true)` path. Recheck generation,
mask, gate, state, and retry budget under one coordinator mutex; persist
`READY_QUEUED` and pending bit before starting the worker. On any failure send
`start`, leave no pending bit, and log a bounded `legacy_ready_refused` reason.

- [ ] **Step 5: Run focused contracts and both uplink builds**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_firmware_transport_contract.py -q
cd esp32/uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3
```

Expected: focused contracts pass and both builds link without IRAM growth.

---

### Task 3: Session-Bound Legacy Relay Receipts

**Files:**
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Consumes: exact `.68` source-version proof during the pre-transfer command-health probe.
- Produces: legacy-mode ACK/progress/done validation while leaving strict mode unchanged.

- [ ] **Step 1: Write failing relay contracts**

Require source contracts proving:

```python
assert "FOF_LEGACY_READY_BOOTSTRAP_VERSION" in relay
assert "relay_line_matches_complete_progress" in legacy_final_chunk
assert "relay_line_matches_legacy_done" in legacy_finalize
assert "relay_line_session_matches" in legacy_finalize
```

Negative contracts require wrong session, incomplete progress, wrong received
size, or non-`.68` source to remain rejected. Strict branches must still call
`relay_line_matches_manifest_ack` for `ota_ack`, `ota_staged`, and `ota_done`.

- [ ] **Step 2: Run the focused contract and observe RED**

Run the Task 2 pytest command. Expected: legacy relay validation is absent.

- [ ] **Step 3: Activate legacy mode only from the exact live source**

After the command-health probe returns the scanner version, require exact
`0.64.68-badge-live-follow` before setting automatic legacy mode. An explicit
legacy flag with any other source version fails with `legacy_source_refused`.

- [ ] **Step 4: Validate session-bound legacy receipts**

For legacy `ota_ack`, pass `expected_manifest=NULL` but retain the exact fresh
session check. Extend `relay_wait_for_staged_or_nack` with `legacy_mode`; after
an exact session-bound 100-percent progress frame it returns success only in
legacy mode, while strict mode continues waiting for `ota_staged`. Add:

```c
static bool relay_line_matches_legacy_done(
    const char *line, const char *session_id, uint32_t expected_received)
{
    if (!relay_line_session_matches(line, session_id)) return false;
    cJSON *root = cJSON_Parse(line);
    if (!root) return false;
    bool matched =
        json_string_matches(root, JSON_KEY_TYPE, MSG_TYPE_OTA_DONE) &&
        json_u32_matches(root, "received", expected_received);
    cJSON_Delete(root);
    return matched;
}
```

Use that helper only in legacy finalize. Then require the unchanged same-MAC,
exact `.69` identity, rollback-clear, command, and role-specific radio health
proof before returning success.

- [ ] **Step 5: Run all transport contracts and build gates**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_firmware_transport_contract.py -q
cd esp32/uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3
```

Expected: all pass; strict mode has no legacy receipt acceptance.

---

### Task 4: Physical Bootstrap and Release Gate

**Files:**
- Modify: `docs/badge/README.md`
- Test: `scripts/test_fof_badge_flash.py`

**Interfaces:**
- Consumes: Tasks 1-3 firmware and the connected badge trio.
- Produces: captured hardware convergence evidence and operator documentation.

- [ ] **Step 1: Run final software gates**

```bash
python3 -m unittest scripts.test_fof_badge_flash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -q
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
```

Expected: host, backend, and native suites all pass.

- [ ] **Step 2: Flash only the uplink and stage once**

```bash
python3 scripts/fof_badge_flash.py \
  --transport usb --port /dev/cu.usbmodem11401 --skip-build
```

Expected: uplink `.69` same-version rewrite is skipped or updated once, scanner
image receives one new generation, BLE relay runs first, then Wi-Fi relay.

- [ ] **Step 3: Prove final hardware state**

Require exact scanner MACs `e0:72:a1:f9:48:58` and
`14:c1:9f:52:ca:b0`, both on `0.64.69-badge-defcon34`, exact target/project/
hardware, rollback false, recovery normal, OTA idle, correct BLE/Wi-Fi roles,
coordinator mask 3, pending 0, worker false, and both states `converged`.

- [ ] **Step 4: Complete USB/UI and release gates**

Apply and restore a theme over USB, prove quiet and wake while USB/UART stay
alive, photograph the four-lane LCD with the Mac camera, then run all five
firmware builds, version/manifests, release workflow, tag, assets, hashes, and
download checks before declaring the release shipped.
