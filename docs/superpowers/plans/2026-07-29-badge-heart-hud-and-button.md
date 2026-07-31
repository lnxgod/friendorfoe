# Badge Heart HUD and Button Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the cramped CON CRUD health copy with a role-colored heart
and numeric value, pulse only the existing bottom strip for game activity, and
map OK tap/hold to detail/Easter actions without changing scanning, combat,
Menu gestures, the reset chord, or firmware updating.

**Architecture:** Add two allocation-free pure policies under `esp32/shared`:
one derives a HUD plan from the existing game snapshot and one classifies the
debounced OK button. The canary ST7735 renderer consumes those policies while
retaining scanner/safe-USB priority; production OK behavior is compiled
unchanged. Tests establish exact colors, geometry, gesture boundaries, timer
wrap, and reset-chord priority before renderer integration.

**Tech Stack:** ESP32-S3, ESP-IDF/PlatformIO C11, FreeRTOS, Unity native tests
with AddressSanitizer, Python firmware/version/flasher verification, and the
existing descriptor-bound USB-to-uplink/UART-to-scanner updater.

## Global Constraints

- Work only on private branch `codex/defcon34-badge-final` in
  `.worktrees/defcon34-badge-final`.
- Preserve untracked `.camera-before-zoom.jpg`.
- Keep the four display lanes, scanner radio behavior, BLE/UART protocol,
  CON CRUD math and eight-second cadence, task graph, framebuffer allocation,
  USB commands, updater architecture, partitions, and factory bundle
  unchanged.
- Keep Menu single-tap navigation, Menu double-tap detail, Menu long-press
  investigation, and both-buttons-for-10-seconds recovery unchanged.
- The OK tap cutoff remains the existing `850U` milliseconds: `<850` is a
  detail tap; `850..5999` performs no action; `>=6000` triggers Easter once.
- A chord, quiet mode, boot-held OK, or already-visible Easter presentation
  suppresses the OK policy until full release.
- The heart/readout colors are exact RGB565 values: human/cured human
  `0xF800`, infected/super zombie `0x07E0`, healer `0xF81F`.
- The physical backlight remains untouched because it is hardwired to 3.3 V.
  Only the existing 12-pixel bottom strip may pulse.
- Preserve the immutable `0.67.0-badge-defcon34` artifacts. Source changes
  become a new private `0.67.1-badge-defcon34` scanner/uplink candidate so the
  updater can prove it is newer.
- Do not exceed 180,224 bytes scanner internal RAM, 1,363,148 bytes scanner
  app image, 212,992 bytes uplink internal RAM, or the exact 1,468,016-byte
  uplink app gate. The uplink app gate is a one-time 10-byte exception to the
  former 70% threshold, capped at its first 16-byte alignment boundary
  (about 70.001%) with 629,136 bytes of OTA-slot headroom; it does not permit
  another aligned block or an open-ended relaxation.
- Do not push, tag, merge, release, publish, or replace the public factory
  firmware.
- Do not flash hardware until native, version, flasher, USB-hardening, build,
  and strict artifact gates pass.

## File map

- `esp32/shared/badge_con_presentation.h/.c` owns the pure live HUD plan.
- `esp32/shared/badge_display_contract.h` owns compile-time bottom-strip and
  heart geometry.
- `esp32/shared/badge_ok_button_policy.h/.c` owns the pure OK tap/hold policy.
- `esp32/test/test_badge_con_presentation.c`,
  `esp32/test/test_badge_display_contract.c`, and the new
  `esp32/test/test_badge_ok_button_policy.c` own behavioral regressions.
- `esp32/test/test_runner.c` and `esp32/platformio.ini` register native tests
  and the new pure policy source.
- `esp32/uplink/main/hw/display_st7735.c` is the only runtime integration
  point. No scanner runtime file changes.
- `esp32/shared/version.h` and
  `backend/tests/test_firmware_build_version.py` own the private candidate
  identity.
- `docs/badge/con-crud-canary-acceptance.md` records exact final evidence only
  after it is observed.

---

### Task 1: Lock HUD and OK-button behavior with failing tests

