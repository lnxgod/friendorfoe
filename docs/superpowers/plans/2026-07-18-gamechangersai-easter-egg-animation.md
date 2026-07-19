# GameChangersAI Easter Egg Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the badge's Hell presentation with a two-stage GameChangersAI thank-you and color-cycling DVD-logo animation, then verify, flash, tag, and publish the DEFCON-safe release.

**Architecture:** Keep all scanner trigger matching and UART transport unchanged. Extend the existing RAM-only Easter egg state machine, add a pure bounded integer animation helper, generate one verified 64x64 indexed logo asset stored in flash, and render both presentation phases through the existing ST7735 frame buffer and display task.

**Tech Stack:** ESP-IDF C, FreeRTOS, ST7735 RGB565 frame buffer, PlatformIO/Unity native tests, Python 3/Pillow asset conversion, USB plus uplink-to-scanner UART flashing, Git/GitHub.

## Global Constraints

- Preserve the exact BLE Remote ID and exact case-sensitive `fof-goblue` trigger contracts.
- Preserve the normal four-lane badge interface and scanner task priorities.
- Use the phase sequence `ARMED -> THANKS -> BOUNCE -> CONSUMED`.
- Either physical badge button advances `THANKS -> BOUNCE` and `BOUNCE -> CONSUMED`.
- Consume the matching release and permit at most one transition per button polling batch.
- `CONSUMED` cannot retrigger until reboot; the latch remains RAM-only.
- Render no remotely supplied text.
- Store the logo as a verified 64x64 indexed `const` asset and allocate no image memory at runtime.
- Change animation color only on an edge collision; a corner collision changes color exactly once.
- Add no task, timer, queue, mutex, PNG decoder, network dependency, or scanner-side animation code.
- Do not add `.camera-before-zoom.jpg` to version control.
- Do not label or tag the release safe until automated builds/tests and connected-badge verification succeed.
- Publish only `codex/defcon34-badge-final` and the annotated `defcon34-valid-safe-v0.64.76` tag; do not merge or push directly to `main`.

---

## File Map

- `esp32/shared/badge_easter_egg.h`: presentation phase enum and state-machine transition API.
- `esp32/shared/badge_easter_egg.c`: one-shot trigger and button-driven phase transitions.
- `esp32/shared/badge_easter_egg_animation.h`: pure animation state and bounded step interface.
- `esp32/shared/badge_easter_egg_animation.c`: deterministic integer bounce/collision behavior.
- `esp32/test/test_badge_easter_egg.c`: state-machine, button-batch, and animation regression tests.
- `esp32/test/test_runner.c`: Unity declarations and registrations.
- `esp32/platformio.ini`: native-test source inclusion for the new animation helper.
- `esp32/uplink/main/core/badge_easter_egg_runtime.h`: thread-safe presentation advance API.
- `esp32/uplink/main/core/badge_easter_egg_runtime.c`: locked wrapper around the shared state machine.
- `esp32/uplink/main/hw/display_st7735.c`: thank-you layout, indexed tinted sprite renderer, and bounce frame rendering.
- `esp32/uplink/assets/gamechangersai-logo.png`: canonical official source asset.
- `esp32/scripts/convert_gamechangersai_logo.py`: source-hash verification and deterministic indexed conversion.
- `esp32/scripts/test_convert_gamechangersai_logo.py`: converter hash and output contract tests.
- `esp32/uplink/main/hw/assets/gamechangersai_logo.h`: generated dimensions and asset declarations.
- `esp32/uplink/main/hw/assets/gamechangersai_logo.c`: generated palette indices stored in flash.
- `esp32/CHANGELOG.md`, `docs/badge/README.md`: release-facing presentation and verification notes.

---

### Task 1: Presentation state machine and bounded animation policy

**Files:**
- Modify: `esp32/shared/badge_easter_egg.h`
- Modify: `esp32/shared/badge_easter_egg.c`
- Create: `esp32/shared/badge_easter_egg_animation.h`
- Create: `esp32/shared/badge_easter_egg_animation.c`
- Modify: `esp32/test/test_badge_easter_egg.c`
- Modify: `esp32/test/test_runner.c`
- Modify: `esp32/platformio.ini`

**Interfaces:**
- Produces: `badge_easter_egg_phase_t`, `badge_easter_egg_machine_advance(badge_easter_egg_machine_t *)`, `badge_easter_egg_claim_press_in_batch(bool, bool *)`, `badge_easter_egg_animation_init(badge_easter_egg_animation_t *)`, and `badge_easter_egg_animation_step(badge_easter_egg_animation_t *, int16_t, int16_t, int16_t, int16_t, uint8_t)`.
- Preserves: `badge_easter_egg_machine_trigger`, `triggered_once`, `visible`, and all exact trigger matching APIs.

