# Backend Lite USB, Recovery AP, and Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade only the Backend Badge Lite uplink firmware with buffered live USB, acknowledged New Dash delivery, USB configuration, Wi-Fi recovery AP behavior, and a bounded PSRAM dashboard while preserving native badge, scanner, FastAPI backend, Android, and New Dash source.

**Architecture:** The coordinator keeps HTTP as its authoritative upload sink and adds a one-shot canonical observer for independent local consumers. Pure C modules own Wi-Fi recovery policy, live-session state, USB framing/configuration, compact event projection, and the rolling ring; ESP-IDF adapters own USB Serial/JTAG and AP HTTP I/O. The Lite build selects these modules explicitly, while Fullsize keeps its existing AP/USB behavior.

**Tech Stack:** ESP-IDF 5.x, PlatformIO, ESP32-S3 USB Serial/JTAG, FreeRTOS, ESP HTTP server, PSRAM, C11, Unity native tests, and focused Python contract tests.

## Global Constraints

- Work only on branch `codex/backend-firmware` in `/Users/billh/gai/friendorfoe/.worktrees/backend-firmware`.
- Do not modify `esp32/`, either Backend Lite scanner image, native badge assets or release targets, `backend/`, `android/`, New Dash source, flasher source, or APKs.
- Lite identity stays exactly `badge_lite` / `uplink-s3-backend` / `fof_backend_uplink` / `seeed_xiao_esp32s3`; never spoof `fof_badge_uplink`.
- Keep scanner UART unchanged at 921600 baud and keep simultaneous HTTP upload active while USB is attached.
- The recovery AP is eligible only when Wi-Fi is unconfigured or one complete configured-network association pass failed.
- A received-and-matching New Dash heartbeat acknowledgement is the only USB state that suppresses an eligible recovery AP; USB power, enumeration, PING, STATUS, configuration, and LIVE_START alone never suppress it.
- Heartbeats are emitted every 5000 ms; a valid new acknowledgement grants a 15000 ms lease.
- Use ESP32-S3 USB Serial/JTAG with 8192-byte RX/TX driver rings and driver writes no larger than 4096 bytes.
- Commands are at most 2047 bytes, `FOF_DET` is at most 1535 bytes, status is at most 8192 bytes, and required replies always outrank optional frames.
- The dashboard ring is PSRAM-only, exactly 128 records, at most 65536 bytes total, never persisted, and empty after reboot.
- Core setup routes remain usable if any optional dashboard route fails to register.
- Do not compile anything under `backend-firmware/vendor/`.
- Do not flash hardware in this plan. A flash and hardware smoke test require separate explicit approval.
- Run only focused firmware/native/contract tests plus one `uplink-s3-backend` build; do not run the FastAPI backend suite.

---

### Task 1: Lite Wi-Fi recovery policy

**Files:**
- Create: `backend-firmware/shared/backend_lite_ap_policy.h`
- Create: `backend-firmware/shared/backend_lite_ap_policy.c`
- Modify: `backend-firmware/uplink/main/network/backend_wifi_manager.h`
- Modify: `backend-firmware/uplink/main/network/backend_wifi_manager.c`
- Modify: `backend-firmware/platformio.ini`
- Test: `backend-firmware/test/test_backend_lite_ap_policy/test_main.c`
- Test: `backend-firmware/test/test_backend_wifi_manager/test_main.c`

**Interfaces:**
- Consumes: `backend_wifi_manager_t`, configured-network retry events, and `BACKEND_AP_START`/`BACKEND_AP_STOP` semantics.
- Produces:

```c
typedef enum {
    BACKEND_LITE_AP_REASON_NONE = 0,
    BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED,
    BACKEND_LITE_AP_REASON_WIFI_JOIN_FAILED,
} backend_lite_ap_reason_t;

typedef struct {
    bool wifi_configured;
    bool wifi_connected;
    bool wifi_join_failed;
    bool usb_live_confirmed;
} backend_lite_ap_input_t;

typedef struct {
    bool running;
    backend_lite_ap_reason_t reason;
} backend_lite_ap_policy_t;

void backend_lite_ap_policy_init(backend_lite_ap_policy_t *policy);
backend_ap_action_t backend_lite_ap_policy_tick(
    backend_lite_ap_policy_t *policy,
    backend_lite_ap_input_t input);
backend_lite_ap_reason_t backend_lite_ap_policy_reason(
    const backend_lite_ap_policy_t *policy);
bool backend_wifi_manager_join_failed(
    const backend_wifi_manager_t *manager);
```

- [ ] **Step 1: Write the failing Wi-Fi-manager pass test**

```c
void test_join_failed_only_after_every_saved_network_fails(void)
{
    backend_wifi_manager_t manager;
    backend_config_record_t config = two_network_config();
    TEST_ASSERT_TRUE(backend_wifi_manager_init(&manager, &config, 0));
    TEST_ASSERT_FALSE(backend_wifi_manager_join_failed(&manager));
    TEST_ASSERT_TRUE(backend_wifi_manager_handle_event(
        &manager, BACKEND_WIFI_EVENT_NO_AP, 1));
    TEST_ASSERT_FALSE(backend_wifi_manager_join_failed(&manager));
    TEST_ASSERT_TRUE(backend_wifi_manager_handle_event(
        &manager, BACKEND_WIFI_EVENT_AUTH_FAILED, 2));
    TEST_ASSERT_TRUE(backend_wifi_manager_join_failed(&manager));
    TEST_ASSERT_TRUE(backend_wifi_manager_handle_event(
        &manager, BACKEND_WIFI_EVENT_CONNECTED, 3));
    TEST_ASSERT_FALSE(backend_wifi_manager_join_failed(&manager));
}
```

Also assert generation reset and a transient disconnect clear the signal.

