#include "uplink_usb_ota.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef UNIT_TESTING
#include "fw_store.h"
#include "uart_rx.h"

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"
#endif

#define UPLINK_USB_OTA_APP_TYPE 0x00U
#define UPLINK_USB_OTA_0_SUBTYPE 0x10U
#define UPLINK_USB_OTA_1_SUBTYPE 0x11U
#define UPLINK_USB_OTA_0_OFFSET 0x20000U
#define UPLINK_USB_OTA_1_OFFSET 0x220000U
#define UPLINK_USB_OTA_SLOT_SIZE 0x200000U
#define UPLINK_USB_OTA_DESCRIPTOR_BYTES 144U

typedef struct {
    atomic_uint state;
    atomic_uint received;
    atomic_uint total;
    _Atomic unsigned char partition[UPLINK_USB_OTA_PARTITION_LABEL_BYTES];
    _Atomic unsigned char target_version[33];
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
    _Atomic unsigned char target_sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
#endif
    _Atomic unsigned char last_error[UPLINK_USB_OTA_ERROR_BYTES];
} atomic_status_t;

typedef struct {
    uplink_usb_ota_state_t state;
    uplink_ota_policy_session_t policy;
    fw_operation_token_t operation_token;
    uplink_usb_ota_partition_t running_partition;
    uplink_usb_ota_partition_t target_partition;
    uintptr_t ota_handle;
    bool operation_owned;
    bool ota_handle_valid;
    bool integrity_active;
    bool http_pause_owned;
    bool scanner_pause_owned[2];
    bool cleanup_done;
    bool operation_release_pending;
    bool terminal_cleanup_receipt_emitted;
    uint8_t descriptor[UPLINK_USB_OTA_DESCRIPTOR_BYTES];
    size_t descriptor_received;
    uint32_t transport_received;
    uint32_t computed_crc32;
    fof_firmware_image_identity_t embedded_identity;
    size_t target_marker_match;
    size_t hardware_marker_match;
    bool target_marker_seen;
    bool hardware_marker_seen;
    char first_error[UPLINK_USB_OTA_ERROR_BYTES];
} uplink_usb_ota_session_t;

static atomic_uint s_status_sequence;
static atomic_status_t s_published_status;
static uplink_usb_ota_session_t s_session;
static uplink_usb_ota_hooks_t s_hooks;
static bool s_hooks_installed;
static atomic_flag s_mutator = ATOMIC_FLAG_INIT;

static bool precommit_cleanup(const char *error,
                              uplink_usb_ota_result_t *out);

const char *uplink_usb_ota_state_name(uplink_usb_ota_state_t state)
{
    switch (state) {
        case UPLINK_USB_OTA_IDLE:      return "idle";
        case UPLINK_USB_OTA_PREPARING: return "preparing";
        case UPLINK_USB_OTA_RECEIVING: return "receiving";
        case UPLINK_USB_OTA_VERIFYING: return "verifying";
        case UPLINK_USB_OTA_COMMITTED: return "committed";
        case UPLINK_USB_OTA_ERROR:
        default:                       return "error";
    }
}

static void atomic_text_store(_Atomic unsigned char *destination,
                              size_t capacity,
                              const char *source)
{
    size_t index = 0U;
    if (source) {
        while (index + 1U < capacity && source[index] != '\0') {
            atomic_store_explicit(&destination[index],
                                  (unsigned char)source[index],
                                  memory_order_relaxed);
            index++;
        }
    }
    while (index < capacity) {
        atomic_store_explicit(&destination[index], 0U, memory_order_relaxed);
        index++;
    }
}

static void atomic_text_load(const _Atomic unsigned char *source,
                             size_t capacity,
                             char *destination)
{
    for (size_t index = 0U; index < capacity; ++index) {
        destination[index] = (char)atomic_load_explicit(
            &source[index], memory_order_relaxed);
    }
    destination[capacity - 1U] = '\0';
}

#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
static void atomic_sha256_store(
    _Atomic unsigned char destination[FOF_FIRMWARE_SHA256_HEX_SIZE],
    const char source[FOF_FIRMWARE_SHA256_HEX_SIZE])
{
    char canonical[FOF_FIRMWARE_SHA256_HEX_SIZE] = {0};
    if (source && fof_firmware_sha256_hex_is_valid(source)) {
        for (size_t index = 0U;
             index < FOF_FIRMWARE_SHA256_HEX_LENGTH;
             ++index) {
            char byte = source[index];
            canonical[index] =
                byte >= 'A' && byte <= 'F' ? (char)(byte + ('a' - 'A'))
                                           : byte;
        }
    }
    atomic_text_store(
        destination, FOF_FIRMWARE_SHA256_HEX_SIZE, canonical);
}
#endif

