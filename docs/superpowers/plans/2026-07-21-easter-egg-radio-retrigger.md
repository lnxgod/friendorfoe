# Badge Easter Egg Radio Retrigger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the exact `GameChangersAI-67` SSID and exact Hell, Michigan Remote ID relaunch the badge Easter egg 90 seconds after dismissal while keeping physical-button activation one-shot.

**Architecture:** The portable Easter egg state machine owns a wrap-safe monotonic cooldown and exposes timestamped trigger, advance, and dismiss entry points. Existing untimed entry points remain as compatibility wrappers with one-shot behavior. The badge runtime samples `esp_timer_get_time()` outside its critical section and supplies the low 32 bits in milliseconds.

**Tech Stack:** ESP-IDF C, FreeRTOS critical sections, PlatformIO native Unity tests, PlatformIO badge scanner/uplink builds.

## Global Constraints

- Wi-Fi matching remains exact, case-sensitive bytes equal to `GameChangersAI-67`.
- Remote ID matching remains Basic ID `fof-michagain`, latitude `42.4347200`, longitude `-83.9850000`, and geodetic altitude exactly 666 metres.
- Radio detections while the Easter egg is visible do not restart it.
- The cooldown begins only when the Easter egg is dismissed or leaves the bounce phase.
- Exact radio retrigger is rejected before 90,000 milliseconds and accepted at 90,000 milliseconds.
- Rejected triggers do not extend the cooldown.
- Physical-button activation remains one-shot until reboot.
- No firmware version, factory bundle, release manifest, tag, push, or production badge is changed in this plan.
- Flash only the explicitly authorized connected test badge after all software verification passes.

---

### Task 1: Portable cooldown state machine

**Files:**
- Modify: `esp32/shared/badge_easter_egg.h`
- Modify: `esp32/shared/badge_easter_egg.c`
- Test: `esp32/test/test_badge_easter_egg.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `badge_easter_egg_source_t` and the existing Easter egg phases.
- Produces: `BADGE_EASTER_EGG_RADIO_RETRIGGER_COOLDOWN_MS`, `badge_easter_egg_machine_trigger_at`, `badge_easter_egg_machine_advance_at`, and `badge_easter_egg_machine_dismiss_at`, all using `uint32_t now_ms`.

- [ ] **Step 1: Write failing radio cooldown tests**

Add tests that call the timestamped API and prove these boundaries:

```c
void test_badge_easter_radio_retrigger_waits_exactly_90_seconds(void)
{
    badge_easter_egg_machine_t machine;
    badge_easter_egg_machine_init(&machine);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 1000U));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, 2000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss_at(&machine, 3000U));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 92999U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, 93000U));
}

void test_badge_easter_radio_retrigger_is_wrap_safe(void)
{
    const uint32_t dismissed_at = UINT32_MAX - 44999U;
    badge_easter_egg_machine_t machine;
    badge_easter_egg_machine_init(&machine);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
        dismissed_at - 1000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss_at(
        &machine, dismissed_at));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 44999U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 45000U));
}
```

Add separate assertions that a failed trigger at 89,999 ms does not move the stored dismissal time, a button remains rejected after 90 seconds, and `badge_easter_egg_machine_init` clears the cooldown.

- [ ] **Step 2: Register and run the tests to verify RED**

Add declarations and `RUN_TEST` entries in `esp32/test/test_runner.c` beside the existing Easter egg tests.

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: build failure because the timestamped functions and cooldown state do not exist yet.

- [ ] **Step 3: Implement minimal wrap-safe state**

Add this public constant and state:

```c
#define BADGE_EASTER_EGG_RADIO_RETRIGGER_COOLDOWN_MS 90000U

typedef struct {
    bool triggered_once;
    bool visible;
    bool radio_cooldown_active;
    uint32_t dismissed_at_ms;
    badge_easter_egg_phase_t phase;
    badge_easter_egg_source_t source;
} badge_easter_egg_machine_t;
```

Implement timestamped APIs. A first valid source remains immediately accepted. Once `triggered_once` is true, only Wi-Fi or Remote ID may retrigger, only while hidden, only with an active cooldown, and only when:

```c
(uint32_t)(now_ms - machine->dismissed_at_ms) >=
    BADGE_EASTER_EGG_RADIO_RETRIGGER_COOLDOWN_MS