**Files:**
- Modify: `esp32/shared/badge_con_presentation.h`
- Create: `esp32/shared/badge_ok_button_policy.h`
- Modify: `esp32/test/test_badge_con_presentation.c`
- Modify: `esp32/test/test_badge_display_contract.c`
- Create: `esp32/test/test_badge_ok_button_policy.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Produces:
  `badge_con_hud_plan_t badge_con_presentation_hud(const badge_con_snapshot_t *)`.
- Produces:
  `badge_ok_button_policy_update(badge_ok_button_policy_t *, bool, bool, uint32_t)`.
- Consumes the existing `badge_power_chord_update()` and dispatch gate only in
  a composite regression; neither power-chord file changes.

- [ ] **Step 1: Declare the HUD contract before implementing it**

Add to `badge_con_presentation.h`:

```c
enum {
    BADGE_CON_HUD_HUMAN_RGB565 = 0xF800U,
    BADGE_CON_HUD_INFECTED_RGB565 = 0x07E0U,
    BADGE_CON_HUD_HEALER_RGB565 = 0xF81FU,
};

typedef struct {
    badge_con_present_state_t state;
    uint16_t color_rgb565;
    uint8_t current;
    uint8_t maximum;
    bool visible;
} badge_con_hud_plan_t;

badge_con_hud_plan_t badge_con_presentation_hud(
    const badge_con_snapshot_t *snapshot);
```

- [ ] **Step 2: Add live and hidden HUD tests**

Add these exact tests to `test_badge_con_presentation.c`:

```c
void test_badge_con_presentation_maps_live_hud_values_and_colors(void)
{
    badge_con_snapshot_t snapshot = {
        .role = BADGE_CON_ROLE_NORMAL,
        .active = true,
        .shield = 49U,
        .maximum = 50U,
    };

    badge_con_hud_plan_t hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_TRUE(hud.visible);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_HUMAN, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_HUMAN_RGB565, hud.color_rgb565);
    TEST_ASSERT_EQUAL_UINT8(49U, hud.current);
    TEST_ASSERT_EQUAL_UINT8(50U, hud.maximum);

    snapshot.cured = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_HUMAN, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_HUMAN_RGB565, hud.color_rgb565);

    snapshot.role = BADGE_CON_ROLE_INFECTED;
    snapshot.shield = 45U;
    snapshot.maximum = 25U;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_INFECTED, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_INFECTED_RGB565, hud.color_rgb565);
    TEST_ASSERT_EQUAL_UINT8(45U, hud.current);
    TEST_ASSERT_EQUAL_UINT8(100U, hud.maximum);

    snapshot.super = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_SUPER, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_INFECTED_RGB565, hud.color_rgb565);

    snapshot.super = false;
    snapshot.role = BADGE_CON_ROLE_IMMUNE;
    snapshot.shield = 91U;
    snapshot.maximum = 100U;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_EQUAL(BADGE_CON_PRESENT_IMMUNE, hud.state);
    TEST_ASSERT_EQUAL_HEX16(BADGE_CON_HUD_HEALER_RGB565, hud.color_rgb565);
}

