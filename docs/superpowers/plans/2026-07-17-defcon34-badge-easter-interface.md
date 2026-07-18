# DEF CON 34 Badge Easter Egg and Interface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the exact one-shot Hell, Michigan Easter egg and theme-driven Instrument Stack chrome without changing the badge's four-lane selection behavior.

**Architecture:** A pure shared matcher/state machine owns exact protocol-unit matching and one-shot lifecycle. Badge scanners emit one allowlisted UART event; an uplink runtime owns the RAM-only latch, while the ST7735 renderer treats it as a top-level overlay. Existing lane selectors remain untouched and consume semantic chrome from the version-1 theme model.

**Tech Stack:** ESP-IDF C, PlatformIO, Unity, ST7735 RGB565 framebuffer, newline-delimited scanner UART JSON, Python asset conversion.

## Global Constraints

- Remote ID requires Basic ID `fof-michagain`, latitude E7 `424347200`, longitude E7 `-839850000`, and geodetic altitude in half-meters `1332`.
- SSID requires exactly ten lower-case bytes: `fof-goblue`.
- Trigger sources are BLE Remote ID, Wi-Fi SSID, or a short BTN1 press.
- The first trigger changes `ARMED -> VISIBLE`; any physical button changes `VISIBLE -> DISMISSED`; neither visible nor dismissed states can retrigger before reboot.
- A dismissal press is consumed through release and cannot also navigate, investigate, pair, or open another overlay.
- Preserve the two global lanes, lower BLE lane, lower Wi-Fi lane, focus order/capacity, paging, scanner-health strip at `y=148`, and all BTN2 semantics.
- Keep palette wire tokens at `field`, `night`, `neon`, and `mono`.
- Add no `IRAM_ATTR` code. Keep static image/theme data in flash; baseline IRAM is already full.
- Render exactly `Welcome to Hell`, `Just Kidding`, and `Defcon 34 FoF`.

---

### Task 1: Exact Matcher and One-Shot State Machine

**Files:**
- Create: `esp32/shared/badge_easter_egg.h`
- Create: `esp32/shared/badge_easter_egg.c`
- Create: `esp32/test/test_badge_easter_egg.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Produces: `badge_easter_egg_remote_id_matches`, `badge_easter_egg_ssid_matches`, `badge_easter_egg_machine_init`, `badge_easter_egg_machine_trigger`, and `badge_easter_egg_machine_dismiss`.
- Consumes: only fixed-width scalar protocol values; no FreeRTOS, cJSON, or heap allocation.

- [ ] **Step 1: Write failing exact-match and lifecycle tests**

```c
void test_badge_easter_remote_id_requires_every_exact_field(void)
{
    badge_easter_egg_remote_id_t rid = {
        .has_basic_id = true,
        .basic_id = "fof-michagain",
        .has_location = true,
        .latitude_e7 = 424347200,
        .longitude_e7 = -839850000,
        .has_geodetic_altitude = true,
        .geodetic_altitude_half_m = 1332,
    };
    TEST_ASSERT_TRUE(badge_easter_egg_remote_id_matches(&rid));
    rid.latitude_e7++;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
    rid.latitude_e7--;
    rid.geodetic_altitude_half_m--;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
}

void test_badge_easter_ssid_is_exact_case_sensitive_bytes(void)
{
    TEST_ASSERT_TRUE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"fof-goblue", 10));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"FOF-GOBLUE", 10));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"fof-goblue-x", 12));
}

void test_badge_easter_machine_is_one_shot_until_init(void)
{
    badge_easter_egg_machine_t machine;
    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_BUTTON));
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss(&machine));
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
}
```

Add separate negative assertions for missing Basic ID, intentional spelling changes, case changes, longitude E7 ±1, latitude E7 ±1, missing geodetic altitude, altitude half-meter ±1, pressure-only altitude, SSID prefix/suffix, and embedded NUL.

- [ ] **Step 2: Run the focused suite to verify RED**

Run:

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
```

Expected: compile failure because `badge_easter_egg.h` and its contracts do not exist.

- [ ] **Step 3: Implement the pure contracts**

```c
typedef enum {
    BADGE_EASTER_EGG_SOURCE_NONE = 0,
    BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID,
    BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
    BADGE_EASTER_EGG_SOURCE_BUTTON,
} badge_easter_egg_source_t;

typedef struct {
    bool has_basic_id;
    const char *basic_id;
    bool has_location;
    int32_t latitude_e7;
    int32_t longitude_e7;
    bool has_geodetic_altitude;
    int32_t geodetic_altitude_half_m;
} badge_easter_egg_remote_id_t;

typedef struct {
    bool triggered_once;
    bool visible;
    badge_easter_egg_source_t source;
} badge_easter_egg_machine_t;
```

Use `strcmp` for Basic ID after parser padding removal and `len == 10 && memcmp(...) == 0` for SSID. `trigger` succeeds only when `triggered_once == false`; `dismiss` clears only `visible`.

- [ ] **Step 4: Run the native suite to verify GREEN**

