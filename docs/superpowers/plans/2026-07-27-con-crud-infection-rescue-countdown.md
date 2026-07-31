# CON CRUD Infection Rescue Countdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every infected player a 45-minute rescue countdown that nearby
infected badges accelerate, healers reverse, and zero converts into permanent
but still-spreading death.

**Architecture:** Reuse the existing one-byte `shield` field as remaining
rescue points while the live role is infected. Extend only the pure shared
game state machine; retain the existing BLE payload, scanner forwarding,
encounter quorum, updater, persistence, display snapshot, and super-zombie
derivation.

**Tech Stack:** ESP-IDF/PlatformIO C, Unity native tests, Python firmware and
artifact gates, existing descriptor-bound USB/UART badge flasher.

## Global Constraints

- Work only in `.worktrees/defcon34-badge-final`.
- Keep the work private and commit locally; do not push, tag, merge, release,
  or alter the public factory bundle.
- Do not change scanner radio behavior, BLE/UART packet formats, peer-table
  capacity, task count, queues, NVS/RTC record formats, or USB schemas.
- Start infected players at exactly 45 rescue points and decay exactly one
  point per elapsed whole minute.
- A qualified regular infected peer removes `1 × RSSI multiplier`; a valid
  super peer removes `2 × RSSI multiplier`.
- Preserve healer cure gain at `10 × RSSI multiplier`, cure threshold 100,
  and the existing `50 → 25 → 12 → 6 → 3 → 1` scar ladder.
- Dead badges remain infected advertisers. Only dead healer seeds derive the
  existing super flag.
- Do not relax scanner or uplink RAM/image ceilings.
- Flash hardware only after native tests, version tests, clean builds, and
  strict artifact verifiers pass.

---

### Task 1: Specify the rescue countdown with failing native tests

**Files:**
- Modify: `esp32/test/test_badge_con_game.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `badge_con_game_activate(...)`,
  `badge_con_game_apply_peer(...)`, and `badge_con_game_snapshot(...)`.
- Produces: native expectations for a 45-point infected countdown, proximity
  damage, zero death, dead spreading identity, and cured-human stability.

- [ ] **Step 1: Update existing transition expectations**

Change infected-seed activation and human zero-crossing assertions from zero
to 45. Keep the role assertion `BADGE_CON_ROLE_INFECTED`.

```c
TEST_ASSERT_TRUE(badge_con_game_activate(&infected, 1000U));
TEST_ASSERT_EQUAL_UINT8(45U, infected.shield);

TEST_ASSERT_EQUAL(
    BADGE_CON_EFFECT_INFECTED,
    badge_con_game_apply_peer(
        &human, BADGE_CON_ROLE_INFECTED, false, -45, 2000U));
TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, human.role);
TEST_ASSERT_EQUAL_UINT8(45U, human.shield);
```

- [ ] **Step 2: Add isolated countdown and exact-zero death coverage**

Add
`test_badge_con_game_infected_rescue_countdown_reaches_permanent_death`.
Activate a factory-infected state at time 1000, snapshot at
`1000 + 44 * 60000` and require one point, then snapshot at
`1000 + 45 * 60000` and require dead infected state with zero points and
`super == false`. Apply a healer afterward and require
`BADGE_CON_EFFECT_NONE`.

- [ ] **Step 3: Add proximity acceleration and healer-race coverage**

Add `test_badge_con_game_infected_pressure_and_healer_race`:

```c
badge_con_game_state_t game = {
    .seed = BADGE_CON_ROLE_NORMAL,
    .role = BADGE_CON_ROLE_INFECTED,
    .active = true,
    .shield = 45U,
    .last_decay_ms = 1000U,
};
TEST_ASSERT_EQUAL(
    BADGE_CON_EFFECT_SHIELD_DRAINED,
    badge_con_game_apply_peer(
        &game, BADGE_CON_ROLE_INFECTED, false, -60, 1000U));
