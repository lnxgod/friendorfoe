# CON CRUD Property Game Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement and physically accept the balanced, distance-scaled CON CRUD game on three complete badges without regressing normal scanning, USB control, display lanes, or uplink-to-scanner UART updating.

**Architecture:** Keep the existing uplink-only advertiser and scanner-only observer. Extend the authenticated fixed-size frame with one super-zombie flag, keep one compact live game state on the uplink, derive distance from the existing RSSI byte, and reuse the current display framebuffer and WS2812 task. Persist only the factory seed; every reboot starts an inactive run.

**Tech Stack:** ESP32-S3, ESP-IDF/PlatformIO, C11, Unity native tests, Python artifact verifiers, USB serial diagnostics, and the existing guarded uplink/UART firmware updater.

## Global Constraints

- Work only in `/Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final`.
- Keep the feature private: make local commits, but do not push, tag, release, merge, or copy the candidate into the embedded factory bundle during this plan.
- Preserve the current advertisement interval and the exact constants:
  - `BADGE_CON_SERVICE_PAYLOAD_BYTES == 10`
  - `BADGE_CON_LEGACY_ADV_BYTES == 31`
  - `BADGE_CON_UART_LINE_CHARS == 33`
  - `BADGE_CON_UART_WIRE_BYTES == 34`
  - `BADGE_CON_UART_BUFFER_BYTES == 35`
- Do not add a FreeRTOS task, dynamic allocation, peer table, queue, bitmap, framebuffer, scanner advertising role, or floating-point distance calculation.
- Do not increase the strict scanner ceilings of 180,224 internal-RAM bytes and 1,363,148 app-image bytes.
- Do not increase the strict uplink ceilings of 212,992 internal-RAM bytes and 1,468,006 app-image bytes.
- Keep update/recovery/scanner-loss/hardware-error indicators above game presentation priority.
- Keep the existing expected-reboot hook non-null because update maintenance requires it. Its game record becomes seed-only; it must never restore live state.
- Preserve the existing `game_shield` USB field for Android and factory compatibility. Add fields rather than renaming this field.
- Run every red test before its implementation and record the failure in the task notes.
- Commit after each green task with the specified local commit subject.

## File Structure

### Modify

- `esp32/shared/badge_con_protocol.h`
- `esp32/shared/badge_con_protocol.c`
  - Carry the authenticated super-zombie flag in bit 2 and the existing UART role nibble.
- `esp32/shared/badge_con_game.h`
- `esp32/shared/badge_con_game.c`
  - Hold the compact live state, distance tiers, health rules, scar table, lazy decay/regeneration, death, and seed-only persistence.
- `esp32/shared/badge_con_vhci_policy.h`
- `esp32/shared/badge_con_vhci_policy.c`
- `esp32/uplink/main/game/badge_con_vhci.h`
- `esp32/uplink/main/game/badge_con_vhci.c`
  - Refresh the unchanged-cadence advertisement when role or super status changes.
- `esp32/uplink/main/core/badge_con_runtime.h`
- `esp32/uplink/main/core/badge_con_runtime.c`
  - Make all live state RAM-only, forward RSSI/super evidence, and preserve the updater dependency hook without live restoration.
- `esp32/uplink/main/main.c`
- `esp32/uplink/main/comms/uart_rx.c`
  - Propagate current role/super state and retain the existing update-preemption path.
- `esp32/uplink/main/core/serial_config.c`
- `esp32/uplink/main/network/http_status.c`
  - Expose compact game diagnostics while retaining current compatibility fields.
- `esp32/shared/badge_theme.h`
- `esp32/shared/badge_theme.c`
- `esp32/uplink/main/hw/display_st7735.c`
  - Render distinct human, cured, infected, immune, super-zombie, and dead states with existing primitives.
- `esp32/uplink/main/hw/led_status.h`
- `esp32/uplink/main/hw/led_status.c`
  - Add game patterns to the existing LED task.
- `esp32/platformio.ini`
- `esp32/uplink/main/CMakeLists.txt`
  - Compile the small presentation policy into native tests and canary firmware.
- `esp32/shared/version.h`
  - Advance only the private badge canary to `0.64.90-badge-defcon34`.
