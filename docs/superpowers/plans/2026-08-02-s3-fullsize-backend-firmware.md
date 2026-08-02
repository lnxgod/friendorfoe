# S3 Fullsize Backend Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Track every checkbox in this file and stop at each review gate.

**Goal:** Add the classic three-board, 16 MB ESP32-S3 N16R8 assembly as a separately identified **S3 Fullsize** backend sensor platform, with headless sensing, HTTP/AP uplink, GPIO48 RGB threat LEDs, and guarded future network-to-uplink-to-UART updates, without changing native Badge firmware or confusing it with Badge Lite.

**Architecture:** Keep all firmware implementation under the isolated `backend-firmware/` tree. Add an explicit compile-time hardware profile to the existing backend sensing codebase, build two new Fullsize targets beside the two existing Badge Lite targets, and fail compilation if a build does not select exactly one profile. The FastAPI service classifies exact identities, exposes management state, and owns a separate durable Fullsize OTA rollout channel. Initial legacy 0.63 conversion remains an attended scanners-first USB migration with one retained 16 MB backup per board; routine updates are pull-based over the uplink network connection and serialized over UART for scanners.

**Tech stack:** C11, ESP-IDF/PlatformIO `espressif32@6.13.0`, ESP32-S3 N16R8, FreeRTOS, Espressif `led_strip`, Python 3.12, FastAPI/Pydantic/SQLAlchemy, pytest, Unity native tests, esptool, GitHub Actions, ESP Web Tools manifests.

## Global constraints

- Work only on branch `codex/backend-firmware` in `/Users/billh/gai/friendorfoe/.worktrees/backend-firmware`.
- Treat commit `cf378db` as the protected-path baseline for this Fullsize feature. The design commits after it may change documentation only.
- Do not modify `android/`, `esp32/`, `scripts/`, `flash-badges.command`, `tools/badge_flasher/`, `.github/workflows/android-build.yml`, `.github/workflows/esp32-web-flasher.yml`, native Badge manifests, native Badge artifacts, or `docs/badge/**`.
- The embedded native Badge bundle and USB/factory default remain exactly `0.67.2-badge-defcon34`. This plan does not change the existing parser policy for separately authorized future native Badge images, but no backend image may enter the Badge source, factory bundle, launcher, manifest, or release channel, and Fullsize work may never replace the `0.67.2` default.
- Badge Lite remains a separate product. Preserve its target/project/hardware strings, 8 MB partition maps, UART pins, GPIO21 active-low LED behavior, default PlatformIO environments, and maintenance-flasher choices. Shared refactors require explicit Lite regression tests.
- Use backend version `0.2.0-backend` for the next four-image backend release. Rebuilding shared Lite code under the already published `0.1.0-backend` version is forbidden. A version bump does not authorize flashing Lite hardware.
- Support exactly these product families: `badge`, `badge_lite`, and `s3_fullsize`. `scanner-s3-combo-seed`, ESP32-C5, classic ESP32, mixed trios, OLED-dependent nodes, and unknown identities remain unsupported for this Fullsize path.
- The `s3_fullsize` profile is the existing GPIO48 WS2812 production-board contract proven by the legacy source and attended inventory. ESP32-S3-DevKitC-1 v1.1 boards whose onboard RGB LED is GPIO38 are not accepted under this identity; supporting them requires a future distinct hardware profile.
- Runtime backend firmware line values are `backend`; native Badge is server-resolved as `native_badge`. Legacy 0.63 is reported server-side as `firmware_line: legacy`, `desired_firmware_line: backend`, and `migration_required: true`; it is never falsely labeled as installed backend firmware.
- Keep the embedded backend identity record at schema 1 and exactly 164 bytes. Do not add management fields to that binary record.
- Keep scanner UART status schema 1 and its exact wire field count unchanged. Fullsize family/component data is emitted in USB records and the uplink HTTP heartbeat, where scanner family is derived from the exact target/project/hardware tuple. This avoids breaking Lite rolling compatibility.
- Keep `/nodes/firmware/latest/{name}` at its exact existing 11 top-level fields: `name`, `target`, `description`, `board`, `project`, `hardware`, `version`, `size`, `sha256`, `crc32`, `download_url`. The deployed decoder rejects added or missing fields.
- Keep the existing eight-field BLE command envelope and `/nodes/{device_id}/commands/...` lifecycle unchanged. Fullsize OTA uses separate `/nodes/{device_id}/backend-ota/...` endpoints and a separate strict firmware parser.
- Fullsize scanner updates bind the entire trio: exact uplink plus exactly two distinct exact Fullsize scanners. Validate that binding before any awaited image fetch and revalidate it afterward.
- Never route S3 Fullsize through legacy `/api/ota`, `/api/fw/upload`, `staged_legacy`, or `direct_legacy` paths.
- Initial legacy 0.63 conversion is USB-only. Before the first write, inventory all three MACs and read each complete 16 MB flash twice with matching SHA-256. Flash BLE scanner, then Wi-Fi scanner, then uplink. Preserve NVS at `0x9000..0xefff`.
- Routine OTA does not repeat full-flash backups. It relies on inactive slots, health-gated rollback, the retained initial backups, and attended USB recovery.
- During implementation, run only the focused tests named in each task. Run the consolidated gates once at the end; do not repeatedly run the full backend suite.
- Commit after each completed task. Never stage unrelated user changes.

## Immutable target matrix

| Product/component | Environment and target | ESP-IDF project | Hardware identity | Flash | App/cache capacity |
| --- | --- | --- | --- | ---: | ---: |
| Badge Lite uplink | `uplink-s3-backend` | `fof_backend_uplink` | `seeed_xiao_esp32s3` | 8 MB | 2 MB app, 2 MB scanner cache |
| Badge Lite scanner | `scanner-s3-combo-backend` | `fof_backend_scanner` | `seeed_xiao_esp32s3` | 8 MB | 2 MB app |
| S3 Fullsize uplink | `uplink-s3-fullsize-backend` | `fof_backend_uplink_fullsize` | `esp32s3_n16r8_fullsize` | 16 MB | 2 MB app, 3 MB scanner cache |
| S3 Fullsize scanner | `scanner-s3-combo-fullsize-backend` | `fof_backend_scanner_fullsize` | `esp32s3_n16r8_fullsize` | 16 MB | 3 MB app |

At `0.2.0-backend`, the schema-1 embedded identity CRC32 constants are:

- Badge Lite uplink: `0xB42AE8FC`
- Badge Lite scanner: `0xD972A7E7`
- S3 Fullsize uplink: `0xF03A379D`
- S3 Fullsize scanner: `0x86C70497`

---

### Task 1: Add the fail-closed hardware profile and exact management identity

**Files:**

- Create: `backend-firmware/shared/backend_hardware_profile.h`
- Create: `backend-firmware/tools/backend_targets.py`
- Create: `backend-firmware/tools/tests/test_backend_targets.py`
- Modify: `backend-firmware/shared/backend_version.h`
- Modify: `backend-firmware/shared/backend_identity.h`
- Modify: `backend-firmware/shared/backend_identity.c`
- Modify: `backend-firmware/shared/backend_embedded_identity.c`
- Modify: `backend-firmware/platformio.ini`
- Modify: `backend-firmware/test/test_backend_identity/test_main.c`
- Modify: `backend-firmware/tools/tests/test_embedded_identity_object.py`
- Modify: `backend-firmware/tools/tests/test_source_isolation.py`

**Interfaces:**

```c
#if (defined(FOF_BACKEND_PROFILE_BADGE_LITE) + \
     defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)) != 1
#error "select exactly one backend hardware profile"
#endif

typedef struct {
    const char *product_family;
    const char *firmware_line;
    const char *component;
    const char *target;
    const char *project;
    const char *hardware;
    const char *version;
} backend_firmware_identity_t;
```

`backend_hardware_profile.h` is the only C source of profile-dependent family, target, project, hardware, flash/capacity, UART, and LED macros. `backend_targets.py` is the host-tooling source of the same four immutable target specifications; a parity test compares every duplicated value.

- [ ] **Step 1: Write the failing C identity/profile tests**

Add exact assertions to `test_backend_identity/test_main.c`:

```c
TEST_ASSERT_EQUAL_STRING(FOF_BACKEND_PRODUCT_FAMILY,
                         uplink->product_family);
TEST_ASSERT_EQUAL_STRING("backend", uplink->firmware_line);
TEST_ASSERT_EQUAL_STRING("uplink", uplink->component);
TEST_ASSERT_EQUAL_STRING("scanner", scanner->component);
TEST_ASSERT_EQUAL_UINT(sizeof(backend_embedded_identity_record_t), 164U);
```

Add a compile-failure fixture in `test_embedded_identity_object.py` that invokes the native compiler once with neither profile and once with both profiles; both must contain `select exactly one backend hardware profile` in stderr.

- [ ] **Step 2: Write the failing host target-matrix tests**

`test_backend_targets.py` must assert the set of exact target keys, the table above, unique target/project pairs, Lite default flags, Fullsize 16 MB flash, 2/3 MB capacity differences, and no target containing `fof_badge` or resolving into `esp32/`.

- [ ] **Step 3: Run the focused tests and confirm the intended failures**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
/Users/billh/.platformio/penv/bin/pio test -e backend-native -f test_backend_identity
python3 -m pytest -q \
  tools/tests/test_backend_targets.py \
  tools/tests/test_embedded_identity_object.py \
  tools/tests/test_source_isolation.py
