# Badge Scanner 0.64.68 Legacy Bootstrap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bootstrap the exact `0.64.68-badge-live-follow` scanners through the durable automatic updater without weakening strict `.69` manifest receipts.

**Architecture:** A pure authorization helper validates the legacy frame against the cached scanner identity and committed staged manifest. The uplink parser accepts legacy syntax only when all strict-only fields are absent, and the durable coordinator queues it only from exact `offered` state. The relay core uses session-bound legacy acknowledgements, complete progress, and done receipts, then retains the existing staged SHA, CRC, ESP image validation, same-MAC, rollback, and radio convergence gates.

**Tech Stack:** ESP-IDF C, cJSON, NVS coordinator state, UART OTA, PlatformIO/Unity native tests, pytest source-contract tests, Python hardware flasher.

## Global Constraints

- Automatically accept legacy readiness only from exact source version `0.64.68-badge-live-follow`.
- Require the identity-capable `.68` dialect observed on the physical scanners;
  the tagged `.68` binary lacks the extended identity contract and must remain
  ineligible.
- Never treat a partial or malformed strict receipt as legacy.
- Require exact cached target, project, hardware, immutable MAC, board, and version before queueing.
- Require a complete identity frame newer than the current coordinator
  generation's per-slot identity floor, and bind the accepted check/offer/ready
  sequence to the same volatile identity generation and MAC.
- Publish/read the legacy identity through a mutex-protected small snapshot;
  do not make authorization from the unsynchronized large scanner-info pointer.
- Persist the bound MAC before reserving a relay and restore interrupted relays
  to durable `recovering`, never generic `awaiting_check`.
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

### Task 2: Identity Snapshot and Behavioral Coordinator Policy

**Files:**
- Create: `esp32/shared/firmware_auto_policy.h`
- Create: `esp32/shared/firmware_auto_policy.c`
- Create: `esp32/test/test_firmware_auto_policy.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Modify: `esp32/uplink/main/comms/uart_rx.h`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Produces pure decisions for identity freshness, exact offered-only queueing,
  BLE success-only gating, recovery cooldown/probe eligibility, and interrupted
  relay convergence/re-offer.
- Produces `scanner_identity_snapshot_t` and
  `uart_rx_get_scanner_identity_snapshot(int, scanner_identity_snapshot_t *)`.

- [ ] **Step 1: Write failing behavioral native tests and source contracts**

Cover one-negative-at-a-time cases for an incomplete tagged `.68` identity,
identity generation at/below the generation floor, expired receive time,
changed offer generation/manifest/slot/identity generation/MAC, non-`OFFERED`
queue state, BLE failed/refused/newer not opening Wi-Fi, cooldown not consuming a
probe, recovery equality without fresh same-MAC healthy identity, and recovery
re-offer from any source except exact identity-capable `.68`.

Add source contracts requiring a small mutex-protected snapshot populated from
the *current* `scanner_info` frame, with `complete=false` when any extended
identity field is absent or malformed. Do not authorize from the existing raw
pointer getter.

- [ ] **Step 2: Run RED gates**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  ../backend/tests/test_badge_firmware_transport_contract.py -q
```

- [ ] **Step 3: Implement the pure policy and identity snapshot**

Keep the policy module free of ESP-IDF dependencies. Use a static FreeRTOS
mutex only around publication/copy of the small identity snapshot, not around
the large scanner status structure. A snapshot carries exact identity strings,
`complete`, monotonically increasing generation, and monotonic receive time.
Missing fields clear the new snapshot rather than retaining prior values.

- [ ] **Step 4: Run GREEN gates and both uplink builds**

Run Step 2 plus both `uplink-s3-fof_badge` and `uplink-s3` PlatformIO builds.

---

### Task 3: Parser and Durable Coordinator Bridge

