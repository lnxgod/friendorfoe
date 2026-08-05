# Badge USB Flashing Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the assembled badge's existing USB-C connection dependable for Android control and for a laptop to update the uplink, stage one scanner image, and automatically update both scanners without disconnecting the battery.

**Architecture:** Install the supported ESP-IDF USB Serial/JTAG driver once and give one lifetime task exclusive ownership of input through its VFS. A pure stream state machine separates newline commands from exact-length scanner and uplink binary uploads. A dedicated uplink OTA adapter writes only the inactive application slot, while the existing scanner store remains isolated. The 10-second OK+Menu chord offers confirmed ROM recovery when the public USB SOF API sees an active host and otherwise performs a normal reboot. The laptop uses application OTA first, MAC-bound ROM fallback second, and verifies all three boards. Android retains control/status/detection features but no firmware mutation actions.

**Tech Stack:** ESP-IDF 5.5.3 C, FreeRTOS, USB Serial/JTAG, ESP OTA, PlatformIO native Unity tests, Python 3.11 `unittest`/pyserial/esptool, Kotlin/Jetpack Compose/JUnit, pytest source-contract tests.

## Global Constraints

- No GPIO, PCB trace, UART pin, partition offset, flash-size, or battery-wiring change.
- Android may read detections/status and control themes, palettes, display policy, navigation, investigations, badge mode, and safe runtime settings; it may not upload or relay firmware or enter ROM download mode.
- Firmware mutation uses laptop USB only. Do not add HTTP, Wi-Fi AP, LAN, BLE, or Android firmware paths.
- Normal uplink updates use application USB OTA. A silent application requires the operator to hold OK+Menu for 10 seconds, release both, and press OK at the flash confirmation; never depend on DTR/RTS or ask for a battery disconnect.
- Scanner firmware is uploaded once to `fw_scanner_s3`; the uplink's existing automatic coordinator updates both scanners over the fixed UART topology.
- Preserve scanner firmware format, BLE-primary/Wi-Fi-primary roles, all four display lanes, themes, privacy detector policy, and radio behavior.
- A task heartbeat does not prove USB communication. Post-update validation requires a completed command response.
- Never overwrite the rollback slot while the running application is `ESP_OTA_IMG_PENDING_VERIFY`.
- No factory-bundle replacement, tag, release, or GitHub push until every automated gate and all six battery-connected physical gates pass. A separately user-authorized local provisional version identity is canary-only and does not change readiness, public manifests, or gate status.
- Preserve the unrelated untracked `.camera-before-zoom.jpg` file.

---

### Task 0: Prove the One-Time Bootstrap Access on the Assembled Badge

**Files:**
- Create: `docs/badge/xiao-uplink-bootstrap.md`
- No firmware files modified.

**Interfaces:**
- Consumes: the uplink's existing onboard XIAO ESP32-S3 BOOT and RESET switch contacts.
- Produces: a photographed contact map, exact no-battery-disconnect procedure, ROM base MAC, and a read-only esptool transcript.

- [ ] **Step 1: Identify only the labeled onboard XIAO controls**

Use the official XIAO ESP32-S3 board orientation to locate its onboard BOOT and
RESET switches or their two switch contacts on the assembled uplink. Confirm
continuity across each switch before touching the powered board. Do not infer
unknown badge pads and do not add or reroute a PCB pin.

- [ ] **Step 2: Enter ROM without disconnecting the battery**

With USB attached and the battery still connected, hold the onboard XIAO BOOT
switch/contact, momentarily press/bridge the onboard XIAO RESET switch/contact,
release RESET, wait for ROM USB enumeration, then release BOOT. This is the
manufacturer's hardware strap sequence and does not depend on the silent
application.

- [ ] **Step 3: Prove identity read-only**

Run esptool with `--before no_reset --after no_reset chip_id`, record the exact
base MAC and ROM port, and perform no erase/write. Photograph the accessible
contacts and write the successful sequence into the bootstrap document.

- [ ] **Step 4: Enforce the stop rule**

If both onboard switch contacts cannot be positively identified and reached,
do not short another pad and do not write flash. Firmware implementation may
continue, but Task 10 remains physically blocked until existing-board ROM
access is proven.

- [ ] **Step 5: Commit**

```bash
git add docs/badge/xiao-uplink-bootstrap.md
git commit -m "docs: prove badge uplink bootstrap access"
```

---

### Task 1: Specify Pure USB Stream, Health, and Recovery Policies

**Files:**
- Create: `esp32/shared/badge_usb_stream.h`
- Create: `esp32/shared/badge_usb_stream.c`
- Create: `esp32/shared/badge_usb_health_policy.h`
- Create: `esp32/shared/badge_usb_health_policy.c`
- Create: `esp32/shared/badge_usb_recovery_policy.h`
- Create: `esp32/shared/badge_usb_recovery_policy.c`
- Create: `esp32/test/test_badge_usb_stream.c`
- Create: `esp32/test/test_badge_usb_health_policy.c`
- Create: `esp32/test/test_badge_usb_recovery_policy.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

```c
typedef enum {
    BADGE_USB_BINARY_NONE = 0,
    BADGE_USB_BINARY_SCANNER,
    BADGE_USB_BINARY_UPLINK,
} badge_usb_binary_target_t;

typedef enum {
    BADGE_USB_EVENT_NONE = 0,
    BADGE_USB_EVENT_LINE,
    BADGE_USB_EVENT_BINARY_CHUNK,
    BADGE_USB_EVENT_BINARY_COMPLETE,
    BADGE_USB_EVENT_ERROR,
} badge_usb_stream_event_t;

typedef struct {
    badge_usb_binary_target_t target;
    char *line;
    size_t line_capacity;
    uint32_t exact_size;
    uint32_t received;
    uint32_t last_activity_ms;
    size_t line_length;
    bool discarding_oversize_line;
} badge_usb_stream_t;

typedef struct {
    badge_usb_stream_event_t event;
    badge_usb_binary_target_t target;
    const uint8_t *bytes;
    size_t bytes_len;
    const char *line;
    size_t input_consumed;
    const char *error;
} badge_usb_stream_result_t;

void badge_usb_stream_init(badge_usb_stream_t *state,
                           char *line, size_t line_capacity);
bool badge_usb_stream_begin_binary(badge_usb_stream_t *state,
                                   badge_usb_binary_target_t target,
                                   uint32_t exact_size, uint32_t now_ms);
badge_usb_stream_event_t badge_usb_stream_feed(
    badge_usb_stream_t *state, const uint8_t *src, size_t src_len,
    uint32_t now_ms, badge_usb_stream_result_t *result);
badge_usb_stream_event_t badge_usb_stream_poll_timeout(
    badge_usb_stream_t *state, uint32_t now_ms, uint32_t idle_timeout_ms);
badge_usb_stream_event_t badge_usb_stream_abort(
    badge_usb_stream_t *state, const char *reason,
    badge_usb_stream_result_t *result);