```

Expected: FAIL because the profile header, Fullsize identities, second native environment, and target registry do not exist.

- [ ] **Step 4: Implement the exact profile macro matrix**

Define these values without runtime guessing:

```c
/* Badge Lite */
#define FOF_BACKEND_PRODUCT_FAMILY "badge_lite"
#define FOF_BACKEND_HARDWARE "seeed_xiao_esp32s3"
#define FOF_BACKEND_UPLINK_TARGET "uplink-s3-backend"
#define FOF_BACKEND_UPLINK_PROJECT "fof_backend_uplink"
#define FOF_BACKEND_SCANNER_TARGET "scanner-s3-combo-backend"
#define FOF_BACKEND_SCANNER_PROJECT "fof_backend_scanner"

/* S3 Fullsize */
#define FOF_BACKEND_PRODUCT_FAMILY "s3_fullsize"
#define FOF_BACKEND_HARDWARE "esp32s3_n16r8_fullsize"
#define FOF_BACKEND_UPLINK_TARGET "uplink-s3-fullsize-backend"
#define FOF_BACKEND_UPLINK_PROJECT "fof_backend_uplink_fullsize"
#define FOF_BACKEND_SCANNER_TARGET "scanner-s3-combo-fullsize-backend"
#define FOF_BACKEND_SCANNER_PROJECT "fof_backend_scanner_fullsize"
```

Both branches define `FOF_BACKEND_FIRMWARE_LINE "backend"`. Add profile-specific flash, app, cache, UART, LED type, and LED GPIO macros used in later tasks.

- [ ] **Step 5: Bump the backend version and preserve the embedded record**

Set `FOF_VERSION_BACKEND "0.2.0-backend"`. Extend only the runtime identity struct; keep every embedded-record field and offset unchanged. Select the four CRC constants by image kind and profile in `backend_embedded_identity.c`.

- [ ] **Step 6: Add explicit native profile environments**

Move all shared native properties into a profile-free custom `[backend_native_base]` section. Make `[env:backend-native]` and `[env:backend-native-fullsize]` extend that section and add exactly one flag themselves: `-DFOF_BACKEND_PROFILE_BADGE_LITE=1` or `-DFOF_BACKEND_PROFILE_S3_FULLSIZE=1`. Do not make one concrete environment extend the other; PlatformIO merges inherited `build_flags` and cannot safely "replace" the Lite flag.

Have `test_backend_targets.py` inspect `pio project config --json-output` and assert the effective flags for each concrete environment contain exactly one profile. Retain the direct compiler fixtures proving zero and two profiles fail.

- [ ] **Step 7: Run both identity profiles and the host checks**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
/Users/billh/.platformio/penv/bin/pio test -e backend-native -f test_backend_identity
/Users/billh/.platformio/penv/bin/pio test -e backend-native-fullsize -f test_backend_identity
python3 -m pytest -q \
  tools/tests/test_backend_targets.py \
  tools/tests/test_embedded_identity_object.py \
  tools/tests/test_source_isolation.py
```

Expected: PASS for both profiles; the embedded schema remains 1/164 bytes and the two product identities remain disjoint.

- [ ] **Step 8: Commit Task 1**

```bash
git add backend-firmware/shared backend-firmware/platformio.ini \
  backend-firmware/test/test_backend_identity \
  backend-firmware/tools/backend_targets.py \
  backend-firmware/tools/tests/test_backend_targets.py \
  backend-firmware/tools/tests/test_embedded_identity_object.py \
  backend-firmware/tools/tests/test_source_isolation.py
git commit -m "v0.2.0-backend: add Fullsize hardware identity"
```

---

### Task 2: Add Fullsize UART, partitions, capacities, and build environments

**Files:**

- Create: `backend-firmware/scanner/sdkconfig.fullsize.defaults`
- Create: `backend-firmware/uplink/sdkconfig.fullsize.defaults`
- Create: `backend-firmware/scanner/partitions_backend_scanner_fullsize_16mb.csv`
- Create: `backend-firmware/uplink/partitions_backend_uplink_fullsize_16mb.csv`
- Modify: `backend-firmware/scanner/platformio.ini`
- Modify: `backend-firmware/uplink/platformio.ini`
- Modify: `backend-firmware/scanner/CMakeLists.txt`
- Modify: `backend-firmware/uplink/CMakeLists.txt`
- Modify: `backend-firmware/scanner/main/CMakeLists.txt`
- Modify: `backend-firmware/uplink/main/CMakeLists.txt`
- Modify: `backend-firmware/scanner/main/main.c`
- Modify: `backend-firmware/scanner/main/core/backend_scanner_runtime.h`
- Modify: `backend-firmware/scanner/main/comms/uart_ota.h`
- Modify: `backend-firmware/scanner/main/comms/uart_ota.c`
- Modify: `backend-firmware/uplink/main/comms/backend_uart_slot.c`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_buffer.h`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_store.h`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_buffer.c`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_store.c`
- Modify: `backend-firmware/uplink/main/ota/backend_scanner_relay.c`
- Modify: `backend-firmware/uplink/main/ota/backend_ota_maintenance.c`
- Modify: `backend-firmware/tools/tests/test_backend_build_contract.py`
- Modify: `backend-firmware/tools/tests/test_backend_scanner_build_contract.py`
- Modify: `backend-firmware/tools/tests/test_backend_uplink_build_contract.py`
- Modify: `backend-firmware/test/test_backend_recovery_policy/test_main.c`
- Modify: `backend-firmware/test/test_backend_uart_ota/test_main.c`
- Modify: `backend-firmware/test/test_backend_firmware_buffer/test_main.c`
- Modify: `backend-firmware/test/test_backend_scanner_relay/test_main.c`

**Interfaces:**

```c
#define FOF_BACKEND_SCANNER_UART_PORT 1
#define FOF_BACKEND_SCANNER_UART_BAUD 921600
/* Selected by backend_hardware_profile.h. */
#define FOF_BACKEND_SCANNER_OTA_CAPACITY UINT32_C(0x200000) /* Lite */
#define FOF_BACKEND_SCANNER_CACHE_CAPACITY UINT32_C(0x200000)
/* Fullsize selects UINT32_C(0x300000) for both macros instead. */
```

- [ ] **Step 1: Write failing pin, capacity, environment, and partition tests**

For Badge Lite, retain scanner TX/RX `1/2`, uplink slots `RX2/TX1` and `RX4/TX3`, 8 MB flash, 2 MB scanner slot/cache. For Fullsize assert scanner TX/RX `17/18`, uplink slot 0 `RX18/TX17`, slot 1 `RX16/TX15`, 16 MB flash, 3 MB scanner slot/cache. Both use 921600 8N1 without flow control.

Add negative tests proving a `0x200001` image is rejected by Lite, a `0x300000` image is admitted by Fullsize, and a `0x300001` image is rejected by Fullsize.

Add source-contract assertions that both profiles compile the same backend detector, threat, buffering, AP, HTTP, command, and OTA modules; only profile adapters/configuration differ. Fullsize scanner builds must not compile AP/HTTP-server code, and neither Fullsize target may include OLED headers, display tasks, display assets, or a source/include path under protected `esp32/`.

- [ ] **Step 2: Run the focused tests and confirm failure**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 -m pytest -q \
  tools/tests/test_backend_build_contract.py \
  tools/tests/test_backend_scanner_build_contract.py \
  tools/tests/test_backend_uplink_build_contract.py
/Users/billh/.platformio/penv/bin/pio test -e backend-native \
  -f test_backend_recovery_policy -f test_backend_uart_ota \
  -f test_backend_firmware_buffer -f test_backend_scanner_relay
/Users/billh/.platformio/penv/bin/pio test -e backend-native-fullsize \
  -f test_backend_recovery_policy -f test_backend_uart_ota \
  -f test_backend_firmware_buffer -f test_backend_scanner_relay
```

Expected: FAIL because the new environments/layouts and profile capacities are absent.

- [ ] **Step 3: Add the exact Fullsize partition tables**

Scanner:

```text
nvs,data,nvs,0x9000,0x6000,
otadata,data,ota,0xf000,0x2000,
phy_init,data,phy,0x11000,0x1000,
ota_0,app,ota_0,0x20000,0x300000,
ota_1,app,ota_1,0x320000,0x300000,
storage,data,spiffs,0x620000,0x100000,
reserved,data,fat,0x720000,0x8e0000,
```

Uplink:

```text
nvs,data,nvs,0x9000,0x6000,
otadata,data,ota,0xf000,0x2000,
phy_init,data,phy,0x11000,0x1000,
ota_0,app,ota_0,0x20000,0x200000,
ota_1,app,ota_1,0x220000,0x200000,
fw_scanner_be,data,0x40,0x420000,0x300000,
storage,data,spiffs,0x720000,0x100000,
reserved,data,fat,0x820000,0x7e0000,
```

Both end at `0x1000000`. Do not include or read `esp32/**/partitions_s3_16mb.csv` at build time.

- [ ] **Step 4: Add explicit Fullsize PlatformIO environments**

Use `board = esp32-s3-devkitc-1`, `board_upload.flash_size = 16MB`, app offset `0x20000`, QIO 80 MHz flash, octal PSRAM, `BOARD_HAS_PSRAM`, and `FOF_BACKEND_PROFILE_S3_FULLSIZE=1`. Preserve Lite as each file's `default_envs` and add `FOF_BACKEND_PROFILE_BADGE_LITE=1` there.

Every embedded environment must pass three explicit CMake cache values through `board_build.cmake_extra_args`: its exact `FOF_BACKEND_PROJECT_NAME`, `FOF_BACKEND_PROFILE_NAME=badge_lite|s3_fullsize`, and `SDKCONFIG_DEFAULTS`. The Lite environments use `sdkconfig.defaults` (preserving the uplink's current value and making the scanner value explicit); the Fullsize environments use `sdkconfig.fullsize.defaults`. Each root `CMakeLists.txt` must reject a missing, mismatched, or unknown project/profile pair before `project(${FOF_BACKEND_PROJECT_NAME})` so the ESP app descriptor and conditional component graph agree with the embedded identity.

Build-contract tests must inspect each generated `sdkconfig` and assert Lite remains 8 MB with its existing settings, while Fullsize selects 16 MB QIO flash and octal PSRAM. A checked-in Fullsize defaults file that was not selected by the build is a test failure.

- [ ] **Step 5: Make pins and scanner capacities profile-selected**

Replace hardcoded GPIO and 2 MB constants with the profile macros. Keep `backend_self_ota.c` at a 2 MB uplink inactive-slot limit for both profiles. Keep `fw_scanner_be` as the cache label. Update the stale comment that says every scanner inactive slot is 2 MB.

- [ ] **Step 6: Run the focused tests for both profiles**

Repeat Step 2. Expected: PASS, including the Lite regression assertions and the Fullsize 3 MB boundary cases.

- [ ] **Step 7: Compile both new Fullsize targets once as an early descriptor check**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware/scanner
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fullsize-backend
cd ../uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fullsize-backend
```

