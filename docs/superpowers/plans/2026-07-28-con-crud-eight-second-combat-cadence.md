# CON CRUD Eight-Second Combat Cadence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Slow repeated CON CRUD peer effects from once per second to once
every eight seconds, promote the private candidate to
`0.67.0-badge-defcon34`, and prove all three connected badge graphs healthy.

**Architecture:** Reuse the existing allocation-free per-peer
`last_emitted_ms` limiter. Change only its interval constant; preserve quorum,
RSSI multipliers, independent peer stacking, game-state records, scanner
behavior, and updater behavior. Establish the behavior with native tests
before changing production code, then freeze and verify one scanner and one
uplink artifact for all physical flashes.

**Tech Stack:** ESP32-S3 C firmware, PlatformIO/ESP-IDF, Unity native tests,
Python firmware identity/verifier tests, descriptor-bound USB/UART flasher.

## Global Constraints

- The exact effect interval is `8000U` milliseconds.
- The first qualified effect remains immediate.
- Quorum remains three distinct packets inside 6,000 ms.
- RSSI bands, role damage/healing values, passive one-point-per-minute infected
  decay, and independent per-peer stacking remain unchanged.
- Do not add a task, timer, queue, lock, allocation, protocol field, or
  persistent state.
- Do not change scanner observation, forwarding, radio profiles, the four-lane
  UI, Android/backend schemas, or updater architecture.
- The final private version is exactly `0.67.0-badge-defcon34`.
- Do not publish, tag, merge, update the public factory bundle, or start the
  sensor-node port before attended badge acceptance.
- Preserve the unrelated untracked `.camera-before-zoom.jpg`.

---

### Task 1: Lock the Eight-Second Per-Peer Boundary

**Files:**
- Modify: `esp32/test/test_badge_con_encounter.c`
- Modify: `esp32/test/test_badge_con_observer.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `badge_con_encounter_consume(table, packet, now_ms)` and the
  existing `badge_con_peer_entry_t.last_emitted_ms`.
- Produces: regression evidence for the exact 7,999/8,000 ms boundary, timer
  wrap, independent peers, and observer forwarding cadence.

- [ ] **Step 1: Change the focused cadence test before production code**

Rename the one-second test to
`test_badge_con_encounter_later_effects_are_limited_to_one_per_eight_seconds`.
Keep the first qualification at `1000U`, feed unique one-second packet
sequences so the quorum stays warm, assert
`BADGE_CON_OBSERVE_RATE_LIMITED` through `8999U`, and assert
`BADGE_CON_OBSERVE_QUALIFIED` at `9000U`.

- [ ] **Step 2: Add an independent-peer regression**

In a fresh table, qualify peer A, leave it inside its cooldown, then send three
strong unique packets from peer B and require peer B to qualify. Finally send
another unique peer-A packet before its 8,000 ms boundary and require
`BADGE_CON_OBSERVE_RATE_LIMITED`.

- [ ] **Step 3: Extend the timer-wrap regression**

Start the qualified effect immediately before `UINT32_MAX`, feed continuous
unique packets across wrap, require rate limiting through elapsed 7,999 ms,
and qualification at elapsed 8,000 ms using unsigned subtraction.

- [ ] **Step 4: Update observer cadence expectations**

For the existing one-packet-per-second observer stream, expect qualified
sequences `3`, `11`, `19`, `27`, and `35`; all intervening distinct packets
after quorum must be rate-limited. Keep all validation/drop counters and the
scanner-to-uplink pending-frame contract unchanged.

- [ ] **Step 5: Register renamed/new tests**

Update declarations and `RUN_TEST` entries in `esp32/test/test_runner.c`
without changing unrelated test order.

- [ ] **Step 6: Run focused tests and prove RED**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test -f test_badge_con_encounter
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test -f test_badge_con_observer
```

Expected: failures specifically show the production one-second limiter
qualifying before the new 8,000 ms boundary.

- [ ] **Step 7: Commit the failing regressions**