- [ ] **Step 1: Write failing phase and animation tests**

Add tests that require this public contract:

```c
typedef enum {
    BADGE_EASTER_EGG_PHASE_ARMED = 0,
    BADGE_EASTER_EGG_PHASE_THANKS,
    BADGE_EASTER_EGG_PHASE_BOUNCE,
    BADGE_EASTER_EGG_PHASE_CONSUMED,
} badge_easter_egg_phase_t;

typedef struct {
    int16_t x;
    int16_t y;
    int8_t vx;
    int8_t vy;
    uint8_t color_index;
} badge_easter_egg_animation_t;
```

Tests must assert trigger enters `THANKS`, first advance enters `BOUNCE`, second advance enters `CONSUMED` with `visible == false`, further advance and trigger calls fail, and `init` returns to `ARMED`. Button-batch tests must prove two presses in one batch claim only one transition while both remain consumed by the overlay.

Animation tests use a 160x160 screen, 64x64 sprite, and six colors. They assert initial `(8, 12, +3, +2, 0)`, no-collision position updates, right/bottom/left/top reversal and clamping, corner reversal of both axes, exactly one color increment per collision update, color wrap from five to zero, and safe fallback `(0, 0)` for invalid dimensions.

- [ ] **Step 2: Run the native suite and verify red failures**

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: compilation or Unity failures for the missing phase, batch-claim, and animation APIs; existing trigger tests continue compiling up to those new requirements.

- [ ] **Step 3: Implement the minimal pure contracts**

Extend `badge_easter_egg_machine_t` with `badge_easter_egg_phase_t phase`. `machine_trigger` sets `triggered_once = true`, `visible = true`, `phase = THANKS`, and the fixed source. Implement:

```c
bool badge_easter_egg_machine_advance(badge_easter_egg_machine_t *machine)
{
    if (!machine || !machine->visible) return false;
    if (machine->phase == BADGE_EASTER_EGG_PHASE_THANKS) {
        machine->phase = BADGE_EASTER_EGG_PHASE_BOUNCE;
        return true;
    }
    if (machine->phase == BADGE_EASTER_EGG_PHASE_BOUNCE) {
        machine->phase = BADGE_EASTER_EGG_PHASE_CONSUMED;
        machine->visible = false;
        return true;
    }
    return false;
}
```

Implement a first-claim gate without touching runtime state:

```c
bool badge_easter_egg_claim_press_in_batch(bool visible_at_batch_start,
                                           bool *transition_claimed)
{
    if (!visible_at_batch_start || !transition_claimed || *transition_claimed) {
        return false;
    }
    *transition_claimed = true;
    return true;
}
```

Implement animation stepping with signed intermediate positions. Reject invalid screen/sprite/color dimensions by resetting position to `(0, 0)` and returning `false`. For valid dimensions, clamp each collided axis, reverse its velocity, and advance `color_index` once when either axis collided.

- [ ] **Step 4: Run the native suite and verify green**

Run the same PlatformIO native command. Expected: all tests pass with zero failures.

- [ ] **Step 5: Commit the policy unit**

```bash
git add esp32/shared/badge_easter_egg.h \
  esp32/shared/badge_easter_egg.c \
  esp32/shared/badge_easter_egg_animation.h \
  esp32/shared/badge_easter_egg_animation.c \
  esp32/test/test_badge_easter_egg.c \
  esp32/test/test_runner.c esp32/platformio.ini
git commit -m "badge: add two-stage Easter egg animation policy"
```

---

### Task 2: Verified official GameChangersAI indexed asset

**Files:**
- Create: `esp32/uplink/assets/gamechangersai-logo.png`
- Create: `esp32/scripts/convert_gamechangersai_logo.py`
- Create: `esp32/scripts/test_convert_gamechangersai_logo.py`
- Create: `esp32/uplink/main/hw/assets/gamechangersai_logo.h`
- Create: `esp32/uplink/main/hw/assets/gamechangersai_logo.c`

**Interfaces:**
- Consumes: canonical 128x128 RGBA source whose SHA-256 is `903d20f0b3d52c8b5b785686680cbb5e884ea17a5636fdf381e9752ade92efce`.
- Produces: `GAMECHANGERSAI_LOGO_WIDTH`, `GAMECHANGERSAI_LOGO_HEIGHT`, `GAMECHANGERSAI_LOGO_PALETTE_SIZE`, `gamechangersai_logo_levels[]`, and a 64x64 transparent-plus-luminance indexed sprite.

