# CON CRUD Healer Balance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebalance healer-versus-zombie encounters, remove solo healer regeneration, and make cured humans visibly and mechanically reinfectable without adding runtime state.

**Architecture:** Keep all gameplay math in the existing allocation-free `badge_con_game_apply_peer()` state machine and keep role selection in `badge_con_presentation_select()`. The existing scanner peer table, one-effect-per-peer limiter, BLE/UART protocol, display transition detector, and updater remain untouched.

**Tech Stack:** C11, Unity native tests, PlatformIO native/ESP-IDF builds, existing strict badge uplink artifact verifier.

## Global Constraints

- Keep this work on the private `codex/defcon34-badge-final` branch.
- Do not push, tag, merge, release, update the public factory bundle, or flash connected hardware.
- Preserve the frozen `0.64.92-badge-defcon34` artifacts; this changed private
  candidate uses `0.64.93-badge-defcon34`. Keep `0.67.0-badge-defcon34`
  reserved for attended final acceptance.
- Healer cure pressure is exactly `12 × badge_con_game_rssi_multiplier(rssi)`.
- Regular zombie damage to a healer is exactly `10 × badge_con_game_rssi_multiplier(rssi)`; authenticated super damage is exactly twice that.
- Two zombies remain independent effects; do not add attacker-count state.
- Healers have no passive regeneration and heal only from qualified healer peers at the existing `5 × RSSI multiplier`.
- Cured normal players display `HUMAN`; `CURED` remains only the existing roughly three-second transition activity.
- Do not change BLE payloads, advertising interval, scanner firmware, UART grammar, encounter table, NVS/RTC schema, task graph, heap allocations, or four detection lanes.
- Keep the strict canary ceilings at 212,992 bytes internal RAM and 1,468,006 bytes app image.

---

### Task 1: Lock the balance and reinfection contract in native tests