Expected: PASS and produce app descriptors named `fof_backend_scanner_fullsize` and `fof_backend_uplink_fullsize`. Do not flash.

- [ ] **Step 8: Commit Task 2**

```bash
git add backend-firmware/scanner backend-firmware/uplink \
  backend-firmware/shared/backend_hardware_profile.h \
  backend-firmware/test/test_backend_recovery_policy \
  backend-firmware/test/test_backend_uart_ota \
  backend-firmware/test/test_backend_firmware_buffer \
  backend-firmware/test/test_backend_scanner_relay \
  backend-firmware/tools/tests/test_backend_build_contract.py \
  backend-firmware/tools/tests/test_backend_scanner_build_contract.py \
  backend-firmware/tools/tests/test_backend_uplink_build_contract.py
git commit -m "v0.2.0-backend: add Fullsize build profile"
```

---

### Task 3: Add GPIO48 RGB threat rendering while preserving Lite LEDs

**Files:**

- Create: `backend-firmware/shared/backend_rgb_led_pattern.h`
- Create: `backend-firmware/shared/backend_rgb_led_pattern.c`
- Create: `backend-firmware/shared/backend_status_led.h`
- Create: `backend-firmware/shared/backend_status_led.c`
- Create: `backend-firmware/fullsize-components/backend_fullsize_led/CMakeLists.txt`
- Create: `backend-firmware/fullsize-components/backend_fullsize_led/idf_component.yml`
- Create: `backend-firmware/fullsize-components/backend_fullsize_led/include/backend_fullsize_rgb_led.h`
- Create: `backend-firmware/fullsize-components/backend_fullsize_led/backend_fullsize_rgb_led.c`
- Create: `backend-firmware/test/test_backend_rgb_led_pattern/test_main.c`
- Create: `backend-firmware/test/test_backend_rgb_led_adapter/test_main.c`
- Create: `backend-firmware/test/test_backend_rgb_led_adapter/adapter_stubs/led_strip.h`
- Create: `backend-firmware/test/test_backend_rgb_led_adapter/adapter_stubs/led_strip_rmt.h`
- Modify: `backend-firmware/scanner/main/main.c`
- Modify: `backend-firmware/uplink/main/main.c`
- Modify: `backend-firmware/scanner/CMakeLists.txt`
- Modify: `backend-firmware/uplink/CMakeLists.txt`
- Modify: `backend-firmware/scanner/main/CMakeLists.txt`
- Modify: `backend-firmware/uplink/main/CMakeLists.txt`
- Modify: `backend-firmware/platformio.ini`

Do not change the timing ABI or behavior in `shared/backend_led_pattern.[ch]`, `scanner/main/hw/backend_yellow_led.[ch]`, or `uplink/main/hw/backend_yellow_led.[ch]`.

**Interfaces:**

```c
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint16_t duration_ms;
} backend_rgb_led_step_t;

const backend_rgb_led_step_t *backend_rgb_led_pattern(
    backend_led_state_t state, size_t *count);
bool backend_status_led_init(backend_led_state_t initial);
bool backend_status_led_set_state(backend_led_state_t state);
backend_led_state_t backend_status_led_state(void);
```

- [ ] **Step 1: Write failing RGB pattern tests**

Assert these exact steps and no others:

```text
healthy:       green 80, off 2920
degraded:      amber 300, off 300, amber 300, off 1800
drone:         purple 400, off 120, orange 120, off 1360
Meta:          red 100, off 100, blue 100, off 100,
               red 100, off 100, blue 100, off 1000
drone + Meta:  one complete drone sequence, then one complete Meta sequence
UART lost:     yellow 1000, off 1000
fatal:         red 120, off 120, red 120, off 120, red 120, off 800
```

Use bounded brightness constants: green `(0,32,0)`, amber `(32,12,0)`, purple `(24,0,32)`, orange `(32,8,0)`, red `(32,0,0)`, blue `(0,0,32)`, yellow `(32,24,0)`.

- [ ] **Step 2: Write failing RGB adapter and Lite-dispatch tests**

The stub must observe GPIO48, exactly one WS2812, GRB byte order, `led_strip_set_pixel` followed by `led_strip_refresh`, clear on off steps, initialization failure propagation, and atomic revision interruption. A separate dispatch assertion must prove the Lite build still calls the unchanged active-low GPIO21 driver. Build-contract tests must also prove the Lite scanner/uplink compilation database, link map, and app strings contain neither `backend_fullsize_rgb_led`, `led_strip`, nor the Fullsize component path.

- [ ] **Step 3: Confirm the tests fail**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
/Users/billh/.platformio/penv/bin/pio test -e backend-native-fullsize \
  -f test_backend_rgb_led_pattern -f test_backend_rgb_led_adapter
/Users/billh/.platformio/penv/bin/pio test -e backend-native \
  -f test_backend_led_pattern \
  -f test_backend_led_scanner_adapter \
  -f test_backend_led_uplink_adapter
```

Expected: Fullsize suites FAIL because the RGB modules are absent; the existing Lite suites remain PASS.

- [ ] **Step 4: Implement profile dispatch and conditional component selection**

Use one shared Fullsize-only hardware component under `backend-firmware/fullsize-components/backend_fullsize_led/`. Its manifest pins `espressif/led_strip: "3.0.3"`; the component exports `backend_fullsize_rgb_led_*` and retains the yellow driver's atomic state/revision lifecycle so state changes interrupt at the next step.

Before including ESP-IDF's `project.cmake`, each root `CMakeLists.txt` checks `FOF_BACKEND_PROFILE_NAME`. Only `s3_fullsize` appends the Fullsize component directory to `EXTRA_COMPONENT_DIRS`. Each main component builds `hw/backend_yellow_led.c` for `badge_lite`; for `s3_fullsize` it omits the yellow source and adds `backend_fullsize_led` to `REQUIRES`. Unknown values are fatal. `backend_status_led.c` delegates at compile time to the selected API. The profile-free native base contains neither adapter; the Lite concrete test selection adds only yellow adapter fixtures, and the Fullsize concrete selection adds only the shared RGB component plus its stubs.

- [ ] **Step 5: Replace main call sites with the generic API**

Only replace `backend_yellow_led_*` calls/includes in the two backend `main.c` files. Do not rename or edit native/Lite driver symbols.

- [ ] **Step 6: Run all LED tests for both profiles**

Repeat Step 3. Expected: PASS with exact colors and unchanged Lite timing/electrical behavior.

- [ ] **Step 7: Commit Task 3**

```bash
git add backend-firmware/shared/backend_rgb_led_pattern.* \
  backend-firmware/shared/backend_status_led.* \
  backend-firmware/fullsize-components/backend_fullsize_led \
  backend-firmware/scanner/CMakeLists.txt backend-firmware/uplink/CMakeLists.txt \
  backend-firmware/scanner/main backend-firmware/uplink/main \
  backend-firmware/platformio.ini \
  backend-firmware/test/test_backend_rgb_led_pattern \
  backend-firmware/test/test_backend_rgb_led_adapter
git commit -m "v0.2.0-backend: add Fullsize RGB threat LEDs"
```

---

### Task 4: Emit explicit backend platform metadata without changing scanner wire schema

**Files:**

- Create: `backend/tests/fixtures/backend_firmware_fullsize_detection_batch.json`
- Modify: `backend-firmware/shared/backend_upload_batch.h`
- Modify: `backend-firmware/shared/backend_upload_batch.c`
- Modify: `backend-firmware/scanner/main/main.c`
- Modify: `backend-firmware/uplink/main/main.c`
- Modify: `backend-firmware/test/test_backend_upload_batch/test_main.c`
- Modify: `backend-firmware/test/support/backend_serializer_fixture.c`
- Modify: `backend-firmware/tools/emit_serializer_fixture.py`
- Modify: `backend-firmware/tools/tests/test_serializer_fixture.py`
- Modify: `backend/tests/fixtures/backend_firmware_detection_batch.json`
- Modify: `backend/tests/test_backend_firmware_ingest.py`

**Interfaces:**

Add to `backend_batch_context_t` and its top-level JSON:

```c
char product_family[24];  /* badge_lite | s3_fullsize */
char firmware_line[24];   /* backend */
char component[16];       /* uplink */
```

Each scanner object in HTTP JSON also contains `product_family`, `firmware_line`, and `component: scanner`, derived from the uplink's compiled profile plus the scanner's exact target/project/hardware. Do not add fields to `backend_scanner_status_t` or change its UART schema.

- [ ] **Step 1: Write failing serializer and USB-record assertions**

In `test_backend_upload_batch`, assert exact top-level values and exact scanner values for both profiles. Add USB boot/health source-contract assertions that scanner and uplink records include product family, line, component, target, project, hardware, version, MAC, and runtime role where applicable.

Assert Fullsize uplink capabilities exactly include `display_none`, `rgb_led`, `scanner_uart`, `http_uplink`, `config_ap`, `remote_ota`, and `uart_relay_ota`. Lite reports `yellow_led` instead of `rgb_led`. Fullsize scanners report exactly `display_none`, `rgb_led`, `ble_wifi_sensing`, `uart_control`, `uart_ota`, and `remote_ota_via_uplink`; Lite scanners substitute `yellow_led` for `rgb_led`. Scanners never report `config_ap`.

- [ ] **Step 2: Confirm focused tests fail**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
/Users/billh/.platformio/penv/bin/pio test -e backend-native -f test_backend_upload_batch
/Users/billh/.platformio/penv/bin/pio test -e backend-native-fullsize -f test_backend_upload_batch
python3 -m pytest -q tools/tests/test_serializer_fixture.py
cd ../backend
.venv312/bin/pytest -q tests/test_backend_firmware_ingest.py
```