```bash
git add esp32/test/test_badge_con_encounter.c \
  esp32/test/test_badge_con_observer.c esp32/test/test_runner.c
git commit -m "test: lock eight-second CON CRUD cadence"
```

### Task 2: Apply the Allocation-Free Cadence Change

**Files:**
- Modify: `esp32/shared/badge_con_encounter.h`

**Interfaces:**
- Consumes: existing per-peer `emitted` and `last_emitted_ms` fields.
- Produces: `BADGE_CON_EFFECT_RATE_MS == 8000U`; no ABI or structure change.

- [ ] **Step 1: Make the minimal production edit**

Change:

```c
#define BADGE_CON_EFFECT_RATE_MS 1000U
```

to:

```c
#define BADGE_CON_EFFECT_RATE_MS 8000U
```

Do not change any other game, observer, protocol, or runtime constant.

- [ ] **Step 2: Run the focused cadence tests and prove GREEN**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test -f test_badge_con_encounter
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test -f test_badge_con_observer
```

Expected: both focused suites pass.

- [ ] **Step 3: Run the complete native suite**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test
```

Expected: every native test passes under the existing sanitizer build.

- [ ] **Step 4: Commit the production change**

```bash
git add esp32/shared/badge_con_encounter.h
git commit -m "game: slow CON CRUD peer cadence"
```

### Task 3: Promote the Private Candidate to 0.67.0

**Files:**
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `esp32/shared/version.h`

**Interfaces:**
- Consumes: the existing `FOF_VERSION_BADGE_CANARY` identity selector.
- Produces: exact private scanner/uplink version
  `0.67.0-badge-defcon34`; production identities remain unchanged.

- [ ] **Step 1: Change canary identity expectations first**

Replace the four exact badge-canary expectations
`0.64.93-badge-defcon34` with `0.67.0-badge-defcon34`. Do not change
production, sensor, or Android version expectations.

- [ ] **Step 2: Run the focused identity test and prove RED**

Run:

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py::test_shared_header_selects_production_and_badge_tracks -q
```

Expected: the canary header still returns `.93`.

- [ ] **Step 3: Update only the badge-canary version macro**

Set:

```c
#define FOF_VERSION_BADGE_CANARY "0.67.0-badge-defcon34"
```

Leave every production and sensor macro unchanged.

- [ ] **Step 4: Run focused and full identity contracts**

Run:

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py::test_shared_header_selects_production_and_badge_tracks -q
/Users/billh/gai/friendorfoe/backend/.venv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py -q
```

Expected: both commands pass.

- [ ] **Step 5: Commit the version promotion**

```bash
git add backend/tests/test_firmware_build_version.py esp32/shared/version.h
git commit -m "v0.67.0: promote final badge canary"
```

### Task 4: Freeze and Verify the Final Artifacts

**Files:**
- Generated only:
  `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary/`
- Generated only:
  `esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary/`

**Interfaces:**
- Consumes: committed `0.67.0-badge-defcon34` source.
- Produces: one immutable scanner artifact and one immutable uplink artifact
  with strict hashes and memory evidence.

- [ ] **Step 1: Safely clean only the two generated canary environments**

Run PlatformIO scoped clean targets; do not delete source or production build
evidence:

```bash
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/scanner \
  -e scanner-s3-combo-fof_badge-con-crud-canary -t clean
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/uplink \
  -e uplink-s3-fof_badge-con-crud-canary -t clean
```

- [ ] **Step 2: Build and verify the scanner artifact**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/scanner \
  -e scanner-s3-combo-fof_badge-con-crud-canary