- [ ] **Step 2: Run the Wi-Fi test and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_wifi_manager
```

Expected: compilation fails because `backend_wifi_manager_join_failed()` does not exist.

- [ ] **Step 3: Implement the completed-pass signal**

Add `bool join_failed` to `backend_wifi_policy_t`. Clear it in generation reset, CONNECTED, and DISCONNECTED from a previously connected state. Set it only when `advance_network()` reaches the final configured network and schedules a retry. Expose this accessor without changing retry timing:

```c
bool backend_wifi_manager_join_failed(const backend_wifi_manager_t *manager)
{
    return manager != NULL && manager->initialized &&
           manager->policy.join_failed;
}
```

- [ ] **Step 4: Write the failing Lite AP policy tests**

Cover unconfigured start, no start during an initial join pass, join-failed start, immediate stop on GOT_IP, immediate stop on confirmed USB, no suppression from unconfirmed USB, and restart after USB lease loss.

```c
void test_only_confirmed_usb_or_wifi_suppresses_recovery(void)
{
    backend_lite_ap_policy_t policy;
    backend_lite_ap_policy_init(&policy);
    backend_lite_ap_input_t input = {
        .wifi_configured = false,
        .wifi_connected = false,
        .wifi_join_failed = false,
        .usb_live_confirmed = false,
    };
    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
    input.usb_live_confirmed = true;
    TEST_ASSERT_EQUAL(BACKEND_AP_STOP,
        backend_lite_ap_policy_tick(&policy, input));
    input.usb_live_confirmed = false;
    TEST_ASSERT_EQUAL(BACKEND_AP_START,
        backend_lite_ap_policy_tick(&policy, input));
}
```

- [ ] **Step 5: Run the Lite AP test and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_lite_ap_policy
```

Expected: compilation fails because `backend_lite_ap_policy.h` is absent.

- [ ] **Step 6: Implement the minimal Lite policy**

Compute `eligible = !wifi_configured || wifi_join_failed` and `desired = eligible && !wifi_connected && !usb_live_confirmed`. Return START or STOP only on a `running` transition. Preserve the eligibility reason while suppressed so status explains why the AP will reopen. Add only the pure policy source to the native build filter.

- [ ] **Step 7: Run both focused policy tests and commit**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_wifi_manager -f test_backend_lite_ap_policy
```

Expected: both suites pass.

```bash
git add backend-firmware/shared/backend_lite_ap_policy.* backend-firmware/uplink/main/network/backend_wifi_manager.* backend-firmware/platformio.ini backend-firmware/test/test_backend_lite_ap_policy backend-firmware/test/test_backend_wifi_manager/test_main.c
git commit -m "feat: add Lite Wi-Fi recovery policy"
```

---

### Task 2: Exactly-once canonical events and PSRAM ring core

**Files:**
- Create: `backend-firmware/uplink/main/core/backend_dashboard_event.h`
- Create: `backend-firmware/uplink/main/core/backend_dashboard_event.c`
- Create: `backend-firmware/uplink/main/storage/backend_event_ring.h`
- Create: `backend-firmware/uplink/main/storage/backend_event_ring.c`
- Modify: `backend-firmware/uplink/main/core/backend_coordinator.h`
- Modify: `backend-firmware/uplink/main/core/backend_coordinator.c`
- Modify: `backend-firmware/platformio.ini`
- Test: `backend-firmware/test/test_backend_dashboard_event/test_main.c`
- Test: `backend-firmware/test/test_backend_event_ring/test_main.c`
- Test: `backend-firmware/test/test_backend_threat_policy/test_main.c`

**Interfaces:**
- Consumes: canonical `backend_detection_observation_t`, `fof_policy_detection_identity_key()`, and `rssi_distance_estimate_m()`.
- Produces:

```c
typedef void (*backend_coordinator_canonical_sink_fn)(
    void *context,
    const backend_detection_observation_t *observation);

void backend_coordinator_set_canonical_sink(
    backend_coordinator_t *coordinator,
    backend_coordinator_canonical_sink_fn sink,
    void *context);

typedef struct {
    uint64_t sequence;
    bool timestamp_valid;
    int64_t timestamp_epoch_ms;
    char id[64];
    char manufacturer[32];
    char model[32];
    char badge_label[24];
    char badge_class[24];
    char badge_entity_key[192];
    uint8_t source;
    float confidence;
    uint8_t threat_score;
    int8_t rssi;
    double distance_m;
    double aircraft_lat;
    double aircraft_lon;
    double operator_lat;
    double operator_lon;
    uint8_t scanner_slot_mask;
} backend_dashboard_event_t;

bool backend_dashboard_event_project(
    const backend_detection_observation_t *observation,
    backend_dashboard_event_t *out);
size_t backend_dashboard_event_encode_json(
    const backend_dashboard_event_t *event,
    char *output,
    size_t capacity);
size_t backend_dashboard_event_encode_fof_det(
    const backend_dashboard_event_t *event,
    char *output,
    size_t capacity);

typedef struct {
    backend_dashboard_event_t *records;
    size_t capacity;
    size_t start;
    size_t count;
    uint64_t next_sequence;
    uint64_t dropped_contention;
} backend_event_ring_t;

typedef struct {
    size_t count;
    uint64_t oldest_sequence;
    uint64_t newest_sequence;
    bool cursor_reset;
} backend_event_ring_snapshot_t;

bool backend_event_ring_init(
    backend_event_ring_t *ring,
    backend_dashboard_event_t *storage,
    size_t capacity);
bool backend_event_ring_append(
    backend_event_ring_t *ring,
    const backend_dashboard_event_t *event);
bool backend_event_ring_snapshot(
    const backend_event_ring_t *ring,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *output,
    size_t output_capacity,
    backend_event_ring_snapshot_t *snapshot);
```

- [ ] **Step 1: Write failing coordinator observer tests**

Use literal counters to prove HTTP is attempted before the observer, repeated HTTP refusal notifies once, eventual success does not notify again, and observer presence cannot change backpressure.

```c
TEST_ASSERT_EQUAL_UINT32(3, fixture.upload_calls);
TEST_ASSERT_EQUAL_UINT32(1, fixture.canonical_calls);
TEST_ASSERT_EQUAL_UINT32(1, fixture.order[0]);
TEST_ASSERT_EQUAL_UINT32(2, fixture.order[1]);
```

- [ ] **Step 2: Run the coordinator suite and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_threat_policy
```

Expected: compilation fails because the canonical observer API is absent.