static void publish_status(void)
{
    atomic_fetch_add_explicit(&s_status_sequence, 1U, memory_order_acq_rel);
    atomic_store_explicit(&s_published_status.state,
                          (unsigned)s_session.state, memory_order_relaxed);
    atomic_store_explicit(&s_published_status.received,
                          s_session.policy.durable_written,
                          memory_order_relaxed);
    atomic_store_explicit(&s_published_status.total,
                          s_session.policy.manifest.size,
                          memory_order_relaxed);
    atomic_text_store(s_published_status.partition,
                      UPLINK_USB_OTA_PARTITION_LABEL_BYTES,
                      s_session.target_partition.label);
    atomic_text_store(s_published_status.target_version,
                      sizeof(s_published_status.target_version),
                      s_session.policy.manifest.version);
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
    atomic_sha256_store(
        s_published_status.target_sha256,
        s_session.policy.manifest.sha256);
#endif
    atomic_text_store(s_published_status.last_error,
                      UPLINK_USB_OTA_ERROR_BYTES,
                      s_session.first_error);
    atomic_fetch_add_explicit(&s_status_sequence, 1U, memory_order_release);
}

static void result_clear(uplink_usb_ota_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

static void result_busy(uplink_usb_ota_result_t *out, const char *error)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->retryable = true;
    out->phase = UPLINK_USB_OTA_PHASE_NONE;
    out->emit_required = true;
    snprintf(out->error, sizeof(out->error), "%s",
             error ? error : "adapter_busy");
}

static bool copy_bounded_text(char *destination, size_t destination_capacity,
                              const char *source, size_t source_capacity)
{
    if (!destination || destination_capacity == 0U) {
        return false;
    }
    destination[0] = '\0';
    if (!source) {
        return false;
    }
    const char *terminator = memchr(source, '\0', source_capacity);
    if (!terminator) {
        return false;
    }
    size_t length = (size_t)(terminator - source);
    if (length >= destination_capacity) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static void result_error(uplink_usb_ota_result_t *out, const char *error,
                         bool emit_required)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->phase = UPLINK_USB_OTA_PHASE_ABORTED;
    out->emit_required = emit_required;
    snprintf(out->error, sizeof(out->error), "%s",
             error ? error : "invalid_argument");
    (void)copy_bounded_text(
        out->partition, sizeof(out->partition),
        s_session.target_partition.label,
        sizeof(s_session.target_partition.label));
    out->received = s_session.policy.durable_written;
    out->total = s_session.policy.manifest.size;
}

static void result_success(uplink_usb_ota_result_t *out,
                           uplink_usb_ota_phase_t phase,
                           uint32_t credit_bytes)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->ok = true;
    out->emit_required = phase != UPLINK_USB_OTA_PHASE_PROGRESS;
    out->reboot_required = phase == UPLINK_USB_OTA_PHASE_COMMITTED;
    out->phase = phase;
    (void)copy_bounded_text(
        out->partition, sizeof(out->partition),
        s_session.target_partition.label,
        sizeof(s_session.target_partition.label));
    out->received = s_session.policy.durable_written;
    out->total = s_session.policy.manifest.size;
    out->credit_bytes = credit_bytes;
}

static bool hooks_valid(const uplink_usb_ota_hooks_t *hooks)
{
    return hooks && hooks->get_running && hooks->get_next &&
           hooks->get_image_state && hooks->get_partition_identity &&
           hooks->ota_begin && hooks->ota_write && hooks->ota_end &&
           hooks->ota_abort && hooks->set_boot_partition &&
           hooks->operation_begin && hooks->operation_end &&
           hooks->pause_http && hooks->pause_scanner &&
           hooks->resume_http && hooks->resume_scanner &&
           hooks->integrity_start && hooks->integrity_update &&
           hooks->integrity_finish && hooks->integrity_abort;
}

static bool bounded_text(const char *text, size_t capacity)
{
    return text && memchr(text, '\0', capacity) != NULL;
}

static bool exact_slot(const uplink_usb_ota_partition_t *partition,
                       bool slot_zero)
{
    if (!partition ||
        !bounded_text(partition->label, sizeof(partition->label)) ||
        partition->native_id == 0U ||
        partition->type != UPLINK_USB_OTA_APP_TYPE ||
        partition->size != UPLINK_USB_OTA_SLOT_SIZE) {
        return false;
    }
    return slot_zero
        ? partition->subtype == UPLINK_USB_OTA_0_SUBTYPE &&
          partition->offset == UPLINK_USB_OTA_0_OFFSET &&
          strcmp(partition->label, "ota_0") == 0
        : partition->subtype == UPLINK_USB_OTA_1_SUBTYPE &&
          partition->offset == UPLINK_USB_OTA_1_OFFSET &&
          strcmp(partition->label, "ota_1") == 0;
}

static bool partition_pair_valid(
    const uplink_usb_ota_partition_t *running,
    const uplink_usb_ota_partition_t *target)
{
    bool running_zero = exact_slot(running, true);
    bool running_one = exact_slot(running, false);
    bool target_zero = exact_slot(target, true);
    bool target_one = exact_slot(target, false);
    return running->native_id != target->native_id &&
           ((running_zero && target_one) || (running_one && target_zero));
}