```

```c
typedef enum {
    BADGE_USB_HEALTH_OK = 0,
    BADGE_USB_HEALTH_WAITING,
    BADGE_USB_HEALTH_RESTART_SAFE_USB,
} badge_usb_health_action_t;

typedef struct {
    bool safe_usb;
    bool one_boot_recovery_consumed;
    bool task_started;
    bool host_connected;
    bool transaction_active;
    int64_t now_ms;
    int64_t task_heartbeat_ms;
    int64_t last_rx_ms;
    int64_t last_command_ms;
    int64_t last_response_ms;
    int64_t last_transaction_progress_ms;
    int64_t boot_grace_ms;
    int64_t stale_after_ms;
} badge_usb_health_inputs_t;

badge_usb_health_action_t badge_usb_health_decide(
    const badge_usb_health_inputs_t *inputs);

typedef enum {
    BADGE_USB_RESET_APP = 0,
    BADGE_USB_RESET_ROM,
} badge_usb_reset_target_t;

badge_usb_reset_target_t badge_usb_recovery_target(bool host_active,
                                                   bool flash_confirmed);
```

- [ ] **Step 1: Add failing stream tests**

Cover fragmented LF/CRLF commands, multiple commands in one read, an exact
binary boundary, command bytes immediately following the payload in the same
read, a scanner target that cannot complete as uplink, an uplink target that
cannot complete as scanner, line overflow recovery, a five-second binary idle
timeout, explicit abort, and `uint32_t` wrap.

- [ ] **Step 2: Add failing health and recovery tests**

Require task heartbeat without host traffic to remain healthy-idle but not
count as completed I/O. Require restart for a stale task and for received
command bytes without a terminal response. Suppress restart during fresh OTA
or scanner-relay progress, but require restart when that progress stalls.
Prove the one-boot safe mode cannot reboot-loop. Prove SOF host activity plus
an explicit OK confirmation selects ROM; host activity without confirmation,
Android-host timeout, and charger/no-SOF select an application reboot.

- [ ] **Step 3: Register sources/tests and verify RED**

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: compilation fails on the new headers/functions, proving the tests
exercise absent behavior rather than the old runtime.

- [ ] **Step 4: Implement fixed-buffer, allocation-free policies**

The stream must retain unconsumed bytes after a binary boundary, clear binary
ownership after every terminal event, and report consumed byte counts so the
runtime never loses a coalesced command. Health decisions must use separate
task-heartbeat, RX, command-completion, and transaction-progress timestamps.

- [ ] **Step 5: Run native tests and verify GREEN**

Run the Task 1 native command again.

Expected: all current tests plus the new policy tests pass with zero failures.

- [ ] **Step 6: Commit**

```bash
git add esp32/shared/badge_usb_* esp32/test/test_badge_usb_* esp32/test/test_runner.c esp32/platformio.ini
git commit -m "badge: specify hardened USB state machines"
```

---

### Task 2: Replace Mixed USB Readers with One Lifetime Transport

**Files:**
- Create: `esp32/uplink/main/core/badge_usb_transport.h`
- Create: `esp32/uplink/main/core/badge_usb_transport.c`
- Modify: `esp32/uplink/main/core/serial_config.h`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/uplink/main/CMakeLists.txt`
- Modify: `esp32/uplink/platformio.ini`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`
- Create: `docs/badge/protocol/badge_usb_health_v1.fixture.json`
- Modify: `backend/tests/test_firmware_build_version.py`

**Interfaces:**

```c
typedef enum {
    BADGE_USB_FRAME_REQUIRED = 0,
    BADGE_USB_FRAME_PROGRESS,
    BADGE_USB_FRAME_OPTIONAL,
} badge_usb_frame_priority_t;

typedef struct {
    bool task_started;
    bool host_connected;
    badge_usb_binary_target_t parser_target;
    uint64_t rx_bytes;
    uint32_t valid_commands;
    uint32_t responses_completed;
    uint32_t required_response_failures;
    uint32_t malformed_lines;
    uint32_t dropped_progress_frames;
    uint32_t dropped_optional_frames;
    uint32_t upload_received;
    uint32_t upload_size;
    int64_t task_heartbeat_ms;
    int64_t last_rx_ms;
    int64_t last_command_ms;
    int64_t last_response_ms;
    int64_t last_upload_progress_ms;
} badge_usb_health_t;

bool badge_usb_transport_start(uint32_t boot_window_ms);
bool badge_usb_transport_wait_boot_window(TickType_t timeout);
void badge_usb_transport_set_dispatch_ready(void);
bool badge_usb_transport_begin_binary(badge_usb_binary_target_t target,
                                      uint32_t exact_size);
bool badge_usb_transport_emit(const void *data, size_t len,
                              badge_usb_frame_priority_t priority,
                              TickType_t timeout);
void badge_usb_transport_snapshot(badge_usb_health_t *out);
bool badge_usb_transport_host_active(uint32_t sample_window_ms);
```

Freeze this exact status object and types in
`docs/badge/protocol/badge_usb_health_v1.fixture.json`:

```json
{
  "usb_health": {
    "schema": 1,
    "task_started": true,
    "host_connected": true,
    "parser_state": "command",
    "rx_bytes": 128,
    "valid_commands": 3,
    "responses_completed": 3,
    "required_response_failures": 0,
    "malformed_lines": 0,
    "dropped_progress_frames": 0,
    "dropped_optional_frames": 0,
    "upload_received": 0,
    "upload_size": 0,
    "task_heartbeat_age_s": 0,
    "last_rx_age_s": 0,
    "last_command_age_s": 0,
    "last_response_age_s": 0,
    "last_upload_progress_age_s": null
  }
}
```

The only parser strings are `command`, `scanner_upload`, and
`uplink_upload`. Counters are non-negative JSON integers; an age is a
non-negative integer or `null` when no event has occurred.

- [ ] **Step 1: Write source-contract tests that reject the current defect**

Require one `badge_usb_transport_start(3000)` call before the rest of badge
startup, one lifetime reader task, driver installation, and VFS driver
selection. Reject `usb_serial_jtag_ll_read_rxfifo` anywhere in the uplink
application. Reject a second `fgetc(stdin)`/`read(STDIN_FILENO, ...)` owner.
Require separate scanner/uplink parser targets and all USB health fields in
`FOF_STATUS`. Load the shared fixture in the backend contract so key names
and types cannot drift.
Require the badge environment's generated application offset to be
`0x20000`, matching decoded `ota_0`, rather than PlatformIO's board default
`0x10000`.

- [ ] **Step 2: Run the focused contract and verify RED**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_firmware_transport_contract.py \
  backend/tests/test_firmware_build_version.py -v
```

Expected: failures identify the missing driver, mixed boot/runtime readers, and
missing transport status.

- [ ] **Step 3: Install the supported ESP-IDF driver exactly once**

Before any USB read, initialize:

