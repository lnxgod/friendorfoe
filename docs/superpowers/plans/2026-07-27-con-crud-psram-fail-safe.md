# CON CRUD PSRAM Fail-Safe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CON CRUD fail closed without consuming internal RAM when badge PSRAM is unhealthy, then prove the same `.88` canary on two complete badges through the one-cable uplink/UART update path.

**Architecture:** Keep the game controller admission policy pure and native-testable, while the VHCI adapter samples the real ESP-IDF heaps once before its only initialization attempt. Reuse the existing strict PSRAM allocator for the ST7735 framebuffer, preserve every USB/UART/scanner path, and mutate one physically identified badge at a time from a single frozen artifact set.

**Tech Stack:** ESP-IDF C, PlatformIO/Unity native tests, repo strict firmware verifiers, native USB serial, existing badge flasher and factory seed verifier.

## Global Constraints

- Work only in `/Users/billh/gai/friendorfoe/.worktrees/defcon34-badge-final`.
- Change only `FOF_VERSION_BADGE_CANARY`; production badge versions and artifacts remain unchanged.
- The uplink remains the only game advertiser; neither scanner advertises.
- Preserve the four privacy lanes, BLE-primary scanning, Wi-Fi-primary scanning, Android USB control, and scanner UART updating.
- Keep `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`, `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`, and `CONFIG_SPIRAM_USE_MALLOC=n`.
- In the private game canary, the 40,960-byte LCD framebuffer is PSRAM-only;
  production allocation behavior is unchanged. The 5,120-byte DMA staging
  chunk remains internal/DMA-capable in both builds.
- Game admission requires 8 MiB total and 5 MiB free PSRAM, 24 KiB free internal heap, and a 16 KiB largest internal block.
- No GitHub push, tag, release, factory-bundle promotion, or public canary artifact.
- Preserve the untracked `.camera-before-zoom.jpg`.

---

### Task 1: Add Red Memory-Admission Coverage

**Files:**
- Modify: `esp32/test/test_badge_con_radio_runtime_policy.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: existing `badge_con_radio_runtime_policy.h`.
- Produces: a required `badge_con_radio_runtime_memory_gate()` interface and exact boundary behavior.

- [x] **Step 1: Write the failing boundary test**

Add a table-driven test whose hand-derived cases require:

```c
typedef enum {
    BADGE_CON_RADIO_MEMORY_OK = 0,
    BADGE_CON_RADIO_MEMORY_PSRAM,
    BADGE_CON_RADIO_MEMORY_INTERNAL,
} badge_con_radio_memory_gate_t;

badge_con_radio_memory_gate_t badge_con_radio_runtime_memory_gate(
    uint32_t internal_free,
    uint32_t internal_largest,
    bool psram_initialized,
    uint32_t psram_total,
    uint32_t psram_free);
```

Literal expectations:

```c
{24576U, 16384U, true, 8388608U, 5242880U,
 BADGE_CON_RADIO_MEMORY_OK},
{24575U, 16384U, true, 8388608U, 5242880U,
 BADGE_CON_RADIO_MEMORY_INTERNAL},
{24576U, 16383U, true, 8388608U, 5242880U,
 BADGE_CON_RADIO_MEMORY_INTERNAL},
{24576U, 16384U, false, 8388608U, 5242880U,
 BADGE_CON_RADIO_MEMORY_PSRAM},
{24576U, 16384U, true, 8388607U, 5242880U,
 BADGE_CON_RADIO_MEMORY_PSRAM},
{24576U, 16384U, true, 8388608U, 5242879U,
 BADGE_CON_RADIO_MEMORY_PSRAM},