static bool partition_same(const uplink_usb_ota_partition_t *left,
                           const uplink_usb_ota_partition_t *right)
{
    return left && right && left->native_id == right->native_id &&
           left->type == right->type && left->subtype == right->subtype &&
           left->offset == right->offset && left->size == right->size &&
           bounded_text(left->label, sizeof(left->label)) &&
           bounded_text(right->label, sizeof(right->label)) &&
           strcmp(left->label, right->label) == 0;
}

static void feed_marker(const uint8_t *bytes, size_t length,
                        const char *marker, size_t marker_length,
                        size_t *matched, bool *seen)
{
    if (*seen) {
        return;
    }
    for (size_t i = 0U; i < length; ++i) {
        if (bytes[i] == (uint8_t)marker[*matched]) {
            (*matched)++;
            if (*matched == marker_length) {
                *seen = true;
                return;
            }
        } else {
            *matched = bytes[i] == (uint8_t)marker[0] ? 1U : 0U;
        }
    }
}

static bool durable_piece(const uint8_t *bytes, size_t length,
                          uint32_t transport_received,
                          uplink_usb_ota_result_t *out)
{
    if (!s_session.policy.credit_outstanding ||
        s_session.policy.next_credit_at <
            s_session.policy.durable_written ||
        length > (size_t)(s_session.policy.next_credit_at -
                          s_session.policy.durable_written)) {
        return precommit_cleanup("credit_overshoot", out);
    }
    if (!s_hooks.ota_write(s_hooks.context, s_session.ota_handle,
                           bytes, length)) {
        return precommit_cleanup("ota_write_failed", out);
    }
    if (!s_hooks.integrity_update(s_hooks.context, bytes, length,
                                  &s_session.computed_crc32)) {
        return precommit_cleanup("integrity_update_failed", out);
    }
    feed_marker(bytes, length, UPLINK_OTA_TARGET,
                sizeof(UPLINK_OTA_TARGET),
                &s_session.target_marker_match,
                &s_session.target_marker_seen);
    feed_marker(bytes, length, UPLINK_OTA_HARDWARE,
                sizeof(UPLINK_OTA_HARDWARE),
                &s_session.hardware_marker_match,
                &s_session.hardware_marker_seen);
    const char *policy_error = NULL;
    if (!uplink_ota_policy_note_durable_write(
            &s_session.policy, (uint32_t)length, transport_received,
            &policy_error)) {
        return precommit_cleanup(policy_error ? policy_error
                                              : "durable_write_failed", out);
    }
    return true;
}

static bool precommit_cleanup(const char *error,
                              uplink_usb_ota_result_t *out)
{
    if (s_session.state == UPLINK_USB_OTA_COMMITTED) {
        result_success(out, UPLINK_USB_OTA_PHASE_COMMITTED, 0U);
        return false;
    }
    if (s_session.cleanup_done) {
        result_error(out, s_session.first_error, false);
        return false;
    }
    bool first_cleanup_attempt = s_session.first_error[0] == '\0';
    if (first_cleanup_attempt) {
        snprintf(s_session.first_error, sizeof(s_session.first_error), "%s",
                 error ? error : "aborted");
        uplink_ota_policy_fail(&s_session.policy, error);
        s_session.state = UPLINK_USB_OTA_ERROR;
    }
    if (s_session.integrity_active) {
        s_hooks.integrity_abort(s_hooks.context);
        s_session.integrity_active = false;
    }
    if (s_session.ota_handle_valid) {
        (void)s_hooks.ota_abort(s_hooks.context, s_session.ota_handle);
        s_session.ota_handle_valid = false;
    }
    if (s_session.scanner_pause_owned[1]) {
        s_hooks.resume_scanner(s_hooks.context, 1U);
        s_session.scanner_pause_owned[1] = false;
    }
    if (s_session.scanner_pause_owned[0]) {
        s_hooks.resume_scanner(s_hooks.context, 0U);
        s_session.scanner_pause_owned[0] = false;
    }
    if (s_session.http_pause_owned) {
        s_hooks.resume_http(s_hooks.context);
        s_session.http_pause_owned = false;
    }
    bool release_recovered = s_session.operation_release_pending;
    if (s_session.operation_owned) {
        if (!s_hooks.operation_end(
                s_hooks.context, s_session.operation_token)) {
            s_session.operation_release_pending = true;
            publish_status();
            result_busy(out, "operation_release_failed");
            return false;
        }
        s_session.operation_owned = false;
        s_session.operation_release_pending = false;
    }
    s_session.cleanup_done = true;
    publish_status();
    bool emit_terminal = !s_session.terminal_cleanup_receipt_emitted &&
                         (first_cleanup_attempt || release_recovered);
    s_session.terminal_cleanup_receipt_emitted = true;
    result_error(out, s_session.first_error, emit_terminal);
    return false;
}

#ifndef UNIT_TESTING
typedef struct {
    uart_rx_pause_guard_t scanner_guards[2];
    mbedtls_sha256_context sha256;
    bool sha256_initialized;
} default_hook_context_t;

static default_hook_context_t s_default_context;