```c
usb_serial_jtag_driver_config_t config = {
    .rx_buffer_size = 8192,
    .tx_buffer_size = 2048,
};
esp_err_t err = usb_serial_jtag_driver_install(&config);
if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return false;
}
usb_serial_jtag_vfs_use_driver();
usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
```

Add `esp_driver_usb_serial_jtag` to `FOF_UPLINK_REQUIRES`. The transport task is
the only input owner and reads nonblocking bytes through the configured VFS.
Remove the raw LL readers. `app_main()` starts the task and waits on its boot
window event; `serial_config_listen()` no longer reads stdin.

Handle already-installed, allocation, and task-creation outcomes explicitly;
do not panic with `ESP_ERROR_CHECK`. A real initialization failure displays
USB recovery state, leaves the button/SOF ROM path alive, and requests at most
one RTC-token-controlled restart.

The driver may start early, but command dispatch stays gated until NVS, badge
runtime state, the firmware-operation coordinator, display state, and serial
configuration dependencies are initialized. Complete commands received before
`badge_usb_transport_set_dispatch_ready()` receive a deterministic
`FOF_ERROR:booting` and never call a subsystem handler. Add a test that feeds
PING, STATUS, scanner begin, and uplink begin during this gate, then proves all
work after readiness.

Set `board_upload.offset_address = 0x20000` for
`uplink-s3-fof_badge`. After a clean rebuild, require `flash_args`,
`flash_app_args`, `flash_project_args`, and `flasher_args.json` to agree
with the decoded partition table.

- [ ] **Step 4: Route every machine frame through the transport**

Required responses use supported `usb_serial_jtag_write_bytes()` plus bounded
`usb_serial_jtag_wait_tx_done()`, and increment `responses_completed` only
after successful drain. Progress frames use a short bounded wait. `FOF_DET`,
`FOF_INV`, and optional logs use zero/short wait and increment drop counters.
Replace direct `printf()` calls in scanner relay progress and upload terminals.
Build the large `FOF_STATUS` frame from subsystem snapshots into a bounded
PSRAM buffer, then emit it as one required transaction.

Chunk a required frame into writes no larger than the configured 2,048-byte TX
ring while holding one transaction lock. Verify every write returns its full
chunk length, then call `usb_serial_jtag_wait_tx_done()`. Add a transport
regression using a frame larger than 2,048 bytes and cap every status frame
below Android's 64 KiB line limit.

If the PSRAM status buffer cannot be allocated, emit a bounded internal-memory
minimal status frame containing schema, base MAC, target/project/hardware,
version, running partition, rollback state, recovery mode, and USB health.
Status required for repair/rebinding may not disappear with PSRAM failure.

Register a re-entrant `esp_log_set_vprintf()` wrapper guarded by the same
recursive transport transaction lock so ESP logs cannot split a machine frame.
The wrapper is optional priority and may drop under backpressure; it must never
hold the command task indefinitely.

A required response that cannot drain is never treated as success: increment
`required_response_failures` and leave the command incomplete. Before OTA boot
selection, abort any active upload safely and let the health policy request the
single safe-USB restart. After boot selection, follow Task 5's committed-state
rule: restart into the selected image and let the host reconcile by identity.
This makes the failure observable without blocking forever or silently
pretending an acknowledgement completed.

- [ ] **Step 5: Add immutable uplink identity and transport health to status**

Read the ESP32 base MAC, format it as uppercase colon-separated
`hardware_id`, and include exact `target`, `project`, `hardware_type`,
`version`, running partition, pending-verify state, parser target, counters,
upload progress, and nullable ages. Snapshot shared 64-bit timestamps under a
lock; do not tear values across tasks.
Preserve the existing `firmware_name` alias for `target` and
`app_project` alias for `project` until all released hosts have migrated;
add regression assertions for both names.

- [ ] **Step 6: Run focused contracts and build uplink**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest \
  backend/tests/test_badge_firmware_transport_contract.py \
  backend/tests/test_firmware_build_version.py -v
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: source contracts pass and the badge uplink build exits zero.

- [ ] **Step 7: Commit**

```bash
git add esp32/uplink/main/core/badge_usb_transport.* esp32/uplink/main/core/serial_config.* esp32/uplink/main/network/fw_store.c esp32/uplink/main/main.c esp32/uplink/main/CMakeLists.txt esp32/uplink/platformio.ini backend/tests/test_badge_firmware_transport_contract.py backend/tests/test_firmware_build_version.py docs/badge/protocol/badge_usb_health_v1.fixture.json
git commit -m "badge: unify USB transport ownership"
```

---

### Task 3: Make the Ten-Second Chord Host-Aware and Recovery One-Boot

**Files:**
- Create: `esp32/uplink/main/core/badge_usb_recovery.h`
- Create: `esp32/uplink/main/core/badge_usb_recovery.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/core/badge_runtime.h`
- Modify: `esp32/uplink/main/core/badge_runtime.c`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/shared/badge_runtime_policy.h`
- Modify: `esp32/shared/badge_runtime_policy.c`
- Modify: `esp32/test/test_badge_runtime_policy.c`
- Modify: `backend/tests/test_badge_quiet_mode_contract.py`

**Interfaces:**

```c
_Noreturn void badge_usb_recovery_restart(
    badge_usb_reset_target_t target, const char *reason);
```

- [ ] **Step 1: Extend failing runtime and source-contract tests**

Keep every existing exact-10,000-ms chord timing test unchanged. Add runtime
tests proving a one-boot USB recovery token is consumed once, safe USB remains
repairable, task heartbeat alone cannot validate an OTA image, and a completed
post-boot `FOF_PING` or `FOF_STATUS` response can satisfy the USB half of the
validation gate. Require the button task to call host-aware recovery and reject
all physical quiet/off toggles. Add an integration contract proving the
connectivity-watchdog loop calls `badge_usb_health_decide()` and executes
`BADGE_USB_HEALTH_RESTART_SAFE_USB` exactly once.

- [ ] **Step 2: Run focused tests and verify RED**

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
cd ..
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_quiet_mode_contract.py -v
```

Expected: failures identify the unconditional `esp_restart()`, persistent NVS
safe-mode behavior, and heartbeat-only health gate.

- [ ] **Step 3: Detect a real data host through the public SOF API**

Implement `badge_usb_transport_host_active(25)` by sampling
`usb_serial_jtag_is_connected()` for up to 25 ms. This ESP-IDF API uses USB SOF
activity, so a power-only charger returns false. Do not inspect DTR/RTS and do
not touch USB interrupt registers directly.

- [ ] **Step 4: Unify ROM and application restart paths**

At the existing chord event, consume both button releases and cancel pending
single-button gestures. With no host, arm `button_reboot` and restart normally.
If a host is active, show `USB FLASH? OK=YES MENU=RESET` for five seconds after
both buttons are released. A fresh OK press arms `button_usb_rom`, drains
required output for a bounded interval, sets
`RTC_CNTL_FORCE_DOWNLOAD_BOOT`, and restarts. Menu or timeout arms
`button_reboot` and restarts normally. This second physical confirmation
prevents an Android phone, which also produces SOF, from accidentally stranding
the badge in ROM. Route `FOF_BOOTLOADER` through the same helper for the laptop
protocol only; Android will remove that action in Task 7.