Run the Step 2 command. Expected: all native tests pass with the new matcher tests registered.

- [ ] **Step 5: Commit**

```bash
git add esp32/shared/badge_easter_egg.c esp32/shared/badge_easter_egg.h esp32/test/test_badge_easter_egg.c esp32/test/test_runner.c esp32/platformio.ini
git commit -m "badge: add one-shot Easter egg contract"
```

---

### Task 2: Scanner Protocol-Unit Tracking and UART Event

**Files:**
- Modify: `esp32/scanner/main/detection/open_drone_id_parser.h`
- Modify: `esp32/scanner/main/detection/open_drone_id_parser.c`
- Modify: `esp32/scanner/main/detection/ble_remote_id.c`
- Modify: `esp32/scanner/main/detection/wifi_scanner.c`
- Modify: `esp32/scanner/main/comms/uart_tx.h`
- Modify: `esp32/scanner/main/comms/uart_tx.c`
- Modify: `esp32/scanner/main/CMakeLists.txt`
- Modify: `esp32/shared/uart_protocol.h`
- Modify: `esp32/test/test_open_drone_id_parser.c`
- Modify: `esp32/test/test_badge_easter_egg.c`

**Interfaces:**
- Consumes: Task 1 matcher, raw ODID Basic ID/location units, exact SSID IE bytes.
- Produces: `uart_tx_note_badge_easter_egg(source)` and scanner frames `{"type":"badge_easter_egg","source":"ble_remote_id|wifi_ssid"}`.

- [ ] **Step 1: Add failing parser-state and event-coalescing tests**

```c
void test_odid_retains_exact_units_for_badge_easter_match(void)
{
    odid_state_t state = {0};
    odid_parse_message(basic_id_message("fof-michagain"), 25, &state, 0);
    odid_parse_message(location_message(424347200, -839850000, 1332), 25,
                       &state, 0);
    TEST_ASSERT_TRUE(state.has_basic_id);
    TEST_ASSERT_EQUAL_INT32(424347200, state.latitude_e7);
    TEST_ASSERT_EQUAL_INT32(-839850000, state.longitude_e7);
    TEST_ASSERT_TRUE(state.has_geodetic_altitude);
    TEST_ASSERT_EQUAL_INT32(1332, state.geodetic_altitude_half_m);
}
```

Add a test proving that changing a nonempty Basic ID clears old location/altitude, while receiving a first Basic ID after location preserves the same identity's accumulated location.

- [ ] **Step 2: Run native tests to verify RED**

Run the Task 1 native command. Expected: missing raw-unit fields and event APIs.

- [ ] **Step 3: Preserve exact ODID fields and prevent mixed identities**

Extend `odid_state_t` with `has_basic_id`, signed E7 coordinates, and a geodetic half-meter field. Populate them from raw payload integers before floating-point conversion. In `parse_basic_id`, clear prior position/altitude only when replacing one nonempty Basic ID with a different nonempty ID.

Immediately after `odid_parse_message()` in `process_odid_service_data`, build `badge_easter_egg_remote_id_t` from the slot state and notify UART before normal detection rate limiting.

- [ ] **Step 4: Observe the exact SSID in every Wi-Fi path**

Call the matcher after SSID IE extraction in beacon/probe-response handling, before probe-request broadcast/policy filters, and before active-scan privacy/pattern filtering. Pass the actual IE length; never use a C-string-only comparison for frame data.

- [ ] **Step 5: Add a nonblocking coalesced UART emitter**

```c
void uart_tx_note_badge_easter_egg(badge_easter_egg_source_t source)
{
    atomic_fetch_or(&s_badge_easter_pending,
        source == BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID ? 0x01U : 0x02U);
}
```

Drain pending bits from `uart_tx_task` before the normal detection queue and emit fixed, allocation-free JSON strings. Do not transmit remotely supplied text.

- [ ] **Step 6: Run native tests and both scanner builds**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -t clean -e scanner-s3-combo-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo
```

Expected: all pass; scanner IRAM does not increase beyond its link limit.

- [ ] **Step 7: Commit**

```bash
git add esp32/scanner esp32/shared/uart_protocol.h esp32/test
git commit -m "badge: emit exact Hell trigger events"
```

---

### Task 3: Uplink Runtime, Buttons, and PHV Overlay

**Files:**
- Create: `esp32/uplink/main/core/badge_easter_egg_runtime.h`
- Create: `esp32/uplink/main/core/badge_easter_egg_runtime.c`
- Create: `esp32/uplink/main/hw/assets/wall_of_sheep_logo.h`
- Create: `esp32/uplink/main/hw/assets/wall_of_sheep_logo.c`
- Create: `esp32/scripts/convert_badge_logo.py`
- Add: `esp32/uplink/assets/wall-of-sheep.png`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`

**Interfaces:**
- Consumes: fixed scanner event from Task 2 and Task 1 state machine.
- Produces: thread-safe `badge_easter_egg_runtime_trigger`, `badge_easter_egg_runtime_dismiss`, and `badge_easter_egg_runtime_snapshot`.