- `esp32/test/test_badge_con_protocol.c`
- `esp32/test/test_badge_con_game.c`
- `esp32/test/test_badge_con_runtime_policy.c`
- `esp32/test/test_badge_con_vhci_policy.c`
- `esp32/test/test_badge_theme.c`
- `esp32/test/test_runner.c`
  - Replace superseded persistence/game expectations and add the accepted matrix.
- `docs/badge/con-crud-canary-acceptance.md`
  - Record exact candidate hashes, budgets, three-badge results, and promotion status.

### Create

- `esp32/shared/badge_con_presentation.h`
- `esp32/shared/badge_con_presentation.c`
  - Pure allocation-free selection of display and LED game states.
- `esp32/test/test_badge_con_presentation.c`
  - Native presentation and priority tests.
- `docs/badge/con-crud-0.64.90-acceptance.json`
  - Machine-readable exact artifact hashes and physical-acceptance gate for the private factory builder.

---

## Task 1: Extend the Fixed-Size Authenticated Protocol

**Files:**

- Modify: `esp32/shared/badge_con_protocol.h`
- Modify: `esp32/shared/badge_con_protocol.c`
- Modify: `esp32/shared/badge_con_vhci_policy.c`
- Test: `esp32/test/test_badge_con_protocol.c`
- Test: `esp32/test/test_badge_con_observer.c`
- Test: `esp32/test/test_badge_con_vhci_policy.c`
- Modify: `esp32/test/test_runner.c`

- [ ] Add failing protocol tests for regular version-1 frames, authenticated super-infected frames, UART role nibble `5`, a modified flag with a stale tag, super with normal/immune roles, and every remaining reserved-bit combination.
- [ ] Add compile-time assertions in the test that all five payload/UART size constants remain exact.
- [ ] Run the focused native suite and confirm the new tests fail:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

- [ ] Extend the packet and builder interfaces exactly as follows:

```c
typedef struct {
    uint8_t version;
    uint8_t round;
    badge_con_role_t role;
    bool super;
    uint32_t peer;
    uint8_t session;
    uint8_t sequence;
    int8_t rssi;
} badge_con_packet_t;

bool badge_con_build_service_payload(
    badge_con_role_t role,
    bool super,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_SERVICE_PAYLOAD_BYTES]);

bool badge_con_build_legacy_advertisement(
    badge_con_role_t role,
    bool super,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_LEGACY_ADV_BYTES]);
```

- [ ] Encode `super` as bit 2 of payload byte 0 before SipHash. Accept it only when the decoded role is infected. Reject bits 3 and all invalid role/flag combinations.
- [ ] Render UART role as `(uint8_t)role | (super ? 0x4U : 0U)` and parse only `0`, `1`, `2`, and `5`; preserve the exact line width.
- [ ] Update all builder call sites to pass `false` until the runtime/VHCI task wires live super state.
- [ ] Run the full native suite and confirm green.
- [ ] Commit:

```sh
git add esp32/shared/badge_con_protocol.h esp32/shared/badge_con_protocol.c esp32/shared/badge_con_vhci_policy.c esp32/test/test_badge_con_protocol.c esp32/test/test_badge_con_observer.c esp32/test/test_badge_con_vhci_policy.c esp32/test/test_runner.c
git commit -m "v0.64.90: authenticate super zombie flag"
```

## Task 2: Implement the Compact Game State and Distance Rules

**Files:**

- Modify: `esp32/shared/badge_con_game.h`
- Modify: `esp32/shared/badge_con_game.c`
- Modify: `esp32/uplink/main/core/badge_con_runtime.c`
- Test: `esp32/test/test_badge_con_game.c`
- Test: `esp32/test/test_badge_con_runtime_policy.c`
- Modify: `esp32/test/test_runner.c`

- [ ] Replace the superseded game tests with failing tests for all RSSI boundaries: `-45/-46/-52/-53/-60/-61`.
- [ ] Add failing tests for the activation table:
  - normal seed becomes active human at `30/100`;
  - infected seed becomes active infected at `0/100` cure progress;
  - immune seed becomes active immune at `100/100`;
  - inactive snapshots remain seed-only with value zero.