- [ ] **Step 5: Replace persistent forced-safe NVS with a consumed RTC token**

Set a `RTC_NOINIT_ATTR` magic immediately before the one automatic USB-recovery
restart. Consume it only when reset cause is the expected software reset and
clear it on power-on, brownout, watchdog, panic, or any mismatched reset cause.
Start display, USB transport, and button handling before network/scanner work.
In safe USB,
keep `FOF_PING`, `FOF_STATUS`, uplink OTA, application reboot, and ROM recovery
available; never suppress the repair path. A safe-USB pending image may clear
rollback only after display health and a completed USB response. Normal boot
also requires the scanner UART worker to be initialized and heartbeating, but
never requires scanner identity, version, connection, or radio health; blank or
broken scanners must remain repairable after the uplink validates.

Replace the current watchdog's `badge_runtime_usb_control_recovery_due()`
branch with a periodic snapshot passed to `badge_usb_health_decide()`.
Include active-upload and scanner-relay progress in that snapshot, then call
`badge_usb_recovery_restart(BADGE_USB_RESET_APP, "usb_safe_once")` only for
`BADGE_USB_HEALTH_RESTART_SAFE_USB`. The consumed RTC token prevents a second
automatic restart. Persist the expected reboot reason separately from the
one-boot token and expose `last_expected_reboot_reason` in `FOF_STATUS`, so a
post-reconnect check can distinguish `button_reboot`, `button_usb_rom`, and
`usb_uplink_ota`.

- [ ] **Step 6: Run native/contracts/build and verify GREEN**

Run the Task 3 native and pytest commands, then:

```bash
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: all tests pass and the uplink build succeeds.

- [ ] **Step 7: Commit**

```bash
git add esp32/uplink/main/core/badge_usb_recovery.* esp32/uplink/main/hw/display_st7735.c esp32/uplink/main/core/serial_config.c esp32/uplink/main/core/badge_runtime.* esp32/uplink/main/main.c esp32/shared/badge_runtime_policy.* esp32/test/test_badge_runtime_policy.c backend/tests/test_badge_quiet_mode_contract.py
git commit -m "badge: add host-aware USB recovery"
```

---

### Task 4: Specify Uplink OTA Validation and State Transitions

**Files:**
- Create: `esp32/shared/uplink_ota_policy.h`
- Create: `esp32/shared/uplink_ota_policy.c`
- Create: `esp32/test/test_uplink_ota_policy.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**

```c
typedef enum {
    UPLINK_OTA_IDLE = 0,
    UPLINK_OTA_RECEIVING,
    UPLINK_OTA_VERIFYING,
    UPLINK_OTA_COMMITTED,
    UPLINK_OTA_ERROR,
} uplink_ota_state_t;

typedef struct {
    char target[33];
    char project[33];
    char hardware[33];
    char version[33];
    char sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
    uint32_t size;
    uint32_t crc32;
    bool recovery_rewrite_same_version;
} uplink_ota_manifest_t;

bool uplink_ota_policy_manifest_allowed(
    const uplink_ota_manifest_t *manifest,
    const char *running_version, uint32_t partition_size,
    bool running_pending_verify, const char **error);

typedef struct {
    uplink_ota_state_t state;
    uplink_ota_manifest_t manifest;
    uint32_t durable_written;
    uint32_t next_credit_at;
    bool credit_outstanding;
    const char *last_error;
} uplink_ota_policy_session_t;

void uplink_ota_policy_init(uplink_ota_policy_session_t *session);
bool uplink_ota_policy_begin(uplink_ota_policy_session_t *session,
                             const uplink_ota_manifest_t *manifest,
                             const char *running_version,
                             uint32_t partition_size,
                             bool running_pending_verify,
                             const char **error);
bool uplink_ota_policy_note_durable_write(
    uplink_ota_policy_session_t *session, uint32_t length,
    uint32_t transport_received, const char **error);
bool uplink_ota_policy_grant_credit(
    uplink_ota_policy_session_t *session, uint32_t *credit_bytes,
    uint32_t *durable_received, const char **error);
bool uplink_ota_policy_commit_allowed(
    uplink_ota_policy_session_t *session, uint32_t transport_received,
    uint32_t computed_crc32, const char *computed_sha256,
    const fof_firmware_image_identity_t *embedded_identity,
    bool target_marker_seen, bool hardware_marker_seen,
    const char **error);
void uplink_ota_policy_fail(uplink_ota_policy_session_t *session,
                            const char *error);
```

- [ ] **Step 1: Write failing manifest and transition tests**

Accept only `target=uplink-s3-fof_badge`,
`project=fof_badge_uplink`, and
`hardware=seeed_xiao_esp32s3`. Cover strictly newer acceptance; downgrade,
unordered, and invalid version rejection; equal version rejection without the
explicit recovery flag and acceptance with it; pending-verify rejection; size
below 1 KiB; partition overflow; zero CRC; malformed SHA; exact byte count;
CRC/SHA mismatch; embedded
project/version mismatch; target/hardware marker mismatch; a 4096-byte credit
grant; refusal to grant new credit before the prior window is consumed; and an
exact durable received count at each grant.

- [ ] **Step 2: Run native tests and verify RED**

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: compile failure because `uplink_ota_policy` is absent.

- [ ] **Step 3: Implement the pure policy with existing helpers**

Reuse `fof_firmware_version_compare()`,
`fof_firmware_sha256_hex_is_valid()`, and the fixed release-target constants.
The ESP app descriptor directly authenticates the embedded project/version
only. Map that exact project to the compile-time target/hardware contract and
treat streamed target/hardware string markers as defense in depth, not as a
cryptographic identity claim.

Keep byte framing in one layer: `badge_usb_stream` alone owns declared receive
length, oversend boundaries, idle timeout, leftover bytes, binary abort, and
return to command mode. `uplink_ota_policy` owns manifest/version acceptance,
durable-written count, credit grants, and integrity/identity results. Before
commit, assert `declared_size == transport_received == ota_written`. The
transport dispatch layer alone emits the terminal frame and releases binary
ownership; the OTA adapter only returns a structured result.

- [ ] **Step 4: Run native tests and verify GREEN**