TEST_ASSERT_EQUAL_UINT8(44U, game.shield);
TEST_ASSERT_EQUAL(
    BADGE_CON_EFFECT_SHIELD_DRAINED,
    badge_con_game_apply_peer(
        &game, BADGE_CON_ROLE_INFECTED, true, -45, 1000U));
TEST_ASSERT_EQUAL_UINT8(38U, game.shield);
TEST_ASSERT_EQUAL(
    BADGE_CON_EFFECT_CURE_GAINED,
    badge_con_game_apply_peer(
        &game, BADGE_CON_ROLE_IMMUNE, false, -45, 1000U));
TEST_ASSERT_EQUAL_UINT8(68U, game.shield);
```

Then set the meter to six, apply one close super peer, and require
`BADGE_CON_EFFECT_DIED`, zero points, infected role, dead true, and regular
spreading identity for a normal seed. Retain the existing healer-death test
that proves a dead healer is super.

- [ ] **Step 4: Extend cured-at-49 and wraparound assertions**

Set a cured scar-1 human to 49, snapshot several minutes later, and require it
to remain 49. Add an infected state whose decay epoch crosses `UINT32_MAX`;
require 45 points to reach zero after exactly 45 wrapped minutes.

- [ ] **Step 5: Register both new test functions**

Add declarations and `RUN_TEST(...)` entries in
`esp32/test/test_runner.c`.

- [ ] **Step 6: Run the native suite and prove RED**

Run:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: new/updated assertions fail because infected activation and
human-to-infected transitions still produce zero, infected peers remain
idempotent, and zero does not yet mark ordinary infected players dead.

### Task 2: Implement the minimal shared state-machine change

**Files:**
- Modify: `esp32/shared/badge_con_game.h`
- Modify: `esp32/shared/badge_con_game.c`

**Interfaces:**
- Produces: `BADGE_CON_INFECTED_RESCUE_POINTS` equal to 45.
- Preserves: all existing public game function signatures and snapshot fields.

- [ ] **Step 1: Define the exact rescue constant**

Add to `badge_con_game.h`:

```c
#define BADGE_CON_INFECTED_RESCUE_POINTS 45U
```

- [ ] **Step 2: Initialize infected live states at 45**

In `badge_con_game_activate(...)`, initialize an infected seed with
`BADGE_CON_INFECTED_RESCUE_POINTS`. In the normal-human damage branch, when
damage reaches zero, change role to infected and set the meter to the same
constant before resetting `last_decay_ms`.

- [ ] **Step 3: Make infected lazy decay terminal**

Keep the current wrap-safe elapsed-minute calculation. Subtract complete
minutes only from infected state. When decay reaches or exceeds the meter,
set meter zero and `dead = true`. Expand reachable dead state to permit any
valid seed/scar combination whose role is infected and meter is zero.

- [ ] **Step 4: Apply infected-on-infected pressure**

In the infected-recipient branch, handle an infected peer before the healer
branch:

```c
unsigned damage = (peer_super ? 2U : 1U) * multiplier;
if (damage >= state->shield) {
    state->shield = 0U;
    state->dead = true;
    return BADGE_CON_EFFECT_DIED;
}
state->shield = (uint8_t)(state->shield - damage);
return BADGE_CON_EFFECT_SHIELD_DRAINED;
```

After lazy time is applied in `badge_con_game_apply_peer(...)`, return
`BADGE_CON_EFFECT_DIED` immediately if elapsed decay reached zero so a healer
arriving at that exact deadline cannot revive an already-dead player.

- [ ] **Step 5: Run native tests and prove GREEN**

Run the full native command from Task 1. Require every test to pass, including
existing codec, encounter, display-policy, persistence, update-policy, and
wraparound tests.

- [ ] **Step 6: Commit the state machine**

```sh
git add esp32/shared/badge_con_game.h esp32/shared/badge_con_game.c \
  esp32/test/test_badge_con_game.c esp32/test/test_runner.c