- [ ] Add failing tests for human damage/healing at multipliers 1, 2, and 3; immediate infection on a zero crossing; the scar health cap; and normal peers having no effect.
- [ ] Add failing tests for infected cure progress, one-point-per-minute cure decay, and every scar maximum `100/50/25/12/6/3/1`.
- [ ] Add failing tests for immune lazy regeneration at one point per two seconds, cap 100, same-timestamp ordering, `uint32_t` wrap, regular/super damage, immune healing, independent sequential peers, and zero-shield conversion.
- [ ] Add failing tests that dead state ignores healing/cure and that only an immune-seed death derives super status.
- [ ] Confirm the new game tests fail with the native test command from Task 1.
- [ ] Keep `shield` as the compact generalized current value for ABI churn control, and extend the structs exactly as follows:

```c
typedef struct {
    badge_con_role_t seed;
    badge_con_role_t role;
    bool active;
    uint8_t shield;
    uint8_t scar_level;
    bool dead;
    uint32_t last_decay_ms;
} badge_con_game_state_t;

typedef struct {
    badge_con_role_t seed;
    badge_con_role_t role;
    bool active;
    uint8_t shield;
    uint8_t maximum;
    uint8_t scar_level;
    bool cured;
    bool dead;
    bool super;
} badge_con_snapshot_t;

uint8_t badge_con_game_rssi_multiplier(int8_t rssi);
uint8_t badge_con_game_maximum(uint8_t scar_level);
badge_con_effect_t badge_con_game_apply_peer(
    badge_con_game_state_t *state,
    badge_con_role_t peer_role,
    bool peer_super,
    int8_t peer_rssi,
    uint32_t now_ms);
```

- [ ] Implement the constant maximum table `{100, 50, 25, 12, 6, 3, 1}` and reject unreachable scar/state combinations.
- [ ] Apply lazy time exactly once before a peer effect. Human health and infected cure progress decay one point per 60 seconds; immune shield regenerates one point per two seconds. Rebase `last_decay_ms` on activation and every role transition.
- [ ] Treat lazy human decay reaching zero as the same immediate infection transition so an active human never remains at zero health.
- [ ] Update the runtime's existing game call to pass packet role, super flag, and RSSI into the new signature. Task 4 will add the dedicated runtime/VHCI behavior assertions.
- [ ] Implement damage/healing with saturating integer arithmetic:
  - human: regular infected `10*m`, super infected `20*m`, immune heal `5*m`;
  - infected: immune cure `10*m`;
  - immune: regular infected `1*m`, super infected `2*m`, immune heal `5*m`.
- [ ] On cure at 100, increment scar level first. Restore human at the derived maxima 50 through 3. When the new maximum is 1, set `dead=true`, retain infected role, and do not restore.
- [ ] On immune shield crossing zero, set infected role and `dead=true`; derive `super=true` only from `seed==IMMUNE && dead`.
- [ ] Keep the public effect enum small. Add only `BADGE_CON_EFFECT_DIED`; use the existing gain/drain/infected/cured effects for ordinary transitions.
- [ ] Run the full native suite and confirm green under AddressSanitizer.
- [ ] Commit:

```sh
git add esp32/shared/badge_con_game.h esp32/shared/badge_con_game.c esp32/uplink/main/core/badge_con_runtime.c esp32/test/test_badge_con_game.c esp32/test/test_badge_con_runtime_policy.c esp32/test/test_runner.c
git commit -m "v0.64.90: balance distance scaled CON CRUD"
```

## Task 3: Make Reboot State Seed-Only Without Breaking Update Arming

**Files:**

- Modify: `esp32/shared/badge_con_game.h`
- Modify: `esp32/shared/badge_con_game.c`
- Modify: `esp32/uplink/main/core/badge_con_runtime.c`
- Test: `esp32/test/test_badge_con_game.c`
- Test: `esp32/test/test_badge_con_runtime_policy.c`
- Modify: `esp32/test/test_runner.c`