Run the Task 4 native command again.

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add esp32/shared/uplink_ota_policy.* esp32/test/test_uplink_ota_policy.c esp32/test/test_runner.c esp32/platformio.ini
git commit -m "badge: validate USB uplink OTA manifests"
```

---

### Task 5: Implement Isolated Uplink Application OTA over USB

**Files:**
- Create: `esp32/uplink/main/core/uplink_usb_ota.h`
- Create: `esp32/uplink/main/core/uplink_usb_ota.c`
- Modify: `esp32/uplink/main/core/badge_usb_transport.c`
- Modify: `esp32/uplink/main/core/serial_config.h`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/network/fw_store.h`
- Modify: `esp32/uplink/main/network/fw_store.c`
- Modify: `esp32/uplink/main/network/http_status.c`
- Modify: `esp32/uplink/main/network/fw_auto_check.c`
- Modify: `esp32/uplink/main/core/badge_runtime.h`
- Modify: `esp32/uplink/main/core/badge_runtime.c`
- Modify: `esp32/uplink/main/main.c`
- Modify: `backend/tests/test_badge_firmware_transport_contract.py`

**Interfaces:**

```c
bool uplink_usb_ota_begin(const uplink_ota_manifest_t *manifest,
                          char *response, size_t response_size);
bool uplink_usb_ota_write(const uint8_t *bytes, size_t length,
                          char *response, size_t response_size);
bool uplink_usb_ota_finish(char *response, size_t response_size);
void uplink_usb_ota_abort(const char *reason);
uint32_t uplink_usb_ota_remaining(void);
void uplink_usb_ota_get_status(uplink_usb_ota_status_t *out);
```

```c
typedef struct {
    fw_operation_owner_t owner;
    uint32_t generation;
    bool valid;
} fw_operation_token_t;

bool fw_store_operation_try_begin(fw_operation_owner_t owner,
                                  bool acquire_uart_lease,
                                  fw_operation_token_t *out);
bool fw_store_operation_end(fw_operation_token_t token);
```

Wire begin:

```text
FOF_CTL:{"cmd":"uplink_ota_begin","target":"uplink-s3-fof_badge","project":"fof_badge_uplink","hardware_type":"seeed_xiao_esp32s3","version":"<version>","size":<bytes>,"crc32":<uint32>,"sha256":"<64 hex>","flow_control":"credit-v1","recovery_rewrite_same_version":false}
```

Wire responses:

```text
FOF_UPLINK_OTA:{"ok":true,"phase":"ready","partition":"<inactive_ota_label>",...}
FOF_UPLINK_OTA:{"ok":true,"phase":"credit","received":4096,"credit_bytes":4096,...}
FOF_UPLINK_OTA:{"ok":true,"phase":"committed","partition":"<inactive_ota_label>","reboot_required":true,...}
FOF_UPLINK_OTA:{"ok":false,"phase":"aborted","error":"sha256_mismatch",...}
```

- [ ] **Step 1: Extend source contracts and verify RED**

Require a dedicated uplink runtime adapter, inactive OTA partition selection,
separate scanner/uplink completion callbacks, exact wire prefix, embedded
identity/digest checks before boot selection, and scanner cache isolation.
Require begin rejection while the running partition is pending verification.
Task 3 deliberately routes a consumed one-boot token into startup
recovery-only mode. The recovery command classifier and transport must
therefore allow the dedicated application-uplink OTA begin, binary, commit,
and abort path using only USB and local OTA APIs, even when the event loop,
scanner UART workers, and network services never started. Keep `FOF_CTL ota`
as the existing ROM-download alias; do not overload it as application OTA.
Add recovery-only begin/chunk/complete/reconnect tests proving a damaged badge
can install a valid uplink image without leaving the battery-connected USB
recovery surface.
For `FOF_BADGE_VARIANT`, reject registration or execution of every non-USB
mutation path: `POST /api/ota`, `POST /api/ota/relay`,
`POST /api/fw/upload`, `POST /api/fw/relay`,
`POST /api/fw/trigger`, HTTP `bootloader`, and the backend-driven
`fw_auto_check` worker. Keep read-only `/api/ota/info` and `/api/fw/info`
available when the badge's explicitly selected network mode enables HTTP.

Run:

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_firmware_transport_contract.py -v
```

Expected: failures identify missing application OTA behavior.

- [ ] **Step 2: Make firmware-operation ownership atomic**

Refactor the existing private `operation_try_begin()`/`operation_end()` into a
narrow owner-aware interface. Scanner upload/relay retains its UART lease.
Uplink OTA claims the same exclusive firmware-operation token without writing
scanner metadata. The claim must be atomic so a scanner relay cannot start
between a precheck and `esp_ota_begin()`. Acquisition increments a generation
and returns an owner-plus-generation token; only that exact token may release
the operation. Test wrong-owner release, stale-generation release, double
release, timeout racing automatic scanner convergence, and scanner staging
immediately after an aborted uplink OTA.

- [ ] **Step 3: Stream the image to only the inactive app slot**

Use `esp_ota_get_next_update_partition(NULL)`,
`esp_ota_begin(..., OTA_WITH_SEQUENTIAL_WRITES, ...)`, a 512-byte transport
buffer, incremental CRC32/SHA-256, a bounded descriptor prefix, and streaming
target/hardware marker matchers. Never call
`fw_store_get_target_partition()`, never touch `fw_*` scanner NVS keys, never
invalidate `fw_scanner_s3`, and never allocate the scanner's 64 KiB PSRAM
staging buffer.

At every begin, query live running-image state with
`esp_ota_get_state_partition(esp_ota_get_running_partition(), ...)`; do not
trust cached status. Require the target partition to be non-null, APP/OTA,
different from the running partition, exactly the configured slot size, and
never the data subtype/offset used by `fw_scanner_s3`. Add adapter-level
injected tests for stale cached pending state and partition drift.

After acquiring the token, pause HTTP upload and both scanner RX tasks before
`esp_ota_begin()`. Keep USB control, buttons, display, and health monitoring
alive. Restore paused tasks through one cleanup path on every non-reboot exit.

The ready receipt grants exactly 4096 bytes of host credit. After consuming
each window, emit and drain a required `phase=credit` receipt containing the
durable received count and the next credit. The host may never have more than
one window outstanding. The final short window ends with the committed receipt.
Add the same opt-in `flow_control=credit-v1` behavior to scanner staging while
retaining the uncredited legacy begin behavior for backward compatibility.

- [ ] **Step 4: Validate and commit in fail-closed order**

Require exact bytes, computed CRC/SHA, parsed embedded descriptor
project/version, fixed target/hardware mapping, and marker presence. Then call
`esp_ota_end()`, re-read the partition descriptor with
`esp_ota_get_partition_description()`, recheck project/version, and finally
call `esp_ota_set_boot_partition()`. Emit and drain the committed response,
arm `badge_runtime_arm_expected_reboot("usb_uplink_ota")`, delay only for the
bounded drain, and restart.

Once `esp_ota_set_boot_partition()` succeeds, the transaction is committed:
arm the expected reboot and restart even if the terminal receipt cannot drain.
Do not abort the now-valid OTA or resume ordinary runtime work. The laptop
treats a missing committed receipt as an uncertain outcome, reconnects by MAC,
and resolves success only from the running identity/version/rollback state.
Populate every receipt from `next_partition->label`; test both
`ota_0 -> ota_1` and `ota_1 -> ota_0` directions.

If the existing explicit USB rollback command's
`esp_ota_mark_app_invalid_rollback_and_reboot()` call returns, emit a terminal
failure and clear its armed expected-reboot marker so a later software reset
cannot be misclassified. A successful rollback remains non-returning.

On malformed begin, remain in line mode. On five seconds without binary
progress, abort the OTA handle, release operation ownership, emit one terminal
error, and return to line mode. An interruption must leave the running boot
partition and cached scanner image untouched.

Use one idempotent pre-commit cleanup routine for every failure from operation
claim, partition validation, `esp_ota_begin`, any `esp_ota_write`, digest or
descriptor validation, `esp_ota_end`, descriptor reread, and boot selection.
It conditionally aborts a valid handle, resumes only tasks this transaction
paused, releases only its exact operation token, clears transport binary
ownership, and returns exactly one structured terminal result. Add injected
failure tests for each stage.

- [ ] **Step 5: Disable non-USB mutation for badge builds**

Under `FOF_BADGE_VARIANT`, do not register the mutating OTA/store/relay/trigger
HTTP handlers, remove the web UI upload and Bootloader controls, reject an HTTP
control payload naming `bootloader`, and do not start `fw_auto_check_init()`.
Do not delete these paths for non-badge uplink targets. The USB-staged scanner
cache and automatic UART coordinator remain enabled; only their HTTP/network
entrypoints are disabled.

- [ ] **Step 6: Report OTA and rollback state**

Add this stable status object:

```json
{"uplink_ota":{"state":"idle","partition":"","received":0,"total":0,"target_version":"","last_error":""}}
```

Keep top-level `hardware_id`, `running_partition`, and `pending_verify` for
host rebinding and post-reboot proof. A post-OTA heartbeat alone cannot mark
the image valid; require a completed `FOF_PING` or `FOF_STATUS` response.

- [ ] **Step 7: Run native tests, contracts, and uplink build**

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
cd ..
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_badge_firmware_transport_contract.py -v
cd esp32/uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: all tests pass and the 8 MiB badge build still fits both 2 MiB OTA
slots without changing `partitions_s3_fof_badge_8mb.csv`.

- [ ] **Step 8: Commit**

```bash
git add esp32/uplink/main/core/uplink_usb_ota.* esp32/uplink/main/core/serial_config.c esp32/uplink/main/network/fw_store.* esp32/uplink/main/network/http_status.c esp32/uplink/main/network/fw_auto_check.c esp32/uplink/main/core/badge_runtime.* esp32/uplink/main/main.c backend/tests/test_badge_firmware_transport_contract.py
git commit -m "badge: add USB uplink application OTA"
```

---

### Task 6: Harden the Laptop's One-Command USB Flow

**Files:**
- Modify: `scripts/fof_badge_flash.py`
- Modify: `scripts/test_fof_badge_flash.py`

**Interfaces:**

```python
def probe_application(port: str, timeout_s: int) -> dict[str, object] | None: ...
def wait_for_application_port(expected_hardware_id: str,
                              timeout_s: int) -> tuple[str, dict[str, object]]: ...