git commit -m "game: add infected rescue countdown"
```

### Task 3: Cut and strictly verify the private `.92` candidate

**Files:**
- Modify: `esp32/shared/version.h`
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `scripts/test_verified_badge_artifacts.py`
- Modify: `docs/badge/con-crud-canary-acceptance.md`
- Create: `docs/badge/con-crud-0.64.92-acceptance.json`

**Interfaces:**
- Produces: exact private
  `0.64.92-badge-defcon34` uplink/scanner artifacts and pending acceptance
  metadata.

- [ ] **Step 1: Bump only the private badge canary**

Set `FOF_VERSION_BADGE_CANARY` and actual-version assertions to
`0.64.92-badge-defcon34`. Leave production versions unchanged.

- [ ] **Step 2: Run version and artifact tests RED then update metadata**

Run:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
/Users/billh/.platformio/penv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py -q
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_verified_badge_artifacts
```

Update the `.92` pending manifest and acceptance test candidate path only
after strict builds provide exact hashes and sizes. Keep
`"physically_accepted": false`.

- [ ] **Step 3: Build sequentially**

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32/scanner
/Users/billh/.platformio/penv/bin/pio run \
  -e scanner-s3-combo-fof_badge-con-crud-canary
cd ../uplink
/Users/billh/.platformio/penv/bin/pio run \
  -e uplink-s3-fof_badge-con-crud-canary
```

Never build scanner and uplink concurrently.

- [ ] **Step 4: Run unchanged strict verifiers**

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
/Users/billh/.platformio/penv/bin/python \
  esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge
/Users/billh/.platformio/penv/bin/python \
  esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

Stop before hardware if either unchanged RAM/image ceiling fails.

- [ ] **Step 5: Run focused regression suites**

```sh
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_fof_badge_flash
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_verified_badge_artifacts
/Users/billh/.platformio/penv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py -q
```

- [ ] **Step 6: Record hashes/headroom and commit `.92` locally**

Write exact SHA-256, image sizes, RAM use, and remaining headroom to the `.92`
pending manifest and canary ledger. Commit locally without push or promotion.

### Task 4: Update the three connected canary badges and collect evidence

**Files:**
- Modify only after evidence:
  `docs/badge/con-crud-0.64.92-acceptance.json`
- Modify only after evidence:
  `docs/badge/con-crud-canary-acceptance.md`

**Interfaces:**
- Consumes: the exact frozen `.92` artifacts from Task 3.
- Produces: descriptor-bound update evidence while leaving physical game
  acceptance pending for the waking operator.

- [ ] **Step 1: Re-enumerate and bind exact uplinks**

Require A=`E0:72:A1:F8:4C:68`, healer C=`E0:72:A1:F9:47:FC`, and infected
B=`E0:72:A1:F8:86:74`. Stop rather than guessing if identities or ports
change.

- [ ] **Step 2: Update each complete trio through its uplink**

Use `scripts/fof_badge_flash.py` with the canary platform, explicit uplink
port, `--only all`, `--skip-build`, and `--bind-selected-uplink`. If a
multi-lane identity/begin handshake fails before bytes, let it recover to
normal and retry only the stale `ble` or `wifi` lane. Never direct-flash a
scanner.

- [ ] **Step 3: Verify all nine application roles**

Require `.92` on three uplinks and six scanners, both scanner health values
`ok`, fixed BLE/Wi-Fi roles acknowledged, `recovery_mode=normal`,
`scanner_uart_alive=true`, no rollback/safe mode, and preserved seeds
normal/healer/infected.

- [ ] **Step 4: Leave physical acceptance pending**

Do not set `physically_accepted=true` while the operator is asleep. Record
only machine-verifiable update and health evidence. The waking test must prove
45-minute-equivalent lazy decay through controlled timestamps or a test
command, proximity acceleration, healer reversal, permanent death screen,
and infection spread from a dead badge.