- [ ] **Step 1: Add failing converter tests**

Use Python `unittest` to import the converter and assert:

```python
self.assertEqual(EXPECTED_SHA256,
                 "903d20f0b3d52c8b5b785686680cbb5e884ea17a5636fdf381e9752ade92efce")
self.assertEqual(verify_source(official_bytes), EXPECTED_SHA256)
with self.assertRaises(ValueError):
    verify_source(official_bytes + b"changed")
```

Convert into a temporary directory and assert width/height are both 64, palette size is eight, pixels total 4096, and generated header/source contain the exact attribution URL, hash, dimensions, and `const` declarations.

- [ ] **Step 2: Run converter tests and verify red**

Run:

```bash
python3 -m unittest esp32/scripts/test_convert_gamechangersai_logo.py -v
```

Expected: import failure because the converter does not exist.

- [ ] **Step 3: Add the official source and converter**

Retrieve only `https://gamechangersai.org/assets/gamechangers-128.png`, verify its exact hash before writing generated outputs, resize with `Image.Resampling.LANCZOS`, treat alpha below 32 or luminance below 24 as transparent, and quantize remaining luminance into indices 1 through 7. Write text outputs only after verification and conversion succeed so rejected input leaves existing generated assets untouched.

Generate the 64x64 firmware asset with:

```bash
python3 esp32/scripts/convert_gamechangersai_logo.py \
  --input esp32/uplink/assets/gamechangersai-logo.png \
  --header esp32/uplink/main/hw/assets/gamechangersai_logo.h \
  --source esp32/uplink/main/hw/assets/gamechangersai_logo.c \
  --size 64
```

- [ ] **Step 4: Run converter tests and deterministic regeneration**

Run the unit test command, hash the generated files, run the converter again, and assert the hashes do not change. Expected: all unit tests pass and regeneration produces no Git diff.

- [ ] **Step 5: Commit the verified asset unit**

```bash
git add esp32/uplink/assets/gamechangersai-logo.png \
  esp32/scripts/convert_gamechangersai_logo.py \
  esp32/scripts/test_convert_gamechangersai_logo.py \
  esp32/uplink/main/hw/assets/gamechangersai_logo.h \
  esp32/uplink/main/hw/assets/gamechangersai_logo.c
git commit -m "badge: embed verified GameChangersAI logo"
```

---

### Task 3: Button transitions and ST7735 presentation

**Files:**
- Modify: `esp32/uplink/main/core/badge_easter_egg_runtime.h`
- Modify: `esp32/uplink/main/core/badge_easter_egg_runtime.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Modify: `esp32/test/test_badge_easter_egg.c`
- Modify: `esp32/CHANGELOG.md`
- Modify: `docs/badge/README.md`

**Interfaces:**
- Consumes: `badge_easter_egg_machine_advance`, `badge_easter_egg_claim_press_in_batch`, generated 64x64 logo levels, and `badge_easter_egg_animation_step`.
- Produces: `badge_easter_egg_runtime_advance()` and two presentation renderers selected by the shared phase.

- [ ] **Step 1: Add the renderer/button source-contract regression**

Add a native source-contract test that reads `display_st7735.c` in the existing test style and asserts it contains `Thank you from`, `GameChangers AI`, `GAMECHANGERSAI_LOGO_WIDTH`, and `BADGE_EASTER_EGG_PHASE_BOUNCE`, while rejecting the former string literals `Welcome to Hell`, `Just Kidding`, and `Defcon 34 FoF`.

- [ ] **Step 2: Run native tests and verify the contract fails**

Run the PlatformIO native suite. Expected: the new renderer contract fails on the old strings and missing phase renderer.

- [ ] **Step 3: Implement locked runtime advance and one-transition button batches**

Replace the display's call to `badge_easter_egg_runtime_dismiss()` with `badge_easter_egg_runtime_advance()`. At each button polling batch, snapshot whether the Easter egg was visible and initialize `bool easter_transition_claimed = false`. Every stable press that began inside the visible overlay consumes its release, but only the first press for which `badge_easter_egg_claim_press_in_batch(...)` returns `true` invokes the locked runtime advance. Cancel pending button-two gestures and clear normal overlays exactly as the current dismissal path does.

- [ ] **Step 4: Implement the thank-you and DVD renderers**

Keep the purple frame and Wall of Sheep asset for `THANKS`, remove the old Hell copy, and draw the exact two approved lines. Add a tinted indexed logo drawer that maps levels 1 through 7 to increasing brightness of the current RGB565 hue and skips index zero.

Maintain a static renderer-local `badge_easter_egg_animation_t`. Initialize it only on entry to `BOUNCE`, clear the frame buffer to black, draw the logo at the bounded position, then step once for the next display tick. Use a fixed six-color cycle: cyan, magenta, green, amber, electric blue, and white. Leaving `BOUNCE` clears the renderer-local initialization latch. Do not change normal display cadence or scanner priorities.

- [ ] **Step 5: Run native tests, converter tests, and both firmware builds**

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
cd scanner
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
cd ../..
python3 -m unittest esp32/scripts/test_convert_gamechangersai_logo.py -v
```