static void partition_from_native(const esp_partition_t *native,
                                  uplink_usb_ota_partition_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!native) {
        return;
    }
    out->native_id = (uintptr_t)native;
    snprintf(out->label, sizeof(out->label), "%s", native->label);
    out->type = native->type;
    out->subtype = native->subtype;
    out->offset = native->address;
    out->size = native->size;
}

static bool default_get_running(void *context,
                                uplink_usb_ota_partition_t *out)
{
    (void)context;
    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (!partition || !out) {
        return false;
    }
    partition_from_native(partition, out);
    return true;
}

static bool default_get_next(void *context,
                             const uplink_usb_ota_partition_t *running,
                             uplink_usb_ota_partition_t *out)
{
    (void)context;
    if (!running || !out || running->native_id == 0U) {
        return false;
    }
    const esp_partition_t *partition = esp_ota_get_next_update_partition(
        (const esp_partition_t *)running->native_id);
    if (!partition) {
        return false;
    }
    partition_from_native(partition, out);
    return true;
}

static bool default_get_image_state(
    void *context, const uplink_usb_ota_partition_t *partition,
    uplink_usb_ota_image_state_t *out)
{
    (void)context;
    if (!partition || !out || partition->native_id == 0U) {
        return false;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(
            (const esp_partition_t *)partition->native_id,
            &state) != ESP_OK) {
        return false;
    }
    *out = state == ESP_OTA_IMG_VALID
        ? UPLINK_USB_OTA_IMAGE_STATE_VALID
        : state == ESP_OTA_IMG_PENDING_VERIFY
            ? UPLINK_USB_OTA_IMAGE_STATE_PENDING_VERIFY
            : UPLINK_USB_OTA_IMAGE_STATE_OTHER;
    return true;
}

static bool default_get_partition_identity(
    void *context, const uplink_usb_ota_partition_t *partition,
    fof_firmware_image_identity_t *out)
{
    (void)context;
    if (!partition || !out || partition->native_id == 0U) {
        return false;
    }
    esp_app_desc_t descriptor = {0};
    if (esp_ota_get_partition_description(
            (const esp_partition_t *)partition->native_id,
            &descriptor) != ESP_OK) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->version, descriptor.version, sizeof(descriptor.version));
    memcpy(out->project, descriptor.project_name,
           sizeof(descriptor.project_name));
    out->version[sizeof(out->version) - 1U] = '\0';
    out->project[sizeof(out->project) - 1U] = '\0';
    return true;
}

static bool default_ota_begin(void *context,
                              const uplink_usb_ota_partition_t *partition,
                              uintptr_t *handle)
{
    (void)context;
    if (!partition || !handle || partition->native_id == 0U) {
        return false;
    }
    esp_ota_handle_t native_handle = 0;
    if (esp_ota_begin((const esp_partition_t *)partition->native_id,
                      OTA_WITH_SEQUENTIAL_WRITES,
                      &native_handle) != ESP_OK) {
        return false;
    }
    *handle = (uintptr_t)native_handle;
    return true;
}

static bool default_ota_write(void *context, uintptr_t handle,
                              const uint8_t *bytes, size_t length)
{
    (void)context;
    return esp_ota_write((esp_ota_handle_t)handle, bytes, length) == ESP_OK;
}

static bool default_ota_end(void *context, uintptr_t handle)
{
    (void)context;
    return esp_ota_end((esp_ota_handle_t)handle) == ESP_OK;
}

static bool default_ota_abort(void *context, uintptr_t handle)
{
    (void)context;
    return esp_ota_abort((esp_ota_handle_t)handle) == ESP_OK;
}

static bool default_set_boot(void *context,
                             const uplink_usb_ota_partition_t *partition)
{
    (void)context;
    return partition && partition->native_id != 0U &&
           esp_ota_set_boot_partition(
               (const esp_partition_t *)partition->native_id) == ESP_OK;
}

static bool default_operation_begin(void *context,
                                    fw_operation_token_t *token)
{
    (void)context;
    return fw_store_operation_try_begin(
        FW_OPERATION_OWNER_UPLINK_OTA, false, &*token);
}

static bool default_operation_end(void *context,
                                  fw_operation_token_t token)
{
    (void)context;
    return fw_store_operation_end(token);
}

static bool default_pause_http(void *context, bool *owned)
{
    (void)context;
    if (!owned) {
        return false;
    }
#ifdef FOF_BADGE_VARIANT
    *owned = false;
    return true;
#else
    *owned = false;
    return false;
#endif
}

static bool default_pause_scanner(void *context, uint8_t slot, bool *owned)
{
    default_hook_context_t *defaults = context;
    if (!defaults || !owned || slot > 1U) {
        return false;
    }
    *owned = false;
    if (!uart_rx_scanner_task_started((int)slot)) {
        return true;
    }
    if (uart_rx_scanner_is_paused((int)slot)) {
        return false;
    }
    bool paused = uart_rx_pause_scanner_guarded(
        (int)slot, &defaults->scanner_guards[slot]);
    *owned = defaults->scanner_guards[slot].acquired;
    return paused;
}

static void default_resume_http(void *context)
{
    (void)context;
}