```

Successful retrigger clears `radio_cooldown_active`, restores `visible`, sets phase to `THANKS`, and records the new radio source. Transition from `BOUNCE` to `CONSUMED`, or explicit dismissal, records `now_ms` and activates the cooldown. Compatibility wrappers call the timestamped APIs with `0U`, preserving their old one-shot behavior.

- [ ] **Step 4: Run the complete native suite to verify GREEN**

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: all tests pass, including the new boundary, wrap, no-extension, button, and initialization cases.

- [ ] **Step 5: Commit the portable state machine**

```bash
git add esp32/shared/badge_easter_egg.h esp32/shared/badge_easter_egg.c esp32/test/test_badge_easter_egg.c esp32/test/test_runner.c
git commit -m "badge: allow cooled radio Easter retriggers"
```

### Task 2: Badge runtime monotonic clock wiring

**Files:**
- Modify: `esp32/uplink/main/core/badge_easter_egg_runtime.c`

**Interfaces:**
- Consumes: the Task 1 timestamped state-machine APIs.
- Produces: unchanged `badge_easter_egg_runtime_trigger`, `badge_easter_egg_runtime_advance`, and `badge_easter_egg_runtime_dismiss` interfaces for all existing callers.

- [ ] **Step 1: Wire the runtime to the monotonic clock**

Include `esp_timer.h` and add:

```c
static uint32_t badge_easter_egg_now_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}
```

Each runtime operation samples this value before entering `s_lock`, then calls its matching timestamped state-machine function inside the critical section. Do not call `esp_timer_get_time()` while the spinlock is held.

- [ ] **Step 2: Build the uplink badge target**

Run:

```bash
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: successful build with no new warnings; record RAM and flash use.

- [ ] **Step 3: Commit runtime wiring**

```bash
git add esp32/uplink/main/core/badge_easter_egg_runtime.c
git commit -m "badge: time radio Easter cooldown"
```

### Task 3: Local firmware acceptance

**Files:**
- Verify only: `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin`
- Verify only: `esp32/uplink/.pio/build/uplink-s3-fof_badge/firmware.bin`
- Preserve: `tools/badge_flasher/resources/badge-factory-flasher-embedded.zip`

**Interfaces:**
- Consumes: locally verified scanner/uplink badge images.
- Produces: build and connected-hardware evidence; no release artifact.

- [ ] **Step 1: Re-run the complete native suite**

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: every native test succeeds with zero failures.

- [ ] **Step 2: Build both badge firmware targets**

Run:

```bash
cd esp32/scanner
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge
```

```bash
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: both targets succeed. Record firmware byte counts and available flash headroom.

- [ ] **Step 3: Confirm release artifacts remain untouched**

Run:

```bash
git diff --exit-code f646133 -- esp32/shared/version.h tools/badge_flasher/resources/badge-factory-flasher-embedded.zip esp32/web-flasher
```

Expected: no output and exit status 0.

- [ ] **Step 4: Detect and flash the authorized test badge**

List connected serial devices and query existing runtime identity before selecting ports. Flash only the connected badge boards whose topology/role can be proven. Use the locally built uplink image for the LCD center and locally built scanner image for both leaves; do not regenerate or select the factory bundle.

Expected: all flashed roles boot with the expected project, target, hardware type, and unchanged local development version.

- [ ] **Step 5: Exercise the trigger on hardware**

Trigger with exact `GameChangersAI-67`, verify the thank-you screen, advance to bounce, and dismiss. Confirm a second matching signal is rejected during the next 89 seconds and accepted after 90 seconds. If full timing observation cannot be automated, report precisely which portion was confirmed from logs/display and which still needs human observation.

- [ ] **Step 6: Review the final diff**

Run:

```bash
git diff --check
git status --short --branch
git diff --stat f646133..HEAD
```

Expected: only intended source, tests, and design/plan commits plus the pre-existing untracked `.camera-before-zoom.jpg`; no factory bundle or version changes.