void test_badge_con_presentation_hides_inactive_and_dead_hud(void)
{
    badge_con_hud_plan_t hud = badge_con_presentation_hud(NULL);
    TEST_ASSERT_FALSE(hud.visible);

    badge_con_snapshot_t snapshot = {
        .role = BADGE_CON_ROLE_NORMAL,
        .active = false,
        .shield = 100U,
        .maximum = 100U,
    };
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_FALSE(hud.visible);

    snapshot.active = true;
    snapshot.dead = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_FALSE(hud.visible);

    snapshot.super = true;
    hud = badge_con_presentation_hud(&snapshot);
    TEST_ASSERT_FALSE(hud.visible);
}
```

- [ ] **Step 3: Add the exact heart-geometry contract test**

Extend `test_badge_display_contract.c`:

```c
void test_badge_display_contract_keeps_heart_inside_health_strip(void)
{
    TEST_ASSERT_EQUAL_INT(
        160,
        BADGE_DISPLAY_HEALTH_STRIP_Y +
            BADGE_DISPLAY_HEALTH_STRIP_HEIGHT);
    TEST_ASSERT_EQUAL_INT(
        BADGE_DISPLAY_HEALTH_STRIP_HEIGHT,
        BADGE_DISPLAY_HEALTH_VALUE_Y_OFFSET +
            BADGE_DISPLAY_HEART_HEIGHT);
    TEST_ASSERT_EQUAL_INT(7, BADGE_DISPLAY_HEART_WIDTH);
    TEST_ASSERT_EQUAL_INT(5, BADGE_DISPLAY_HEART_HEIGHT);
}
```

- [ ] **Step 4: Declare the pure OK-button policy**

Create `badge_ok_button_policy.h`:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_OK_BUTTON_ACTION_NONE = 0,
    BADGE_OK_BUTTON_ACTION_DETAIL,
    BADGE_OK_BUTTON_ACTION_EASTER,
} badge_ok_button_action_t;

typedef struct {
    uint32_t tap_cutoff_ms;
    uint32_t easter_hold_ms;
    uint32_t pressed_at_ms;
    bool tracking;
    bool easter_dispatched;
    bool suppress_until_release;
} badge_ok_button_policy_t;

void badge_ok_button_policy_init(
    badge_ok_button_policy_t *state,
    uint32_t tap_cutoff_ms,
    uint32_t easter_hold_ms,
    bool button_pressed_at_init);

badge_ok_button_action_t badge_ok_button_policy_update(
    badge_ok_button_policy_t *state,
    bool button_pressed,
    bool dispatch_allowed,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 5: Add boundary, suppression, wrap, and chord tests**

Create `test_badge_ok_button_policy.c`. Include
`badge_ok_button_policy.h` and `badge_power_chord.h`. Cover these exact
boundaries:

```c
void test_badge_ok_button_policy_maps_tap_dead_zone_and_hold(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);

    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 1000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_DETAIL,
        badge_ok_button_policy_update(&policy, false, true, 1849U));

    badge_ok_button_policy_update(&policy, true, true, 2000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 2850U));

    badge_ok_button_policy_update(&policy, true, true, 3000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 8999U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_EASTER,
        badge_ok_button_policy_update(&policy, true, true, 9000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 10000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 10001U));
}
```

Add these exact tests:

```c
void test_badge_ok_button_policy_release_at_hold_threshold_dispatches_once(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);
    badge_ok_button_policy_update(&policy, true, true, 1000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_EASTER,
        badge_ok_button_policy_update(&policy, false, true, 7000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 7001U));
}

void test_badge_ok_button_policy_suppresses_chord_until_fresh_press(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, false);
    badge_ok_button_policy_update(&policy, true, true, 1000U);
    badge_ok_button_policy_update(&policy, true, false, 2000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 8000U));
    badge_ok_button_policy_update(&policy, true, true, 9000U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_DETAIL,
        badge_ok_button_policy_update(&policy, false, true, 9800U));
}

void test_badge_ok_button_policy_ignores_boot_hold_and_handles_wrap(void)
{
    badge_ok_button_policy_t policy;
    badge_ok_button_policy_init(&policy, 850U, 6000U, true);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, true, true, 9000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 9010U));

    badge_ok_button_policy_update(
        &policy, true, true, UINT32_MAX - 2999U);
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_EASTER,
        badge_ok_button_policy_update(&policy, true, true, 3000U));
    TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
        badge_ok_button_policy_update(&policy, false, true, 3001U));
}
```

Prove the combined reset priority in a fourth exact test:

```c
void test_badge_ok_button_policy_preserves_ten_second_chord_priority(void)
{
badge_ok_button_policy_t policy;
badge_power_chord_t chord;
badge_power_chord_dispatch_gate_t gate;
badge_power_chord_init(&chord, 10000U, false, false, 0U);
badge_power_chord_dispatch_gate_init(&gate, false, false);
badge_ok_button_policy_init(&policy, 850U, 6000U, false);

TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_NONE,
    badge_power_chord_update(&chord, true, true, true, 1000U));
bool suppress = badge_power_chord_dispatch_gate_update(
    &gate, true, true, BADGE_POWER_CHORD_NONE);
TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
    badge_ok_button_policy_update(&policy, true, !suppress, 1000U));
TEST_ASSERT_EQUAL(BADGE_OK_BUTTON_ACTION_NONE,
    badge_ok_button_policy_update(&policy, true, false, 7000U));