- [ ] **Step 3: Implement one-shot coordinator notification**

Add `pending_canonical_notified` to `backend_coordinator_t`. In `drain_uploads()`, obtain or retain the canonical observation, call the HTTP sink first, notify the canonical sink once regardless of HTTP result, retain the observation when HTTP refuses, and clear the notification flag only after HTTP accepts and pending is cleared.

```c
const bool uploaded = coordinator->upload_sink != NULL &&
    coordinator->upload_sink(
        coordinator->upload_sink_context,
        &coordinator->pending_upload);
if (!coordinator->pending_canonical_notified) {
    if (coordinator->canonical_sink != NULL) {
        coordinator->canonical_sink(
            coordinator->canonical_sink_context,
            &coordinator->pending_upload);
    }
    coordinator->pending_canonical_notified = true;
}
if (!uploaded) {
    return false;
}
coordinator->pending_upload_valid = false;
coordinator->pending_canonical_notified = false;
```

- [ ] **Step 4: Write failing projector tests with literal frames**

Test deterministic Lite semantics: Meta Glasses evidence in manufacturer/model/class reason maps first to `Meta Glasses`/`meta_glasses`; otherwise sources BLE_RID, WIFI_SSID, WIFI_DJI_IE, WIFI_BEACON, and WIFI_OUI map to `Drone`/`drone`; other observations use empty label/class. Entity key comes from shared policy; score is clamped rounded `max(confidence, fused_confidence) * 100`; NaN/Inf coordinates and confidence fail closed; JSON escaping is exact; `FOF_DET` contains the nine required compatibility keys and stays below 1535 bytes.

- [ ] **Step 5: Run projector tests and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_dashboard_event
```

Expected: compilation fails because the projector files do not exist.

- [ ] **Step 6: Implement the compact projector and encoders**

Use bounded copies and `backend_json_writer`. Add both assertions:

```c
_Static_assert(sizeof(backend_dashboard_event_t) <= 512U,
               "dashboard event exceeds 512 bytes");
_Static_assert(128U * sizeof(backend_dashboard_event_t) <= 65536U,
               "dashboard ring exceeds 64 KiB");
```

Do not import or compile classifier code from `vendor/` or `esp32/`.

```c
if (mentions_meta_glasses(&observation->detection)) {
    copy_text(out->badge_label, sizeof(out->badge_label), "Meta Glasses");
    copy_text(out->badge_class, sizeof(out->badge_class), "meta_glasses");
} else if (drone_source(observation->detection.source)) {
    copy_text(out->badge_label, sizeof(out->badge_label), "Drone");
    copy_text(out->badge_class, sizeof(out->badge_class), "drone");
}
```

- [ ] **Step 7: Write failing ring tests**

Cover sequences beginning at 1, 128-record overwrite, limit clipping, stale cursor reset, ahead-of-head empty response, unavailable storage, and `UINT64_MAX` wrap clearing before restarting at 1.

- [ ] **Step 8: Run ring tests and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_event_ring
```

Expected: compilation fails because the ring API is absent.

- [ ] **Step 9: Implement the caller-owned ring and run Task 2 tests**

The ring performs no allocation and no locking. It copies records into caller-provided storage, overwrites the oldest at 128 entries, and snapshots into caller-provided output.

```c
const size_t slot = (ring->start + ring->count) % ring->capacity;
if (ring->count == ring->capacity) {
    ring->start = (ring->start + 1U) % ring->capacity;
} else {
    ring->count++;
}
ring->records[slot] = *event;
ring->records[slot].sequence = ring->next_sequence++;
```

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_threat_policy -f test_backend_dashboard_event -f test_backend_event_ring
```

Expected: all three suites pass.

- [ ] **Step 10: Commit**

```bash
git add backend-firmware/uplink/main/core/backend_coordinator.* backend-firmware/uplink/main/core/backend_dashboard_event.* backend-firmware/uplink/main/storage/backend_event_ring.* backend-firmware/platformio.ini backend-firmware/test/test_backend_threat_policy/test_main.c backend-firmware/test/test_backend_dashboard_event backend-firmware/test/test_backend_event_ring
git commit -m "feat: add canonical Lite event history"
```

---

### Task 3: Pure USB protocol, live lease, and configuration transaction

**Files:**
- Create: `backend-firmware/uplink/main/usb/backend_usb_protocol.h`
- Create: `backend-firmware/uplink/main/usb/backend_usb_protocol.c`
- Create: `backend-firmware/uplink/main/usb/backend_usb_transport_core.h`
- Create: `backend-firmware/uplink/main/usb/backend_usb_transport_core.c`
- Create: `backend-firmware/uplink/main/usb/backend_usb_config.h`
- Create: `backend-firmware/uplink/main/usb/backend_usb_config.c`
- Modify: `backend-firmware/platformio.ini`
- Test: `backend-firmware/test/test_backend_usb_protocol/test_main.c`
- Test: `backend-firmware/test/test_backend_usb_transport_core/test_main.c`
- Test: `backend-firmware/test/test_backend_usb_config/test_main.c`

**Interfaces:**
- Consumes: strict backend JSON reader/writer, `backend_config_record_t`, canonical config validation, and commit/reconnect callbacks.
- Produces:

```c
#define BACKEND_USB_COMMAND_MAX 2047U
#define BACKEND_USB_STATUS_MAX 8192U
#define BACKEND_USB_DET_MAX 1535U
#define BACKEND_USB_HEARTBEAT_MS INT64_C(5000)
#define BACKEND_USB_LIVE_LEASE_MS INT64_C(15000)
#define BACKEND_USB_REQUIRED_QUEUE_CAPACITY 4U
#define BACKEND_USB_OPTIONAL_QUEUE_CAPACITY 32U