static void default_resume_scanner(void *context, uint8_t slot)
{
    default_hook_context_t *defaults = context;
    if (defaults && slot <= 1U) {
        uart_rx_resume_scanner_guarded(
            (int)slot, &defaults->scanner_guards[slot]);
    }
}

static bool default_integrity_start(void *context)
{
    default_hook_context_t *defaults = context;
    if (!defaults || defaults->sha256_initialized) {
        return false;
    }
    mbedtls_sha256_init(&defaults->sha256);
    defaults->sha256_initialized = true;
    if (mbedtls_sha256_starts(&defaults->sha256, 0) != 0) {
        mbedtls_sha256_free(&defaults->sha256);
        defaults->sha256_initialized = false;
        return false;
    }
    return true;
}

static bool default_integrity_update(void *context, const uint8_t *bytes,
                                     size_t length, uint32_t *crc32)
{
    default_hook_context_t *defaults = context;
    if (!defaults || !defaults->sha256_initialized || !bytes || !crc32 ||
        mbedtls_sha256_update(&defaults->sha256, bytes, length) != 0) {
        return false;
    }
    *crc32 = esp_rom_crc32_le(*crc32, bytes, length);
    return true;
}

static bool default_integrity_finish(
    void *context, uint8_t digest[FOF_FIRMWARE_SHA256_SIZE])
{
    default_hook_context_t *defaults = context;
    if (!defaults || !defaults->sha256_initialized || !digest) {
        return false;
    }
    bool ok = mbedtls_sha256_finish(&defaults->sha256, digest) == 0;
    mbedtls_sha256_free(&defaults->sha256);
    defaults->sha256_initialized = false;
    return ok;
}

static void default_integrity_abort(void *context)
{
    default_hook_context_t *defaults = context;
    if (defaults && defaults->sha256_initialized) {
        mbedtls_sha256_free(&defaults->sha256);
        defaults->sha256_initialized = false;
    }
}

static void ensure_default_hooks(void)
{
    if (s_hooks_installed) {
        return;
    }
    memset(&s_default_context, 0, sizeof(s_default_context));
    s_hooks = (uplink_usb_ota_hooks_t) {
        .context = &s_default_context,
        .get_running = default_get_running,
        .get_next = default_get_next,
        .get_image_state = default_get_image_state,
        .get_partition_identity = default_get_partition_identity,
        .ota_begin = default_ota_begin,
        .ota_write = default_ota_write,
        .ota_end = default_ota_end,
        .ota_abort = default_ota_abort,
        .set_boot_partition = default_set_boot,
        .operation_begin = default_operation_begin,
        .operation_end = default_operation_end,
        .pause_http = default_pause_http,
        .pause_scanner = default_pause_scanner,
        .resume_http = default_resume_http,
        .resume_scanner = default_resume_scanner,
        .integrity_start = default_integrity_start,
        .integrity_update = default_integrity_update,
        .integrity_finish = default_integrity_finish,
        .integrity_abort = default_integrity_abort,
    };
    s_hooks_installed = true;
}
#endif