TEST_ASSERT_EQUAL(BADGE_POWER_CHORD_RESET,
    badge_power_chord_update(&chord, true, true, true, 11000U));
}
```

- [ ] **Step 6: Register every new Unity test**

Add declarations and matching `RUN_TEST(...)` entries in `test_runner.c`
beside the existing presentation, display-contract, gesture, and power-chord
tests. Use these exact OK-policy names:

```c
test_badge_ok_button_policy_maps_tap_dead_zone_and_hold
test_badge_ok_button_policy_release_at_hold_threshold_dispatches_once
test_badge_ok_button_policy_suppresses_chord_until_fresh_press
test_badge_ok_button_policy_ignores_boot_hold_and_handles_wrap
test_badge_ok_button_policy_preserves_ten_second_chord_priority
```

Keep the current Menu gesture tests registered unchanged.

- [ ] **Step 7: Run one native RED phase**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test
```

Expected: compilation/linking fails only because the new HUD function,
geometry constants, and OK policy implementation do not exist yet.

- [ ] **Step 8: Commit the failing contracts locally**

```bash
git add esp32/shared/badge_con_presentation.h \
  esp32/shared/badge_ok_button_policy.h \
  esp32/test/test_badge_con_presentation.c \
  esp32/test/test_badge_display_contract.c \
  esp32/test/test_badge_ok_button_policy.c \
  esp32/test/test_runner.c
git commit -m "test: lock badge heart HUD and OK gestures"
```

---

### Task 2: Implement the pure allocation-free policies

**Files:**
- Modify: `esp32/shared/badge_con_presentation.c`
- Modify: `esp32/shared/badge_display_contract.h`
- Create: `esp32/shared/badge_ok_button_policy.c`
- Modify: `esp32/platformio.ini`

**Interfaces:**
- Consumes the Task 1 declarations exactly.
- Produces a stack-only HUD value and a six-field button state with no heap,
  FreeRTOS, GPIO, display, scanner, updater, or game-runtime dependency.

- [ ] **Step 1: Implement the HUD mapping through existing state selection**

Add:

```c
badge_con_hud_plan_t badge_con_presentation_hud(
    const badge_con_snapshot_t *snapshot)
{
    badge_con_hud_plan_t hud = {0};
    hud.state = badge_con_presentation_select(snapshot);
    if (!snapshot) {
        return hud;
    }

    switch (hud.state) {
    case BADGE_CON_PRESENT_HUMAN:
    case BADGE_CON_PRESENT_CURED:
        hud.color_rgb565 = BADGE_CON_HUD_HUMAN_RGB565;
        hud.maximum = snapshot->maximum;
        break;
    case BADGE_CON_PRESENT_INFECTED:
    case BADGE_CON_PRESENT_SUPER:
        hud.color_rgb565 = BADGE_CON_HUD_INFECTED_RGB565;
        hud.maximum = 100U;
        break;
    case BADGE_CON_PRESENT_IMMUNE:
        hud.color_rgb565 = BADGE_CON_HUD_HEALER_RGB565;
        hud.maximum = snapshot->maximum;
        break;
    case BADGE_CON_PRESENT_INACTIVE:
    case BADGE_CON_PRESENT_DEAD:
    case BADGE_CON_PRESENT_DEAD_SUPER:
    default:
        return hud;
    }

    hud.current = snapshot->shield;
    hud.visible = true;
    return hud;
}
```

- [ ] **Step 2: Add compile-time strip geometry**

Extend the enum in `badge_display_contract.h`:

```c
BADGE_DISPLAY_HEALTH_STRIP_HEIGHT = 12,
BADGE_DISPLAY_HEALTH_VALUE_Y_OFFSET = 7,
BADGE_DISPLAY_HEART_WIDTH = 7,
BADGE_DISPLAY_HEART_HEIGHT = 5,
```

- [ ] **Step 3: Implement the isolated OK policy**

Create `badge_ok_button_policy.c`:

```c
#include "badge_ok_button_policy.h"

static void finish_press(badge_ok_button_policy_t *state)
{
    state->tracking = false;
    state->easter_dispatched = false;
    state->suppress_until_release = false;
}

void badge_ok_button_policy_init(
    badge_ok_button_policy_t *state,
    uint32_t tap_cutoff_ms,
    uint32_t easter_hold_ms,
    bool button_pressed_at_init)
{
    if (!state) {
        return;
    }
    state->tap_cutoff_ms = tap_cutoff_ms == 0U ? 1U : tap_cutoff_ms;
    state->easter_hold_ms = easter_hold_ms == 0U ? 1U : easter_hold_ms;
    state->pressed_at_ms = 0U;
    state->tracking = button_pressed_at_init;
    state->easter_dispatched = false;
    state->suppress_until_release = button_pressed_at_init;
}

badge_ok_button_action_t badge_ok_button_policy_update(
    badge_ok_button_policy_t *state,
    bool button_pressed,
    bool dispatch_allowed,
    uint32_t now_ms)
{
    if (!state) {
        return BADGE_OK_BUTTON_ACTION_NONE;
    }

    if (button_pressed) {
        if (!state->tracking) {
            state->tracking = true;
            state->pressed_at_ms = now_ms;
            state->easter_dispatched = false;
            state->suppress_until_release = false;
        }
        if (!dispatch_allowed) {
            state->suppress_until_release = true;
        }
        if (!state->suppress_until_release &&
            !state->easter_dispatched &&
            (uint32_t)(now_ms - state->pressed_at_ms) >=
                state->easter_hold_ms) {
            state->easter_dispatched = true;
            return BADGE_OK_BUTTON_ACTION_EASTER;
        }
        return BADGE_OK_BUTTON_ACTION_NONE;
    }

    if (!state->tracking) {
        return BADGE_OK_BUTTON_ACTION_NONE;
    }
    uint32_t held_ms = now_ms - state->pressed_at_ms;
    bool easter = !state->suppress_until_release &&
        !state->easter_dispatched &&
        held_ms >= state->easter_hold_ms;
    bool detail = !state->suppress_until_release &&
        !state->easter_dispatched &&
        held_ms < state->tap_cutoff_ms;
    finish_press(state);
    return easter
        ? BADGE_OK_BUTTON_ACTION_EASTER
        : (detail ? BADGE_OK_BUTTON_ACTION_DETAIL
                  : BADGE_OK_BUTTON_ACTION_NONE);
}
```

- [ ] **Step 4: Register only the native source filter**

Add:

```ini
+<shared/badge_ok_button_policy.c>
```

beside `badge_button_gesture.c` and `badge_power_chord.c` in
`esp32/platformio.ini`. Do not edit scanner CMake. Uplink CMake already
compiles `../../shared`; linker garbage collection keeps unused production
code out.

- [ ] **Step 5: Run the complete native GREEN phase**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test
```

Expected: every Unity test passes under AddressSanitizer, including all
unchanged Menu and 10-second chord regressions.

- [ ] **Step 6: Commit the pure implementation**

```bash
git add esp32/shared/badge_con_presentation.c \
  esp32/shared/badge_display_contract.h \
  esp32/shared/badge_ok_button_policy.c esp32/platformio.ini
git commit -m "badge: add heart HUD and OK gesture policies"
```

---

### Task 3: Integrate the canary renderer and buttons

**Files:**
- Modify: `esp32/test/test_badge_easter_egg.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`

**Interfaces:**
- Consumes `badge_con_presentation_hud()` and
  `badge_ok_button_policy_update()`.
- Produces no new task, timer, queue, lock, packet, persistent state, or
  scanner behavior.

- [ ] **Step 1: Add an integration contract before changing the renderer**

Extend `test_badge_easter_renderer_uses_only_approved_presentation_copy()`:

```c
TEST_ASSERT_NOT_NULL(strstr(source, "badge_con_presentation_hud("));
TEST_ASSERT_NOT_NULL(strstr(source, "badge_ok_button_policy_update("));
TEST_ASSERT_NOT_NULL(strstr(source, "fb_draw_heart_7x5("));
TEST_ASSERT_NULL(strstr(source, "\"UNDER ATTACK\""));
TEST_ASSERT_NULL(strstr(source, "\"HEALING\""));
TEST_ASSERT_NULL(strstr(source, "\"CURE FADING\""));
TEST_ASSERT_NULL(strstr(source, "\"HEALTH\""));
TEST_ASSERT_NULL(strstr(source, "\"CURE\""));
```

These copy checks are limited to the display translation unit; pure tests own
behavioral correctness.

- [ ] **Step 2: Run one integration RED phase**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test
```

Expected: only the new display integration/copy assertions fail.