- [ ] Add failing tests proving both current schema and legacy `.89` NVS records retain only a valid seed and discard active/role/value/scar/death state.
- [ ] Add failing runtime tests for cold reboot, ordinary software reboot, expected firmware-update reboot, stale generation, and corrupt RTC. Every case must retain the seed and return inactive with zero live value, scar zero, death false, and super false.
- [ ] Add a failing test proving the expected-reboot hook remains non-null and returns success for update arming without preserving live state.
- [ ] Add failing tests proving activation, encounters, snapshots, cures, and deaths perform no NVS write; factory seed provisioning performs exactly one durable NVS write.
- [ ] Confirm these tests fail before implementation.
- [ ] Keep `BADGE_CON_NVS_RECORD_BYTES == 12` and `BADGE_CON_RTC_RECORD_BYTES == 20` so the retained-memory layout and updater ABI do not move.
- [ ] Encode NVS schema version 2 as magic, version, seed, five zero bytes, and CRC32. Decode valid version-1 records for migration, but return `BADGE_CON_NVS_SEED_ONLY` and reset every mutable field.
- [ ] Encode the expected-reboot RTC dependency record as seed-only with generation and CRC. Never decode it into live game state during runtime initialization.
- [ ] Remove NVS persistence from activation, peer effects, snapshot decay/regeneration, cure, infection, and death. Keep persistence only in `badge_con_runtime_set_factory_seed`.
- [ ] Keep `badge_runtime_set_expected_reboot_hook(expected_reboot_hook)` registered. The hook writes the seed-only RTC dependency record required by `update_maintenance` and `usb_uplink_ota`.
- [ ] Clear or overwrite legacy live RTC bytes during initialization so a downgrade/upgrade cycle cannot revive them.
- [ ] Run all native tests and confirm green.
- [ ] Commit:

```sh
git add esp32/shared/badge_con_game.h esp32/shared/badge_con_game.c esp32/uplink/main/core/badge_con_runtime.c esp32/test/test_badge_con_game.c esp32/test/test_badge_con_runtime_policy.c esp32/test/test_runner.c
git commit -m "v0.64.90: reset live game state on every reboot"
```

## Task 4: Wire RSSI and Super State Through Runtime and Advertising

**Files:**

- Modify: `esp32/shared/badge_con_vhci_policy.h`
- Modify: `esp32/shared/badge_con_vhci_policy.c`
- Modify: `esp32/uplink/main/game/badge_con_vhci.h`
- Modify: `esp32/uplink/main/game/badge_con_vhci.c`
- Modify: `esp32/uplink/main/core/badge_con_runtime.c`
- Modify: `esp32/uplink/main/main.c`
- Test: `esp32/test/test_badge_con_runtime_policy.c`
- Test: `esp32/test/test_badge_con_vhci_policy.c`
- Modify: `esp32/test/test_runner.c`

- [ ] Add failing runtime tests that pass packets at every RSSI tier and prove `packet.super` doubles only infected damage.
- [ ] Add failing VHCI tests proving the policy emits regular infected and super-infected authenticated payloads at the existing one-second payload epoch.
- [ ] Add failing tests proving a role/super change refreshes data without changing advertising parameters, cadence, updater inhibition, retry budget, or quiescence semantics.
- [ ] Confirm the tests fail.
- [ ] Replace role-only setters with:

```c
void badge_con_vhci_policy_set_identity_state(
    badge_con_vhci_policy_t *policy,
    badge_con_role_t role,
    bool super);

void badge_con_vhci_set_identity_state(
    badge_con_role_t role,
    bool super);
```

- [ ] Add one `bool super` to `badge_con_vhci_policy_t`. Validate it with the same infected-only rule as the protocol.
- [ ] In `badge_con_runtime_apply_qualified_peer`, pass `packet->role`, `packet->super`, and `packet->rssi` directly to the game function. Do not add a second RSSI history.
- [ ] In the uplink radio poll, advertise `snapshot.role` and `snapshot.super`; dead badges remain active infected advertisers.
- [ ] Preserve the current scanner observer, eight-peer table, three-packet/six-second quorum, one-effect-per-peer-per-second limiter, and update inhibition without structural changes.
- [ ] Run the native suite and confirm green.
- [ ] Commit:

```sh
git add esp32/shared/badge_con_vhci_policy.h esp32/shared/badge_con_vhci_policy.c esp32/uplink/main/game/badge_con_vhci.h esp32/uplink/main/game/badge_con_vhci.c esp32/uplink/main/core/badge_con_runtime.c esp32/uplink/main/main.c esp32/test/test_badge_con_runtime_policy.c esp32/test/test_badge_con_vhci_policy.c esp32/test/test_runner.c
git commit -m "v0.64.90: propagate distance and super state"
```

## Task 5: Add Stable USB and HTTP Diagnostics

**Files:**

- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/network/http_status.c`
- Test: `esp32/test/test_badge_con_runtime_policy.c`
- Test: existing Python USB/status schema tests under `scripts/`

- [ ] Add failing assertions for the backward-compatible status fields and the new compact fields:

```json
{
  "game_seed": "normal",
  "game_state": "normal",
  "game_active": true,
  "game_shield": 30,
  "game_max": 100,
  "game_scar": 0,
  "game_cured": false,
  "game_dead": false,
  "game_super": false
}
```

- [ ] Ensure `game_shield` continues to mean the current game value: human health, infected cure progress, or immune shield.
- [ ] Add the fields to both USB status and local HTTP status using bounded existing render buffers; do not add a new response object or allocation.
- [ ] Run:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m unittest scripts.test_fof_badge_flash -v
python3 -m unittest scripts.test_user_visible_redaction -v
```

- [ ] Confirm no raw MAC, token, or secret appears in public output.
- [ ] Commit:

```sh
git add esp32/uplink/main/core/serial_config.c esp32/uplink/main/network/http_status.c esp32/test/test_badge_con_runtime_policy.c scripts/test_fof_badge_flash.py scripts/test_user_visible_redaction.py
git commit -m "v0.64.90: expose compact game health status"
```

## Task 6: Select Presentation States With a Pure Policy

**Files:**

- Create: `esp32/shared/badge_con_presentation.h`
- Create: `esp32/shared/badge_con_presentation.c`
- Create: `esp32/test/test_badge_con_presentation.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Modify: `esp32/test/test_runner.c`

- [ ] Write failing tests for inactive, human, cured, infected, immune, super-zombie, dead-regular, and dead-super snapshots.
- [ ] Write failing priority tests proving update maintenance, safe/recovery mode, scanner loss, and hardware error suppress game LED selection.
- [ ] Define the allocation-free interface:

```c
typedef enum {
    BADGE_CON_PRESENT_INACTIVE = 0,
    BADGE_CON_PRESENT_HUMAN,
    BADGE_CON_PRESENT_CURED,
    BADGE_CON_PRESENT_INFECTED,
    BADGE_CON_PRESENT_IMMUNE,
    BADGE_CON_PRESENT_SUPER,
    BADGE_CON_PRESENT_DEAD,
    BADGE_CON_PRESENT_DEAD_SUPER,
} badge_con_present_state_t;

badge_con_present_state_t badge_con_presentation_select(
    const badge_con_snapshot_t *snapshot);

bool badge_con_presentation_game_led_allowed(
    bool update_active,
    bool safe_or_recovery,
    bool both_scanners_healthy,
    bool hardware_error);
```

- [ ] Add `badge_con_presentation.c` to the native source filter. The uplink component already compiles `shared/`; add an explicit canary-only target source only if CMake does not include it automatically.
- [ ] Run native tests and confirm green.
- [ ] Commit:

```sh
git add esp32/shared/badge_con_presentation.h esp32/shared/badge_con_presentation.c esp32/test/test_badge_con_presentation.c esp32/platformio.ini esp32/uplink/main/CMakeLists.txt esp32/test/test_runner.c
git commit -m "v0.64.90: centralize game presentation policy"
```

## Task 7: Render the Six Distinct Game Experiences

**Files:**

- Modify: `esp32/shared/badge_theme.h`
- Modify: `esp32/shared/badge_theme.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Test: `esp32/test/test_badge_theme.c`
- Test: `esp32/test/test_badge_con_presentation.c`
- Modify: `esp32/test/test_runner.c`

- [ ] Add failing palette tests for readable human green, cured cyan, toxic infected purple/green, hot-pink immune, and aggressive super treatments under the existing brightness/contrast rules.
- [ ] Extend `badge_theme_derive_con_palette` to accept `badge_con_present_state_t` instead of only role. Keep colors fixed in firmware; full hex game theming remains outside this build.
- [ ] In the existing dashboard render pass:
  - human retains all normal lanes and receives a `HEALTH current/max` HUD;
  - cured retains all lanes and adds persistent `CURED` plus the reduced maximum;
  - infected retains all lanes and adds procedural contamination marks plus `INFECTED`;
  - immune retains all lanes and adds `IMMUNE / HEALER` plus shield;
  - super retains all lanes and adds `SUPER ZOMBIE x2`;
  - dead replaces the lane dashboard with `YOU DIED OF CON CRUD`, the existing GameChangersAI logo, `REBOOT TO FIX`, and a bounded frame-counter glitch.