typedef enum {
    BACKEND_USB_COMMAND_PING = 0,
    BACKEND_USB_COMMAND_STATUS,
    BACKEND_USB_COMMAND_LIVE_START,
    BACKEND_USB_COMMAND_LIVE_ACK,
    BACKEND_USB_COMMAND_LIVE_STOP,
    BACKEND_USB_COMMAND_CONFIG_GET,
    BACKEND_USB_COMMAND_CONFIG_SET,
    BACKEND_USB_COMMAND_SET,
    BACKEND_USB_COMMAND_SAVE,
    BACKEND_USB_COMMAND_BACKEND_STATUS,
    BACKEND_USB_COMMAND_AP_START,
    BACKEND_USB_COMMAND_UNKNOWN,
    BACKEND_USB_COMMAND_INVALID,
} backend_usb_command_kind_t;

typedef struct {
    backend_usb_command_kind_t kind;
    char key[32];
    char value[192];
    char session_id[33];
    uint64_t sequence;
    const char *json;
    size_t json_length;
} backend_usb_command_t;

bool backend_usb_protocol_parse_line(
    const char *line,
    size_t length,
    backend_usb_command_t *out);
size_t backend_usb_protocol_encode_ready(char *output, size_t capacity);
size_t backend_usb_protocol_encode_pong(
    const backend_firmware_identity_t *identity,
    char *output,
    size_t capacity);
size_t backend_usb_protocol_encode_live_ready(
    const char *session_id,
    char *output,
    size_t capacity);
size_t backend_usb_protocol_encode_live_heartbeat(
    const char *session_id,
    uint64_t sequence,
    char *output,
    size_t capacity);
size_t backend_usb_protocol_encode_investigation(
    const char *investigation_json,
    size_t json_length,
    char *output,
    size_t capacity);

typedef struct {
    char session_id[33];
    uint64_t last_sent_sequence;
    uint64_t last_ack_sequence;
    uint64_t pending_heartbeat_sequence;
    int64_t next_heartbeat_ms;
    int64_t lease_expires_ms;
    bool started;
    bool confirmed;
    bool heartbeat_pending;
} backend_usb_live_state_t;

bool backend_usb_live_start(
    backend_usb_live_state_t *state,
    const char *session_id,
    int64_t now_ms);
bool backend_usb_live_prepare_heartbeat(
    backend_usb_live_state_t *state,
    int64_t now_ms,
    uint64_t *out_heartbeat_sequence);
bool backend_usb_live_note_heartbeat_sent(
    backend_usb_live_state_t *state,
    uint64_t sequence,
    int64_t now_ms);
void backend_usb_live_note_heartbeat_failed(
    backend_usb_live_state_t *state,
    uint64_t sequence);
bool backend_usb_live_acknowledge(
    backend_usb_live_state_t *state,
    const char *session_id,
    uint64_t sequence,
    int64_t now_ms);
void backend_usb_live_stop(backend_usb_live_state_t *state);
bool backend_usb_live_confirmed(
    backend_usb_live_state_t *state,
    int64_t now_ms);

typedef struct {
    backend_config_record_t staged;
    bool dirty;
} backend_usb_config_t;

typedef enum {
    BACKEND_USB_FRAME_REQUIRED = 0,
    BACKEND_USB_FRAME_OPTIONAL,
} backend_usb_frame_priority_t;

typedef enum {
    BACKEND_USB_FRAME_GENERIC = 0,
    BACKEND_USB_FRAME_LIVE_HEARTBEAT,
} backend_usb_frame_kind_t;

typedef struct {
    backend_usb_frame_priority_t priority;
    backend_usb_frame_kind_t kind;
    uint64_t correlation_sequence;
    size_t length;
    char bytes[BACKEND_USB_STATUS_MAX];
} backend_usb_frame_t;

typedef struct {
    backend_usb_frame_kind_t kind;
    uint64_t correlation_sequence;
    size_t length;
    char bytes[BACKEND_USB_STATUS_MAX];
} backend_usb_required_frame_t;

typedef struct {
    backend_usb_frame_kind_t kind;
    uint64_t correlation_sequence;
    size_t length;
    char bytes[BACKEND_USB_DET_MAX];
} backend_usb_optional_frame_t;

typedef struct {
    backend_usb_required_frame_t *required;
    backend_usb_optional_frame_t *optional;
    size_t required_capacity;
    size_t optional_capacity;
    size_t required_head;
    size_t required_count;
    size_t optional_head;
    size_t optional_count;
    uint64_t optional_drops;
    uint64_t required_failures;
    backend_usb_live_state_t live;
} backend_usb_transport_core_t;

bool backend_usb_transport_init(
    backend_usb_transport_core_t *transport,
    backend_usb_required_frame_t *required_storage,
    size_t required_capacity,
    backend_usb_optional_frame_t *optional_storage,
    size_t optional_capacity);

bool backend_usb_transport_enqueue(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_priority_t priority,
    backend_usb_frame_kind_t kind,
    uint64_t correlation_sequence,
    const char *frame,
    size_t length);
bool backend_usb_transport_pop(
    backend_usb_transport_core_t *transport,
    backend_usb_frame_t *out);

void backend_usb_config_init(
    backend_usb_config_t *state,
    const backend_config_record_t *current);
backend_portal_update_result_t backend_usb_config_stage(
    backend_usb_config_t *state,
    const char *key,
    const char *value);
backend_portal_update_result_t backend_usb_config_save(
    backend_usb_config_t *state,
    backend_config_portal_commit_fn commit,
    backend_config_portal_reconnect_fn reconnect,
    void *context,
    int64_t now_ms,
    uint32_t *out_generation);
```

- [ ] **Step 1: Write failing strict parser/encoder tests**

Cover exact PING/STATUS/CONFIG commands; 2047/2048-byte boundaries; CR/LF rejection inside values; strict LIVE_START client/protocol; duplicate, missing, extra, negative, and overflow JSON members; compatibility SET keys; truthful PONG/status identity; bounded `FOF_INV` wrapping; and fail-closed bounded encoders.

- [ ] **Step 2: Run protocol tests and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_usb_protocol
```

Expected: compilation fails because `backend_usb_protocol.h` is absent.

- [ ] **Step 3: Implement strict parsing and frame encoders**

Use length-aware comparisons and the repository JSON reader. Never retain pointers past dispatch; `json` in `backend_usb_command_t` is valid only during line dispatch. Encoding failure sets `output[0] = '\0'`.