Expected: FAIL because management fields and the Fullsize fixture are missing.

- [ ] **Step 3: Implement runtime metadata and remove hardcoded scanner target**

Replace scanner `APP_TARGET` with `backend_identity_for_image(BACKEND_IMAGE_SCANNER)`. Fill both USB records and uplink batch context from the selected identity. Keep scanner status schema 1 byte-for-byte compatible.

- [ ] **Step 4: Emit and ingest two real serializer fixtures**

Teach `emit_serializer_fixture.py` to compile once with `FOF_BACKEND_PROFILE_BADGE_LITE` and once with `FOF_BACKEND_PROFILE_S3_FULLSIZE`, writing the existing Lite fixture and the new Fullsize fixture atomically. Each fixture must be generated from the real C serializer, not hand-authored JSON.

- [ ] **Step 5: Run the focused tests for both profiles**

Repeat Step 2. Expected: PASS; FastAPI sees both payloads, and no scanner UART schema test changes.

- [ ] **Step 6: Commit Task 4**

```bash
git add backend-firmware/shared/backend_upload_batch.* \
  backend-firmware/scanner/main/main.c backend-firmware/uplink/main/main.c \
  backend-firmware/test/test_backend_upload_batch \
  backend-firmware/test/support/backend_serializer_fixture.c \
  backend-firmware/tools/emit_serializer_fixture.py \
  backend-firmware/tools/tests/test_serializer_fixture.py \
  backend/tests/fixtures/backend_firmware_detection_batch.json \
  backend/tests/fixtures/backend_firmware_fullsize_detection_batch.json \
  backend/tests/test_backend_firmware_ingest.py
git commit -m "v0.2.0-backend: report backend platform identity"
```

---

### Task 5: Add authoritative backend catalog and three-family management gates

**Files:**

- Create: `backend/app/services/firmware_management.py`
- Create: `backend/tests/test_firmware_management.py`
- Modify: `backend/app/services/firmware_manager.py`
- Modify: `backend/app/models/schemas.py`
- Modify: `backend/app/services/backend_node_status.py`
- Modify: `backend/app/routers/detections.py`
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/app/static/dashboard.html`
- Modify: `backend/tests/test_firmware_catalog.py`
- Modify: `backend/tests/test_firmware_auto_endpoints.py`
- Modify: `backend/tests/test_backend_firmware_ingest.py`
- Modify: `backend/tests/test_scanner_ota_relay_paths.py`
- Modify: `backend/tests/test_scanner_firmware_fleet.py`

**Interfaces:**

```python
def resolve_component_management_identity(
    report: dict, component_hint: str | None = None,
) -> dict: ...

def enrich_node_management(heartbeat: dict, now: float | None = None) -> dict: ...

def remote_update_blockers(
    heartbeat: dict, requested_target: str | None = None,
    now: float | None = None,
) -> list[str]: ...
```

- [ ] **Step 1: Write the failing catalog and identity matrix**

Add exact catalog assertions for all Badge, Badge Lite, and Fullsize entries. Add the two Fullsize backend entries with app/cache capacities from the immutable matrix. Generate the cross-family negative matrix: Lite↔Fullsize, scanner↔uplink, every backend image↔native Badge, legacy↔backend, unknown↔all.

Assert `scanner-s3-combo-seed` is not classified as S3 Fullsize and is remote-ineligible. Assert a generic legacy `esp32-s3-devkitc-1` report—including a DevKitC-1 v1.1/GPIO38 fixture—does not resolve to Fullsize from target/project/hardware strings alone.

Retain the existing native Badge parser-policy tests, including diagnostic parsing of a separately supplied future Badge version. Add independent assertions that the embedded USB/factory default stays exactly `0.67.2-badge-defcon34`, both native Badge targets remain separate catalog choices, and no backend target can substitute for either. Do not change native Badge selection or serving behavior in this Fullsize task.

- [ ] **Step 2: Write the failing management/readiness tests**

Cover exact healthy trios plus these blockers: stale heartbeat, missing uplink, missing scanner, duplicate scanner MAC/boot ID, mixed family, contradictory claimed family, wrong component, legacy 0.63, unsupported Seed, unknown target, and identity change during an awaited fetch.

Legacy `uplink-s3`/`fof_uplink` and `scanner-s3-combo`/`fof_scanner` reports resolve to `product_family: null`, `firmware_line: legacy`, `desired_firmware_line: backend`, `migration_required: true`, and `remote_update_eligible: false` until attended inventory supplies positive evidence. Client-reported `flash_size`, LED GPIO, or family claims are never trusted for this promotion.

The separate Fullsize canary may label a board a `s3_fullsize_migration_candidate` only from a server/local-operator-owned inventory receipt proving ROM chip model, exact `0x1000000` flash size, the complete recognized legacy partition table, GPIO48 production-board attestation, bound MAC/physical role, and no secure boot/flash encryption. That candidate label authorizes only the attended USB migration; runtime family becomes `s3_fullsize` only after the exact backend target/project/hardware identity boots. Add negative receipts for 8 MB, GPIO38, mixed trio, and an altered partition table.

- [ ] **Step 3: Assert deployed endpoint compatibility before implementation**

Add tests that `/nodes/firmware/latest/uplink-s3-backend` and the Fullsize latest endpoint each have exactly the existing 11 keys. Add download-header assertions for family/line/component, but forbid those fields in the latest JSON body.

Add size tests: Lite custom scanner uploads reject `0x200001`; Fullsize custom scanner uploads accept through `0x300000` and reject `0x300001`.

- [ ] **Step 4: Run the focused backend tests and confirm failure**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend
.venv312/bin/pytest -q \
  tests/test_firmware_catalog.py \
  tests/test_firmware_auto_endpoints.py \
  tests/test_firmware_management.py \
  tests/test_backend_firmware_ingest.py \
  tests/test_scanner_ota_relay_paths.py \
  tests/test_scanner_firmware_fleet.py
```

Expected: FAIL on absent Fullsize catalog/management support.

- [ ] **Step 5: Extend `FIRMWARE_TYPES` without changing old identities**

Every catalog entry gains server metadata: optional `product_family`, `firmware_line`, `component`, `capabilities`, `partition_capacity`, optional `scanner_cache_capacity`, companion target, and migration/support flags. Exact native Badge entries resolve to `badge`; exact new Fullsize backend identities resolve to `s3_fullsize`; generic legacy `uplink-s3`/`scanner-s3-combo` and `scanner-s3-combo-seed` entries have `product_family: null` and remain remote-ineligible. Do not change existing target/project/hardware triples.

Use target-specific capacity in `_validated_backend_image_info` and custom upload. Preserve longest exact asset matching for the new names.

- [ ] **Step 6: Implement authoritative management enrichment**

Add optional typed `product_family`, `firmware_line`, and `component` to `DroneDetectionBatch`; add them to `STICKY_BATCH_FIELDS`. Preserve reported values for diagnostics, then overwrite management decisions with exact server resolution. Contradictory reported metadata creates a blocker; it never overrides catalog truth.

- [ ] **Step 7: Bind the entire trio for every Fullsize operation**

Extend `_ota_target_snapshot`, `_ota_identity_binding`, `_reported_identities`, `_require_named_family_preflight`, and `_require_ota_compatibility` so a scanner operation binds the host uplink and both scanner identities. Recheck after every awaited firmware fetch. Validate scanner bytes against both the 3 MB scanner slot and the Fullsize uplink's 3 MB cache. Reject legacy conversion and direct legacy fallbacks with HTTP 409 before subprocess or network mutation.

- [ ] **Step 8: Group management UI by family and display blockers**

Group `Badge`, `Badge Lite`, and `S3 Fullsize`, keep uplink/scanner choices separate, show exact targets, show `migration_required`/blockers, and disable the client control when ineligible. Server rejection remains authoritative.

- [ ] **Step 9: Run the focused backend tests once**

Repeat Step 4. Expected: PASS. Do not run the whole backend suite here.

- [ ] **Step 10: Commit Task 5**

```bash
git add backend/app/services/firmware_management.py \
  backend/app/services/firmware_manager.py \
  backend/app/services/backend_node_status.py \
  backend/app/models/schemas.py backend/app/routers/detections.py \
  backend/app/routers/nodes.py backend/app/static/dashboard.html \
  backend/tests/test_firmware_catalog.py \
  backend/tests/test_firmware_auto_endpoints.py \
  backend/tests/test_firmware_management.py \
  backend/tests/test_backend_firmware_ingest.py \
  backend/tests/test_scanner_ota_relay_paths.py \
  backend/tests/test_scanner_firmware_fleet.py
git commit -m "backend: manage S3 Fullsize as an exact family"
```

---

### Task 6: Add a separate durable backend OTA rollout service

**Files:**