static bool uplink_usb_ota_begin_locked(
    const uplink_ota_manifest_t *manifest, uplink_usb_ota_result_t *out)
{
    result_clear(out);
#ifndef UNIT_TESTING
    ensure_default_hooks();
#endif
    if (s_session.state == UPLINK_USB_OTA_COMMITTED) {
        result_success(out, UPLINK_USB_OTA_PHASE_COMMITTED, 0U);
        return false;
    }
    if (s_session.operation_owned &&
        s_session.state == UPLINK_USB_OTA_ERROR) {
        result_busy(out, "operation_release_failed");
        return false;
    }
    if (!manifest || !out || !s_hooks_installed || !hooks_valid(&s_hooks)) {
        result_error(out, "invalid_state", true);
        return false;
    }
    if (s_session.state != UPLINK_USB_OTA_IDLE &&
        s_session.state != UPLINK_USB_OTA_ERROR) {
        result_busy(out, "invalid_state");
        return false;
    }
    memset(&s_session, 0, sizeof(s_session));
    uplink_ota_policy_init(&s_session.policy);
    s_session.state = UPLINK_USB_OTA_PREPARING;
    publish_status();

    if (!s_hooks.operation_begin(s_hooks.context,
                                 &s_session.operation_token)) {
        s_session.state = UPLINK_USB_OTA_IDLE;
        publish_status();
        result_busy(out, "operation_active");
        return false;
    }
    s_session.operation_owned = true;

    if (!s_hooks.get_running(s_hooks.context,
                             &s_session.running_partition)) {
        return precommit_cleanup("running_partition_unavailable", out);
    }
    uplink_usb_ota_image_state_t image_state;
    if (!s_hooks.get_image_state(s_hooks.context,
                                 &s_session.running_partition,
                                 &image_state)) {
        return precommit_cleanup("running_state_unavailable", out);
    }
    if (image_state == UPLINK_USB_OTA_IMAGE_STATE_PENDING_VERIFY) {
        return precommit_cleanup("pending_verify", out);
    }
    if (image_state != UPLINK_USB_OTA_IMAGE_STATE_VALID) {
        return precommit_cleanup("running_state_invalid", out);
    }
    fof_firmware_image_identity_t running_identity = {0};
    if (!s_hooks.get_partition_identity(
            s_hooks.context, &s_session.running_partition,
            &running_identity)) {
        return precommit_cleanup("running_identity_unavailable", out);
    }
    if (!s_hooks.get_next(s_hooks.context, &s_session.running_partition,
                          &s_session.target_partition) ||
        !partition_pair_valid(&s_session.running_partition,
                              &s_session.target_partition)) {
        return precommit_cleanup("invalid_target_partition", out);
    }
    const char *policy_error = NULL;
    if (!uplink_ota_policy_begin(
            &s_session.policy, manifest, running_identity.version,
            s_session.target_partition.size, false, &policy_error)) {
        return precommit_cleanup(policy_error ? policy_error
                                              : "invalid_manifest", out);
    }
    bool owned = false;
    bool paused = s_hooks.pause_http(s_hooks.context, &owned);
    s_session.http_pause_owned = owned;
    if (!paused) {
        return precommit_cleanup("http_pause_failed", out);
    }
    owned = false;
    paused = s_hooks.pause_scanner(s_hooks.context, 0U, &owned);
    s_session.scanner_pause_owned[0] = owned;
    if (!paused) {
        return precommit_cleanup("scanner_0_pause_failed", out);
    }
    owned = false;
    paused = s_hooks.pause_scanner(s_hooks.context, 1U, &owned);
    s_session.scanner_pause_owned[1] = owned;
    if (!paused) {
        return precommit_cleanup("scanner_1_pause_failed", out);
    }
    if (!s_hooks.ota_begin(s_hooks.context, &s_session.target_partition,
                           &s_session.ota_handle)) {
        return precommit_cleanup("ota_begin_failed", out);
    }
    s_session.ota_handle_valid = true;
    if (!s_hooks.integrity_start(s_hooks.context)) {
        return precommit_cleanup("sha256_start_failed", out);
    }
    s_session.integrity_active = true;
    uint32_t credit_bytes = 0U;
    uint32_t durable_received = 0U;
    if (!uplink_ota_policy_grant_credit(
            &s_session.policy, &credit_bytes, &durable_received,
            &policy_error)) {
        return precommit_cleanup(policy_error ? policy_error
                                              : "credit_failed", out);
    }
    s_session.state = UPLINK_USB_OTA_RECEIVING;
    publish_status();
    result_success(out, UPLINK_USB_OTA_PHASE_READY, credit_bytes);
    return true;
}

static bool uplink_usb_ota_write_locked(
    const uint8_t *bytes, size_t length,
    uint32_t cumulative_transport_received,
    uplink_usb_ota_result_t *out)
{
    result_clear(out);
    if (s_session.state == UPLINK_USB_OTA_COMMITTED) {
        result_success(out, UPLINK_USB_OTA_PHASE_COMMITTED, 0U);
        return false;
    }
    if (s_session.state != UPLINK_USB_OTA_RECEIVING) {
        result_error(out, "not_active", true);
        return false;
    }
    if (!out || !bytes) {
        return precommit_cleanup("invalid_argument", out);
    }
    if (length == 0U) {
        return precommit_cleanup("zero_write", out);
    }
    if (length > UPLINK_USB_OTA_MAX_WRITE_BYTES) {
        return precommit_cleanup("write_too_large", out);
    }
    if (length > UINT32_MAX - s_session.transport_received ||
        cumulative_transport_received !=
            s_session.transport_received + (uint32_t)length) {
        return precommit_cleanup("transport_mismatch", out);
    }
    if (cumulative_transport_received > s_session.policy.manifest.size) {
        return precommit_cleanup("image_overshoot", out);
    }

    size_t offset = 0U;
    if (s_session.descriptor_received < UPLINK_USB_OTA_DESCRIPTOR_BYTES) {
        size_t needed = UPLINK_USB_OTA_DESCRIPTOR_BYTES -
                        s_session.descriptor_received;
        size_t take = length < needed ? length : needed;
        memcpy(s_session.descriptor + s_session.descriptor_received,
               bytes, take);
        s_session.descriptor_received += take;
        s_session.transport_received += (uint32_t)take;
        offset += take;
        if (s_session.descriptor_received == UPLINK_USB_OTA_DESCRIPTOR_BYTES) {
            if (!fof_firmware_image_parse_identity(
                    s_session.descriptor, sizeof(s_session.descriptor),
                    &s_session.embedded_identity)) {
                return precommit_cleanup("identity_missing", out);
            }
            if (strcmp(s_session.embedded_identity.project,
                       s_session.policy.manifest.project) != 0) {
                return precommit_cleanup("project_mismatch", out);
            }
            if (strcmp(s_session.embedded_identity.version,
                       s_session.policy.manifest.version) != 0) {
                return precommit_cleanup("version_mismatch", out);
            }
            if (!durable_piece(s_session.descriptor,
                               sizeof(s_session.descriptor),
                               s_session.transport_received, out)) {
                return false;
            }
        }
    }
    if (offset < length) {
        size_t remaining = length - offset;
        s_session.transport_received += (uint32_t)remaining;
        if (!durable_piece(bytes + offset, remaining,
                           s_session.transport_received, out)) {
            return false;
        }
    }

    s_session.state = s_session.policy.state == UPLINK_OTA_VERIFYING
        ? UPLINK_USB_OTA_VERIFYING : UPLINK_USB_OTA_RECEIVING;
    publish_status();
    uint32_t credit = 0U;
    if (s_session.state == UPLINK_USB_OTA_RECEIVING &&
        !s_session.policy.credit_outstanding) {
        uint32_t durable = 0U;
        const char *policy_error = NULL;
        if (!uplink_ota_policy_grant_credit(
                &s_session.policy, &credit, &durable, &policy_error)) {
            return precommit_cleanup(policy_error ? policy_error
                                                  : "credit_failed", out);
        }
    }
    result_success(out, credit > 0U ? UPLINK_USB_OTA_PHASE_CREDIT
                                    : UPLINK_USB_OTA_PHASE_PROGRESS,
                   credit);
    return true;
}