def wait_for_rom_device(expected_hardware_id: str | None,
                        timeout_s: int) -> UsbDevice: ...
def flash_complete_uplink_layout(device: UsbDevice,
                                 platform: dict[str, object]) -> None: ...
```

```python
class BadgeSerial:
    def upload_uplink_firmware(self, platform, version,
                               recovery_rewrite_same_version=False) -> dict: ...
```

- [ ] **Step 1: Add failing host-selection and OTA tests**

Cover a responsive application selecting application OTA rather than esptool;
silence never attempting a watchdog/DTR/RTS reset; the exact begin manifest;
zero binary bytes before a validated ready receipt; exact committed receipt;
one-window-at-a-time credit enforcement and mismatched credit rejection;
interruption aborting before scanner staging; downgrade/unordered/equal policy;
same-version recovery flag; renamed post-reboot ports; MAC continuity; pending
rollback clearance; and scanner staging exactly once after uplink proof.

- [ ] **Step 2: Add failing ROM fallback tests**

Require one
`HOLD OK + MENU FOR 10 SECONDS, RELEASE, THEN PRESS OK` prompt, no
battery-disconnect text,
ambiguous-device rejection, base-MAC continuity when a pre-reset identity is
known, and the explicit full layout:

```text
0x000000 bootloader.bin
0x008000 partitions.bin
0x00f000 ota_data_initial.bin
0x020000 firmware.bin
```

Require esptool write verification plus explicit `verify_flash`. Reject any
generated flash argument that places the app at `0x10000`; the badge partition
table's `ota_0` starts at `0x20000`.

Decode the packaged `partitions.bin` before any write and require exact,
non-overlapping entries for NVS, OTA data, `ota_0 @ 0x20000 / 0x200000`,
`ota_1 @ 0x220000 / 0x200000`, and
`fw_scanner_s3 @ 0x420000 / 0x200000`. Cross-check every selected image
address and size against the decoded table and the regenerated flash manifests.

Also require scanner staging to request `flow_control=credit-v1`, honor exact
4096-byte credit receipts, and remain compatible with a legacy firmware ready
receipt that does not advertise credit.

- [ ] **Step 3: Run Python tests and verify RED**

```bash
/Users/billh/.platformio/penv/bin/python -m unittest scripts.test_fof_badge_flash -v
```

Expected: failures show that `usb_flow()` still always calls PlatformIO upload,
`wait_ping()` still tries watchdog reset, and port rebinding is pathname-based.

- [ ] **Step 4: Implement application-first identity-bound flow**

Locally validate the artifact with existing
`validate_firmware_artifact()`. Probe PING/STATUS without reset. If alive,
validate target/project/hardware/base MAC and use application OTA only when the
version policy permits. Validate every ready-receipt field, stream 1 KiB chunks,
but stop at each 4096-byte credit boundary until the device acknowledges the
exact received count. Require the committed receipt, reconnect by exact base
MAC, and require the new
version plus a fresh PING/STATUS and `pending_verify=false` before scanner
staging.

The probe must capture the exact `FOF_PONG:<version>`, then require it to equal
`FOF_STATUS.version` and require `usb_health.responses_completed` to increase
across the fresh PING/STATUS round trip. If the committed receipt is missing
after all payload bytes were accepted, treat the result as uncertain rather
than failed: reconnect by base MAC and accept only the exact target version,
partition, and cleared rollback proof.

- [ ] **Step 5: Implement independent ROM recovery fallback**

On bounded application silence, close serial, print the chord instruction once,
and poll ROM devices with esptool `--before no_reset --after no_reset`. Fail
closed on ambiguity or MAC mismatch. Flash and read-verify the four explicit
regions, run `esptool --before no_reset --after no_reset run` against the
already selected ROM device, then wait for application rebinding by MAC. Do not call
`reset_uplink_from_bootloader()` or `flash_uplink_usb()` from normal flow.

- [ ] **Step 6: Preserve scanner auto-convergence and add final control proof**

After uplink health is proven, call existing `stage_scanner_firmware()` once,
using the same negotiated credit window, wait for both slots through
`wait_for_scanners_usb()`, and require unique
scanner MACs, exact versions, clear rollback state, correct BLE-primary and
Wi-Fi-primary roles, and live radio health. Snapshot the current theme, apply a
temporary non-persisted brightness change, verify readback, restore the exact
original theme in `finally`, and verify restoration.

- [ ] **Step 7: Run Python tests and verify GREEN**

Run the Task 6 unittest command again.

Expected: all flasher tests pass.

- [ ] **Step 8: Commit**

```bash
git add scripts/fof_badge_flash.py scripts/test_fof_badge_flash.py
git commit -m "badge-flasher: use application OTA with ROM fallback"
```

---

### Task 7: Enforce Android's Control-Only USB Boundary

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeControlTransportPolicy.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlAction.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlTransportPolicyTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlStatusParserTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeControlActionTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeNavigationContractTest.kt`