- Create: `backend/app/services/backend_ota_commands.py`
- Create: `backend/tests/test_backend_ota_commands.py`
- Create: `backend-firmware/test/fixtures/backend_ota_receipt_v1.json`
- Modify: `backend/app/models/db_models.py`
- Modify: `backend/app/models/schemas.py`
- Modify: `backend/app/routers/nodes.py`
- Modify: `backend/tests/conftest.py`
- Modify: `backend/tests/test_backend_node_commands.py`

**Endpoints:**

```text
POST /nodes/{device_id}/backend-ota/rollouts
GET  /nodes/{device_id}/backend-ota/next
POST /nodes/{device_id}/backend-ota/{operation_id}/events
GET  /nodes/{device_id}/backend-ota/{operation_id}
```

The existing `/commands/next` endpoint and BLE models remain unchanged.

**Rollout request:**

```python
class BackendOtaRolloutRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    components: Literal["all"] = "all"
    apply_mode: Literal["newer_only", "same_version_recovery"] = "newer_only"
```

Version 1 intentionally supports only an exact all-component rollout. The backend always executes scanner0, scanner1, then uplink; single-component recovery is a future schema change, not an implicit subsequence. Tests reject `scanner0`, `scanner1`, `uplink`, missing/extra component values, and attempts to skip the scanners. The backend resolves exact catalog targets; clients cannot submit arbitrary projects, hardware identities, hashes, or URLs.

The backend generates `operation_id` with `secrets.token_hex(16)`: exactly 32 lowercase hexadecimal characters representing 16 bytes. It persists that value before the first poll and reuses it across every probe, apply, event, retry, and reboot resume; device-generated numeric session IDs are not part of this protocol.

The strict probe envelope contains exactly these keys:

```json
{
  "schema": 1,
  "operation_id": "0123456789abcdef0123456789abcdef",
  "type": "backend_ota_probe",
  "component": "scanner0",
  "catalog_name": "scanner-s3-combo-fullsize-backend",
  "expected_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "expected_size": 1048576,
  "expected_uplink_mac": "AA:BB:CC:DD:EE:01",
  "expected_uplink_boot_id": 101,
  "expected_target_mac": "AA:BB:CC:DD:EE:02",
  "expected_target_boot_id": 202,
  "expected_topology_generation": 7,
  "next_sequence": 0
}
```

The strict apply envelope has the same keys, changes `type` to
`backend_ota_apply`, and adds exactly `apply_mode` plus
`probe_receipt_sha256`. For the uplink component, target MAC/boot ID equal the
uplink binding; no identity field is nullable.

Result bodies are also exact. `backend_ota_begin` has `schema`, `operation_id`,
`sequence`, `type`, `component`, and `catalog_name`. `backend_ota_progress`
has those six plus `stage`, `received`, `total`, and `retry_count`.
`backend_ota_end` has `schema`, `operation_id`, `sequence`, `type`,
`component`, `catalog_name`, `state`, `decision`, `error`, `image_writes`,
`target`, `project`, `hardware`, `version`, `actual_mac`, `actual_boot_id`,
`actual_topology_generation`, `role_healthy`, `radio_healthy`,
`rollback_clear`, and `receipt_sha256`. Probe completion uses
`backend_ota_end` with zero image writes and a receipt hash that the subsequent
apply envelope binds.

All event keys are required and non-null. Strings are UTF-8 without control characters. `operation_id` is 32 lowercase hex; SHA values are 64 lowercase hex; catalog/target/project/hardware/version strings are 1–64 ASCII identity characters; MACs are uppercase colon-separated hex; sequences, byte counts, retry counts, boot IDs, and topology generations are unsigned 32-bit decimal values; booleans are JSON booleans. `stage` is one of `metadata`, `download`, `validate`, `stage`, `uart_relay`, `reboot_wait`, `convergence`. `state` is one of `complete`, `no_update`, `failed`, `rolled_back`; `decision` is one of `eligible`, `applied`, `no_update`, `rejected`, `rolled_back`; `error` is one of `none`, `identity_mismatch`, `stale_binding`, `capacity`, `download`, `hash_mismatch`, `uart`, `reboot_timeout`, `health`, `rollback`, `internal`. Identity strings may be empty only when `state=failed` before identity could be read; otherwise they must be non-empty. The only valid state/decision pairs are `complete/eligible` for a probe, `complete/applied` for an apply, `no_update/no_update`, `failed/rejected`, and `rolled_back/rolled_back`; the first three require `error=none`, while the final two require a non-`none` error. Successful probe is exactly `(state=complete, decision=eligible, error=none, image_writes=0)`; only that tuple may generate an apply command.

Every terminal `receipt_sha256` is SHA-256 over this exact UTF-8/LF preimage, not over JSON and never over itself. Values use the normalized representations above; there is no trailing whitespace and there is one final LF:

```text
fof-backend-ota-end-receipt-v1
operation_id={operation_id}
command_type={backend_ota_probe|backend_ota_apply}
component={component}
catalog_name={catalog_name}
expected_sha256={expected_sha256}
expected_size={expected_size}
expected_uplink_mac={expected_uplink_mac}
expected_uplink_boot_id={expected_uplink_boot_id}
expected_target_mac={expected_target_mac}
expected_target_boot_id={expected_target_boot_id}
expected_topology_generation={expected_topology_generation}
state={state}
decision={decision}
error={error}
image_writes={image_writes}
target={target}
project={project}
hardware={hardware}
version={version}
actual_mac={actual_mac}
actual_boot_id={actual_boot_id}
actual_topology_generation={actual_topology_generation}
role_healthy={0|1}
radio_healthy={0|1}
rollback_clear={0|1}
```

Firmware computes the digest before emitting every end event; the backend reconstructs the same preimage from the persisted command binding plus normalized event and rejects a mismatch. For the one successful probe tuple, it copies that accepted digest into the apply command's `probe_receipt_sha256`. `backend_ota_receipt_v1.json` contains probe and apply input fields, exact preimage bytes, and expected digests; both Python and native C tests must consume those golden vectors.

- [ ] **Step 1: Write failing API/state-machine tests**

Assert exact order:

```text
scanner0 probe -> scanner0 apply -> scanner0 convergence
-> scanner1 probe -> scanner1 apply -> scanner1 convergence
-> uplink probe -> uplink apply -> post-reboot uplink convergence
```

Cover durable duplicate polling, duplicate identical events, conflicting event replay, restart/resume, terminal history, one active rollout per node, and database outage returning retryable 503.

Also assert that `components="all"` is the only accepted v1 request and that every individual-component/unknown value receives 422 without creating a rollout.

- [ ] **Step 2: Write failing safety tests**

Assert no firmware metadata fetch occurs before exact trio preflight. Revalidate the complete binding after the awaited catalog fetch. Reject legacy, Badge, Lite, mixed, stale, incomplete, and unknown nodes before a row becomes active. Assert no call reaches legacy OTA helpers.

- [ ] **Step 3: Confirm failures**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend
.venv312/bin/pytest -q \
  tests/test_backend_ota_commands.py \
  tests/test_backend_node_commands.py \
  tests/test_scanner_ota_relay_paths.py
```

Expected: FAIL because the independent rollout service and routes are missing; existing BLE tests remain PASS.

- [ ] **Step 4: Implement strict probe/apply envelopes**

`backend_ota_probe` contains operation ID, component, exact catalog name/SHA/size, expected uplink MAC/boot ID, expected target MAC/boot ID, and topology generation. `backend_ota_apply` copies the accepted probe binding and adds the exact apply mode. Every Pydantic model uses `extra="forbid"`.

Implement and test the canonical probe-receipt builder before accepting apply. The Python digest must match the shared golden vector exactly; altered field order, case, newline, normalized value, or receipt hash is rejected.

- [ ] **Step 5: Implement strict event models and transitions**

Support exact event types `backend_ota_begin`, `backend_ota_progress`, and `backend_ota_end`. Progress carries component, stage, bytes received/total, and retry count. End is `complete`, `no_update`, `failed`, or `rolled_back`, plus exact post-update identity and convergence fields. Enforce monotonic sequence and stage order, while accepting byte-identical duplicate delivery idempotently.

- [ ] **Step 6: Persist the isolated lifecycle**

Use dedicated `BackendOtaRollout` and `BackendOtaEvent` models/tables rather than adding fields to `NodeCommand`. The one-active-rollout key is nullable after terminal completion. A component cannot advance until the prior component has exact identity, new boot evidence when a reboot is expected, role/radio health, UART rejoin, and rollback clearance.

- [ ] **Step 7: Run the focused service tests**

Repeat Step 3. Expected: PASS, with existing BLE command response bodies byte-for-byte unchanged.

- [ ] **Step 8: Commit Task 6**

```bash
git add backend/app/services/backend_ota_commands.py \
  backend/app/models/db_models.py backend/app/models/schemas.py \
  backend/app/routers/nodes.py backend/tests/conftest.py \
  backend-firmware/test/fixtures/backend_ota_receipt_v1.json \
  backend/tests/test_backend_ota_commands.py \
  backend/tests/test_backend_node_commands.py \
  backend/tests/test_scanner_ota_relay_paths.py