python3 esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge
```

Expected: strict verification passes, internal RAM is at most 180,224 bytes,
and app usage is at most 1,363,148 bytes.

- [ ] **Step 3: Build and verify the uplink artifact**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/uplink \
  -e uplink-s3-fof_badge-con-crud-canary
python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

Expected: strict verification passes, internal RAM is at most 212,992 bytes,
and app usage is at most 1,468,006 bytes.

- [ ] **Step 4: Run updater host regressions**

Run:

```bash
python3 -m unittest scripts.test_fof_badge_flash -v
python3 -m unittest scripts.test_verify_badge_usb_hardening -v
```

Expected: all updater and USB-hardening tests pass.

### Task 5: Flash and Prove the Three Badge Graphs

**Files:**
- No source changes.

**Interfaces:**
- Consumes: the two frozen and verified `.67` artifacts.
- Produces: nine healthy physical applications across three descriptor-bound
  badge graphs.

- [ ] **Step 1: Take a read-only USB census**

Resolve the three uplink descriptors and capture current uplink/scanner
versions, hardware identities, roles, radio profiles, rollback state,
recovery mode, and game seeds before mutation.

- [ ] **Step 2: Flash each badge sequentially**

After the fresh census confirms the retained location/identity bindings, run
the current three ports sequentially:

```bash
/Users/billh/.platformio/penv/bin/python scripts/fof_badge_flash.py \
  --platform badge-trio-xiao-s3-con-crud-canary \
  --transport usb --only all --port /dev/cu.usbmodem1101 \
  --bind-selected-uplink --skip-build
/Users/billh/.platformio/penv/bin/python scripts/fof_badge_flash.py \
  --platform badge-trio-xiao-s3-con-crud-canary \
  --transport usb --only all --port /dev/cu.usbmodem1201 \
  --bind-selected-uplink --skip-build
/Users/billh/.platformio/penv/bin/python scripts/fof_badge_flash.py \
  --platform badge-trio-xiao-s3-con-crud-canary \
  --transport usb --only all --port /dev/cu.usbmodem1401 \
  --bind-selected-uplink --skip-build
```

If any port changes after re-enumeration, replace only that command's device
path with the descriptor-census result for the same hardware identity and USB
location. Do not run badges concurrently. If a zero-byte scanner readiness
failure occurs, preserve the transcript, prove cleanup to normal, and use only
the documented fresh exact-lane retry. Do not direct-flash a scanner.

- [ ] **Step 3: Take a final read-only nine-application census**

Require every badge to report:

- uplink version `0.67.0-badge-defcon34`;
- BLE and Wi-Fi scanner versions `0.67.0-badge-defcon34`;
- both scanners connected, `health:"ok"`, role acknowledged, correct active
  radio profile, OTA idle, recovery normal, and crash count zero;
- uplink pending verification false, rollback clear, recovery normal, and no
  update session;
- original normal/healer/infected seeds retained with inactive reset state.

### Task 6: Attended Physical Acceptance

**Files:**
- Modify only after observation:
  `docs/badge/con-crud-canary-acceptance.md`

**Interfaces:**
- Consumes: three complete healthy `.67` badge graphs.
- Produces: operator-observed final acceptance evidence; no public promotion.

- [ ] **Step 1: Activate all three badges**

Use exact SSID `GameChangersAI-67` or the existing physical Easter path,
dismiss the Easter screen, and verify `HUMAN`, `HEALER`, and `INFECTED`
presentation.

- [ ] **Step 2: Verify the new live cadence**

Hold one infected badge near the human. Confirm the first qualified effect is
responsive and no second effect from that identity occurs before eight
seconds. Confirm the next effect appears at approximately eight seconds.

- [ ] **Step 3: Verify stacking and role behavior**

Bring both infected identities near the healer and confirm they contribute
independently. Verify healer self-regeneration remains absent, cure still
returns a player to `HUMAN`, and the cured player can be reinfected.

- [ ] **Step 4: Confirm game duration and normal badge function**

Observe that the game now lasts minutes rather than seconds while the four
scanner lanes, buttons, USB control, and display continue operating.

- [ ] **Step 5: Record evidence and stop at the private boundary**

Record the final hashes, memory margins, nine-application health result, and
operator timing observation in the private acceptance ledger. Do not push,
tag, publish, update the factory bundle, or begin the sensor-node port until
the operator explicitly accepts the result.