Expected: complete native suite and converter tests pass; both firmware images build within their current partition limits.

- [ ] **Step 6: Commit the presentation unit**

```bash
git add esp32/uplink/main/core/badge_easter_egg_runtime.h \
  esp32/uplink/main/core/badge_easter_egg_runtime.c \
  esp32/uplink/main/hw/display_st7735.c \
  esp32/test/test_badge_easter_egg.c \
  esp32/CHANGELOG.md docs/badge/README.md
git commit -m "badge: animate GameChangersAI Easter egg"
```

---

### Task 4: Connected badge verification, safe release checkpoint, and GitHub publish

**Files:**
- Verify: `esp32/scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin`
- Verify: `esp32/uplink/.pio/build/uplink-s3-fof_badge/firmware.bin`
- Modify only if verification requires an evidence note: `esp32/CHANGELOG.md`

**Interfaces:**
- Consumes: the built uplink and scanner images plus `scripts/fof_badge_flash.py`.
- Produces: verified hardware state, release commit, annotated safe tag, pushed branch, and pushed tag.

- [ ] **Step 1: Run full repository release checks**

Run the complete ESP32 native/converter/build commands from Task 3, Android `./gradlew testDebugUnitTest assembleDebug`, and the backend test suite using its configured virtual environment. Expected: zero test failures and successful Android/firmware builds. Record exact pass counts and image sizes.

- [ ] **Step 2: Discover and preflight the connected badge**

Confirm exactly one intended `/dev/cu.usbmodem*` target, no process owns it, and `FOF_STATUS` reports `uplink-s3-fof_badge`, hardware type `fof_badge`, version `0.64.76-badge-defcon34`, normal recovery mode, healthy PSRAM/heap, and two unique scanner hardware IDs before mutation.

- [ ] **Step 3: Flash the complete badge trio**

Run:

```bash
python3 scripts/fof_badge_flash.py \
  --transport usb \
  --port /dev/cu.usbmodem101 \
  --only all \
  --skip-build \
  --recovery-rewrite-same-version
```

Expected: uplink image hash verifies, both UART scanner relays complete with zero unrecovered NACKs, and the final verifier reports the expected immutable/unique identities, roles, scan profiles, project names, hardware types, and version.

- [ ] **Step 4: Exercise USB and physical Easter egg behavior**

Verify post-boot `FOF_PING` and `FOF_STATUS`, scanner health, and normal four-lane display. Use the temporary badge button to trigger `THANKS`; inspect the physical LCD for both approved lines and the Wall of Sheep mark. Press once to enter `BOUNCE`, observe bounded motion and collision color changes, then press again to restore the normal UI. Attempt another trigger and confirm it remains consumed until reboot. Reboot and confirm it rearms.

- [ ] **Step 5: Review and commit the complete release scope**

Exclude `.camera-before-zoom.jpg`. Review every remaining modified/untracked file, stage only the release-facing Android/backend/docs/ESP32/scripts changes already validated by Step 1, run `git diff --cached --check`, and inspect `git diff --cached --stat`. Commit with:

```bash
git commit -m "v0.64.76: mark DEFCON badge release safe"
```

- [ ] **Step 6: Tag and publish without touching main**

Create the annotated tag only after Steps 1 through 5 succeed:

```bash
git tag -a defcon34-valid-safe-v0.64.76 \
  -m "VALID SAFE FOR DEFCON: v0.64.76 badge trio verified"
git push origin codex/defcon34-badge-final
git push origin defcon34-valid-safe-v0.64.76
```

Expected: both pushes succeed, the branch remains separate from `main`, and `git status --short --branch` contains only intentionally excluded local artifacts.