git commit -m "backend: add guarded Fullsize OTA rollouts"
```

---

### Task 7: Connect Fullsize network commands to uplink self-OTA and UART relay

**Files:**

- Create: `backend-firmware/uplink/main/network/backend_ota_command_client.h`
- Create: `backend-firmware/uplink/main/network/backend_ota_command_client.c`
- Create: `backend-firmware/shared/backend_ota_operation_id.h`
- Create: `backend-firmware/shared/backend_ota_operation_id.c`
- Create: `backend-firmware/test/test_backend_ota_command_client/test_main.c`
- Create: `backend-firmware/test/test_backend_ota_profile_gate/test_main.c`
- Create: `backend-firmware/uplink/main/storage/backend_ota_event_outbox.h`
- Create: `backend-firmware/uplink/main/storage/backend_ota_event_outbox.c`
- Create: `backend-firmware/test/test_backend_ota_event_outbox/test_main.c`
- Modify: `backend-firmware/uplink/main/main.c`
- Modify: `backend-firmware/uplink/main/CMakeLists.txt`
- Modify: `backend-firmware/platformio.ini`
- Modify: `backend-firmware/uplink/main/ota/backend_ota_maintenance.h`
- Modify: `backend-firmware/uplink/main/ota/backend_ota_maintenance.c`
- Modify: `backend-firmware/uplink/main/ota/backend_scanner_relay.h`
- Modify: `backend-firmware/uplink/main/ota/backend_scanner_relay.c`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_buffer.h`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_buffer.c`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_store.h`
- Modify: `backend-firmware/uplink/main/storage/backend_firmware_store.c`
- Modify: `backend-firmware/uplink/main/storage/backend_ota_journal.h`
- Modify: `backend-firmware/uplink/main/storage/backend_ota_journal.c`
- Modify: `backend-firmware/uplink/main/network/backend_http_transport.h`
- Modify: `backend-firmware/uplink/main/network/backend_http_transport.c`
- Modify: `backend-firmware/test/test_backend_ota_maintenance/test_main.c`
- Modify: `backend-firmware/test/test_backend_scanner_relay/test_main.c`
- Modify: `backend-firmware/test/test_backend_firmware_buffer/test_main.c`
- Create: `backend-firmware/test/test_backend_firmware_store/test_main.c`
- Modify: `backend-firmware/test/test_backend_ota_journal/test_main.c`
- Modify: `backend-firmware/test/test_backend_http_transport/test_main.c`
- Modify: `backend-firmware/tools/tests/test_backend_build_contract.py`

**Interfaces:**

```c
typedef enum {
    BACKEND_OTA_PROGRESS_METADATA,
    BACKEND_OTA_PROGRESS_DOWNLOAD,
    BACKEND_OTA_PROGRESS_VALIDATE,
    BACKEND_OTA_PROGRESS_STAGE,
    BACKEND_OTA_PROGRESS_UART_RELAY,
    BACKEND_OTA_PROGRESS_REBOOT_WAIT,
    BACKEND_OTA_PROGRESS_CONVERGENCE,
} backend_ota_progress_stage_t;

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE) && \
    !defined(FOF_BACKEND_PROFILE_BADGE_LITE)
typedef struct {
    uint8_t bytes[16];
} backend_ota_operation_id_t;
#elif defined(FOF_BACKEND_PROFILE_BADGE_LITE) && \
      !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
typedef uint32_t backend_ota_operation_id_t; /* Existing Lite ABI. */
#else
#error "select exactly one backend hardware profile"
#endif

typedef struct {
    backend_ota_operation_id_t operation_id;
    backend_ota_component_t component;
    backend_ota_progress_stage_t stage;
    size_t received;
    size_t total;
    uint32_t retry_count;
} backend_ota_progress_t;

#define BACKEND_OTA_EVENT_MAX_BYTES 1536U
typedef struct {
    backend_ota_operation_id_t operation_id;
    uint32_t sequence;
    uint16_t body_length;
    uint8_t body_sha256[32];
    uint8_t body[BACKEND_OTA_EVENT_MAX_BYTES];
} backend_ota_pending_event_t;
```

`shared/backend_ota_operation_id.[ch]` owns the profile-selected operation type and helpers. Its header includes `backend_hardware_profile.h` before this explicit Fullsize/Lite/error chain; it cannot rely on include order or silently select Lite. Under `FOF_BACKEND_PROFILE_S3_FULLSIZE`, it is the 16-byte type above with strict lower-hex decode, canonical encode, equality, and constant-time equality helpers. Under `FOF_BACKEND_PROFILE_BADGE_LITE`, it remains the existing `uint32_t` type and behavior so Lite journal bytes, serial evidence, buffer ownership, `next_operation_id`, and in-flight resume remain byte-for-byte compatible. Direct compiler fixtures include this header with neither and both profile flags and require the exact compile error in both cases.

For Fullsize only, the strict parser decodes the 32 lowercase-hex server ID into `backend_ota_operation_id_t`; the event encoder always writes it back as the same canonical 32 lowercase hex. Profile-select the operation ID field in `backend_ota_request_t`, `backend_ota_evidence_t`, buffer ownership, relay claims, firmware-store claims, and `backend_ota_journal_record_t`; Fullsize removes `next_operation_id` and never invents a local rollout ID. Image generation, scanner UART session ID, topology generation, and role generation remain distinct numeric protocol fields and must never be substituted for the rollout ID. Fullsize gets a distinct journal schema/version and CRC covering all 16 bytes; an unexpected numeric journal fails closed and requires attended recovery. Lite keeps its existing journal schema/version and golden encoded records exactly unchanged.

- [ ] **Step 1: Write failing strict-decoder tests**

The OTA decoder must reject missing/extra/duplicate keys, noncanonical operation IDs, malformed MAC/SHA, wrong component/catalog pairing, wrong Fullsize target/project/hardware, stale boot/topology binding, invalid mode, oversized JSON, and BLE envelopes. The existing BLE decoder must continue rejecting OTA envelopes without changing its accepted eight-field contract. Add native receipt generation/verification against `test/fixtures/backend_ota_receipt_v1.json`; one-byte changes in every bound field must change the digest and fail apply.

- [ ] **Step 2: Write failing orchestration tests**

Map probe to `backend_ota_maintenance_run_probe` and apply to `backend_ota_maintenance_request_apply`. Assert scanner0 and scanner1 are never active together, uplink cannot start before both scanners converge, events are queued with monotonic stages, bounded retry counts are reported, and normal detection UART traffic resumes after success or failure.

Add power-loss/reboot tests at accepted-command, download, staged, UART relay, and reboot-wait boundaries. Every persisted journal record, buffer owner, relay claim, and emitted event must retain the same 16-byte server operation ID. Byte-identical duplicate commands/events are idempotent; a different ID cannot claim an existing buffer; stale numeric-schema journals fail closed; reboot resumes or rolls back the same rollout without inventing a local ID.

Add event-outbox crash tests at: canonical encoding before persistence, persisted-before-send, after server acceptance but before local ACK clearing, ACK clearing before the next event, and torn/corrupt two-slot records. Before-send and after-send-before-clear recovery must retransmit the exact same stored bytes and sequence. The backend may receive duplicates but applies their effect once through its operation/sequence/body-digest idempotency key. No later event may overwrite an unacknowledged body.

- [ ] **Step 3: Add focused HTTP regression tests required by remote OTA**

Add exact cases for a bare 204 poll response, 304 no-body handling, `Accept: application/octet-stream` on firmware downloads, fixed/chunked 404 bodies discarded without invoking the binary sink, and arbitrary bytes containing `0x00`, `0x80`, and `0xff`. Add an ESP-platform guard test that refuses synchronous transport work before lwIP initialization or from the lwIP/core-owning context rather than queueing a callback and waiting on itself.

- [ ] **Step 4: Confirm failures**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 -m pytest -q tools/tests/test_backend_build_contract.py
/Users/billh/.platformio/penv/bin/pio test -e backend-native-fullsize \
  -f test_backend_ota_command_client \
  -f test_backend_ota_maintenance \
  -f test_backend_scanner_relay \
  -f test_backend_firmware_buffer \
  -f test_backend_firmware_store \
  -f test_backend_ota_journal \
  -f test_backend_ota_event_outbox \
  -f test_backend_http_transport
/Users/billh/.platformio/penv/bin/pio test -e backend-native \
  -f test_backend_command_client \
  -f test_backend_ota_profile_gate \
  -f test_backend_ota_maintenance \
  -f test_backend_firmware_buffer \
  -f test_backend_ota_journal \
  -f test_backend_http_transport