```c
if (span_equals(line, length, "FOF_PING")) {
    out->kind = BACKEND_USB_COMMAND_PING;
    return true;
}
if (span_starts_with(line, length, "FOF_LIVE_ACK:")) {
    return parse_live_ack_json(line + strlen("FOF_LIVE_ACK:"),
                               length - strlen("FOF_LIVE_ACK:"), out);
}
out->kind = BACKEND_USB_COMMAND_UNKNOWN;
return true;
```

- [ ] **Step 4: Write failing queue and live-state tests**

Use caller-owned queue storage to prove required-before-optional ordering, whole-frame optional drops, bounded required failure, LIVE_START not confirmed, immediate heartbeat, wrong/stale/future/duplicate ACK rejection, valid ACK confirmation, exact 14999/15000 ms lease boundary, renewal only by a newer ACK, STOP clearing, and new-session invalidation. An ACK must remain invalid until `backend_usb_live_note_heartbeat_sent()` records completion of the entire heartbeat frame; enqueue or partial TX alone never makes its sequence acknowledgeable.

- [ ] **Step 5: Run transport-core tests and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_usb_transport_core
```

Expected: compilation fails because the transport core is absent.

- [ ] **Step 6: Implement bounded frame queues and live-state logic**

Represent each queued frame as `{priority, kind, correlation_sequence, length, bytes}`. Reject oversize frames before mutation. Pop required before optional. `prepare_heartbeat` reserves one sequence and enqueues it as `BACKEND_USB_FRAME_LIVE_HEARTBEAT`; `note_heartbeat_sent` advances `last_sent_sequence` only after complete physical TX, and `note_heartbeat_failed` releases the reservation for retry. A valid ACK matches the current session and satisfies `last_ack_sequence < sequence <= last_sent_sequence`; renew `lease_expires_ms = now_ms + 15000` with overflow-safe arithmetic.

```c
if (!state->started || strcmp(session_id, state->session_id) != 0 ||
    sequence <= state->last_ack_sequence ||
    sequence > state->last_sent_sequence) {
    return false;
}
state->last_ack_sequence = sequence;
state->confirmed = true;
state->lease_expires_ms = deadline_after(now_ms, BACKEND_USB_LIVE_LEASE_MS);
return true;
```

- [ ] **Step 7: Write failing USB configuration tests**

Cover staged slot-0 SSID/password without deleting slots 1-3, backend URL/device ID/AP password, unknown keys, invalid values, SAVE generation increment, commit failure rollback, reconnect failure as saved-but-reconnect-failed, JSON redaction, and no mutation before SAVE.

- [ ] **Step 8: Run configuration tests and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_usb_config
```

Expected: compilation fails because the configuration transaction is absent.

- [ ] **Step 9: Implement staged configuration and run Task 3 tests**

Use a local candidate, validate before commit, increment generation exactly once at SAVE, call commit before reconnect, and copy committed state back only after commit succeeds. Reuse `backend_portal_render_redacted_config()` for CONFIG_GET and `backend_portal_parse_config_update()` for atomic CONFIG_SET at integration.

```c
backend_config_record_t candidate = state->staged;
candidate.generation++;
if (backend_config_validate(&candidate) != BACKEND_CONFIG_VALID ||
    !commit(context, &candidate)) {
    return BACKEND_PORTAL_UPDATE_COMMIT_FAILED;
}
state->staged = candidate;
state->dirty = false;
return reconnect(context, &candidate, now_ms)
    ? BACKEND_PORTAL_UPDATE_OK
    : BACKEND_PORTAL_UPDATE_RECONNECT_FAILED;
```

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_usb_protocol -f test_backend_usb_transport_core -f test_backend_usb_config
```

Expected: all three suites pass.

- [ ] **Step 10: Commit**

```bash
git add backend-firmware/uplink/main/usb backend-firmware/platformio.ini backend-firmware/test/test_backend_usb_protocol backend-firmware/test/test_backend_usb_transport_core backend-firmware/test/test_backend_usb_config
git commit -m "feat: add Lite USB protocol and configuration"
```

---

### Task 4: Recovery dashboard routes and embedded page

**Files:**
- Create: `backend-firmware/uplink/main/network/backend_dashboard_page.h`
- Create: `backend-firmware/uplink/main/network/backend_dashboard_page.c`
- Modify: `backend-firmware/shared/backend_portal_contract.h`
- Modify: `backend-firmware/shared/backend_portal_contract.c`
- Modify: `backend-firmware/uplink/main/network/backend_config_portal.h`
- Modify: `backend-firmware/uplink/main/network/backend_config_portal.c`
- Modify: `backend-firmware/platformio.ini`
- Test: `backend-firmware/test/test_backend_portal_routes/test_main.c`
- Test: `backend-firmware/test/test_backend_portal_contract.py`

**Interfaces:**
- Consumes: `backend_dashboard_event_t`, ring snapshot metadata, existing AP-local destination enforcement, and existing setup/config handlers.
- Produces three optional routes: `GET /dashboard`, `GET /api/dashboard/status`, and `GET /api/events`.

```c
typedef bool (*backend_config_portal_dashboard_status_fn)(
    void *context,
    char *output,
    size_t capacity,
    size_t *out_length);

typedef bool (*backend_config_portal_event_snapshot_fn)(
    void *context,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *events,
    size_t event_capacity,
    backend_event_ring_snapshot_t *snapshot);

typedef struct {
    uint64_t after;
    size_t limit;
} backend_dashboard_query_t;

bool backend_dashboard_query_parse(
    const char *query,
    backend_dashboard_query_t *out);
```

- [ ] **Step 1: Write failing route registry and failure-isolation tests**

Under the Lite profile, assert five required setup routes and three optional dashboard routes. Under Fullsize, assert only the existing five required routes. For required registration failure, expect rollback and `running == false`. For failure at each Lite optional position, expect `running == true`, setup routes available, `dashboard_routes_enabled == false`, reason `route_registration_failed`, and best-effort unregister of earlier optional routes.

- [ ] **Step 2: Run portal tests and verify RED**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_portal_routes
python -m pytest test/test_backend_portal_contract.py -q
```