static bool uplink_usb_ota_finish_locked(
    uint32_t cumulative_transport_received,
    uplink_usb_ota_result_t *out)
{
    result_clear(out);
    if (s_session.state == UPLINK_USB_OTA_COMMITTED) {
        result_success(out, UPLINK_USB_OTA_PHASE_COMMITTED, 0U);
        return false;
    }
    if (s_session.state != UPLINK_USB_OTA_VERIFYING) {
        result_error(out, "not_active", true);
        return false;
    }
    if (!out) {
        return precommit_cleanup("invalid_argument", NULL);
    }
    if (cumulative_transport_received != s_session.transport_received ||
        cumulative_transport_received != s_session.policy.manifest.size) {
        return precommit_cleanup("transport_mismatch", out);
    }
    uint8_t digest[FOF_FIRMWARE_SHA256_SIZE] = {0};
    if (!s_hooks.integrity_finish(s_hooks.context, digest)) {
        return precommit_cleanup("sha256_finish_failed", out);
    }
    s_session.integrity_active = false;
    char digest_hex[FOF_FIRMWARE_SHA256_HEX_SIZE] = {0};
    fof_firmware_sha256_to_hex(digest, digest_hex);
    const char *policy_error = NULL;
    if (!uplink_ota_policy_verify_complete(
            &s_session.policy, cumulative_transport_received,
            s_session.computed_crc32, digest_hex,
            &s_session.embedded_identity,
            s_session.target_marker_seen,
            s_session.hardware_marker_seen, &policy_error)) {
        return precommit_cleanup(policy_error ? policy_error
                                              : "verification_failed", out);
    }
    uintptr_t handle = s_session.ota_handle;
    s_session.ota_handle_valid = false;
    if (!s_hooks.ota_end(s_hooks.context, handle)) {
        return precommit_cleanup("ota_end_failed", out);
    }

    fof_firmware_image_identity_t target_identity = {0};
    if (!s_hooks.get_partition_identity(
            s_hooks.context, &s_session.target_partition,
            &target_identity)) {
        return precommit_cleanup("target_identity_unavailable", out);
    }
    if (!bounded_text(target_identity.project,
                      sizeof(target_identity.project)) ||
        !bounded_text(target_identity.version,
                      sizeof(target_identity.version))) {
        return precommit_cleanup("target_identity_unterminated", out);
    }
    if (
        strcmp(target_identity.project,
               s_session.policy.manifest.project) != 0 ||
        strcmp(target_identity.version,
               s_session.policy.manifest.version) != 0) {
        return precommit_cleanup("target_identity_mismatch", out);
    }

    uplink_usb_ota_partition_t live_running = {0};
    uplink_usb_ota_partition_t live_target = {0};
    if (!s_hooks.get_running(s_hooks.context, &live_running) ||
        !partition_same(&live_running, &s_session.running_partition) ||
        !s_hooks.get_next(s_hooks.context, &live_running, &live_target) ||
        !partition_pair_valid(&live_running, &live_target) ||
        !partition_same(&live_target, &s_session.target_partition)) {
        return precommit_cleanup("partition_changed", out);
    }
    if (!s_hooks.set_boot_partition(s_hooks.context, &live_target)) {
        return precommit_cleanup("set_boot_failed", out);
    }

    s_session.state = UPLINK_USB_OTA_COMMITTED;
    if (!uplink_ota_policy_mark_committed(&s_session.policy,
                                          &policy_error)) {
        /* Boot selection is the irreversible boundary: keep all ownership. */
        snprintf(s_session.first_error, sizeof(s_session.first_error), "%s",
                 policy_error ? policy_error : "commit_latch_failed");
        publish_status();
        result_success(out, UPLINK_USB_OTA_PHASE_COMMITTED, 0U);
        return true;
    }
    publish_status();
    result_success(out, UPLINK_USB_OTA_PHASE_COMMITTED, 0U);
    return true;
}