- [ ] Use only existing framebuffer drawing primitives, fonts, logo asset, and frame counter. Add no image or animation buffer.
- [ ] Keep menus, USB flash mode, update/recovery screens, and scanner-error screens reachable and above the game overlay. The dead lock applies only to the normal dashboard until reboot.
- [ ] Run native tests and both canary builds once to catch display compile errors.
- [ ] Commit:

```sh
git add esp32/shared/badge_theme.h esp32/shared/badge_theme.c esp32/uplink/main/hw/display_st7735.c esp32/test/test_badge_theme.c esp32/test/test_badge_con_presentation.c esp32/test/test_runner.c
git commit -m "v0.64.90: render unmistakable CON CRUD states"
```

## Task 8: Reuse the Existing LED Task

**Files:**

- Modify: `esp32/uplink/main/hw/led_status.h`
- Modify: `esp32/uplink/main/hw/led_status.c`
- Modify: `esp32/uplink/main/main.c`
- Test: `esp32/test/test_badge_con_presentation.c`

- [ ] Add game patterns to the existing enum and static pattern/color tables:
  - human green;
  - cured cyan;
  - infected red;
  - immune blue;
  - super rapid red/purple;
  - dead regular red and dead-super rapid red/purple.
- [ ] Add no task, queue, timer, heap allocation, or second RMT device.
- [ ] In the existing display/health loop, compute the current system pattern first. Apply a game pattern only when `badge_con_presentation_game_led_allowed` returns true. Update maintenance, safe/recovery, both-scanner health, Wi-Fi/hardware error, and explicit error states retain precedence.
- [ ] Run native tests and the uplink canary build.
- [ ] Commit:

```sh
git add esp32/uplink/main/hw/led_status.h esp32/uplink/main/hw/led_status.c esp32/uplink/main/main.c esp32/test/test_badge_con_presentation.c
git commit -m "v0.64.90: add game colors to existing badge LED"
```

## Task 9: Bump the Private Candidate and Run Every Automated Gate

**Files:**

- Modify: `esp32/shared/version.h`
- Modify: version assertions in affected backend/script tests
- Modify: `docs/badge/con-crud-canary-acceptance.md`

- [ ] Set only `FOF_VERSION_BADGE_CANARY` to `0.64.90-badge-defcon34`. Leave `FOF_VERSION_BADGE` and `FOF_VERSION_PROD` unchanged.
- [ ] Run all native tests:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

- [ ] Build unchanged production and private canary scanner targets:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32/scanner
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge-con-crud-canary
```

- [ ] Run the scanner verifier without changing its ceilings:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge
```

- [ ] Build unchanged production and private canary uplink targets:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final/esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge-con-crud-canary
```

- [ ] Run the uplink verifier without changing its ceilings:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

- [ ] Run regression suites:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 -m pytest backend/tests/test_firmware_build_version.py -q
python3 -m unittest scripts.test_verified_badge_artifacts -v
python3 -m unittest scripts.test_fof_badge_flash -v
python3 -m unittest scripts.test_user_visible_redaction -v
python3 -m unittest discover -s tools/badge_flasher/tests -v
cd android
./gradlew testDebugUnitTest
```

- [ ] Record exact `.bin` SHA-256 values, internal RAM, app image, and remaining headroom in the acceptance ledger. Leave promotion blocked.
- [ ] Create `docs/badge/con-crud-0.64.90-acceptance.json` with schema version 1, candidate version, exact uplink/scanner SHA-256 values, verifier budgets, `"physically_accepted": false`, and an empty physical-evidence array. Add the regression coverage to `scripts/test_verified_badge_artifacts.py`; reject malformed hashes, wrong versions, and a true acceptance flag without evidence.
- [ ] Commit:

```sh
git add esp32/shared/version.h docs/badge/con-crud-canary-acceptance.md docs/badge/con-crud-0.64.90-acceptance.json backend/tests/test_firmware_build_version.py scripts/test_verified_badge_artifacts.py
git commit -m "v0.64.90: cut private CON CRUD candidate"
```

## Task 10: Upgrade and Prove the Three Complete Badges

**Files:**

- Modify: `docs/badge/con-crud-canary-acceptance.md`
- Create only under ignored/local evidence directories: serial logs, status JSON, photographs, and hashes