Expected: route-count and optional-failure assertions fail because only five required routes exist.

- [ ] **Step 3: Split required and optional registries**

Expose separate `backend_portal_required_routes()` and Lite-gated `backend_portal_dashboard_routes()` arrays. Register all required routes first with existing rollback. In Lite only, register optional routes second; on failure disable the dashboard set, record the stable reason, unregister already-added optional routes, and leave the AP/server running. Fullsize must neither compile the dashboard page nor register dashboard routes.

```c
static const backend_portal_route_t DASHBOARD_ROUTES[] = {
    {BACKEND_PORTAL_GET, "/dashboard", BACKEND_PORTAL_DASHBOARD},
    {BACKEND_PORTAL_GET, "/api/dashboard/status",
     BACKEND_PORTAL_DASHBOARD_STATUS},
    {BACKEND_PORTAL_GET, "/api/events", BACKEND_PORTAL_EVENTS},
};
```

- [ ] **Step 4: Write failing event-query behavior tests**

Use handler-facing pure helpers to cover default `limit=25`, acceptance at 50, rejection at 51, numeric overflow, missing/invalid `after`, cursor-reset serialization, AP-local enforcement before route dispatch, redacted status, and absence of credential fields.

- [ ] **Step 5: Implement bounded dashboard APIs and page**

Parse query values with full-string unsigned conversion. Copy at most 50 events through the callback, release caller locks before output, and send JSON in chunks one event at a time. Embed HTML/CSS/JavaScript with no external assets; poll both APIs every 1000 ms and keep browser state memory-only.

```c
backend_dashboard_query_t parsed = {.after = 0U, .limit = 25U};
if (!backend_dashboard_query_parse(query, &parsed) || parsed.limit > 50U) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "invalid cursor or limit");
}
```

- [ ] **Step 6: Make the AP identity profile-aware**

For `FOF_BACKEND_PROFILE_BADGE_LITE`, render exactly `FriendOrFoe-Lite-%02X%02X%02X`; preserve `FriendOrFoe-Backend-%02X%02X%02X` in Fullsize builds. Update native tests to cover each profile.

```c
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static const char AP_SSID_FORMAT[] = "FriendOrFoe-Lite-%02X%02X%02X";
#else
static const char AP_SSID_FORMAT[] = "FriendOrFoe-Backend-%02X%02X%02X";
#endif
```

- [ ] **Step 7: Run focused portal tests and commit**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_portal_routes
pio test -e backend-native-fullsize -f test_backend_portal_routes
python -m pytest test/test_backend_portal_contract.py -q
```

Expected: Lite, Fullsize, and Python contract suites pass.

```bash
git add backend-firmware/shared/backend_portal_contract.* backend-firmware/uplink/main/network/backend_config_portal.* backend-firmware/uplink/main/network/backend_dashboard_page.* backend-firmware/platformio.ini backend-firmware/test/test_backend_portal_routes/test_main.c backend-firmware/test/test_backend_portal_contract.py
git commit -m "feat: add Lite recovery dashboard portal"
```

---

### Task 5: ESP-IDF USB service and Lite uplink integration

**Files:**
- Create: `backend-firmware/uplink/main/usb/backend_usb_service.h`
- Create: `backend-firmware/uplink/main/usb/backend_usb_service.c`
- Modify: `backend-firmware/uplink/main/main.c`
- Modify: `backend-firmware/uplink/main/CMakeLists.txt`
- Modify: `backend-firmware/uplink/sdkconfig.defaults`
- Modify: `backend-firmware/tools/tests/test_backend_uplink_build_contract.py`
- Test: `backend-firmware/test/test_backend_usb_protocol/test_main.c`
- Test: `backend-firmware/test/test_backend_threat_policy/test_main.c`

**Interfaces:**
- Consumes: Tasks 1-4 APIs plus ESP-IDF USB Serial/JTAG, FreeRTOS queues/semaphores, strict PSRAM allocation, config portal callbacks, guarded OTA commands, and `scanner_uart_line_framer`.
- Produces:

```c
typedef void (*backend_usb_service_line_fn)(
    void *context,
    const char *line,
    size_t length,
    int64_t now_ms);

typedef struct {
    void *context;
    backend_usb_service_line_fn on_line;
} backend_usb_service_config_t;

bool backend_usb_service_start(
    backend_usb_service_t *service,
    const backend_usb_service_config_t *config);
bool backend_usb_service_emit(
    backend_usb_service_t *service,
    backend_usb_frame_priority_t priority,
    const char *frame,
    size_t length);
bool backend_usb_service_emit_heartbeat(
    backend_usb_service_t *service,
    uint64_t sequence,
    const char *frame,
    size_t length);
bool backend_usb_service_live_confirmed(
    backend_usb_service_t *service,
    int64_t now_ms);
bool backend_usb_service_live_start(
    backend_usb_service_t *service,
    int64_t now_ms,
    char out_session_id[33]);