**Files:**
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/uplink/main/network/fw_store.h`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**
- Consumes Tasks 1-2 pure policies and synchronized identity snapshots.
- Produces `fw_store_handle_legacy_scanner_ready(...)` and a schema-upgraded
  coordinator with persisted per-slot bound MAC plus `RECOVERING`.

- [ ] **Step 1: Write failing parser/coordinator contracts**

Require `strict_receipt_fields_absent`, the legacy handler, exact numeric/common
field parsing, manual-probe reason, exact `OFFERED`, fresh offer binding, durable
MAC before queue/relay, schema validation, and save-before-worker ordering.
Require partial strict frames to remain `malformed_fw_ready` and transfer zero
bytes.

- [ ] **Step 2: Run focused RED**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_firmware_transport_contract.py -q
```

- [ ] **Step 3: Parse and bind the exact legacy check/ready sequence**

When a generation begins or is restored, record per-slot identity floors. Only
an exact manual `.68` check following a newer complete identity may become
`OFFERED`; capture a short-lived volatile binding containing generation,
manifest CRC, slot, identity generation, MAC, and timestamp. A legacy ready is
eligible only when every strict-only key is absent, all common values parse
exactly, Task 1 authorizes it, and the same binding is still live. Any strict
key means the existing strict parser owns the frame; malformed strict never
falls back.

- [ ] **Step 4: Persist ownership before worker start**

Increment the coordinator schema and persist the bound MAC before setting
`READY_QUEUED`/pending and before starting the worker. Enqueue requires exact
`OFFERED`, requested slot, generation, manifest CRC, BLE gate, and retry budget
under one mutex; an NVS failure restores the prior blob and starts no worker.
The Wi-Fi gate opens only from BLE `CONVERGED` or `CURRENT`. Before automatic
legacy `ota_begin`, wait up to 12 seconds for another complete identity
generation from the stopped scanner and require the same MAC/version/contract.

- [ ] **Step 5: Run focused tests and both uplink builds**

Run the focused backend contract, full native suite, and both uplink builds.

---

### Task 4: Session-Bound Legacy Relay and Interrupted Recovery

**Files:**
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Modify: `esp32/test/test_firmware_auto_policy.c`

**Interfaces:**
- Consumes exact live `.68` plus the durable bound MAC.
- Produces exact legacy ACK/progress/done receipt handling and durable
  interrupted-relay recovery without weakening strict mode.

- [ ] **Step 1: Write failing receipt/recovery tests**

Require behavioral policy and source-contract negatives for type
`ota_ack_extra`, wrong session, incomplete/wrong-size progress, wrong-size done,
non-`.68` legacy source, recovery before cooldown, bare equal check, changed MAC,
rollback/recovery/profile/radio failure, NVS failure, and BLE failure opening
Wi-Fi. Strict branches must still use full manifest ACK/staged/done matchers.

- [ ] **Step 2: Run focused RED**

Run the Task 3 focused backend test and native suite.

- [ ] **Step 3: Validate exact session-bound legacy receipts**

Activate automatic legacy mode only from the exact live source and durable
bound MAC. Use a dedicated JSON matcher requiring exact `type == ota_ack` plus
the fresh session; never pass a null manifest to the strict matcher. Legacy
final-chunk success requires exact session-bound 100-percent progress with
`received == total == staged size`. Legacy done requires exact type, session,
and received size. Strict `.69` continues requiring every manifest field.

- [ ] **Step 4: Recover interrupted relays without inventing success**

Restore durable `RELAYING` as durable `RECOVERING`, retaining the bound MAC and
consumed attempt. Send `ota_abort`, then hold a 35-second not-before cooldown
without consuming readiness probes; afterward use at most three probes 20
seconds apart. A manual response at target version becomes `CONVERGED` only with
a newer complete same-MAC identity and exact rollback-clear, recovery-normal,
command/profile/radio health. If the same proven scanner remains exact `.68`,
return to offer/ready for another bounded attempt. A bare equal fw_check can
never resolve restored `RELAYING` as `CURRENT`.

- [ ] **Step 5: Run transport/native/build gates**

Run the focused contract, full native suite, and both uplink builds. Verify
strict mode contains no legacy receipt acceptance.

---

### Task 5: Physical Bootstrap and Release Gate

**Files:**
- Modify: `docs/badge/README.md`
- Test: `scripts/test_fof_badge_flash.py`

**Interfaces:**
- Consumes: Tasks 1-4 firmware and the connected badge trio.
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