- [ ] Before mutation, enumerate `/dev/cu.usbmodem*`, bind each uplink by immutable hardware ID, and capture fresh status for:
  - Badge A uplink ending `4C68`, currently `.89`;
  - Badge B uplink ending `8674`, currently `.89`;
  - Badge C uplink ending `47FC`, currently `.76`.
- [ ] Freeze one exact `.90` uplink artifact and one exact `.90` scanner artifact by SHA-256. Use those hashes for all three badges.
- [ ] Upgrade Badge C as a complete graph first. Use the existing guarded USB bootstrap/update path; do not direct-flash scanner leaves as the acceptance path.
- [ ] Upgrade Badges A and B through the uplink USB path and have each uplink stage and relay the scanner artifact to both UART lanes.
- [ ] After re-enumeration confirms the current hardware-ID-to-port binding, run the accepted USB flows. The presently known bindings are A=`/dev/cu.usbmodem1101`, B=`/dev/cu.usbmodem1401`, and C=`/dev/cu.usbmodem1201`; stop instead of guessing if any identity moved:

```sh
cd /Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final
python3 scripts/fof_badge_flash.py --transport usb --platform badge-trio-xiao-s3 --port /dev/cu.usbmodem1201 --only all --legacy-usb-bootstrap --bind-selected-uplink
python3 scripts/fof_badge_flash.py --transport usb --platform badge-trio-xiao-s3 --port /dev/cu.usbmodem1101 --only all
python3 scripts/fof_badge_flash.py --transport usb --platform badge-trio-xiao-s3 --port /dev/cu.usbmodem1401 --only all
```

- [ ] After every badge converges, require exact candidate version, immutable three-board identities, BLE/Wi-Fi fixed roles, radio readiness, rollback clear, safe mode false, zero crash loop, USB health, UART health, and stable heap/stack evidence.
- [ ] Provision the three seeds as normal, infected, and immune, reboot each, and prove the game is inactive with live value zero before activation.
- [ ] Activate through the already-approved Easter path and execute the full property matrix:
  - normal begins at 30/100;
  - close/medium/far tiers produce exact 3x/2x/1x effects;
  - immune heals human and immune;
  - immune natural regeneration is one point per two seconds;
  - multiple infected identities stack independently;
  - immune death produces dead-super and authenticated super advertising;
  - super damage is exactly twice regular damage;
  - cure cycles enforce 50/25/12/6/3 and then dead at the attempted 1 maximum;
  - dead ignores healing and cure.
- [ ] Photograph human, cured, infected, immune, super, dead, USB flash/update, and scanner-error presentation. Confirm update/error presentation wins.
- [ ] Reboot each role and prove all live fields reset while the seed persists and Easter activation is required again.
- [ ] During a bounded 15-minute mixed game/scanning/USB soak, verify Remote ID, DJI, privacy, Wi-Fi, and BLE counters continue advancing; no game detection flood, watchdog, UART overflow, queue regression, or monotonic internal-heap decline occurs.
- [ ] Perform one final exact scanner stage from an uplink and update both UART lanes. Require terminal receipts, exact version/identity/role/radio/rollback proof, and no manual scanner USB flash.
- [ ] Update every physical-acceptance row in the ledger with exact evidence and mark the firmware candidate accepted only if every row passes.
- [ ] Set `"physically_accepted": true` in `docs/badge/con-crud-0.64.90-acceptance.json` only after all rows pass. Add the three badge receipts, bounded-soak duration, final updater receipt, and exact accepted binary hashes without storing raw hardware MACs in this tracked manifest.
- [ ] Commit the evidence ledger locally:

```sh
git add docs/badge/con-crud-canary-acceptance.md docs/badge/con-crud-0.64.90-acceptance.json
git commit -m "v0.64.90: accept three badge CON CRUD canary"
```

## Task 11: Final Firmware Review Boundary

- [ ] Inspect `git diff 286e588..HEAD` and verify no Android theme-v2 work, public release work, unrelated firmware feature, or factory embedded-resource change entered the candidate.
- [ ] Re-run the native suite and both strict artifact verifiers from Task 9 against the exact physically accepted build directories.
- [ ] Confirm the acceptance ledger names the exact hashes used on all three badges and still states that no GitHub push/tag/release/merge occurred.
- [ ] Stop at this boundary. Continue with `2026-07-27-badge-factory-batch-inventory.md` only after this firmware plan is fully accepted.