```

- [ ] **Step 1: Write failing build-contract assertions**

Assert that Lite CMake selects each new event/ring/USB/policy source, Fullsize excludes Lite USB/AP policy sources, the service contains RX/TX `8192` and write cap `4096`, `main.c` registers both coordinator sinks, PSRAM uses `psram_alloc_strict`, and native badge identity strings are absent from Lite status builders.

- [ ] **Step 2: Run the contract test and verify RED**

```bash
cd backend-firmware
python -m pytest tools/tests/test_backend_uplink_build_contract.py -q
```

Expected: assertions fail because the ESP service and integration are absent.

- [ ] **Step 3: Implement the sole-owner USB adapter**

Install `usb_serial_jtag_driver_config_t` with both rings at 8192. Allocate four required-frame slots and 32 optional-frame slots with `psram_alloc_strict`; allocation failure disables live USB but leaves HTTP/scanners/AP recovery running. Read bounded blocks, feed `scanner_uart_line_framer`, and dispatch complete lines. One TX task drains required before optional frames and writes a complete frame in consecutive calls no larger than 4096. Route ESP logs through a nonblocking optional enqueue. If a partial write cannot finish, poison output until bounded newline recovery rather than concatenating a later machine frame.

Generate each boot session ID from 16 bytes filled by `esp_fill_random()` and encode it as 32 lowercase hex characters. A session ID is never reused after LIVE_STOP or a new LIVE_START.

```c
usb_serial_jtag_driver_config_t driver = {
    .rx_buffer_size = 8192,
    .tx_buffer_size = 8192,
};
if (usb_serial_jtag_driver_install(&driver) != ESP_OK) {
    return false;
}
usb_serial_jtag_vfs_use_driver();
```

- [ ] **Step 4: Refactor coordinator/upload locking before fan-out**

Add `coordinator_lock` and `upload_build_lock`. In UART dispatch, snapshot epoch under `s_runtime.lock`, resolve outside it, run coordinator ingest under `coordinator_lock`, then update threats under `s_runtime.lock`. In the coordinator worker, tick/retry under `coordinator_lock` and release it before runtime/LED/UART work. Replace `queue_upload_locked()` with `queue_upload()` that snapshots context under runtime lock, serializes under `upload_build_lock` outside runtime lock, then briefly locks runtime to push the immutable batch and counters. Enforce lock order `coordinator_lock -> upload_build_lock -> s_runtime.lock` and remove every reverse path.

```c
lock_runtime();
const int64_t epoch_ms = current_epoch_ms_locked(now_ms);
unlock_runtime();
backend_observation_resolve(&detection, &stamp, epoch_ms, &observation);
lock_coordinator();
const backend_coordinator_ingest_result_t result =
    backend_coordinator_ingest_detection(
        &s_runtime.coordinator, (uint8_t)slot, &observation, now_ms);