**Interfaces:**

```kotlin
data class BadgeUsbHealthStatus(
    val taskStarted: Boolean = false,
    val hostConnected: Boolean = false,
    val parserState: String = "",
    val rxBytes: Long = 0L,
    val validCommands: Long = 0L,
    val responsesCompleted: Long = 0L,
    val requiredResponseFailures: Long = 0L,
    val malformedLines: Long = 0L,
    val droppedProgressFrames: Long = 0L,
    val droppedOptionalFrames: Long = 0L,
    val uploadReceived: Long = 0L,
    val uploadSize: Long = 0L,
    val taskHeartbeatAgeSeconds: Long? = null,
    val lastRxAgeSeconds: Long? = null,
    val lastCommandAgeSeconds: Long? = null,
    val lastResponseAgeSeconds: Long? = null,
    val lastUploadProgressAgeSeconds: Long? = null,
)
```

- [ ] **Step 1: Write failing boundary and parser tests**

Require `REBOOT` to be the only dangerous Android badge action. Reject
`fw_upload_begin`, `uplink_ota_begin`, `fw_relay`, `FOF_BOOTLOADER`,
`enterBootloader`, `relayScannerFirmware`, and `flashScannerFirmware` from
the repository/ViewModel/screen. Keep confirmed reboot. Parse every new USB
health field as a `Long`/nullable age, and prove older firmware without the
fields retains safe defaults. Load
`docs/badge/protocol/badge_usb_health_v1.fixture.json` in this test so firmware
snake-case keys, parser enum strings, numeric/null types, host tests, and
Android mappings share one contract.

- [ ] **Step 2: Run focused Android tests and verify RED**

```bash
cd android
./gradlew testDebugUnitTest --tests '*BadgeControlTransportPolicyTest' \
  --tests '*BadgeControlStatusParserTest' \
  --tests '*BadgeControlActionTest' \
  --tests '*BadgeNavigationContractTest'
```

Expected: tests fail on current bootloader and per-slot relay UI/repository
actions and missing USB-health parsing.

- [ ] **Step 3: Remove firmware mutation actions and add health parsing**

Remove the bootloader and manual scanner recovery buttons/methods. Keep PING,
status, badge detections, investigations, themes, custom palettes, display
policy, navigation, runtime mode controls, and confirmed application reboot.
Continue to parse firmware progress frames as diagnostic input for backward
compatibility, but never emit a firmware command from Android.

- [ ] **Step 4: Run the complete Android unit suite**

```bash
cd android
./gradlew testDebugUnitTest
```

Expected: all Android unit tests pass, including identity handshake, line
framing, frame trust, lifecycle gate, activity, theme, and display-policy tests.

- [ ] **Step 5: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe android/app/src/test/java/com/friendorfoe
git commit -m "android: keep badge USB control-only"
```

---

### Task 8: Add Reproducible Physical Acceptance Evidence

**Files:**
- Create: `scripts/verify_badge_usb_hardening.py`
- Create: `scripts/test_verify_badge_usb_hardening.py`
- Create: `docs/badge/usb-hardening-acceptance.md`

**Interfaces:**

```python
@dataclass(frozen=True)
class BadgeAcceptanceSession:
    session_id: str
    uplink_hardware_id: str
    ble_hardware_id: str
    wifi_hardware_id: str

def record_gate(evidence_path: Path, session: BadgeAcceptanceSession,
                gate: str, status: str,
                facts: dict[str, object]) -> None: ...
def verify_badge_snapshot(status: dict[str, object],
                          session: BadgeAcceptanceSession,
                          expected_version: str) -> dict[str, object]: ...
```

- [ ] **Step 1: Write failing evidence and validation tests**

Require append-only timestamped JSONL; exact uplink/scanner versions and MACs;
unique IDs; USB counters/ages; rollback state; roles; radio health; reversible
control result; operator confirmations; and six named gates. Reject detection
payloads, SSIDs, nearby MACs, or other captured ambient data from evidence.
Establish the three immutable MACs once at baseline and reject a board/slot swap
in every later gate. Require `last_expected_reboot_reason` evidence when a gate
expects a software reboot.

Add a deterministic acceptance-only command:

```bash
/Users/billh/.platformio/penv/bin/python scripts/verify_badge_usb_hardening.py \
  --gate interrupted-upload --abort-after 65536 --port /dev/cu.usbmodemXXXX
```

It uses the production begin/credit protocol, writes exactly 65,536 bytes,
closes the serial port without a terminal command, waits seven seconds for the
five-second device idle abort, reconnects by MAC, and verifies old running
partition/version plus unchanged scanner cache generation/SHA before retrying.

- [ ] **Step 2: Run verifier tests and verify RED**

```bash
/Users/billh/.platformio/penv/bin/python -m unittest scripts.test_verify_badge_usb_hardening -v
```

Expected: import failure because the verifier is absent.

- [ ] **Step 3: Implement the thin verifier and operator runbook**

Import the production `BadgeSerial` and status validators instead of copying
protocol logic. Record only the bounded release evidence fields. The runbook
must state that the currently silent badge does not yet contain the recovery
chord and may need one existing-board bootstrap into ROM before this hardened
image can be installed; after installation, battery disconnect is forbidden
for the acceptance run.
The interrupted-upload gate must be explicit and unavailable from the normal
`fof_badge_flash.py` CLI.

- [ ] **Step 4: Run verifier and flasher tests**

```bash
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_verify_badge_usb_hardening \
  scripts.test_fof_badge_flash -v
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/verify_badge_usb_hardening.py scripts/test_verify_badge_usb_hardening.py docs/badge/usb-hardening-acceptance.md
git commit -m "badge: add USB hardening acceptance gate"
```

---

### Task 9: Run the Complete Automated Release Gate

**Files:**
- No new files expected.

**Interfaces:**
- Consumes: completed firmware, laptop, Android, and evidence changes.
- Produces: reproducible build/test results before any physical write.

- [ ] **Step 1: Run all native ESP32 tests**

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: zero failed tests.

- [ ] **Step 2: Run backend contracts**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests -v
```