```

Expected: FAIL on absent Fullsize OTA command/parser/progress behavior and the new gate assertions; legacy BLE decoder tests remain PASS.

- [ ] **Step 5: Implement a separate poll/apply/event client**

Poll only `/nodes/{device_id}/backend-ota/next`; never multiplex OTA fields into `/commands/next`. Bind each accepted envelope to exact local uplink/scanner MAC, boot ID, topology generation, selected profile, catalog target, expected SHA, size, and the decoded 16-byte operation ID before any download or cache write.

Compile and start this client only when `FOF_BACKEND_PROFILE_S3_FULLSIZE` is selected. The uplink main-component CMake source list includes `backend_ota_command_client.c` only for `FOF_BACKEND_PROFILE_NAME=s3_fullsize`, and `main.c` wraps initialization/task creation in the same fail-closed profile guard. The Lite native/embedded source lists omit the client entirely. Lite build-contract tests inspect its compilation database, link map, and firmware strings to prove there is no client object, `/backend-ota/next` path, poll task, or strict OTA decoder; a Lite startup fixture proves no OTA task is created. Fullsize envelopes supplied to any compiled Lite parser remain rejected by the unchanged BLE decoder.

- [ ] **Step 6: Execute mutation on the OTA worker and report from the command worker**

Use a bounded FreeRTOS request queue from network command handling to the existing OTA worker. Add a bounded progress queue in the opposite direction. The OTA maintenance/relay callbacks publish stage, byte, retry, and terminal evidence; the command worker posts events and retries byte-identical bodies until acknowledged. Never perform a long relay in the command polling loop.

Persist the server operation ID and canonical command/receipt binding before acknowledging an apply. Buffer ownership and journal replay compare all 16 bytes in constant time.

For each event, encode the complete canonical JSON once into a bounded buffer, calculate its SHA-256, and durably write the exact length, bytes, operation ID, sequence, and digest to a two-slot CRC/generation outbox before the first POST. POST only those stored bytes. After a matching 2xx ACK, atomically clear/advance the outbox before encoding the next event. On reboot, a valid pending record is retransmitted byte-for-byte with the same sequence until acknowledged; a torn/corrupt record blocks progression and requires recovery. A digest alone is never treated as sufficient to reconstruct an event.

- [ ] **Step 7: Reuse guarded maintenance and relay logic**

Do not create a second flash writer. Uplink apply calls the existing inactive-slot self updater. Scanner apply downloads and validates the complete exact image into `fw_scanner_be`, claims one scanner, performs the dry run and guarded 921600-baud UART relay, waits for exact reboot/identity/role/radio/rollback convergence, releases the claim, then permits the next component.

- [ ] **Step 8: Harden only the HTTP paths needed by the new channel**

Make body framing status-aware, discard non-success bodies without touching the OTA sink, emit the correct binary `Accept` header, and fail closed on unsafe lwIP execution context. Preserve existing inclusive total/no-progress deadlines.

- [ ] **Step 9: Run both profile regressions**

Repeat Step 4. Expected: PASS; Fullsize command-to-maintenance flow and reboot resume use the same server ID, while the Lite binary contains and starts no Fullsize OTA client and its BLE command/latest polling contracts remain unchanged.

- [ ] **Step 10: Commit Task 7**

```bash
git add backend-firmware/uplink/main/network/backend_ota_command_client.* \
  backend-firmware/uplink/main/network/backend_http_transport.* \
  backend-firmware/shared/backend_ota_operation_id.* \
  backend-firmware/uplink/main/ota backend-firmware/uplink/main/storage \
  backend-firmware/uplink/main/main.c \
  backend-firmware/uplink/main/CMakeLists.txt backend-firmware/platformio.ini \
  backend-firmware/test/test_backend_ota_command_client \
  backend-firmware/test/test_backend_ota_profile_gate \
  backend-firmware/test/test_backend_ota_maintenance \
  backend-firmware/test/test_backend_scanner_relay \
  backend-firmware/test/test_backend_firmware_buffer \
  backend-firmware/test/test_backend_firmware_store \
  backend-firmware/test/test_backend_ota_journal \
  backend-firmware/test/test_backend_ota_event_outbox \
  backend-firmware/test/test_backend_http_transport \
  backend-firmware/tools/tests/test_backend_build_contract.py
git commit -m "v0.2.0-backend: relay network updates over UART"
```

---

### Task 8: Package and verify all four backend targets without touching Badge release tooling

**Files:**

- Create: `backend-firmware/web-flasher/manifest-uplink-s3-fullsize-backend.json`
- Create: `backend-firmware/web-flasher/manifest-scanner-s3-combo-fullsize-backend.json`
- Create: `backend-firmware/web-flasher/s3-fullsize.html`
- Modify: `backend-firmware/tools/firmware_identity.py`
- Modify: `backend-firmware/tools/verify_backend_build.py`
- Modify: `backend-firmware/tools/pio_verify_backend_build.py`
- Modify: `backend-firmware/tools/verify_backend_release.py`
- Modify: `backend-firmware/tools/tests/test_firmware_identity.py`
- Modify: `backend-firmware/tools/tests/test_verify_backend_build.py`
- Modify: `backend-firmware/tools/tests/test_verify_backend_release.py`
- Modify: `backend-firmware/tools/tests/test_backend_web_flasher.py`
- Modify: `backend-firmware/web-flasher/build.sh`
- Modify: `backend-firmware/web-flasher/manifest-uplink-s3-backend.json`
- Modify: `backend-firmware/web-flasher/manifest-scanner-s3-combo-backend.json`
- Modify: `backend-firmware/release/backend-release-index.json`
- Modify: `.github/workflows/backend-firmware.yml`

**Contract:** Release index schema stays 1 and contains exactly four backend targets. Family/component/capacity are resolved by the internal target registry and backend catalog; do not add unversioned fields that break the existing Lite canary parser.

- [ ] **Step 1: Write failing four-target verifier tests**

Replace global flash/capacity assumptions with per-target specs. Test all four exact environments, projects, hardware strings, flash sizes, app capacities, cache capacities, partition CSVs, identity kinds, artifact directories, and manifests. Add 8 MB↔16 MB partition/artifact substitution negatives.

- [ ] **Step 2: Write failing web-flasher separation tests**

Keep existing Lite choices intact. `s3-fullsize.html` must reference only the two Fullsize manifests and visibly state: `16 MB N16R8 only`, `not Badge`, `not Badge Lite`, `recovery only`, and `not the initial 0.63 conversion path`. Each Fullsize manifest uses offsets `0`, `0x8000`, `0xf000`, and `0x20000`.

- [ ] **Step 3: Confirm tooling tests fail**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 -m pytest -q \
  tools/tests/test_firmware_identity.py \
  tools/tests/test_verify_backend_build.py \
  tools/tests/test_verify_backend_release.py \
  tools/tests/test_backend_web_flasher.py
```

Expected: FAIL because tooling assumes two 8 MB targets.

- [ ] **Step 4: Generalize verification around `backend_targets.py`**

Make release specs target-keyed, not merely `scanner`/`uplink` keyed. Carry flash size, app capacity, cache capacity, partition expectations, environment, project, hardware, product family, component, artifact directory, and manifest. `pio_verify_backend_build.py` uses the selected environment's capacity. The verifier requires all four sets before atomically publishing an index/package.

- [ ] **Step 5: Build and package all four targets exactly once**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware
bash backend-firmware/web-flasher/build.sh
```

`build.sh` must clean/build each exact environment once, run every post-build
identity gate, package only verified outputs, and atomically regenerate the
four-target index/manifests. Expected: PASS. Do not flash.

- [ ] **Step 6: Verify the packaged release without rebuilding**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 tools/verify_backend_release.py \
  --index release/backend-release-index.json \
  --flasher web-flasher
```

Expected: exactly four targets at `0.2.0-backend`, correct schema-1 identity records, correct app descriptors, exact partition ranges, and matching SHA-256/CRC32.

- [ ] **Step 7: Update CI without changing native release workflows**

Build/test/package all four in `.github/workflows/backend-firmware.yml`. Keep distribution as the existing private Actions artifact; do not publish a GitHub Release. Extend protected-path audit to Android and its workflow.

- [ ] **Step 8: Re-run the four focused tooling tests**

Repeat Step 3. Expected: PASS.

- [ ] **Step 9: Commit Task 8**

```bash
git add backend-firmware/tools backend-firmware/web-flasher \
  backend-firmware/release/backend-release-index.json \
  .github/workflows/backend-firmware.yml
git commit -m "v0.2.0-backend: package four backend targets"
```

---

### Task 9: Add a dedicated one-time Fullsize migration and remote-OTA canary

**Files:**

- Create: `backend-firmware/tools/s3_fullsize_canary.py`
- Create: `backend-firmware/tools/s3_fullsize_canary_evidence.py`
- Create: `backend-firmware/tools/tests/test_s3_fullsize_canary.py`
- Create: `backend-firmware/tools/tests/test_s3_fullsize_canary_evidence.py`
- Modify: `backend-firmware/tools/backend_canary.py`
- Modify: `backend-firmware/tools/tests/test_backend_canary.py`
- Modify: `backend-firmware/tools/backend_canary_evidence.py`
- Modify: `backend-firmware/tools/tests/test_backend_canary_evidence.py`
- Modify: `backend-firmware/.gitignore`

The existing `backend_canary.py` remains Badge Lite-only. Its only four-target change is selecting its exact two Lite entries from the expanded release index.

- [ ] **Step 1: Write failing profile-isolation and destructive-action tests**

Fullsize canary accepts a legacy board only after positive attended inventory proves ESP32-S3, exact 16 MB flash, the recognized full legacy partition map, GPIO48 production-board layout, and a bound physical role/MAC. Generic `esp32-s3-devkitc-1` identity strings are insufficient. It rejects 8 MB XIAO, GPIO38 DevKitC-1 v1.1, native Badge, Badge Lite, Seed, C5, mixed identities, secure boot, flash encryption, unknown partitions, mismatched MAC/port, symlinks, broad paths, and release artifacts outside its exact Fullsize pair.

Lite canary tests must prove it still rejects Fullsize and remains 8 MB-only.

- [ ] **Step 2: Write failing backup/order tests**

Require two independent `read_flash 0x0 0x1000000` captures per board with matching hash, plus focused NVS, partition-table, and app-descriptor reads cross-checked against the full image. Require private directory mode 0700/files 0600, exact restore plan before first write, one-use MAC-bound challenge, scanners-first order, and NVS omission from every write command.

There is exactly one original full backup per role. Tests must reject any routine-OTA attempt to create another full backup.

- [ ] **Step 3: Write failing remote proof tests**

The canary must stage exact Fullsize binaries through the backend catalog, disconnect USB data after migration, create a `/backend-ota/rollouts` operation, and consume its event history. Assert scanner0 convergence before scanner1 begins, both scanners before uplink, exact GETs to Fullsize latest/download endpoints, 921600-baud relay evidence, changed boot IDs, exact post-update identities, role/radio health, rollback clearance, and no legacy/USB OTA route evidence.

- [ ] **Step 4: Confirm failures**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 -m pytest -q \
  tools/tests/test_s3_fullsize_canary.py \
  tools/tests/test_s3_fullsize_canary_evidence.py \
  tools/tests/test_backend_canary.py \
  tools/tests/test_backend_canary_evidence.py