unlock_coordinator();
```

- [ ] **Step 5: Integrate the exactly-once local observer**

Project the canonical observation on the stack. Take `event_ring_lock` with zero wait, append if available, and release before encoding. Enqueue one optional `FOF_DET`; never write the driver or wait. Allocate exactly 128 events with `psram_alloc_strict()`; allocation failure sets `history_available=false` and is nonfatal.

```c
backend_dashboard_event_t event;
if (!backend_dashboard_event_project(observation, &event)) {
    return;
}
if (xSemaphoreTake(s_runtime.event_ring_lock, 0) == pdTRUE) {
    (void)backend_event_ring_append(&s_runtime.event_ring, &event);
    xSemaphoreGive(s_runtime.event_ring_lock);
} else {
    atomic_fetch_add_explicit(
        &s_runtime.event_ring_contention_drops, 1U, memory_order_relaxed);
}
```

- [ ] **Step 6: Integrate USB commands and canonical configuration**

Dispatch PING/STATUS/LIVE/CONFIG/SET/SAVE through pure modules. Keep `FOF_BACKEND_STATUS`, `FOF_AP_START`, and guarded Lite OTA aliases. Gate AP_START through Lite recovery eligibility and return `recovery_ap_not_needed` or `usb_live_confirmed` when ineligible. Build snapshots under runtime lock, encode after unlocking, and enqueue replies as required. `FOF_CONFIG_SET` calls the portal parser and commit/reconnect path. Existing completed investigation JSON is wrapped as `FOF_INV` without changing scanner UART. Screen commands return `unsupported_capability`. Emit `FOF_READY` only after dispatch callbacks are usable. Mark a heartbeat sequence sent only after the TX worker completes all driver writes for that frame.

```c
backend_usb_command_t command;
if (!backend_usb_protocol_parse_line(line, length, &command)) {
    emit_required_control_error("malformed_command");
    return;
}
switch (command.kind) {
case BACKEND_USB_COMMAND_PING:
    emit_required_pong();
    break;
case BACKEND_USB_COMMAND_STATUS:
    emit_required_status_snapshot();
    break;
default:
    dispatch_lite_usb_command(&command, now_ms);
    break;
}
```

- [ ] **Step 7: Replace only Lite AP orchestration**

Under `FOF_BACKEND_PROFILE_BADGE_LITE`, feed `wifi_configured`, `wifi_connected`, `backend_wifi_manager_join_failed()`, and `backend_usb_service_live_confirmed()` to `backend_lite_ap_policy_tick()`. Remove backend-reachability activation and backend-success close from Lite. Continue using existing Fullsize `backend_ap_policy` unchanged.

```c
const backend_lite_ap_input_t input = {
    .wifi_configured = config_valid,
    .wifi_connected = s_runtime.wifi_connected,
    .wifi_join_failed = backend_wifi_manager_join_failed(&s_runtime.wifi),
    .usb_live_confirmed = backend_usb_service_live_confirmed(
        &s_runtime.usb, now_ms),
};
ap_action = backend_lite_ap_policy_tick(&s_runtime.lite_ap_policy, input);
```

- [ ] **Step 8: Connect portal status/event callbacks**

Copy status under runtime lock. Copy ring events under `event_ring_lock` into bounded scratch storage, release the lock, and let the portal serialize/send. Status includes truthful identity, capabilities, recovery reason, full-pass state, USB queue/drop counters, live session/ACK/lease, and history degradation.

```c
if (xSemaphoreTake(s_runtime.event_ring_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
}
const bool copied = backend_event_ring_snapshot(
    &s_runtime.event_ring, after, limit, events, capacity, snapshot);
xSemaphoreGive(s_runtime.event_ring_lock);
return copied;
```

- [ ] **Step 9: Run focused native and contract tests**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_usb_protocol -f test_backend_usb_transport_core -f test_backend_usb_config -f test_backend_dashboard_event -f test_backend_event_ring -f test_backend_lite_ap_policy -f test_backend_wifi_manager -f test_backend_threat_policy -f test_backend_portal_routes
python -m pytest tools/tests/test_backend_uplink_build_contract.py tools/tests/test_backend_build_contract.py tools/tests/test_source_isolation.py test/test_backend_portal_contract.py -q
python tools/check_source_isolation.py --root .
```

Expected: all selected tests and isolation checks pass.

- [ ] **Step 10: Build only the Lite uplink and commit**

```bash
cd backend-firmware/uplink
pio run -e uplink-s3-backend
```

Expected: build succeeds within the existing app partition capacity.

```bash
git add backend-firmware/uplink/main/usb/backend_usb_service.* backend-firmware/uplink/main/main.c backend-firmware/uplink/main/CMakeLists.txt backend-firmware/uplink/sdkconfig.defaults backend-firmware/tools/tests/test_backend_uplink_build_contract.py
git commit -m "feat: integrate Lite USB and recovery runtime"
```

---

### Task 6: New Dash handoff specification and host fixture

**Files:**
- Create: `docs/backend-lite-new-dash-usb-protocol.md`
- Create: `backend-firmware/tools/backend_lite_usb_fixture.py`
- Create: `backend-firmware/tools/tests/test_backend_lite_usb_fixture.py`

**Interfaces:**
- Consumes: exact firmware protocol and identity from Tasks 3 and 5.
- Produces: a backend/New Dash implementation handoff without modifying either codebase.

- [ ] **Step 1: Write failing fixture tests**

Test literal transcripts for truthful PONG/STATUS identity, LIVE_START generation, LIVE_READY parsing, heartbeat ACK generation with the same session/sequence, stale-session rejection, `FOF_DET` parsing, CONFIG_SET serialization, and proof that no password appears in CONFIG_GET output.

```python
def test_ack_echoes_current_heartbeat_only():
    session = LiveSession.from_ready(
        'FOF_LIVE_READY:{"session_id":"boot-a1","heartbeat_ms":5000,"lease_ms":15000}'
    )
    assert session.ack(
        'FOF_LIVE_HEARTBEAT:{"session_id":"boot-a1","sequence":7}'
    ) == 'FOF_LIVE_ACK:{"session_id":"boot-a1","sequence":7}'
```

- [ ] **Step 2: Run fixture tests and verify RED**

```bash
cd backend-firmware
python -m pytest tools/tests/test_backend_lite_usb_fixture.py -q
```

Expected: import fails because `backend_lite_usb_fixture.py` is absent.

- [ ] **Step 3: Implement the fixture**

Use Python standard library only. Validate exact Lite identity before enabling mutation, treat screen capabilities as absent, produce an ACK only for the current LIVE_READY session and received heartbeat, and expose helpers for PING/STATUS/config/detection transcripts. Do not open serial ports or require hardware in unit tests.

```python
def build_live_start() -> str:
    return 'FOF_LIVE_START:{"client":"new_dash","protocol":1}'

def build_ack(session_id: str, sequence: int) -> str:
    body = json.dumps(
        {"session_id": session_id, "sequence": sequence},
        separators=(",", ":"),
    )
    return f"FOF_LIVE_ACK:{body}"
```

- [ ] **Step 4: Write the handoff document**

Document exact VID/PID discovery assumptions, line limits, identity tuple, capabilities, command/response frames, acknowledged-live timing diagram, recovery AP truth table, Wi-Fi/config schema and redaction, DET fields, required/optional ordering, reconnect behavior, unsupported screen controls, OTA family boundary, retry guidance, and acceptance transcript. State clearly that the FastAPI backend needs no ingestion change and New Dash must add live ACK/config client behavior.

- [ ] **Step 5: Run fixture tests and commit**

```bash
cd backend-firmware
python -m pytest tools/tests/test_backend_lite_usb_fixture.py -q
```

Expected: all fixture tests pass.

```bash
git add docs/backend-lite-new-dash-usb-protocol.md backend-firmware/tools/backend_lite_usb_fixture.py backend-firmware/tools/tests/test_backend_lite_usb_fixture.py
git commit -m "docs: specify Lite New Dash USB support"
```

---

### Task 7: Final firmware-only verification and release artifacts

**Files:**
- Modify only when a focused failing regression test proves a defect: files already listed in Tasks 1-6 plus their focused tests.
- Do not create or publish a backend release, APK, New Dash build, or hardware flash.

**Interfaces:**
- Consumes: all prior task commits.
- Produces: a clean Lite firmware build, protected-path evidence, size/hash metadata, and the handoff spec path.

- [ ] **Step 1: Run the complete focused native set once**

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_usb_protocol -f test_backend_usb_transport_core -f test_backend_usb_config -f test_backend_dashboard_event -f test_backend_event_ring -f test_backend_lite_ap_policy -f test_backend_wifi_manager -f test_backend_threat_policy -f test_backend_portal_routes
```

Expected: every selected Unity suite passes with no sanitizer output.

- [ ] **Step 2: Run contract and isolation checks once**

```bash
cd backend-firmware
python -m pytest tools/tests/test_backend_uplink_build_contract.py tools/tests/test_backend_build_contract.py tools/tests/test_source_isolation.py test/test_backend_portal_contract.py tools/tests/test_backend_lite_usb_fixture.py -q
python tools/check_source_isolation.py --root .
```

Expected: all selected checks pass.

- [ ] **Step 3: Build the Lite uplink once**

```bash
cd backend-firmware/uplink
pio run -e uplink-s3-backend
```

Expected: `firmware.bin` succeeds for `uplink-s3-backend`; scanner and native badge images are not built or altered.

- [ ] **Step 4: Prove protected paths and inspect artifacts**

```bash
git diff --check
git status --short
git diff --name-only $(git merge-base main HEAD)..HEAD
shasum -a 256 backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin
ls -lh backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin
```

Expected: changed paths are confined to approved design/plan/handoff docs and `backend-firmware/` uplink/shared/tests/tools; no changes appear under `backend/`, `android/`, `esp32/`, scanner runtime/source, flasher, or New Dash source.

- [ ] **Step 5: Run final code review and fix only covered defects**

Review the branch diff against the approved design, emphasizing lock ordering, exactly-once observer behavior, required-frame integrity, lease fail-open behavior, password redaction, PSRAM-only allocation, and Lite-only build selection. Each fix begins with a failing focused regression test and repeats only the affected command from Steps 1-3.

- [ ] **Step 6: Commit verification fixes only when needed**

Stage the exact reviewed firmware and focused-test files, then use:

```bash
git commit -m "fix: close Lite USB verification gaps"
```

When review is clean, skip this commit. Do not flash hardware.