- [ ] **Step 3: Add the fixed 7-by-5 heart primitive**

Near the existing framebuffer primitives in `display_st7735.c`, add:

```c
static void fb_draw_heart_7x5(int x, int y, uint16_t color)
{
    fb_hline(x + 1, x + 2, y, color);
    fb_hline(x + 4, x + 5, y, color);
    fb_hline(x, x + 6, y + 1, color);
    fb_hline(x, x + 6, y + 2, color);
    fb_hline(x + 1, x + 5, y + 3, color);
    fb_hline(x + 2, x + 4, y + 4, color);
}
```

- [ ] **Step 4: Replace activity words with heart plus number**

After the existing scanner proof and safe-USB values are known, but before the
strip background is filled, construct one canary-only plan:

```c
badge_con_hud_plan_t hud =
    badge_con_presentation_hud(&s_con_render.snapshot);
```

Use the same plan after the fill; keep the game drawing branch gated by
`hud.visible && ok && !safe_usb`. Preserve the role row, but map the
compatibility `BADGE_CON_PRESENT_CURED` slot to `HUMAN`. Replace the
activity/metric strings with:

```c
char value[8];
snprintf(value, sizeof(value), "%u/%u", hud.current, hud.maximum);
int value_w = tiny_pixel_width(value);
int total_w = BADGE_DISPLAY_HEART_WIDTH + 2 + value_w;
int left = (LCD_W - total_w) / 2;
int value_y = y + BADGE_DISPLAY_HEALTH_VALUE_Y_OFFSET;
fb_draw_heart_7x5(left, value_y, hud.color_rgb565);
fb_draw_tiny_string(
    left + BADGE_DISPLAY_HEART_WIDTH + 2,
    value_y,
    value,
    hud.color_rgb565,
    bg);
```

- [ ] **Step 5: Pulse only the existing strip background**

Move the existing `safe_usb` read before the strip fill. Before calculating
foreground contrast and calling `fb_fill_rect`, select a tint from the
existing display-local activity:

```c
uint16_t activity_tint = 0U;
switch (s_con_render.activity) {
case BADGE_CON_RENDER_ACTIVITY_ATTACKED:
case BADGE_CON_RENDER_ACTIVITY_CURE_FADING:
    activity_tint = BADGE_CON_HUD_HUMAN_RGB565;
    break;
case BADGE_CON_RENDER_ACTIVITY_HEALING:
case BADGE_CON_RENDER_ACTIVITY_CURED:
    activity_tint = BADGE_CON_HUD_HEALER_RGB565;
    break;
case BADGE_CON_RENDER_ACTIVITY_INFECTED:
    activity_tint = BADGE_CON_HUD_INFECTED_RGB565;
    break;
case BADGE_CON_RENDER_ACTIVITY_NONE:
default:
    break;
}
if (hud.visible && ok && !safe_usb && activity_tint != 0U) {
    uint8_t mix = (((uint32_t)badge_now_ms() / 250U) & 1U)
        ? 72U : 40U;
    bg = rgb565_mix_color(bg, activity_tint, mix);
}
```

Delete only the game activity strings and `HEALTH`/`CURE` formatter. Do not
touch the scanner proof line, USB label, safe-mode priority, display task
cadence, or full-screen death renderer.

- [ ] **Step 6: Integrate the OK policy only in the private canary**

Include `badge_ok_button_policy.h`, define
`BADGE_BUTTON_EASTER_HOLD_MS 6000U`, and initialize a local policy in
`badge_button_task()`:

```c
#if defined(FOF_DC34_GAME_CANARY)
badge_ok_button_policy_t ok_policy;
badge_ok_button_policy_init(
    &ok_policy,
    BADGE_BUTTON_LONG_MS,
    BADGE_BUTTON_EASTER_HOLD_MS,
    triforce_pressed_at_boot);
#endif
```

Every loop, after the existing chord dispatch gate is updated and before the
reset branch can `continue`, call:

```c
#if defined(FOF_DC34_GAME_CANARY)
badge_ok_button_action_t ok_action = badge_ok_button_policy_update(
    &ok_policy,
    buttons[0].stable_pressed,
    !suppress_single_button_dispatch &&
        !easter_visible_at_batch_start &&
        !badge_power_runtime_is_quiet(),
    (uint32_t)badge_now_ms());
#endif
```