```

Expected: FAIL because the dedicated Fullsize workflow does not exist.

- [ ] **Step 5: Implement the Fullsize-only state machine**

Use roles `scanner0`, `scanner1`, `uplink`; bind each to exact USB port, ROM MAC, legacy app descriptor, decoded partition SHA, 16 MB flash size, and an operator-confirmed GPIO48 board-layout receipt. The tool records `s3_fullsize_migration_candidate` only in private migration evidence and never reports it as an installed runtime family. Generate restore commands containing the explicit port/MAC/flash size/mode/frequency and the board's original full backup. Warn that legacy uplink storage at `0xa20000` moves to backend storage at `0x720000` and is not migrated automatically.

- [ ] **Step 6: Implement scanners-first conversion gates**

Write only bootloader `0x0`, partition table `0x8000`, initial OTA data `0xf000`, and application `0x20000`. Never erase/write NVS. Require exact provisional and final Fullsize identity/health for scanner0, then scanner1, then uplink. Stop on the first failure and retain unused boards untouched.

- [ ] **Step 7: Implement network-only routine OTA evidence**

After conversion, USB may collect read-only boot/status evidence but may not send OTA mutation commands. Drive a newer Fullsize build through the backend rollout endpoint and validate its ordered event history plus resulting heartbeats. Add failure scenarios for network loss before commit, stale binding, UART disconnect mid-transfer, duplicate event delivery, journal resume, and scanner rollback blocking the next component.

- [ ] **Step 8: Run all four canary suites**

Repeat Step 4. Expected: PASS; Lite and Fullsize tools cannot select each other's hardware or artifacts.

- [ ] **Step 9: Commit Task 9**

```bash
git add backend-firmware/tools/s3_fullsize_canary.py \
  backend-firmware/tools/s3_fullsize_canary_evidence.py \
  backend-firmware/tools/tests/test_s3_fullsize_canary.py \
  backend-firmware/tools/tests/test_s3_fullsize_canary_evidence.py \
  backend-firmware/tools/backend_canary.py \
  backend-firmware/tools/backend_canary_evidence.py \
  backend-firmware/tools/tests/test_backend_canary.py \
  backend-firmware/tools/tests/test_backend_canary_evidence.py \
  backend-firmware/.gitignore
git commit -m "fullsize: add guarded migration and OTA canary"
```

---

### Task 10: Document, audit, and produce a flash-readiness handoff

**Files:**

- Create: `docs/backend-firmware-platforms.md`
- Create: `docs/s3-fullsize-migration.md`
- Create: `docs/s3-fullsize-network-uart-ota.md`
- Create: `docs/s3-fullsize-ota-recovery.md`
- Modify: `backend-firmware/README.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/release-checklist.md`

Leave `docs/backend-firmware-canary.md` explicitly Badge Lite-only and do not edit `docs/badge/**`.

- [ ] **Step 1: Write the exact operator documentation**

Document the three-family identity matrix, Fullsize wiring, 16 MB partitions, GPIO48 RGB meanings, AP settings, one-time backups/restores, scanners-first USB conversion, routine OTA without repeated backups, network→uplink→UART flow, progress/retry/rollback states, and unsupported mixed-generation warnings.

- [ ] **Step 2: Run the consolidated focused firmware/tooling gate once**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 -m pytest -q \
  tools/tests/test_backend_targets.py \
  tools/tests/test_backend_build_contract.py \
  tools/tests/test_backend_scanner_build_contract.py \
  tools/tests/test_backend_uplink_build_contract.py \
  tools/tests/test_firmware_identity.py \
  tools/tests/test_verify_backend_build.py \
  tools/tests/test_serializer_fixture.py \
  tools/tests/test_backend_web_flasher.py \
  tools/tests/test_verify_backend_release.py \
  tools/tests/test_s3_fullsize_canary.py \
  tools/tests/test_s3_fullsize_canary_evidence.py \
  tools/tests/test_backend_canary.py \
  tools/tests/test_backend_canary_evidence.py
/Users/billh/.platformio/penv/bin/pio test -e backend-native \
  -f test_backend_identity -f test_backend_led_pattern \
  -f test_backend_recovery_policy -f test_backend_uart_ota \
  -f test_backend_firmware_buffer -f test_backend_firmware_store \
  -f test_backend_ota_journal -f test_backend_scanner_relay \
  -f test_backend_ota_maintenance -f test_backend_http_transport \
  -f test_ported_detectors -f test_backend_detection_router \
  -f test_backend_threat_policy -f test_backend_upload_fifo \
  -f test_backend_config -f test_backend_portal_routes \
  -f test_backend_upload_batch -f test_backend_command_client \
  -f test_backend_ota_profile_gate
/Users/billh/.platformio/penv/bin/pio test -e backend-native-fullsize \
  -f test_backend_identity -f test_backend_rgb_led_pattern \
  -f test_backend_rgb_led_adapter -f test_backend_recovery_policy \
  -f test_backend_uart_ota -f test_backend_firmware_buffer \
  -f test_backend_firmware_store \
  -f test_backend_ota_journal -f test_backend_ota_event_outbox \
  -f test_backend_scanner_relay \
  -f test_backend_ota_maintenance \
  -f test_backend_ota_command_client -f test_backend_http_transport \
  -f test_backend_upload_batch
```

Expected: PASS.

- [ ] **Step 3: Run the consolidated focused FastAPI gate once**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend
.venv312/bin/pytest -q \
  tests/test_firmware_catalog.py \
  tests/test_firmware_auto_endpoints.py \
  tests/test_firmware_management.py \
  tests/test_backend_firmware_ingest.py \
  tests/test_backend_ota_commands.py \
  tests/test_backend_node_commands.py \
  tests/test_scanner_ota_relay_paths.py \
  tests/test_scanner_firmware_fleet.py
```

Expected: PASS. Run the broad backend suite only in the final CI workflow, not repeatedly on the workstation.

- [ ] **Step 4: Reverify release artifacts and source isolation**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware/backend-firmware
python3 tools/verify_backend_release.py \
  --index release/backend-release-index.json \
  --flasher web-flasher
python3 tools/check_source_isolation.py --root .
cd ..
git diff --name-only cf378db...HEAD -- \
  android esp32 scripts flash-badges.command tools/badge_flasher \
  .github/workflows/android-build.yml \
  .github/workflows/esp32-web-flasher.yml docs/badge
git diff --name-only cf378db -- \
  android esp32 scripts flash-badges.command tools/badge_flasher \
  .github/workflows/android-build.yml \
  .github/workflows/esp32-web-flasher.yml docs/badge
git diff --cached --name-only -- \
  android esp32 scripts flash-badges.command tools/badge_flasher \
  .github/workflows/android-build.yml \
  .github/workflows/esp32-web-flasher.yml docs/badge
git diff --name-only -- \
  android esp32 scripts flash-badges.command tools/badge_flasher \
  .github/workflows/android-build.yml \
  .github/workflows/esp32-web-flasher.yml docs/badge
git ls-files --others --exclude-standard -- \
  android esp32 scripts flash-badges.command tools/badge_flasher \
  .github/workflows/android-build.yml \
  .github/workflows/esp32-web-flasher.yml docs/badge
```

Expected: release verifier PASS; every protected committed, baseline-to-worktree, staged, unstaged, and untracked audit prints no paths.

- [ ] **Step 5: Scan for incomplete implementation and identity drift**

```bash
cd /Users/billh/gai/friendorfoe/.worktrees/backend-firmware
rg -n "TBD|TODO|FIXME|implement later|placeholder|scanner-s3-combo-seed.*s3_fullsize" \
  backend-firmware backend/app backend/tests docs/s3-fullsize-* \
  docs/backend-firmware-platforms.md
rg -n "uplink-s3-fullsize-backend|scanner-s3-combo-fullsize-backend|esp32s3_n16r8_fullsize" \
  backend-firmware backend/app backend/tests
```

Expected: the placeholder scan has no implementation placeholders; exact Fullsize identifiers occur only in the planned backend paths and tests.

- [ ] **Step 6: Commit documentation and final audit evidence**

```bash
git add backend-firmware/README.md docs/backend-firmware-platforms.md \
  docs/s3-fullsize-migration.md docs/s3-fullsize-network-uart-ota.md \
  docs/s3-fullsize-ota-recovery.md docs/ARCHITECTURE.md \
  docs/release-checklist.md
git commit -m "docs: document S3 Fullsize backend operations"
```

- [ ] **Step 7: Stop before hardware writes and present the readiness receipt**

Report the four artifact hashes, exact Fullsize target/project/hardware identities, protected-path audit, focused test counts, connected USB inventory, and the first read-only canary command. Do not flash until the operator separately approves each exact role/MAC write after the one-time backups and restore plans exist.

---

## Physical acceptance gate after implementation

This is an attended operation, not part of unattended plan execution.

1. Run read-only three-board inventory and bind scanner0/BLE, scanner1/Wi-Fi, and uplink by MAC.
2. Capture and verify the one-time double-read 16 MB backups and restore plans.
3. Ask for explicit scanner0 role/MAC approval; flash and verify it.
4. Ask for explicit scanner1 role/MAC approval; flash and verify it.
5. Ask for explicit uplink role/MAC approval; flash and verify it.
6. Configure/verify AP networks, backend URL, device name/location, and automatic-update preference.
7. Prove exact trio heartbeat, BLE/Wi-Fi roles, UART continuity, HTTP upload, offline queue drain, real drone purple/orange LED behavior, and real Meta red/blue behavior on all three boards.
8. After the USB baseline is accepted at `0.2.0-backend`, create a separately reviewed version-only `0.2.1-backend` canary commit, rebuild/verify all four backend targets once, and privately stage only its two Fullsize images. With USB data disconnected, run the newer-only ordered backend rollout and verify both scanner UART updates plus uplink self-update and rollback health. Do not publish or deploy the `0.2.1-backend` Lite images as part of this hardware proof.

Remote Fullsize OTA is not declared ready until step 8 succeeds on the physical trio.