Expected: zero failed tests.

- [ ] **Step 3: Run laptop tooling tests**

```bash
/Users/billh/.platformio/penv/bin/python -m unittest \
  scripts.test_fof_badge_flash \
  scripts.test_verify_badge_usb_hardening -v
```

Expected: zero failed tests.

- [ ] **Step 4: Run Android tests and build the debug APK**

```bash
cd android
./gradlew testDebugUnitTest assembleDebug
```

Expected: tests and APK assembly succeed.

- [ ] **Step 5: Build both badge firmware targets**

```bash
cd esp32/scanner
/Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge -t clean
/Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: both builds exit zero. Record flash/RAM/PSRAM usage and prove the
uplink image is below the 2 MiB OTA slot without changing the partition table.
Decode `partitions.bin` and prove every regenerated flash manifest uses
`0x20000` for the application.

- [ ] **Step 6: Review scope and commit any test-only corrections**

```bash
git diff --check
git status --short
git diff --stat
```

Expected: no whitespace errors; `.camera-before-zoom.jpg` remains untouched;
no version, factory bundle, release manifest, or GitHub workflow artifact has
been promoted.

---

### Task 10: Execute the Battery-Connected Three-Board Physical Gate

**Files:**
- Create at runtime: ignored evidence file under `artifacts/badge-usb-hardening/`
- Modify only after observed results: `docs/badge/usb-hardening-acceptance.md`

**Interfaces:**
- Consumes: one assembled uplink plus two scanners, one data-capable USB cable, Android device/app, and the tested local binaries.
- Produces: six PASS records tied to exact hardware IDs and firmware versions.

- [ ] **Step 1: Install the hardened uplink once without claiming acceptance**

If the current application responds, use application OTA. If it remains silent
and lacks the new chord, use only the Task 0 photographed onboard XIAO
BOOT-plus-RESET procedure once to enter ROM and flash the explicit verified
full layout. Do not erase or write until the exact target base MAC has been
read. This bootstrap is setup, not a passing recovery result.

- [ ] **Step 2: Gate 1 — Android control and reconnect**

Install the exact candidate on the uplink only and leave both scanners running
firmware versions strictly older than that candidate before the session begins.
Gate 1 must anchor that reachable pre-update state, including the exact scanner
identities, roles, live radios, and prior versions. Do not downgrade or directly
USB-flash the scanners after recording Gate 1.

With all three boards powered from the badge battery, prove Android connects,
receives status and one live badge-sourced detection, changes and restores a
theme, disconnects/reconnects after cable removal, and exposes no firmware
mutation action.

- [ ] **Step 3: Gate 2 — three consecutive complete laptop cycles**

Without changing boards or versions after Gate 1, run cycle 1 without
`--recovery-rewrite-same-version`. Require the staged
generation/manifest/slot-mask to move from both slots pending to both complete,
and prove the host issued zero manual `fw_relay` commands. This is the required
automatic strict-newer convergence proof.

Run cycles 2 and 3 through the acceptance verifier. It selects the explicit
same-version repair path internally; do not invoke the flasher directly or add
a manual recovery/relay flag. Keep the battery continuously connected:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate three-update-cycles \
  --cycle 2 \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl

/Users/billh/.platformio/penv/bin/python \
  scripts/verify_badge_usb_hardening.py \
  --gate three-update-cycles \
  --cycle 3 \
  --port /dev/cu.usbmodemXXXX \
  --session-file /private/path/session.json \
  --evidence artifacts/badge-usb-hardening/acceptance.jsonl
```

Each cycle must prove exact uplink version/MAC, scanner versions/MACs, clear
rollback state, correct slot roles, live radios, one scanner upload, both UART
convergences, PING/STATUS, and restored theme.

- [ ] **Step 4: Gate 3 — interrupted application upload**

Run the Task 8 acceptance-only 65,536-byte interruption command. Wait for the
device's five-second idle abort, then prove the prior application/partition
still runs, scanner cache generation/SHA is unchanged, the parser returned to
`command`, and the next application OTA retry succeeds.

- [ ] **Step 5: Gate 4 — chord-to-ROM full recovery**

With a live USB data host attached, hold OK+Menu for exactly 10 seconds, release
both, then press OK at the `USB FLASH?` confirmation. Prove ROM enumeration,
base-MAC continuity, full-layout write and readback,
application return, scanner staging, and both UART updates. Do not touch the
battery.

- [ ] **Step 6: Gate 5 — charger/no-host normal reboot**

Without a data host, hold OK+Menu for 10 seconds. Prove the badge performs a
normal application reboot rather than remaining in ROM download mode. A
power-only charger must produce the same result. After reconnect, require
`last_expected_reboot_reason=button_reboot`; do not rely only on observation.

- [ ] **Step 7: Gate 6 — power-state audit**

Confirm the battery remained connected for the entire gate, no physical chord
entered quiet/off mode, and no persistent safe-mode/reboot loop occurred.

- [ ] **Step 8: Record evidence and stop on any failure**

Write PASS only from observed device responses. Any failed or unobserved gate
keeps the release blocked. Do not version, promote, tag, push, or publish while
one gate is incomplete.

---

### Task 11: Promote the Proven Firmware, Then Verify Released Artifacts

**Files:**
- Modify only after Task 10 passes: version sources selected by `esp32/scripts/firmware_version.py`
- Modify only after Task 10 passes: badge factory bundle/manifests and release notes used by the existing tag workflow

**Interfaces:**
- Consumes: complete automated logs and six physical PASS records.
- Produces: one aligned release version and verified factory/release artifacts.

- [ ] **Step 1: Reconfirm all gates and choose the next version once**

Use the repository version helper to update uplink, scanner, Android-facing
metadata, and release manifests consistently. Do not edit version strings by
search-and-replace.

- [ ] **Step 2: Rebuild and rerun Task 9 at the final version**

Expected: every test/build remains green and artifact identities match the
chosen version.

- [ ] **Step 3: Repeat the complete Task 10 physical gate on final binaries**

The version change alters the bytes. Rerun Android reconnect/control,
strictly-older automatic scanner convergence, three update cycles, interrupted
application upload, confirmed chord-to-ROM recovery, charger/no-host reboot,
and battery/power-state audit on the exact binaries that will enter the factory
bundle. Provisional-version evidence cannot authorize final artifacts.

- [ ] **Step 4: Replace factory artifacts and create the release commit**

Include only validated binaries/manifests and release documentation. Review
`git diff --check`, exact file list, and binary hashes before commit.

- [ ] **Step 5: Push/tag only with explicit release authorization**

After the user confirms the physical result, push the release commit and `v*`
tag. Verify GitHub Actions, all firmware release assets, factory bundle, web
flasher manifest, signed APK version/signature/digest, and live update catalogs.