After normal edge handling, dispatch:

```c
#if defined(FOF_DC34_GAME_CANARY)
if (ok_action == BADGE_OK_BUTTON_ACTION_DETAIL) {
    badge_button_working_double_press();
} else if (ok_action == BADGE_OK_BUTTON_ACTION_EASTER) {
    (void)badge_easter_egg_runtime_trigger(
        BADGE_EASTER_EGG_SOURCE_BUTTON);
}
#endif
```

In the existing B1 short-release branch, retain the old immediate Easter
trigger under `#else` for non-canary builds and do nothing under the canary
branch because the pure policy owns it. Leave B2 single/double/long code and
polling unchanged. Existing Easter-visible press-edge advance/dismiss remains
authoritative.

- [ ] **Step 7: Run native GREEN and compile the canary uplink**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/uplink -e uplink-s3-fof_badge-con-crud-canary
```

Expected: all native tests pass and the canary uplink links without growing
past the current partition.

- [ ] **Step 8: Commit the runtime integration**

```bash
git add esp32/test/test_badge_easter_egg.c \
  esp32/uplink/main/hw/display_st7735.c
git commit -m "badge: render role hearts and hold for Easter"
```

---

### Task 4: Identify and verify the private 0.67.1 candidate

**Files:**
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `esp32/shared/version.h`
- Generated only: scanner and uplink PlatformIO build directories

**Interfaces:**
- Produces exact private identity `0.67.1-badge-defcon34` for both canary
  scanner and canary uplink artifacts.
- Leaves all production, sensor, backend API, Android, and public release
  identities unchanged.

- [ ] **Step 1: Change the four canary expectations first**

Replace only the four `0.67.0-badge-defcon34` canary expectations in
`test_firmware_build_version.py` with `0.67.1-badge-defcon34`.

- [ ] **Step 2: Prove the version test RED**

Run:

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py::test_shared_header_selects_production_and_badge_tracks \
  -q
```

Expected: it reports that the header still selects `0.67.0`.

- [ ] **Step 3: Bump only the private badge-canary macro**

Set:

```c
#define FOF_VERSION_BADGE_CANARY "0.67.1-badge-defcon34"
```

- [ ] **Step 4: Run the complete version contract and commit**

Run:

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/python -m pytest \
  backend/tests/test_firmware_build_version.py -q
```

Expected: all 106 firmware-version checks pass.

Commit:

```bash
git add backend/tests/test_firmware_build_version.py esp32/shared/version.h
git commit -m "v0.67.1: identify heart HUD canary"
```

- [ ] **Step 5: Run the final source-level regression gates once**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio test -d esp32 -e test
python3 -m pytest \
  scripts/test_fof_badge_flash.py \
  scripts/test_fof_badge_flash_phase_a_json.py \
  scripts/test_fof_badge_flash_phase_a_serial.py \
  scripts/test_usb_descriptor_binding.py -q
python3 -m unittest scripts.test_verify_badge_usb_hardening -v
```

Expected: the native suite, flasher suite, and 196 USB-hardening checks all
pass. Stop here if any updater regression fails.

- [ ] **Step 6: Build the scanner artifact sequentially**

Run:

```bash
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/scanner \
  -e scanner-s3-combo-fof_badge-con-crud-canary -t clean
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/scanner \
  -e scanner-s3-combo-fof_badge-con-crud-canary
python3 esp32/scripts/verify_badge_scanner_build.py \
  --build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary \
  --partition-source esp32/scanner/partitions_s3_scanner_8mb.csv \
  --sdkconfig esp32/scanner/sdkconfig.scanner-s3-combo-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/scanner/.pio/build/scanner-s3-combo-fof_badge
```

Expected: strict scanner identity, receive-path, isolation, RAM, image, and
hash checks pass.

- [ ] **Step 7: Build the production comparison then canary uplink**

Run sequentially:

```bash
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/uplink -e uplink-s3-fof_badge
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/uplink \
  -e uplink-s3-fof_badge-con-crud-canary -t clean
/Users/billh/.platformio/penv/bin/pio run \
  -d esp32/uplink -e uplink-s3-fof_badge-con-crud-canary
python3 esp32/scripts/verify_badge_uplink_build.py \
  --build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary \
  --partition-source esp32/uplink/partitions_s3_fof_badge_8mb.csv \
  --sdkconfig esp32/uplink/sdkconfig.uplink-s3-fof_badge-con-crud-canary \
  --canary-production-build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge
```

