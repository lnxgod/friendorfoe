# CON CRUD Role and Activity Ticker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a private `.91` badge candidate whose bottom strip clearly
shows `HUMAN`, `CURED`, `INFECTED`, `HEALER`, or `SUPER ZOMBIE`, reports
recent qualified game effects, and no longer passively damages cured humans.

**Architecture:** Keep gameplay state and radio protocols unchanged except
for the narrow normal-human decay correction. Derive transient activity from
consecutive snapshots inside the canary-only display renderer; do not add
runtime persistence, packet queues, or protocol fields. Reuse the existing
bottom strip and preserve system-error priority.

**Tech Stack:** ESP-IDF/PlatformIO C, Unity native tests, Python artifact and
firmware-version gates, descriptor-bound USB/UART badge flasher.

## Global Constraints

- Work only in `.worktrees/defcon34-badge-final`.
- Keep the work private and commit locally; do not push, tag, merge, release,
  or alter the public factory bundle.
- Do not change scanner radio behavior, the four detection lanes, game packet
  format, NVS/RTC format, or USB status schema.
- Do not relax the 1,468,006-byte uplink image ceiling, 212,992-byte uplink
  internal-RAM ceiling, or either scanner ceiling.
- Flash hardware only after native tests, version tests, clean builds, and
  strict artifact verifiers pass.

---

### Task 1: Correct cured-human passive decay

**Files:**
- Modify: `esp32/test/test_badge_con_game.c`
- Modify: `esp32/test/test_runner.c`
- Modify: `esp32/shared/badge_con_game.c`

**Interfaces:**
- Consumes: `badge_con_game_snapshot(...)`.
- Produces: unchanged gameplay API; normal/cured values remain stable with
  time while infected cure progress still decays once per minute.

- [ ] Add `test_badge_con_game_cured_human_does_not_decay_but_cure_progress_does`
  with a scar-1 normal state at `50/50` and an infected state at `50/100`.
  Snapshot both after 60 seconds and assert the human remains 50 while the
  infected value becomes 49.
- [ ] Register the test in `esp32/test/test_runner.c`.
- [ ] Run `cd esp32 && pio test -e test` and confirm the new assertion fails
  because the cured human becomes 49.
- [ ] Change `apply_lazy_time(...)` so only an infected non-healer follows
  the one-point-per-minute decay branch. Keep healer regeneration unchanged.
- [ ] Run the native suite and require every test to pass.
- [ ] Commit the gameplay correction locally.

### Task 2: Preserve the shared presentation boundary

**Files:**
- Inspect: `esp32/shared/badge_con_presentation.h`
- Inspect: `esp32/shared/badge_con_presentation.c`

**Interfaces:**
- Consumes the existing `badge_con_presentation_select(...)` state selector.
- Produces no new shared API or retained read-only strings.

- [ ] Keep the existing shared presentation state selector unchanged.
- [ ] Keep exact human-facing copy and transient state private to the canary
  display translation unit so production and scanner builds do not grow.
- [ ] Run the native suite and require every existing presentation test to
  pass.

### Task 3: Replace the active-game bottom strip

**Files:**
- Modify: `esp32/uplink/main/hw/display_st7735.c`

**Interfaces:**
- Consumes the existing presentation-state selector.
- Produces a three-second display-local activity hold with no external API.

- [ ] Extend the canary-only render context with the current activity and add
  canary-only previous-role, previous-shield, and frame-hold state.
- [ ] In `badge_con_render_begin()`, classify consecutive active snapshots,
  refresh a 12-frame hold on meaningful transitions, and clear all
  display-local history when the game is inactive.
- [ ] In `draw_scanner_bottom_strip()`, use the game ticker only when both
  scanners are healthy and safe mode is off. Draw the role with the existing
  5x7 font and draw the recent activity or `HEALTH`/`CURE` value with the
  existing tiny font. Preserve the existing health strip otherwise.
- [ ] Remove the old `IMMUNE/HEAL` game-name table and old tiny game line.
- [ ] Run the native suite and build the uplink canary.
- [ ] Run the strict uplink verifier and stop without flashing if either
  image or RAM ceiling fails.
- [ ] Commit the display integration locally.

### Task 4: Cut and verify private `.91` artifacts

**Files:**
- Modify: `esp32/shared/version.h`
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `docs/badge/con-crud-canary-acceptance.md`
- Create: `docs/badge/con-crud-0.64.91-acceptance.json`

**Interfaces:**
- Produces exact `.91` scanner and uplink binaries plus SHA-256 acceptance
  metadata.

- [ ] Change only the private badge canary version to
  `0.64.91-badge-defcon34` and update actual-version assertions.
- [ ] Add the `.91` pending acceptance manifest with exact strict-verifier
  sizes, budgets, and hashes after clean builds.
- [ ] Run the native suite, updater suite, artifact-acceptance tests,
  backend firmware-version tests, and Android JVM unit tests.
- [ ] Clean-build scanner then uplink canary sequentially; never build the two
  PlatformIO targets concurrently.
- [ ] Run both strict artifact verifiers and record the exact hashes and
  headroom. Stop if any ceiling or isolation check fails.
- [ ] Commit the frozen `.91` candidate locally.

### Task 5: Flash and physically accept three badges

**Files:**
- Modify: `docs/badge/con-crud-canary-acceptance.md`
- Modify: `docs/badge/con-crud-0.64.91-acceptance.json` only after all gates
  pass.

**Interfaces:**
- Consumes the exact Task 4 binaries.
- Produces descriptor-bound three-badge update, game, screen, and health
  evidence.

- [ ] Enumerate and bind A=`…4C68`, B=`…8674`, and C=`…47FC`; stop rather
  than guessing if descriptors differ.
- [ ] Update each uplink over USB and relay the exact scanner artifact to both
  UART lanes. Require exact `.91` identity, roles, radio activity, rollback
  clear, safe mode false, USB/UART health, and zero crashes.
- [ ] Seed A=`normal`, B=`infected`, and C=`immune`; prove the seed reboot
  resets live state and all three graphs remain healthy.
- [ ] Activate only A+B. Confirm C remains inactive, A becomes infected, and
  camera inspection shows readable role/activity copy.
- [ ] Activate C. Confirm `HEALER`, healing/cure progress, and `CURED` on
  screen while all scanners continue normal work.
- [ ] Run a bounded health/memory sample, update the ledger, and set
  `physically_accepted=true` only if every required gate passes.
- [ ] Commit physical evidence locally. Do not push or publish.