**Files:**
- Modify: `esp32/test/test_badge_con_game.c`
- Modify: `esp32/test/test_badge_con_runtime_policy.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `badge_con_game_apply_peer()`, `badge_con_game_snapshot()`, and the existing RSSI multiplier bands.
- Produces: Unity regressions for solo healer behavior, healer-peer healing, 1.2-times cure pressure, one/two/super zombie pressure, and cured-human reinfection.

- [ ] **Step 1: Replace the obsolete passive-regeneration test**

Replace `test_badge_con_game_immune_regenerates_lazily_and_orders_encounters`
with a test named
`test_badge_con_game_healer_requires_peer_healing` that snapshots an isolated
90-health healer after ordinary time and `uint32_t` wrap and asserts that it
remains at 90. Then apply a healer peer at `-45 dBm` and assert the existing
`5 × 3 = 15` healing raises it to 100, capped.

```c
void test_badge_con_game_healer_requires_peer_healing(void)
{
    badge_con_game_state_t healer = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 90U,
        .last_decay_ms = 1000U,
    };
    badge_con_snapshot_t snapshot = {0};

    badge_con_game_snapshot(&healer, 9000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(90U, snapshot.shield);

    healer.last_decay_ms = UINT32_MAX - 999U;
    badge_con_game_snapshot(&healer, 1000U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(90U, snapshot.shield);

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_GAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_IMMUNE, false, -45, 1000U));
    TEST_ASSERT_EQUAL_UINT8(100U, healer.shield);
}
```

- [ ] **Step 2: Add exact healer/zombie pressure coverage**

Add `test_badge_con_game_healer_and_zombie_pressure_rates`. Begin with a
100-health healer. A regular `-60 dBm` zombie must reduce it to 90, a second
regular `-60 dBm` effect must reduce it to 80, and a super `-45 dBm` effect
must apply `20 × 3 = 60`, leaving 20. In the same test, start an infected
player at 20 rescue points and prove healer cure effects are 12, 24, and 36
at weak, medium, and strong RSSI.

```c
void test_badge_con_game_healer_and_zombie_pressure_rates(void)
{
    badge_con_game_state_t healer = {
        .seed = BADGE_CON_ROLE_IMMUNE,
        .role = BADGE_CON_ROLE_IMMUNE,
        .active = true,
        .shield = 100U,
    };

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_INFECTED, false, -60, 0U));
    TEST_ASSERT_EQUAL_UINT8(90U, healer.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_INFECTED, false, -60, 0U));
    TEST_ASSERT_EQUAL_UINT8(80U, healer.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &healer, BADGE_CON_ROLE_INFECTED, true, -45, 0U));
    TEST_ASSERT_EQUAL_UINT8(20U, healer.shield);

    badge_con_game_state_t infected = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 20U,
    };
    badge_con_game_apply_peer(
        &infected, BADGE_CON_ROLE_IMMUNE, false, -60, 0U);
    TEST_ASSERT_EQUAL_UINT8(32U, infected.shield);
    badge_con_game_apply_peer(
        &infected, BADGE_CON_ROLE_IMMUNE, false, -52, 0U);
    TEST_ASSERT_EQUAL_UINT8(56U, infected.shield);
    badge_con_game_apply_peer(
        &infected, BADGE_CON_ROLE_IMMUNE, false, -45, 0U);
    TEST_ASSERT_EQUAL_UINT8(92U, infected.shield);
}
```

- [ ] **Step 3: Add a cured-human reinfection characterization**

Add `test_badge_con_game_cured_human_can_be_reinfected`. A normal player with
scar level 1, reduced maximum 50, and health 50 must fall to 20 after one
strong regular zombie effect and become infected with exactly 45 rescue
points after the next. Snapshot maximum remains 50 after infection.

```c
void test_badge_con_game_cured_human_can_be_reinfected(void)
{
    badge_con_game_state_t cured = {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 50U,
        .scar_level = 1U,
    };

    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_SHIELD_DRAINED,
        badge_con_game_apply_peer(
            &cured, BADGE_CON_ROLE_INFECTED, false, -45, 0U));
    TEST_ASSERT_EQUAL_UINT8(20U, cured.shield);
    TEST_ASSERT_EQUAL(
        BADGE_CON_EFFECT_INFECTED,
        badge_con_game_apply_peer(
            &cured, BADGE_CON_ROLE_INFECTED, false, -45, 0U));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, cured.role);
    TEST_ASSERT_EQUAL_UINT8(
        BADGE_CON_INFECTED_RESCUE_POINTS, cured.shield);

    badge_con_snapshot_t snapshot = {0};
    badge_con_game_snapshot(&cured, 0U, &snapshot);
    TEST_ASSERT_EQUAL_UINT8(50U, snapshot.maximum);
}
```

- [ ] **Step 4: Register the renamed and new Unity tests**

Update the declarations and `RUN_TEST` entries in `esp32/test/test_runner.c`
using these exact names:

```c
void test_badge_con_game_healer_requires_peer_healing(void);
void test_badge_con_game_healer_and_zombie_pressure_rates(void);
void test_badge_con_game_cured_human_can_be_reinfected(void);
```

- [ ] **Step 5: Update existing balance assertions to the approved contract**

In `test_badge_con_game_infected_pressure_and_healer_race`, change the strong
healer result after an infected meter of 38 from 68 to 74 because
`38 + 12 × 3 = 74`.

In `test_badge_con_runtime_expected_reboot_resets_live_state`, change the
healer health after one strong regular-zombie packet from 97 to 70 because
`100 - 10 × 3 = 70`. This test continues to prove that the later reboot
restores only the healer seed and clears the live health.

- [ ] **Step 6: Run the native suite and verify the red phase**

Run from `esp32/`:

```sh
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: the new solo-regeneration, damage-rate, and cure-rate assertions
fail against the old production math. The reinfection characterization may
already pass, proving the underlying transition works and the observed
problem is presentation rather than an immunity flag.

- [ ] **Step 7: Commit the failing contract**

```sh
git add esp32/test/test_badge_con_game.c \
  esp32/test/test_badge_con_runtime_policy.c \
  esp32/test/test_runner.c
git commit -m "test: lock CON CRUD healer balance"
```

---

### Task 2: Implement the allocation-free game balance

**Files:**
- Modify: `esp32/shared/badge_con_game.c`
- Test: `esp32/test/test_badge_con_game.c`

**Interfaces:**
- Consumes: `badge_con_game_rssi_multiplier()` returning 0 through 3.
- Produces: unchanged `badge_con_game_apply_peer()` signature with the approved integer effects.

- [ ] **Step 1: Remove solo healer regeneration**

In `apply_lazy_time()`, remove the immune-role two-second tick branch and make
all non-infected roles return without mutating health or `last_decay_ms`:

```c
if (state->role != BADGE_CON_ROLE_INFECTED) {
    return;
}
```

- [ ] **Step 2: Increase zombie damage received by a healer**

In the healer recipient branch of `badge_con_game_apply_peer()`, replace the
old `1/2 × multiplier` damage with:

```c
unsigned damage =
    (peer_super ? 20U : 10U) * multiplier;
```

- [ ] **Step 3: Set healer cure pressure to 1.2 times**

In the infected recipient branch, replace `10U * multiplier` with:

```c
unsigned cure = (unsigned)state->shield + 12U * multiplier;
```

Do not change normal-human healer gain (`5 × multiplier`) or
healer-to-healer gain (`5 × multiplier`).

- [ ] **Step 4: Run the native suite and verify green**

Run:

```sh
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: all Unity checks pass under the native AddressSanitizer build.

- [ ] **Step 5: Commit the gameplay implementation**

```sh
git add esp32/shared/badge_con_game.c
git commit -m "game: rebalance CON CRUD healers"
```

---

### Task 3: Make cured status transient while retaining scar mechanics

**Files:**
- Modify: `esp32/test/test_badge_con_presentation.c`
- Modify: `esp32/shared/badge_con_presentation.c`

**Interfaces:**
- Consumes: `badge_con_snapshot_t` with `role == BADGE_CON_ROLE_NORMAL` and `cured == true`.
- Produces: `badge_con_presentation_select()` returns `BADGE_CON_PRESENT_HUMAN` for every active, living normal role.

- [ ] **Step 1: Change the cured snapshot expectation**

In `test_badge_con_presentation_selects_every_game_state`, retain
`snapshot.cured = true`, `scar_level = 1`, and `maximum = 50`, but expect:

```c
TEST_ASSERT_EQUAL(
    BADGE_CON_PRESENT_HUMAN,
    badge_con_presentation_select(&snapshot));
```

The display-level role transition still emits
`BADGE_CON_RENDER_ACTIVITY_CURED`; do not remove the `CURED` activity string.

- [ ] **Step 2: Run the native suite and verify the presentation test fails**

Run:

```sh
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: the cured presentation assertion fails because production still
returns `BADGE_CON_PRESENT_CURED`.

- [ ] **Step 3: Return human presentation for every normal role**

Replace the final cured conditional in `badge_con_presentation_select()`:

```c
return BADGE_CON_PRESENT_HUMAN;
```

Keep the `BADGE_CON_PRESENT_CURED` enum value and theme slot for binary/source
compatibility; they are no longer selected as a persistent role.

- [ ] **Step 4: Run the native suite and verify green**

Run:

```sh
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: all Unity checks pass.

- [ ] **Step 5: Commit the presentation fix**

```sh
git add esp32/test/test_badge_con_presentation.c \
  esp32/shared/badge_con_presentation.c
git commit -m "game: make cured status transient"
```

---

### Task 4: Verify the private canary build and memory boundary

**Files:**
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `esp32/shared/version.h`
- Verify only: `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary/`
- Verify only: `esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary/`
- Verify only: `esp32/scripts/verify_badge_scanner_build.py`
- Verify only: `esp32/scripts/verify_badge_uplink_build.py`

**Interfaces:**
- Consumes: the private canary source tree and existing production comparison builds.
- Produces: immutable `.93` scanner/uplink identities plus fresh native-test, compile, strict app-image, and internal-RAM evidence without hardware mutation.

- [ ] **Step 1: Lock the new immutable canary identity**

Change all four canary expectations in
`backend/tests/test_firmware_build_version.py` from
`0.64.92-badge-defcon34` to `0.64.93-badge-defcon34`, then run:

```sh
python3 -m pytest \
  backend/tests/test_firmware_build_version.py::test_shared_header_selects_production_and_badge_tracks \
  -q
```

Expected red phase: the test reports the header still selects `.92`.

Change only `FOF_VERSION_BADGE_CANARY` in `esp32/shared/version.h`:

```c
#define FOF_VERSION_BADGE_CANARY "0.64.93-badge-defcon34"
```

Rerun the same pytest. Expected: pass.

- [ ] **Step 2: Commit the private canary identity**

```sh
git add backend/tests/test_firmware_build_version.py esp32/shared/version.h
git commit -m "v0.64.93: identify healer balance canary"
```

- [ ] **Step 3: Run one final native suite**

```sh
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: all checks pass with zero AddressSanitizer failures.

- [ ] **Step 4: Build the private canary scanner once**

```sh
cd esp32/scanner
/Users/billh/.platformio/penv/bin/pio run \
  -e scanner-s3-combo-fof_badge-con-crud-canary
```

Expected: successful ESP-IDF build with immutable `.93` artifact aliases.

- [ ] **Step 5: Verify the private canary scanner**

From the repository root:

```sh
python3 esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge
```

Expected: strict scanner verification passes.

- [ ] **Step 6: Build the private canary uplink once**

```sh
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run \
  -e uplink-s3-fof_badge-con-crud-canary
```

Expected: successful ESP-IDF build.

- [ ] **Step 7: Run the strict uplink verifier**

From the repository root:

```sh
python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

Expected: strict verification passes, app image is no more than 1,468,006
bytes, and internal RAM is no more than 212,992 bytes.

- [ ] **Step 8: Inspect the final diff and status**

Run:

```sh
git diff --check
git status --short
git log --oneline -8
```

Expected: no whitespace errors; only the unrelated pre-existing
`.camera-before-zoom.jpg` remains untracked.

- [ ] **Step 9: Stop at the attended flash gate**

Report the exact test count, app-image size/margin, internal-RAM size/margin,
local commits, and the fact that no badge was flashed. Ask for explicit
attended authorization before installing this candidate on the connected
three-badge test set.