Expected: strict canary/production isolation passes, internal RAM remains at
or below 212,992 bytes, and the app remains at or below the exact
1,468,016-byte one-block exception, leaving 629,136 bytes in the 2 MiB OTA
slot. No additional aligned block is allowed.

- [ ] **Step 8: Inspect the private boundary**

Run:

```bash
git diff --check
git status --short
git log --oneline -10
```

Expected: no whitespace errors; only `.camera-before-zoom.jpg` and generated
ignored artifacts remain outside committed source. Nothing is pushed.

---

### Task 5: Flash and physically accept three complete badge graphs

**Files:**
- Modify only after observation:
  `docs/badge/con-crud-canary-acceptance.md`

**Interfaces:**
- Consumes the exact verified `0.67.1` scanner/uplink artifacts.
- Produces nine healthy applications, readable role HUDs, safe button proof,
  and private acceptance evidence.

- [ ] **Step 1: Bind the current descriptors before mutation**

Take a read-only census and require these identities before using the current
ports:

```text
/dev/cu.usbmodem1101 -> E0:72:A1:F9:47:FC (healer badge)
/dev/cu.usbmodem1201 -> E0:72:A1:F8:4C:68 (normal badge)
/dev/cu.usbmodem1401 -> E0:72:A1:F8:86:74 (infected badge)
```

If a path or identity differs, stop and resolve the descriptor census rather
than guessing.

- [ ] **Step 2: Flash each complete graph sequentially through its uplink**

Run one command at a time:

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

Do not flash badges concurrently. On a zero-byte scanner readiness failure,
retain the transcript, prove the uplink returned to normal, and use only the
documented exact-lane retry. Do not direct-flash scanner USB.

- [ ] **Step 3: Prove all nine applications healthy**

Require every graph to report:

- exact uplink and both scanner versions `0.67.1-badge-defcon34`;
- two unique scanner hardware identities;
- BLE-primary and Wi-Fi-primary role acknowledgments and active radios;
- scanner health `ok`, OTA idle, rollback clear, and zero crashes;
- uplink recovery normal, pending verify false, USB/UART alive, no retained
  update session, and zero crashes;
- original normal/infected/healer seed retained and game inactive after boot.

- [ ] **Step 4: Verify the display on all three roles**

Activate the game through an existing launch path and confirm:

- human: red heart and numeric `current/maximum`;
- infected/super: green heart and numeric `current/100`;
- healer: bright-pink heart and numeric `current/maximum`;
- no `HEALTH`, `CURE`, or transient activity words;
- attack/cure-fading pulses the bottom strip red, healing/cured pulses pink,
  infection pulses green;
- all four scanner lanes remain visible and active.

- [ ] **Step 5: Verify OK/Menu/reset isolation on one canary**

Confirm in order:

1. One quick OK tap opens the exact old Menu-double detail view.
2. A roughly three-second OK hold performs no action.
3. A continuous six-second OK hold opens the existing Easter presentation
   once; release does not advance it.
4. While Easter is visible, its existing press-to-advance/dismiss behavior
   still works.
5. Menu single, double, and long behaviors remain unchanged.
6. Holding both buttons reaches the existing recovery prompt at ten seconds
   without launching Easter; choose Menu/reset to return to the application.

- [ ] **Step 6: Recheck scanning, USB, UART, and memory after interaction**

Take a fresh bounded status sample from each graph. Require the same nine-app
health and no crash, rollback, safe-mode, scanner-role, radio, USB, UART, heap,
PSRAM, or task-stack regression.

- [ ] **Step 7: Record and commit private evidence**

Add a `0.67.1` section to `con-crud-canary-acceptance.md` containing exact test
counts, hashes, RAM/image sizes and margins, three descriptor identities,
nine firmware identities, physical HUD/button observations, and any bounded
retry used.

Commit locally:

```bash
git add docs/badge/con-crud-canary-acceptance.md
git commit -m "docs: record 0.67.1 badge acceptance"
```

Do not push, tag, merge, release, or update the factory bundle.