```

Register `test_badge_con_radio_runtime_memory_gate_is_inclusive_and_fail_closed`
in `test_runner.c`.

- [x] **Step 2: Run the focused native test and observe RED**

Run:

```sh
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_radio_runtime_policy
```

Expected: compilation fails because the enum/function do not exist.

- [x] **Step 3: Commit only after the implementation task is green**

The red test and implementation ship in one local commit; do not commit an
unbuildable intermediate tree.

### Task 2: Enforce PSRAM-Only Display and Sticky Game-Radio Failure

**Files:**
- Modify: `esp32/shared/badge_con_radio_runtime_policy.h`
- Modify: `esp32/shared/badge_con_radio_runtime_policy.c`
- Modify: `esp32/uplink/main/game/badge_con_vhci.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`

**Interfaces:**
- Consumes: `psram_available()`, `psram_total_size()`, `psram_free_size()`, and `psram_alloc_strict()`.
- Produces: `badge_con_radio_runtime_memory_gate()` and terminal VHCI failures named `psram_gate` or `internal_heap_gate`.

- [x] **Step 1: Implement the pure admission policy**

Add these exact inclusive floors to the policy header:

```c
#define BADGE_CON_RADIO_INTERNAL_HEAP_MIN 24576U
#define BADGE_CON_RADIO_INTERNAL_BLOCK_MIN 16384U
#define BADGE_CON_RADIO_PSRAM_TOTAL_MIN 8388608U
#define BADGE_CON_RADIO_PSRAM_FREE_MIN 5242880U
```

Implement `badge_con_radio_runtime_memory_gate()` so PSRAM failure is returned
first, internal-memory failure second, and only an exact pass returns
`BADGE_CON_RADIO_MEMORY_OK`.

- [x] **Step 2: Use the policy at the only VHCI initialization boundary**

In `badge_con_vhci_init()`, sample:

```c
uint32_t internal_free = (uint32_t)heap_caps_get_free_size(
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
uint32_t internal_largest = (uint32_t)heap_caps_get_largest_free_block(
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
badge_con_radio_memory_gate_t memory_gate =
    badge_con_radio_runtime_memory_gate(
        internal_free,
        internal_largest,
        psram_available(),
        (uint32_t)psram_total_size(),
        (uint32_t)psram_free_size());
```

Map `BADGE_CON_RADIO_MEMORY_PSRAM` to
`fail_initialization("psram_gate")`, map
`BADGE_CON_RADIO_MEMORY_INTERNAL` to
`fail_initialization("internal_heap_gate")`, and never call
`esp_bt_controller_init()` after either failure. Preserve the existing
single-attempt latch and updater quiescence behavior.

- [x] **Step 3: Remove the dangerous framebuffer fallback**

Make the strict behavior canary-only:

```c
#if defined(FOF_DC34_GAME_CANARY)
s_fb = psram_alloc_strict(LCD_FB_BYTES);
if (!s_fb) {
    ESP_LOGE(TAG,
             "PSRAM framebuffer allocation failed (%d bytes); "
             "continuing headless for USB/UART recovery",
             LCD_FB_BYTES);
    return;
}
#else
s_fb = heap_caps_malloc(
    LCD_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!s_fb) {
    s_fb = heap_caps_malloc(LCD_FB_BYTES, MALLOC_CAP_8BIT);
}
if (!s_fb) {
    ESP_LOGE(TAG, "Framebuffer allocation failed (%d bytes)", LCD_FB_BYTES);
    return;
}
#endif
```

Do not change the later internal/DMA allocation of `s_tx_chunk`.

- [x] **Step 4: Run focused and full native tests**

Run:

```sh
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test -f test_badge_con_radio_runtime_policy
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: both commands exit 0 with no ASan failure.

- [x] **Step 5: Review the mutation boundary**

Confirm the focused test fails for each realistic mutation: `<` changed to
`<=`, the PSRAM boolean ignored, total/free gates omitted, or internal
free/largest arguments swapped.

- [x] **Step 6: Commit the hardening locally**

```sh
git add \
  docs/superpowers/specs/2026-07-25-con-crud-ble-game-design.md \
  docs/superpowers/plans/2026-07-27-con-crud-psram-fail-safe.md \
  esp32/shared/badge_con_radio_runtime_policy.h \
  esp32/shared/badge_con_radio_runtime_policy.c \
  esp32/uplink/main/game/badge_con_vhci.c \
  esp32/uplink/main/hw/display_st7735.c \
  esp32/test/test_badge_con_radio_runtime_policy.c \
  esp32/test/test_runner.c
git commit -m "v0.64.88-canary: fail game closed on memory faults"
```

### Task 3: Build and Freeze the `.88` Canary

**Files:**
- Modify: `esp32/shared/version.h`
- Verify only: generated scanner/uplink production and canary build directories.

**Interfaces:**
- Consumes: the green memory hardening.
- Produces: one immutable scanner image and one immutable uplink image, both reporting `0.64.88-badge-defcon34`.

- [x] **Step 1: Bump only the private canary**

Change:

```c
#define FOF_VERSION_BADGE_CANARY "0.64.87-badge-defcon34"
```

to:

```c
#define FOF_VERSION_BADGE_CANARY "0.64.88-badge-defcon34"
```

- [x] **Step 2: Run updater/host regression gates**

Run:

```sh
python3 -m pytest backend/tests/test_firmware_build_version.py -q
python3 -m unittest \
  scripts.test_fof_badge_flash \
  scripts.test_fof_badge_flash_phase_a_json \
  scripts.test_fof_badge_flash_phase_a_serial \
  scripts.test_verify_badge_usb_hardening
python3 -m unittest discover \
  -s tools/badge_flasher/tests \
  -p 'test_*.py'
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
cd ..
```

Every command must exit 0 before hardware mutation.

- [x] **Step 3: Build fresh production comparisons and canaries**

Run:

```sh
cd esp32/scanner
/Users/billh/.platformio/penv/bin/pio run -t clean -e scanner-s3-combo-fof_badge
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge
/Users/billh/.platformio/penv/bin/pio run -t clean -e scanner-s3-combo-fof_badge-con-crud-canary
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge-con-crud-canary
cd ../uplink
/Users/billh/.platformio/penv/bin/pio run -t clean -e uplink-s3-fof_badge
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
/Users/billh/.platformio/penv/bin/pio run -t clean -e uplink-s3-fof_badge-con-crud-canary
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge-con-crud-canary
```

- [x] **Step 4: Run both strict isolation verifiers and hash artifacts**

Use the exact verifier commands from
`docs/badge/con-crud-canary-acceptance.md`, then run:

```sh
shasum -a 256 \
  esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary/firmware.bin \
  esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary/firmware.bin
```

Record the two hashes and do not rebuild between badges.

Frozen `.88` canary SHA-256 values:

- scanner: `a7b64db5e02e5bf3f339c80d29ed8d27ec91f3339c0d6d257315010a5fe53317`
- uplink: `a8cb57f7119a9b9224248000997ea22cfc2d21c0877810b6dc78522c097e6e19`

- [x] **Step 5: Commit the private version bump locally**

```sh
git add esp32/shared/version.h
git commit -m "v0.64.88-canary: prepare two-badge game trial"
```

### Task 4: Flash and Exercise Two Complete Badges

**Files:**
- Modify after evidence: `docs/badge/con-crud-canary-acceptance.md`
- No firmware source edits during the physical run.

**Interfaces:**
- Consumes: frozen `.88` artifacts and immutable hardware IDs.
- Produces: exact `.88` uplink/scanner identity proof, A infected, B normal then immune, and physical infection/cure evidence.

- [ ] **Step 1: Preserve the read-only baseline**

Badge A is uplink `E0:72:A1:F8:4C:68`, initially `.80`, with BLE leaf
`E0:72:A1:F8:A0:04` and Wi-Fi leaf `E0:72:A1:F9:4B:AC`.

Badge B is uplink `E0:72:A1:F8:86:74`, initially `.87`, with BLE leaf
`E0:72:A1:F8:85:34` and Wi-Fi leaf `E0:72:A1:F9:42:54`.

Re-read `FOF_STATUS` immediately before each mutation and reject any changed
hardware graph.

- [ ] **Step 2: Flash Badge A from its uplink only**

Disconnect Badge B. Resolve A's current port by full USB serial, then run:

```sh
python3 scripts/fof_badge_flash.py \
  --platform badge-trio-xiao-s3-con-crud-canary \
  --transport usb \
  --only all \
  --port /dev/cu.usbmodem1101 \
  --skip-build
```

Immediately before this command, reject the mutation unless the repo's USB
descriptor census proves `/dev/cu.usbmodem1101` is exactly
`E0:72:A1:F8:4C:68`. Require `.88` on all three boards, rollback clear, both
scanner roles/radios healthy, and an idle firmware campaign. If macOS
renumbers the port, update this plan with the newly proven concrete path
before running the flasher.

- [ ] **Step 3: Flash Badge B from its uplink only**

Disconnect Badge A. Reject the mutation unless the USB descriptor census
proves `/dev/cu.usbmodem1401` is exactly `E0:72:A1:F8:86:74`, then run:

```sh
python3 scripts/fof_badge_flash.py \
  --platform badge-trio-xiao-s3-con-crud-canary \
  --transport usb \
  --only all \
  --port /dev/cu.usbmodem1401 \
  --skip-build
```

Require the same post-flash gates. If macOS renumbers the port, update this
plan with the newly proven concrete path before running the flasher.

- [ ] **Step 4: Seed exact roles with the existing verified transaction**

With only the intended badge connected, use
`tools.badge_flasher.verify.provision_game_seed()` followed by
`wait_for_runtime()` and the immutable three-board `TopologyAssignment`.
Seed A `infected` and B `normal`. Require exact seed acknowledgment, reboot
receipt, a fresh descriptor-bound session with exact successor reboot
generation (the native USB path may persist), `.88` identity, both healthy
scanners, `game_active:false`, and shield `0`.

- [ ] **Step 5: Activate and observe infection**

Reconnect both badges 6–12 inches apart. Turn on the exact SSID
`GameChangersAI-67`, wait for both Easter presentations, dismiss each with one
button press, and turn the SSID off. Require both `game_active:true`, no
self-encounter, A's infected purple/green treatment, and B changing from normal
to infected only after three distinct authenticated packets at `-60 dBm` or
stronger within six seconds.

- [ ] **Step 6: Reseed B immune and observe cure**

Disconnect A, seed B `immune` through the same verified transaction, activate
B again, then reconnect A. Require B's pink immune treatment and shield 100%;
after qualified close-range packets, require A's shield to charge and cure at
100% without changing the normal four privacy lanes.

- [ ] **Step 7: Verify normal work continues**

Capture fresh `FOF_STATUS` from both badges and require:

- USB control alive and responsive;
- BLE scanner `ble_primary`, synchronized and scanning;
- Wi-Fi scanner `wifi_primary`, initialized and active;
- both scanner versions exactly `.88`;
- no rollback pending, crash loop, safe mode, UART overflow growth, or
  monotonic internal/PSRAM decline;
- PSRAM total 8,388,608 bytes, free at least 5 MiB, and internal
  free/largest/minimum-ever at least 24/16/12 KiB.

- [ ] **Step 8: Record evidence and commit locally**

Update `docs/badge/con-crud-canary-acceptance.md` with immutable hardware IDs,
artifact hashes, updater results, memory snapshots, game observations, and
remaining unrun gates. Commit locally; do not push or promote.