static bool uplink_usb_ota_abort_locked(const char *reason,
                                        uplink_usb_ota_result_t *out)
{
    return precommit_cleanup(reason ? reason : "aborted", out);
}

static bool mutator_try_enter(uplink_usb_ota_result_t *out)
{
    if (atomic_flag_test_and_set_explicit(
            &s_mutator, memory_order_acquire)) {
        result_busy(out, "adapter_busy");
        return false;
    }
    return true;
}

static void mutator_leave(void)
{
    atomic_flag_clear_explicit(&s_mutator, memory_order_release);
}

bool uplink_usb_ota_begin(const uplink_ota_manifest_t *manifest,
                          uplink_usb_ota_result_t *out)
{
    if (!mutator_try_enter(out)) {
        return false;
    }
    bool ok = uplink_usb_ota_begin_locked(manifest, out);
    mutator_leave();
    return ok;
}

bool uplink_usb_ota_write(const uint8_t *bytes, size_t length,
                          uint32_t cumulative_transport_received,
                          uplink_usb_ota_result_t *out)
{
    if (!mutator_try_enter(out)) {
        return false;
    }
    bool ok = uplink_usb_ota_write_locked(
        bytes, length, cumulative_transport_received, out);
    mutator_leave();
    return ok;
}

bool uplink_usb_ota_finish(uint32_t cumulative_transport_received,
                           uplink_usb_ota_result_t *out)
{
    if (!mutator_try_enter(out)) {
        return false;
    }
    bool ok = uplink_usb_ota_finish_locked(
        cumulative_transport_received, out);
    mutator_leave();
    return ok;
}

bool uplink_usb_ota_abort(const char *reason,
                          uplink_usb_ota_result_t *out)
{
    if (!mutator_try_enter(out)) {
        return false;
    }
    bool ok = uplink_usb_ota_abort_locked(reason, out);
    mutator_leave();
    return ok;
}

uint32_t uplink_usb_ota_remaining(void)
{
    uplink_usb_ota_status_t status = {0};
    if (!uplink_usb_ota_get_status(&status)) {
        return UPLINK_USB_OTA_REMAINING_UNKNOWN;
    }
    if (status.received >= status.total) {
        return 0U;
    }
    return status.total - status.received;
}

bool uplink_usb_ota_get_status(uplink_usb_ota_status_t *out)
{
    if (!out) {
        return false;
    }
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        unsigned before = atomic_load_explicit(
            &s_status_sequence, memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        uplink_usb_ota_status_t snapshot = {0};
        snapshot.state = (uplink_usb_ota_state_t)atomic_load_explicit(
            &s_published_status.state, memory_order_relaxed);
        snapshot.received = atomic_load_explicit(
            &s_published_status.received, memory_order_relaxed);
        snapshot.total = atomic_load_explicit(
            &s_published_status.total, memory_order_relaxed);
        atomic_text_load(s_published_status.partition,
                         sizeof(snapshot.partition), snapshot.partition);
        atomic_text_load(s_published_status.target_version,
                         sizeof(snapshot.target_version),
                         snapshot.target_version);
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
        atomic_text_load(s_published_status.target_sha256,
                         sizeof(snapshot.target_sha256),
                         snapshot.target_sha256);
#endif
        atomic_text_load(s_published_status.last_error,
                         sizeof(snapshot.last_error), snapshot.last_error);
        unsigned after = atomic_load_explicit(
            &s_status_sequence, memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            *out = snapshot;
            return true;
        }
    }
    memset(out, 0, sizeof(*out));
    out->state = UPLINK_USB_OTA_ERROR;
    snprintf(out->last_error, sizeof(out->last_error), "%s", "status_busy");
    return false;
}

#ifdef UNIT_TESTING
bool uplink_usb_ota_test_install_hooks(const uplink_usb_ota_hooks_t *hooks)
{
    if (!hooks_valid(hooks) ||
        (s_session.state != UPLINK_USB_OTA_IDLE &&
         s_session.state != UPLINK_USB_OTA_ERROR)) {
        return false;
    }
    s_hooks = *hooks;
    s_hooks_installed = true;
    return true;
}

void uplink_usb_ota_test_reset(void)
{
    atomic_flag_clear_explicit(&s_mutator, memory_order_release);
    memset(&s_session, 0, sizeof(s_session));
    memset(&s_hooks, 0, sizeof(s_hooks));
    s_hooks_installed = false;
    uplink_ota_policy_init(&s_session.policy);
    s_session.state = UPLINK_USB_OTA_IDLE;
    publish_status();
}


void uplink_usb_ota_test_set_mutator_busy(bool busy)
{
    if (busy) {
        (void)atomic_flag_test_and_set_explicit(
            &s_mutator, memory_order_acquire);
    } else {
        atomic_flag_clear_explicit(&s_mutator, memory_order_release);
    }
}

void uplink_usb_ota_test_set_status_writer_busy(bool busy)
{
    atomic_store_explicit(&s_status_sequence, busy ? 1U : 2U,
                          memory_order_release);
}
#endif