- [ ] **Step 1: Add failing source-allowlist and button-consumption tests**

Extend `test_badge_easter_egg.c` with `badge_easter_egg_source_from_wire` positives for `ble_remote_id` and `wifi_ssid`, plus missing/unknown/case-changed negatives. Extend `test_badge_button_gesture.c` to prove `badge_button_gesture_cancel` removes a pending BTN2 single.

- [ ] **Step 2: Run native tests to verify RED**

Run the native command. Expected: source parser is absent.

- [ ] **Step 3: Add runtime and UART ingestion**

Initialize the RAM-only runtime before UART/display tasks. Add only `badge_easter_egg` to `msg_type_is_scanner_originated`; validate the source token, trigger the runtime, and return before detection ingestion. Protect runtime reads/writes with a short critical section; do not use NVS.

- [ ] **Step 4: Convert and embed the official logo**

The converter must verify source SHA-256 `41e4d1641b14464a08496a975f7e073bb5692817a65b3eb1052f83e87da4cbe0`, resize to at most 72 x 72, quantize to a small transparent/indexed purple palette, and generate `const` flash data. Generated headers must record the PHV source URL and conversion command.

- [ ] **Step 5: Draw the exact overlay before all normal dashboard branches**

```c
if (badge_easter_egg_runtime_snapshot(&easter) && easter.visible) {
    draw_badge_easter_egg_screen();
    st_flush();
    display_unlock();
    return;
}
```

Use the logo as the focal point, purple/black frame and glow bands, and the three exact strings. Do not allocate while drawing.

- [ ] **Step 6: Implement manual trigger and consumed dismissal**

Add `consume_release` to internal button state. On a debounced press while visible, dismiss, set `consume_release`, clear the normal overlay, and cancel BTN2's pending gesture. On the matching release, clear the flag and return before short/long dispatch. A BTN1 short release while armed triggers the Easter egg; BTN1 legacy actions remain disabled.

- [ ] **Step 7: Run native tests and clean badge uplink build**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -t clean -e uplink-s3-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: tests/build pass, no IRAM overflow, and app flash delta is recorded.

- [ ] **Step 8: Commit**

```bash
git add esp32/uplink/assets esp32/uplink/main esp32/scripts/convert_badge_logo.py esp32/test
git commit -m "badge: render the PHV Hell Easter egg"
```

---

### Task 4: Semantic Chrome and Four-Lane Regression Gate

**Files:**
- Modify: `esp32/shared/badge_theme.h`
- Modify: `esp32/shared/badge_theme.c`
- Create: `esp32/shared/badge_display_contract.h`
- Create: `esp32/shared/badge_display_contract.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Modify: `esp32/test/test_badge_theme.c`
- Create: `esp32/test/test_badge_display_contract.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Produces: `badge_theme_chrome_color(theme, role)`, `badge_theme_contrast_floor`, and a pure four-lane geometry/focus contract.
- Consumes: unchanged version-1 theme payload and existing dashboard candidate results.

- [ ] **Step 1: Add failing palette-role and lane-contract tests**

```c
void test_badge_palettes_drive_distinct_semantic_chrome(void)
{
    badge_theme_t field, neon;
    badge_theme_defaults(&field);
    neon = field;
    snprintf(neon.palette, sizeof(neon.palette), "neon");
    TEST_ASSERT_NOT_EQUAL(
        badge_theme_chrome_color(&field, BADGE_THEME_CHROME_SELECTION),
        badge_theme_chrome_color(&neon, BADGE_THEME_CHROME_SELECTION));
}

void test_badge_display_contract_keeps_four_fixed_lanes(void)
{
    TEST_ASSERT_EQUAL_INT(4, badge_display_contract_focus_capacity());
    TEST_ASSERT_EQUAL_INT(148, badge_display_contract_health_strip_y());
    TEST_ASSERT_EQUAL(BADGE_LANE_GLOBAL_1, badge_display_contract_lane(0));
    TEST_ASSERT_EQUAL(BADGE_LANE_WIFI, badge_display_contract_lane(3));
}
```

- [ ] **Step 2: Run native tests to verify RED**

Expected: missing chrome and display-contract symbols.

- [ ] **Step 3: Implement semantic chrome with safety floors**

Add roles for canvas, panel, alternate panel, primary text, secondary text, selection, and scanner-down. Map only the four accepted palette tokens. Apply brightness after role lookup and enforce contrast floors only at draw time.

- [ ] **Step 4: Apply chrome inside existing drawing helpers only**

Update background, top-tile, lower-row, focus, and health-strip drawing helpers. Do not modify candidate builders, top-two deduplication, focus-model insertion/order, paging, or row coordinates.

- [ ] **Step 5: Run full native tests and production/badge builds**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: all pass; four-lane and existing badge policy suites remain green.

- [ ] **Step 6: Commit**

```bash
git add esp32/shared/badge_theme.* esp32/shared/badge_display_contract.* esp32/uplink/main/hw/display_st7735.c esp32/test
git commit -m "badge: theme the four-lane instrument stack"
```
