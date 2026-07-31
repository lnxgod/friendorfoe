#include "badge_usb_transport.h"
#include "badge_usb_transport_policy.h"
#include "badge_usb_uplink_ota.h"
#include "badge_task_stack_budget.h"
#include "uplink_usb_ota.h"

#include "serial_config.h"
#include "fw_store.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_runtime.h"
#endif
#include "badge_usb_recovery.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#if defined(FOF_BADGE_VARIANT) && defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp_private/periph_ctrl.h"
#include "hal/usb_serial_jtag_ll.h"
#endif
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define USB_RX_BUFFER_BYTES 8192
#define USB_TX_RING_BYTES 2048
#define USB_LINE_BUFFER_BYTES 2048
#define USB_READ_BUFFER_BYTES 512
#define USB_BOOT_WINDOW_DONE BIT0

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
static const char *TAG = "badge_usb";
#endif

static SemaphoreHandle_t s_transaction_lock;
static EventGroupHandle_t s_boot_events;
static badge_usb_stream_t s_stream;
static badge_usb_output_policy_t s_output_policy;
static badge_usb_upload_policy_t s_upload_policy;
static badge_usb_uplink_ota_flow_t s_uplink_flow;
static bool s_uplink_finish_pending;
static bool s_uplink_abort_pending;
static bool s_uplink_terminal_pending;
static bool s_uplink_committed_restart_pending;
static bool s_uplink_recovery_after_cleanup;
static bool s_uplink_suppress_terminal;
static char s_uplink_abort_reason[UPLINK_USB_OTA_ERROR_BYTES];
static char s_uplink_terminal_frame[BADGE_USB_UPLINK_OTA_FRAME_BYTES];
static size_t s_uplink_terminal_length;
static char *s_line_buffer;
static badge_usb_health_t s_health = {
    .parser_target = BADGE_USB_BINARY_NONE,
    .task_heartbeat_ms = -1,
    .last_rx_ms = -1,
    .last_command_ms = -1,
    .last_response_ms = -1,
    .oldest_hard_unanswered_response_ms = -1,
    .oldest_enqueued_response_ms = -1,
    .last_upload_progress_ms = -1,
};
static portMUX_TYPE s_health_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_dispatch_ready;
static atomic_bool s_recovery_only;
static bool s_transport_started;
static uint32_t s_boot_window_ms;
static int64_t s_started_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

#if defined(FOF_BADGE_VARIANT) && defined(CONFIG_IDF_TARGET_ESP32S3)
static void app_reenumerate_enable_bus_clock(void *context)
{
    (void)context;
    PERIPH_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
}

static void app_reenumerate_set_pad_enabled(void *context, bool enabled)
{
    (void)context;
    usb_serial_jtag_ll_phy_enable_pad(enabled);
}

static void app_reenumerate_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    esp_rom_delay_us(delay_ms * 1000U);
}

static void app_reenumerate_select_internal_phy(void *context)
{
    (void)context;
    usb_serial_jtag_ll_phy_enable_external(false);
}

static bool app_reenumerate_usb_serial_jtag(void)
{
    const badge_usb_app_reenumerate_hooks_t hooks = {
        .context = NULL,
        .enable_bus_clock = app_reenumerate_enable_bus_clock,
        .set_pad_enabled = app_reenumerate_set_pad_enabled,
        .delay_ms = app_reenumerate_delay_ms,
        .select_internal_phy = app_reenumerate_select_internal_phy,
    };
    return badge_usb_app_reenumerate(&hooks);
}
#endif

static void release_start_allocations(void)
{
    if (s_line_buffer) {
        heap_caps_free(s_line_buffer);
        s_line_buffer = NULL;
    }
    if (s_boot_events) {
        vEventGroupDelete(s_boot_events);
        s_boot_events = NULL;
    }
    if (s_transaction_lock) {
        vSemaphoreDelete(s_transaction_lock);
        s_transaction_lock = NULL;
    }
}

static void health_note_drop(badge_usb_frame_priority_t priority)
{
    portENTER_CRITICAL(&s_health_lock);
    if (priority == BADGE_USB_FRAME_PROGRESS) {
        s_health.dropped_progress_frames++;
    } else if (priority == BADGE_USB_FRAME_OPTIONAL) {
        s_health.dropped_optional_frames++;
    }
    portEXIT_CRITICAL(&s_health_lock);
}

static void health_clear_enqueued_responses(void)
{
    portENTER_CRITICAL(&s_health_lock);
    if (s_health.enqueued_required_responses > 0U) {
        uint32_t rescued = s_health.enqueued_required_responses;
        if (UINT32_MAX - s_health.responses_completed < rescued) {
            s_health.responses_completed = UINT32_MAX;
        } else {
            s_health.responses_completed += rescued;
        }
        s_health.last_response_ms = now_ms();
    }
    s_health.enqueued_required_responses = 0U;
    s_health.oldest_enqueued_response_ms = -1;
    portEXIT_CRITICAL(&s_health_lock);
}

static void health_note_required_failure(badge_usb_emit_result_t result)
{
    int64_t failed_ms = now_ms();
    portENTER_CRITICAL(&s_health_lock);
    s_health.required_response_failures++;
    if (result == BADGE_USB_EMIT_ENQUEUED) {
        if (s_health.enqueued_required_responses < UINT32_MAX) {
            s_health.enqueued_required_responses++;
        }
        if (s_health.oldest_enqueued_response_ms < 0 ||
            failed_ms < s_health.oldest_enqueued_response_ms) {
            s_health.oldest_enqueued_response_ms = failed_ms;
        }
    } else {
        if (s_health.hard_unanswered_required_responses < UINT32_MAX) {
            s_health.hard_unanswered_required_responses++;
        }
        if (s_health.oldest_hard_unanswered_response_ms < 0 ||
            failed_ms < s_health.oldest_hard_unanswered_response_ms) {
            s_health.oldest_hard_unanswered_response_ms = failed_ms;
        }
    }
    portEXIT_CRITICAL(&s_health_lock);
}

static void health_apply_emit_result(
    badge_usb_emit_result_t result,
    badge_usb_frame_priority_t priority,
    badge_usb_emit_health_mode_t health_mode)
{
    badge_usb_emit_health_effect_t effect =
        badge_usb_emit_health_effect_decide(result, priority, health_mode);
    if (effect == BADGE_USB_EMIT_HEALTH_EFFECT_COMPLETED) {
        /* A successful FIFO drain also proves any earlier complete frame that
         * was fully enqueued has now left the driver. Hard/partial failures
         * remain latched because no later drain can repair their response. */
        health_clear_enqueued_responses();
        if (priority == BADGE_USB_FRAME_REQUIRED) {
            portENTER_CRITICAL(&s_health_lock);
            s_health.responses_completed++;
            s_health.last_response_ms = now_ms();
            portEXIT_CRITICAL(&s_health_lock);
        }
    } else if (effect ==
                   BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_ENQUEUED ||
               effect ==
                   BADGE_USB_EMIT_HEALTH_EFFECT_REQUIRED_HARD_FAILURE) {
        health_note_required_failure(result);
    } else if (effect == BADGE_USB_EMIT_HEALTH_EFFECT_PROGRESS_DROP) {
        health_note_drop(BADGE_USB_FRAME_PROGRESS);
    } else if (effect == BADGE_USB_EMIT_HEALTH_EFFECT_OPTIONAL_DROP) {
        health_note_drop(BADGE_USB_FRAME_OPTIONAL);
    }
}

static bool output_host_connected(void *context)
{
    (void)context;
    return usb_serial_jtag_is_connected();
}

static bool output_lock(void *context, uint32_t timeout_ticks)
{
    (void)context;
    return xSemaphoreTakeRecursive(s_transaction_lock,
                                   (TickType_t)timeout_ticks) == pdTRUE;
}

static void output_unlock(void *context)
{
    (void)context;
    (void)xSemaphoreGiveRecursive(s_transaction_lock);
}

static uintptr_t output_current_owner(void *context)
{
    (void)context;
    return (uintptr_t)xTaskGetCurrentTaskHandle();
}

static uint32_t output_now_ticks(void *context)
{
    (void)context;
    return (uint32_t)xTaskGetTickCount();
}

static int output_write(void *context, const uint8_t *data, size_t len,
                        uint32_t timeout_ticks)
{
    (void)context;
    return usb_serial_jtag_write_bytes(data, len, (TickType_t)timeout_ticks);
}

static bool output_drain(void *context, uint32_t timeout_ticks)
{
    (void)context;
    return usb_serial_jtag_wait_tx_done((TickType_t)timeout_ticks) == ESP_OK;
}

static badge_usb_emit_result_t badge_usb_transport_emit_detailed(
    const void *data, size_t len, badge_usb_frame_priority_t priority,
    TickType_t timeout, badge_usb_emit_health_mode_t health_mode)
{
    badge_usb_output_hooks_t hooks = {
        .context = NULL,
        .host_connected = output_host_connected,
        .lock = output_lock,
        .unlock = output_unlock,
        .current_owner = output_current_owner,
        .now_ticks = output_now_ticks,
        .write = output_write,
        .drain = output_drain,
    };
    TickType_t started = xTaskGetTickCount();
    if (!s_transaction_lock ||
        xSemaphoreTakeRecursive(s_transaction_lock, timeout) != pdTRUE) {
        health_apply_emit_result(
            BADGE_USB_EMIT_FAILED, priority, health_mode);
        return BADGE_USB_EMIT_FAILED;
    }
    TickType_t elapsed = xTaskGetTickCount() - started;
    TickType_t remaining = elapsed >= timeout ? 0 : timeout - elapsed;

    /* Hold one outer recursion of the transaction lock until health state is
     * updated. badge_usb_output_emit() takes/releases an inner recursion.
     * This gives FIFO drain results and their enqueued-response accounting
     * one total order across tasks. */
    badge_usb_emit_result_t result = badge_usb_output_emit(
        &s_output_policy, &hooks, data, len, priority,
        (uint32_t)remaining);
    health_apply_emit_result(result, priority, health_mode);
    (void)xSemaphoreGiveRecursive(s_transaction_lock);
    return result;
}

bool badge_usb_transport_emit(const void *data, size_t len,
                              badge_usb_frame_priority_t priority,
                              TickType_t timeout)
{
    badge_usb_emit_result_t result = badge_usb_transport_emit_detailed(
        data, len, priority, timeout, BADGE_USB_EMIT_HEALTH_TRACKED);
    return result == BADGE_USB_EMIT_COMPLETED ||
           (result == BADGE_USB_EMIT_ENQUEUED &&
            priority != BADGE_USB_FRAME_REQUIRED);
}

static bool badge_usb_transport_emit_health_neutral(
    const void *data, size_t len, badge_usb_frame_priority_t priority,
    TickType_t timeout)
{
    badge_usb_emit_result_t result = badge_usb_transport_emit_detailed(
        data, len, priority, timeout, BADGE_USB_EMIT_HEALTH_NEUTRAL);
    return result == BADGE_USB_EMIT_COMPLETED ||
           (result == BADGE_USB_EMIT_ENQUEUED &&
            priority != BADGE_USB_FRAME_REQUIRED);
}

bool badge_usb_transport_drain(TickType_t timeout)
{
    TickType_t started = xTaskGetTickCount();
    if (!s_transaction_lock ||
        xSemaphoreTakeRecursive(s_transaction_lock, timeout) != pdTRUE) {
        return false;
    }
    TickType_t elapsed = xTaskGetTickCount() - started;
    TickType_t remaining = elapsed >= timeout ? 0 : timeout - elapsed;
    bool drained = usb_serial_jtag_wait_tx_done(remaining) == ESP_OK;
    if (drained) {
        health_clear_enqueued_responses();
    }
    (void)xSemaphoreGiveRecursive(s_transaction_lock);
    return drained;
}

static int transport_log_vprintf(const char *format, va_list args)
{
    char line[384];
    int formatted = vsnprintf(line, sizeof(line) - 2, format, args);
    if (formatted < 0) {
        return formatted;
    }
    size_t len = (size_t)formatted;
    if (len >= sizeof(line) - 2) {
        len = sizeof(line) - 3;
    }
    if (len == 0 || line[len - 1] != '\n') {
        line[len++] = '\n';
    }
    line[len] = '\0';
    (void)badge_usb_transport_emit(line, len, BADGE_USB_FRAME_OPTIONAL, 0);
    return formatted;
}

static void abort_binary_target(badge_usb_binary_target_t target,
                                const char *reason);

static bool normal_line_is_recognized(
    void *context, const uint8_t *line, size_t line_byte_len)
{
    (void)context;
    return serial_config_line_is_recognized(line, line_byte_len);
}

static bool recovery_line_is_allowed(
    void *context, const uint8_t *line, size_t line_byte_len)
{
    (void)context;
    return serial_config_recovery_command_classify(line, line_byte_len) !=
           SERIAL_CONFIG_RECOVERY_DENIED;
}

static void note_recognized_line(void *context)
{
    (void)context;
    int64_t command_ms = now_ms();
    portENTER_CRITICAL(&s_health_lock);
    s_health.valid_commands++;
    s_health.last_rx_ms = command_ms;
    s_health.last_command_ms = command_ms;
    portEXIT_CRITICAL(&s_health_lock);
}

static bool emit_booting_rejection(void *context)
{
    (void)context;
    static const char frame[] = "FOF_ERROR:booting\n";
    return badge_usb_transport_emit_health_neutral(
        frame, sizeof(frame) - 1U, BADGE_USB_FRAME_REQUIRED,
        pdMS_TO_TICKS(250));
}

static bool emit_recovery_only_rejection(void *context)
{
    (void)context;
    static const char frame[] =
        "FOF_ERROR:{\"ok\":false,\"error\":\"startup_recovery_only\"}\n";
    return badge_usb_transport_emit_health_neutral(
        frame, sizeof(frame) - 1U, BADGE_USB_FRAME_REQUIRED,
        pdMS_TO_TICKS(250));
}

static bool emit_unknown_rejection(void *context)
{
    (void)context;
    static const char frame[] = "FOF_ERROR:unknown command\n";
    return badge_usb_transport_emit_health_neutral(
        frame, sizeof(frame) - 1U, BADGE_USB_FRAME_REQUIRED,
        pdMS_TO_TICKS(250));
}

static bool dispatch_normal_line(
    void *context, const uint8_t *line, size_t line_byte_len)
{
    (void)context;
    bool response_completed =
        serial_config_dispatch_line(line, line_byte_len);
    if (!response_completed && s_stream.target != BADGE_USB_BINARY_NONE) {
        badge_usb_binary_target_t failed_target = s_stream.target;
        badge_usb_stream_result_t aborted;
        badge_usb_stream_abort(&s_stream, "required_response_failed", &aborted);
        abort_binary_target(failed_target, "required_response_failed");
    }
    return response_completed;
}

static bool dispatch_recovery_line(
    void *context, const uint8_t *line, size_t line_byte_len)
{
    (void)context;
    return serial_config_dispatch_recovery_command(line, line_byte_len);
}

static bool dispatch_line(const uint8_t *line, size_t line_byte_len)
{
    static const badge_usb_line_dispatch_hooks_t hooks = {
        .context = NULL,
        .normal_line_is_recognized = normal_line_is_recognized,
        .recovery_line_is_allowed = recovery_line_is_allowed,
        .note_recognized = note_recognized_line,
        .emit_booting = emit_booting_rejection,
        .emit_recovery_only = emit_recovery_only_rejection,
        .emit_unknown = emit_unknown_rejection,
        .dispatch_normal_line = dispatch_normal_line,
        .dispatch_recovery_line = dispatch_recovery_line,
    };
    return badge_usb_line_dispatch_run(
        line, line_byte_len,
        atomic_load_explicit(&s_dispatch_ready, memory_order_acquire),
        atomic_load_explicit(&s_recovery_only, memory_order_acquire),
        &hooks);
}

static badge_usb_emit_result_t emit_upload_terminal_detailed(
    badge_usb_binary_target_t target, const char *json)
{
    if (target != BADGE_USB_BINARY_SCANNER) {
        return BADGE_USB_EMIT_FAILED;
    }
    char frame[768];
    int len = snprintf(frame, sizeof(frame), "FOF_FW_UPLOAD:%s\n",
                       json ? json : "{\"ok\":false,\"error\":\"internal\"}");
    if (len > 0 && (size_t)len < sizeof(frame)) {
        return badge_usb_transport_emit_detailed(
            frame, (size_t)len, BADGE_USB_FRAME_REQUIRED,
            pdMS_TO_TICKS(1000), BADGE_USB_EMIT_HEALTH_TRACKED);
    }
    return BADGE_USB_EMIT_FAILED;
}

static bool emit_upload_terminal(badge_usb_binary_target_t target,
                                 const char *json)
{
    return emit_upload_terminal_detailed(target, json) ==
           BADGE_USB_EMIT_COMPLETED;
}

static bool emit_uplink_ota_result(const uplink_usb_ota_result_t *result,
                                   TickType_t timeout)
{
    char frame[BADGE_USB_UPLINK_OTA_FRAME_BYTES];
    size_t length = badge_usb_uplink_ota_render_result(
        result, frame, sizeof(frame));
    return length > 0U && badge_usb_transport_emit(
        frame, length, BADGE_USB_FRAME_REQUIRED, timeout);
}

static badge_usb_uplink_action_t abort_uplink_ota(const char *reason);

static badge_usb_uplink_receipt_decision_t deliver_uplink_receipt(
    const uplink_usb_ota_result_t *result, const char *failure_reason)
{
    char frame[BADGE_USB_UPLINK_OTA_FRAME_BYTES];
    size_t length = badge_usb_uplink_ota_render_result(
        result, frame, sizeof(frame));
    badge_usb_emit_result_t emitted = BADGE_USB_EMIT_FAILED;
    if (length > 0U) {
        emitted = badge_usb_transport_emit_detailed(
            frame, length, BADGE_USB_FRAME_REQUIRED,
            pdMS_TO_TICKS(1000), BADGE_USB_EMIT_HEALTH_TRACKED);
    }

    bool rescued_drain = false;
    if (emitted == BADGE_USB_EMIT_ENQUEUED) {
        rescued_drain = badge_usb_transport_drain(pdMS_TO_TICKS(1000));
    }
    badge_usb_uplink_receipt_decision_t decision =
        badge_usb_uplink_ota_receipt_decide(emitted, rescued_drain);
    if (decision == BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED) {
        badge_usb_uplink_action_t action =
            badge_usb_uplink_ota_flow_receipt_result(&s_uplink_flow, true);
        decision = badge_usb_uplink_ota_receipt_finalize(
            decision, action == BADGE_USB_UPLINK_ACTION_CONTINUE);
        if (decision == BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED) {
            return decision;
        }
    } else {
        (void)badge_usb_uplink_ota_flow_receipt_result(
            &s_uplink_flow, false);
    }

    if (decision == BADGE_USB_UPLINK_RECEIPT_ABORT_TERMINAL) {
        (void)abort_uplink_ota(failure_reason);
        return decision;
    }

    s_uplink_suppress_terminal = true;
    s_uplink_recovery_after_cleanup = true;
    (void)abort_uplink_ota(failure_reason);
    return BADGE_USB_UPLINK_RECEIPT_CLEANUP_RECOVERY;
}

static badge_usb_binary_target_t clear_upload_health(void)
{
    badge_usb_binary_target_t target = s_upload_policy.target;
    portENTER_CRITICAL(&s_health_lock);
    s_health.upload_received = 0;
    s_health.upload_size = 0;
    s_health.parser_target = BADGE_USB_BINARY_NONE;
    portEXIT_CRITICAL(&s_health_lock);
    return target;
}

static bool consume_binary_event(const badge_usb_stream_result_t *result)
{
    if (!result || (result->event != BADGE_USB_EVENT_BINARY_CHUNK &&
                    result->event != BADGE_USB_EVENT_BINARY_COMPLETE)) {
        return false;
    }
    if (result->target == BADGE_USB_BINARY_UPLINK) {
        return false;
    }

    char response[640] = {0};
    if (!fw_store_serial_upload_write(result->bytes, result->bytes_len,
                                      response, sizeof(response))) {
        fw_store_serial_upload_abort("usb_write_failed");
        clear_upload_health();
        (void)emit_upload_terminal(result->target, response);
        (void)badge_usb_upload_abort(&s_upload_policy);
        return false;
    }
    if (!badge_usb_upload_note_bytes(&s_upload_policy, result->bytes_len)) {
        fw_store_serial_upload_abort("usb_accounting_failed");
        clear_upload_health();
        (void)emit_upload_terminal(
            result->target,
            "{\"ok\":false,\"error\":\"usb_accounting_failed\"}");
        (void)badge_usb_upload_abort(&s_upload_policy);
        return false;
    }
    portENTER_CRITICAL(&s_health_lock);
    s_health.upload_received += (uint32_t)result->bytes_len;
    s_health.last_upload_progress_ms = now_ms();
    portEXIT_CRITICAL(&s_health_lock);

    if (result->event == BADGE_USB_EVENT_BINARY_COMPLETE) {
        bool durable_finalized = fw_store_serial_upload_end(
            response, sizeof(response));
        if (!durable_finalized) {
            fw_store_serial_upload_abort("usb_finalize_failed");
        } else {
            badge_usb_upload_note_durable_finalize(&s_upload_policy);
        }
        clear_upload_health();
        bool terminal_delivered = emit_upload_terminal(result->target, response);
        bool activation_allowed = badge_usb_upload_terminal_result(
            &s_upload_policy, terminal_delivered);
        if (durable_finalized) {
            (void)fw_store_serial_upload_complete_terminal(activation_allowed);
        }
        return durable_finalized && terminal_delivered && activation_allowed;
    }
    return true;
}

static void abort_binary_target(badge_usb_binary_target_t target,
                                const char *reason)
{
    if (target == BADGE_USB_BINARY_SCANNER) {
        fw_store_serial_upload_abort(reason);
    }
    clear_upload_health();
    badge_usb_binary_target_t tracked_target = badge_usb_upload_abort(
        &s_upload_policy);
    if (target == BADGE_USB_BINARY_NONE) {
        target = tracked_target;
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
             reason ? reason : "aborted");
    (void)emit_upload_terminal(target, json);
}

typedef struct scanner_credit_context {
    const badge_usb_stream_result_t *stream_result;
    uint32_t sample_ms;
} scanner_credit_context_t;

static bool scanner_credit_write_durable(
    void *context, const uint8_t *data, size_t length,
    char *out_receipt, size_t out_receipt_len)
{
    (void)context;
    return fw_store_serial_upload_write(
        data, length, out_receipt, out_receipt_len);
}

static bool scanner_credit_commit_transport(void *context)
{
    scanner_credit_context_t *credit = context;
    if (!credit || !credit->stream_result ||
        !badge_usb_stream_commit_binary(
            &s_stream, credit->stream_result, credit->sample_ms)) {
        return false;
    }
    portENTER_CRITICAL(&s_health_lock);
    s_health.upload_received = s_stream.received;
    s_health.last_upload_progress_ms = now_ms();
    portEXIT_CRITICAL(&s_health_lock);
    return true;
}

static bool scanner_credit_finalize_durable(
    void *context, char *out_receipt, size_t out_receipt_len)
{
    (void)context;
    badge_usb_stream_clear_binary(&s_stream);
    bool finalized = fw_store_serial_upload_end(
        out_receipt, out_receipt_len);
    clear_upload_health();
    if (!finalized) {
        fw_store_serial_upload_abort("usb_finalize_failed");
    }
    return finalized;
}

static badge_usb_emit_result_t scanner_credit_emit_required(
    void *context, const char *receipt)
{
    (void)context;
    return emit_upload_terminal_detailed(
        BADGE_USB_BINARY_SCANNER, receipt);
}

static bool scanner_credit_drain_required(void *context)
{
    (void)context;
    return badge_usb_transport_drain(pdMS_TO_TICKS(1000));
}

static bool scanner_credit_complete_terminal(
    void *context, bool delivered)
{
    (void)context;
    return fw_store_serial_upload_complete_terminal(delivered);
}

static bool consume_scanner_credit_bytes(
    const uint8_t *bytes, size_t length, uint32_t sample_ms)
{
    size_t allowed = 0U;
    if (!bytes || length == 0U ||
        !badge_usb_upload_plan_credit_bytes(
            &s_upload_policy, length, &allowed) ||
        allowed != length) {
        badge_usb_stream_result_t aborted;
        (void)badge_usb_stream_abort(
            &s_stream, "scanner_credit_overrun", &aborted);
        abort_binary_target(
            BADGE_USB_BINARY_SCANNER, "scanner_credit_overrun");
        return false;
    }

    badge_usb_stream_result_t stream_result;
    badge_usb_stream_event_t event = badge_usb_stream_peek_binary(
        &s_stream, bytes, length, allowed, &stream_result);
    if (event != BADGE_USB_EVENT_BINARY_CHUNK &&
        event != BADGE_USB_EVENT_BINARY_COMPLETE) {
        badge_usb_stream_result_t aborted;
        (void)badge_usb_stream_abort(
            &s_stream, "scanner_binary_peek_failed", &aborted);
        abort_binary_target(
            BADGE_USB_BINARY_SCANNER, "scanner_binary_peek_failed");
        return false;
    }

    scanner_credit_context_t context = {
        .stream_result = &stream_result,
        .sample_ms = sample_ms,
    };
    badge_usb_scanner_credit_hooks_t hooks = {
        .context = &context,
        .write_durable = scanner_credit_write_durable,
        .commit_transport = scanner_credit_commit_transport,
        .finalize_durable = scanner_credit_finalize_durable,
        .emit_required = scanner_credit_emit_required,
        .drain_required = scanner_credit_drain_required,
        .complete_terminal = scanner_credit_complete_terminal,
    };
    char response[640] = {0};
    badge_usb_scanner_credit_result_t result =
        badge_usb_scanner_credit_process(
            &s_upload_policy, &hooks,
            stream_result.bytes, stream_result.bytes_len,
            event == BADGE_USB_EVENT_BINARY_COMPLETE,
            response, sizeof(response));

    if (event == BADGE_USB_EVENT_BINARY_COMPLETE) {
        clear_upload_health();
    }
    if (result == BADGE_USB_SCANNER_CREDIT_CONTINUE ||
        result == BADGE_USB_SCANNER_CREDIT_COMPLETE) {
        return true;
    }

    if (result == BADGE_USB_SCANNER_CREDIT_FINALIZE_FAILED ||
        result == BADGE_USB_SCANNER_CREDIT_TERMINAL_FAILED ||
        result == BADGE_USB_SCANNER_CREDIT_ACTIVATION_FAILED) {
        badge_usb_stream_clear_binary(&s_stream);
        clear_upload_health();
        return false;
    }

    const char *reason = badge_usb_scanner_credit_result_error(result);
    badge_usb_stream_result_t aborted;
    (void)badge_usb_stream_abort(&s_stream, reason, &aborted);
    abort_binary_target(BADGE_USB_BINARY_SCANNER, reason);
    return false;
}

static void clear_uplink_parser_and_health(void)
{
    badge_usb_stream_clear_binary(&s_stream);
    portENTER_CRITICAL(&s_health_lock);
    s_health.upload_received = 0U;
    s_health.upload_size = 0U;
    s_health.parser_target = BADGE_USB_BINARY_NONE;
    portEXIT_CRITICAL(&s_health_lock);
}

static void recover_uplink_usb_once(const char *reason);

static void latch_uplink_abort(const char *reason)
{
    if (!s_uplink_abort_pending) {
        snprintf(s_uplink_abort_reason, sizeof(s_uplink_abort_reason), "%s",
                 reason && reason[0] ? reason : "aborted");
        s_uplink_abort_pending = true;
    }
}

static badge_usb_uplink_action_t deliver_uplink_terminal(void)
{
    if (!s_uplink_terminal_pending || s_uplink_terminal_length == 0U) {
        return BADGE_USB_UPLINK_ACTION_CONTINUE;
    }
    badge_usb_emit_result_t emitted = badge_usb_transport_emit_detailed(
        s_uplink_terminal_frame, s_uplink_terminal_length,
        BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(1000),
        BADGE_USB_EMIT_HEALTH_TRACKED);
    badge_usb_uplink_action_t action =
        badge_usb_uplink_ota_flow_terminal_emit_result(
            &s_uplink_flow, emitted);
    if (action != BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL) {
        s_uplink_terminal_pending = false;
        s_uplink_terminal_length = 0U;
    }
    if (action == BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART) {
        recover_uplink_usb_once(emitted == BADGE_USB_EMIT_POISONED
            ? "uplink_terminal_poisoned"
            : "uplink_terminal_retry_exhausted");
    }
    return action;
}

static void recover_uplink_usb_once(const char *reason)
{
    ESP_LOGE("badge_usb", "Uplink USB OTA recovery: %s",
             reason ? reason : "retry_exhausted");
    if (!badge_usb_recovery_restart(
            BADGE_USB_RESET_APP, "usb_safe_once")) {
        ESP_LOGE(
            "badge_usb",
            "Uplink USB OTA recovery restart blocked without owner");
    }
}

static badge_usb_uplink_action_t complete_uplink_abort(
    const uplink_usb_ota_result_t *result)
{
    (void)badge_usb_uplink_ota_flow_abort(&s_uplink_flow);
    if (badge_usb_uplink_ota_flow_take_cleanup(&s_uplink_flow)) {
        clear_uplink_parser_and_health();
    }
    badge_usb_uplink_action_t terminal_action =
        BADGE_USB_UPLINK_ACTION_ABORT_DROP;
    if (s_uplink_suppress_terminal) {
        (void)badge_usb_uplink_ota_flow_take_terminal(&s_uplink_flow);
        s_uplink_terminal_pending = false;
        s_uplink_terminal_length = 0U;
    }
    if (!s_uplink_suppress_terminal &&
        s_uplink_flow.terminal_available && result &&
        !s_uplink_terminal_pending) {
        s_uplink_terminal_length = badge_usb_uplink_ota_render_result(
            result, s_uplink_terminal_frame,
            sizeof(s_uplink_terminal_frame));
        if (s_uplink_terminal_length > 0U) {
            s_uplink_terminal_pending = true;
            terminal_action = deliver_uplink_terminal();
        } else {
            terminal_action = BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART;
        }
    }
    s_uplink_abort_pending = false;
    if (s_uplink_recovery_after_cleanup ||
        terminal_action == BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART) {
        s_uplink_recovery_after_cleanup = false;
        recover_uplink_usb_once("uplink_cleanup_retry_exhausted");
    }
    s_uplink_suppress_terminal = false;
    return terminal_action == BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL
        ? terminal_action : BADGE_USB_UPLINK_ACTION_ABORT_DROP;
}

static badge_usb_uplink_action_t abort_uplink_ota(const char *reason)
{
    latch_uplink_abort(reason);
    uplink_usb_ota_result_t result = {0};
    (void)uplink_usb_ota_abort(s_uplink_abort_reason, &result);
    if (result.retryable && result.phase == UPLINK_USB_OTA_PHASE_NONE) {
        badge_usb_uplink_action_t action =
            badge_usb_uplink_ota_flow_note_retry(
                &s_uplink_flow, BADGE_USB_UPLINK_RETRY_CLEANUP);
        if (action == BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART) {
            uplink_usb_ota_result_t fatal = result;
            fatal.retryable = false;
            fatal.emit_required = true;
            fatal.phase = UPLINK_USB_OTA_PHASE_ABORTED;
            snprintf(fatal.error, sizeof(fatal.error), "%s",
                     "cleanup_retry_exhausted");
            s_uplink_recovery_after_cleanup = true;
            return complete_uplink_abort(&fatal);
        }
        return action;
    }
    badge_usb_uplink_ota_flow_clear_retry(
        &s_uplink_flow, BADGE_USB_UPLINK_RETRY_CLEANUP);
    return complete_uplink_abort(&result);
}

typedef struct {
    const uplink_usb_ota_result_t *result;
} restart_context_t;

static bool restart_emit_committed(void *context)
{
    restart_context_t *restart = context;
    return restart && emit_uplink_ota_result(
        restart->result, pdMS_TO_TICKS(1000));
}

static bool restart_drain(void *context)
{
    (void)context;
    return badge_usb_transport_drain(pdMS_TO_TICKS(1000));
}

static bool restart_app(void *context)
{
    (void)context;
#ifdef FOF_BADGE_VARIANT
    bool accepted = badge_usb_recovery_restart(
        BADGE_USB_RESET_APP, "usb_uplink_ota");
    if (!accepted) {
        ESP_LOGE(
            "badge_usb",
            "Committed uplink OTA restart blocked without owner");
    }
    return accepted;
#else
    esp_restart();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return true;
#endif
}

static badge_usb_uplink_action_t finish_uplink_ota(void)
{
    uplink_usb_ota_result_t result = {0};
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    uplink_usb_ota_status_t update_summary = {0};
    if (!uplink_usb_ota_get_status(&update_summary) ||
        update_summary.state != UPLINK_USB_OTA_VERIFYING ||
        update_summary.total == 0U ||
        update_summary.received != update_summary.total ||
        !badge_runtime_update_commit_uplink(
            update_summary.target_version,
            update_summary.target_sha256,
            update_summary.total,
            update_summary.partition)) {
        return abort_uplink_ota("maintenance_summary_failed");
    }
    bool update_summary_prepared = true;
#endif
    bool accepted = uplink_usb_ota_finish(
        s_uplink_flow.transport_received, &result);
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if ((!accepted ||
         result.phase != UPLINK_USB_OTA_PHASE_COMMITTED) &&
        update_summary_prepared &&
        !badge_runtime_update_clear_uplink_commit()) {
        ESP_LOGE(TAG,
                 "Update-maintenance summary rollback failed after OTA finish");
        s_uplink_recovery_after_cleanup = true;
        return abort_uplink_ota("maintenance_summary_rollback_failed");
    }
#endif
    if (!accepted && result.retryable &&
        result.phase == UPLINK_USB_OTA_PHASE_NONE &&
        strcmp(result.error, "adapter_busy") == 0) {
        s_uplink_finish_pending = true;
        badge_usb_uplink_action_t retry =
            badge_usb_uplink_ota_flow_note_retry(
                &s_uplink_flow, BADGE_USB_UPLINK_RETRY_FINISH);
        if (retry == BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART) {
            s_uplink_finish_pending = false;
            s_uplink_recovery_after_cleanup = true;
            return abort_uplink_ota("finish_retry_exhausted");
        }
        return retry;
    }
    s_uplink_finish_pending = false;
    badge_usb_uplink_ota_flow_clear_retry(
        &s_uplink_flow, BADGE_USB_UPLINK_RETRY_FINISH);
    badge_usb_uplink_action_t action =
        badge_usb_uplink_ota_flow_finish_result(
            &s_uplink_flow, accepted, &result);
    if (action != BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART) {
        if (result.retryable) {
            return abort_uplink_ota("finish_cleanup_failed");
        }
        return complete_uplink_abort(&result);
    }

    if (badge_usb_uplink_ota_flow_take_cleanup(&s_uplink_flow)) {
        clear_uplink_parser_and_health();
    }
    if (badge_usb_uplink_ota_flow_take_terminal(&s_uplink_flow)) {
        restart_context_t restart = {.result = &result};
        badge_usb_uplink_ota_commit_hooks_t hooks = {
            .context = &restart,
            .emit_committed = restart_emit_committed,
            .drain = restart_drain,
            .restart = restart_app,
        };
        if (!badge_usb_uplink_ota_run_committed(&hooks)) {
            ESP_LOGE(
                "badge_usb",
                "Committed uplink OTA could not acquire restart ownership");
            /*
             * Cleanup and the terminal receipt have already been consumed,
             * so retain a dedicated executor latch until an owned restart
             * succeeds. The selected OTA partition remains durable meanwhile.
             */
            s_uplink_committed_restart_pending = true;
            return BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART;
        }
    }
    return BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART;
}

static badge_usb_uplink_action_t consume_uplink_bytes(
    const uint8_t *bytes, size_t length,
    uint8_t pending_bytes[UPLINK_USB_OTA_MAX_WRITE_BYTES],
    size_t *pending_length, uint32_t sample_ms)
{
    if (!bytes || !pending_bytes || !pending_length || length == 0U) {
        return abort_uplink_ota("invalid_binary_input");
    }
    bool retrying = *pending_length != 0U;
    size_t allowed = 0U;
    if (retrying) {
        if (bytes != pending_bytes || length != *pending_length) {
            return abort_uplink_ota("pending_chunk_mismatch");
        }
        allowed = length;
    } else {
        badge_usb_uplink_action_t plan =
            badge_usb_uplink_ota_flow_plan_read(
                &s_uplink_flow, length, &allowed);
        if (plan == BADGE_USB_UPLINK_ACTION_ABORT_DROP) {
            return abort_uplink_ota("credit_or_image_overrun");
        }
        if (plan != BADGE_USB_UPLINK_ACTION_CONTINUE || allowed != length ||
            allowed > UPLINK_USB_OTA_MAX_WRITE_BYTES) {
            return abort_uplink_ota("invalid_read_plan");
        }
    }

    badge_usb_stream_result_t stream_result;
    badge_usb_stream_event_t stream_event = badge_usb_stream_peek_binary(
        &s_stream, bytes, length, allowed, &stream_result);
    if (stream_event != BADGE_USB_EVENT_BINARY_CHUNK &&
        stream_event != BADGE_USB_EVENT_BINARY_COMPLETE) {
        return abort_uplink_ota("binary_peek_failed");
    }

    uplink_usb_ota_result_t result = {0};
    bool accepted = uplink_usb_ota_write(
        stream_result.bytes, stream_result.bytes_len,
        s_uplink_flow.transport_received +
            (uint32_t)stream_result.bytes_len,
        &result);
    badge_usb_uplink_action_t action =
        badge_usb_uplink_ota_flow_write_result(
            &s_uplink_flow, stream_result.bytes_len, accepted, &result);
    if (action == BADGE_USB_UPLINK_ACTION_RETRY_PENDING) {
        if (!retrying) {
            memcpy(pending_bytes, stream_result.bytes,
                   stream_result.bytes_len);
            *pending_length = stream_result.bytes_len;
        }
        return action;
    }
    if (action == BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART) {
        *pending_length = 0U;
        s_uplink_recovery_after_cleanup = true;
        return abort_uplink_ota("write_retry_exhausted");
    }
    if (action == BADGE_USB_UPLINK_ACTION_ABORT_DROP) {
        *pending_length = 0U;
        if (accepted) {
            return abort_uplink_ota("adapter_result_mismatch");
        }
        if (result.retryable) {
            return abort_uplink_ota("write_cleanup_failed");
        }
        return complete_uplink_abort(&result);
    }

    if (!badge_usb_stream_commit_binary(
            &s_stream, &stream_result, sample_ms)) {
        *pending_length = 0U;
        return abort_uplink_ota("binary_commit_failed");
    }
    *pending_length = 0U;
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    badge_runtime_update_keepalive(sample_ms);
#endif
    portENTER_CRITICAL(&s_health_lock);
    s_health.upload_received = s_stream.received;
    s_health.last_upload_progress_ms = now_ms();
    portEXIT_CRITICAL(&s_health_lock);

    if (action == BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT) {
        badge_usb_uplink_receipt_decision_t receipt =
            deliver_uplink_receipt(&result, "credit_receipt_failed");
        if (receipt != BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED) {
            return BADGE_USB_UPLINK_ACTION_ABORT_DROP;
        }
    } else if (action == BADGE_USB_UPLINK_ACTION_FINISH) {
        if (!badge_usb_uplink_ota_flow_take_finish(&s_uplink_flow)) {
            return abort_uplink_ota("finish_latch_missing");
        }
        return finish_uplink_ota();
    }
    return action;
}

static void uplink_retry_backoff(uint8_t attempt)
{
    uint32_t bounded = attempt == 0U ? 1U : attempt;
    if (bounded > BADGE_USB_UPLINK_OTA_RETRY_LIMIT) {
        bounded = BADGE_USB_UPLINK_OTA_RETRY_LIMIT;
    }
    vTaskDelay(pdMS_TO_TICKS(5U * bounded));
}

static void badge_usb_transport_task(void *arg)
{
    (void)arg;
    uint8_t input[USB_READ_BUFFER_BYTES];
    uint8_t pending_bytes[UPLINK_USB_OTA_MAX_WRITE_BYTES];
    size_t pending_length = 0U;
#ifdef FOF_BADGE_VARIANT
    int64_t last_runtime_heartbeat_ms = -1;
#endif

    portENTER_CRITICAL(&s_health_lock);
    s_health.task_started = true;
    portEXIT_CRITICAL(&s_health_lock);

    for (;;) {
        int64_t tick_ms = now_ms();
        bool host_connected = usb_serial_jtag_is_connected();
        portENTER_CRITICAL(&s_health_lock);
        s_health.task_heartbeat_ms = tick_ms;
        s_health.host_connected = host_connected;
        s_health.parser_target = s_stream.target;
        portEXIT_CRITICAL(&s_health_lock);
#ifdef FOF_BADGE_VARIANT
        if (last_runtime_heartbeat_ms < 0 ||
            tick_ms - last_runtime_heartbeat_ms >= 1000) {
            badge_runtime_note_usb_control_alive();
            badge_runtime_note_usb_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
            last_runtime_heartbeat_ms = tick_ms;
        }
#if defined(FOF_DC34_GAME_CANARY)
        serial_config_poll_update_preparation((uint32_t)tick_ms);
#endif
#endif

        if (tick_ms - s_started_ms >= (int64_t)s_boot_window_ms) {
            xEventGroupSetBits(s_boot_events, USB_BOOT_WINDOW_DONE);
        }

        if (s_uplink_committed_restart_pending) {
            /*
             * A competing PREPARING reboot owner may cancel back to IDLE.
             * Retry instead of dropping the committed handoff; an OWNED
             * competitor will reset the badge before this loop runs again.
             */
            (void)restart_app(NULL);
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        if (s_uplink_abort_pending) {
            badge_usb_uplink_action_t abort_action = abort_uplink_ota(NULL);
            if (abort_action == BADGE_USB_UPLINK_ACTION_RETRY_PENDING ||
                abort_action == BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL) {
                uplink_retry_backoff(s_uplink_flow.cleanup_retry_count);
            }
            continue;
        }
        if (s_uplink_finish_pending) {
            badge_usb_uplink_action_t finish_action = finish_uplink_ota();
            if (finish_action == BADGE_USB_UPLINK_ACTION_RETRY_PENDING) {
                uplink_retry_backoff(s_uplink_flow.finish_retry_count);
            }
            continue;
        }
        if (s_uplink_terminal_pending) {
            badge_usb_uplink_action_t terminal_action =
                deliver_uplink_terminal();
            if (terminal_action == BADGE_USB_UPLINK_ACTION_RETRY_TERMINAL) {
                uplink_retry_backoff(s_uplink_flow.terminal_retry_count);
            }
            continue;
        }
        if (pending_length != 0U) {
            badge_usb_uplink_action_t retry_action = consume_uplink_bytes(
                pending_bytes, pending_length, pending_bytes,
                &pending_length, (uint32_t)now_ms());
            if (retry_action == BADGE_USB_UPLINK_ACTION_RETRY_PENDING) {
                uplink_retry_backoff(s_uplink_flow.write_retry_count);
            }
            continue;
        }

        ssize_t bytes_read = read(STDIN_FILENO, input, sizeof(input));
        if (bytes_read > 0) {
            portENTER_CRITICAL(&s_health_lock);
            s_health.rx_bytes += (uint64_t)bytes_read;
            portEXIT_CRITICAL(&s_health_lock);

            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                if (s_stream.target == BADGE_USB_BINARY_UPLINK) {
                    badge_usb_uplink_action_t action = consume_uplink_bytes(
                        input + offset, (size_t)bytes_read - offset,
                        pending_bytes, &pending_length,
                        (uint32_t)now_ms());
                    /* A complete OS read belongs to the binary transaction.
                     * Any over-credit/final remainder is dropped, never fed
                     * back into line parsing. Retry bytes are retained above. */
                    offset = (size_t)bytes_read;
                    if (action == BADGE_USB_UPLINK_ACTION_ABORT_DROP ||
                        action == BADGE_USB_UPLINK_ACTION_RETRY_PENDING ||
                        action == BADGE_USB_UPLINK_ACTION_RECOVERY_RESTART ||
                        action == BADGE_USB_UPLINK_ACTION_COMMITTED_RESTART) {
                        break;
                    }
                    continue;
                }
                if (s_stream.target == BADGE_USB_BINARY_SCANNER &&
                    badge_usb_upload_credit_enabled(&s_upload_policy)) {
                    bool accepted = consume_scanner_credit_bytes(
                        input + offset, (size_t)bytes_read - offset,
                        (uint32_t)now_ms());
                    /* A credited OS read is one binary transaction. Any
                     * over-credit or post-image remainder is dropped rather
                     * than reinterpreted as a command. */
                    offset = (size_t)bytes_read;
                    if (!accepted) {
                        break;
                    }
                    continue;
                }
                badge_usb_stream_result_t result;
                uint32_t feed_now_ms = (uint32_t)now_ms();
                badge_usb_stream_event_t event = badge_usb_stream_feed(
                    &s_stream, input + offset, (size_t)bytes_read - offset,
                    feed_now_ms, &result);
                if (result.input_consumed == 0) {
                    break;
                }
                offset += result.input_consumed;
                if (event == BADGE_USB_EVENT_LINE) {
                    (void)dispatch_line(
                        (const uint8_t *)result.line,
                        result.line_byte_len);
                    if (s_uplink_abort_pending ||
                        s_uplink_terminal_pending) {
                        offset = (size_t)bytes_read;
                        break;
                    }
                } else if (event == BADGE_USB_EVENT_BINARY_CHUNK ||
                           event == BADGE_USB_EVENT_BINARY_COMPLETE) {
                    if (!consume_binary_event(&result)) {
                        badge_usb_stream_result_t aborted;
                        badge_usb_stream_abort(&s_stream,
                                               "binary_backend_failed", &aborted);
                    }
                } else if (event == BADGE_USB_EVENT_ERROR) {
                    portENTER_CRITICAL(&s_health_lock);
                    s_health.malformed_lines++;
                    portEXIT_CRITICAL(&s_health_lock);
                    (void)badge_usb_transport_emit_health_neutral(
                        "FOF_ERROR:line too long\n", 24,
                        BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(250));
                }
            }
        } else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        uint32_t timeout_now_ms = (uint32_t)now_ms();
        if (s_stream.target == BADGE_USB_BINARY_UPLINK) {
            if (badge_usb_stream_binary_timed_out(
                    &s_stream, timeout_now_ms,
                    BADGE_USB_BINARY_IDLE_TIMEOUT_MS)) {
                /* abort_uplink_ota cleans the adapter before it releases the
                 * parser target via badge_usb_stream_clear_binary(). */
                (void)abort_uplink_ota("usb_idle_timeout");
            }
        } else {
            /* Preserve the legacy scanner upload path unchanged. */
            badge_usb_binary_target_t timeout_target = s_stream.target;
            if (badge_usb_stream_poll_timeout(
                    &s_stream, timeout_now_ms,
                    BADGE_USB_BINARY_IDLE_TIMEOUT_MS) ==
                    BADGE_USB_EVENT_ERROR) {
                abort_binary_target(timeout_target, "usb_idle_timeout");
            }
        }
        if (bytes_read <= 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

bool badge_usb_transport_start(uint32_t boot_window_ms)
{
    if (s_transport_started) {
        return true;
    }
#if defined(FOF_BADGE_VARIANT) && defined(CONFIG_IDF_TARGET_ESP32S3)
    if (usb_serial_jtag_is_driver_installed()) {
        return false;
    }
    if (!app_reenumerate_usb_serial_jtag()) {
        return false;
    }
#endif
    s_transaction_lock = xSemaphoreCreateRecursiveMutex();
    s_boot_events = xEventGroupCreate();
    s_line_buffer = heap_caps_malloc(USB_LINE_BUFFER_BYTES,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_transaction_lock || !s_boot_events || !s_line_buffer) {
        release_start_allocations();
        return false;
    }

    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = 8192,
        .tx_buffer_size = 2048,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (!usb_serial_jtag_is_driver_installed()) {
            release_start_allocations();
            return false;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0) {
        release_start_allocations();
        return false;
    }

    badge_usb_stream_init(&s_stream, s_line_buffer, USB_LINE_BUFFER_BYTES);
    atomic_init(&s_output_policy.poisoned, false);
    atomic_init(&s_output_policy.emission_owner, 0U);
    atomic_init(&s_dispatch_ready, false);
    atomic_init(&s_recovery_only, false);
    badge_usb_upload_policy_init(&s_upload_policy);
    badge_usb_uplink_ota_flow_init(&s_uplink_flow);
    s_uplink_finish_pending = false;
    s_uplink_abort_pending = false;
    s_uplink_terminal_pending = false;
    s_uplink_recovery_after_cleanup = false;
    s_uplink_suppress_terminal = false;
    s_uplink_abort_reason[0] = '\0';
    s_uplink_terminal_length = 0U;
    s_boot_window_ms = boot_window_ms;
    s_started_ms = now_ms();
    BaseType_t task_ok = xTaskCreate(badge_usb_transport_task, "badge_usb",
                                    BADGE_USB_TASK_STACK_BYTES, NULL,
                                    tskIDLE_PRIORITY + 2, NULL);
    if (task_ok != pdPASS) {
        release_start_allocations();
        return false;
    }
    esp_log_set_vprintf(transport_log_vprintf);
    s_transport_started = true;
    return true;
}

bool badge_usb_transport_wait_boot_window(TickType_t timeout)
{
    if (!s_boot_events) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_boot_events,
                                          USB_BOOT_WINDOW_DONE,
                                          pdFALSE, pdTRUE, timeout);
    return (bits & USB_BOOT_WINDOW_DONE) != 0;
}

void badge_usb_transport_set_recovery_only(bool enabled)
{
    atomic_store_explicit(&s_recovery_only, enabled, memory_order_release);
}

void badge_usb_transport_set_dispatch_ready(void)
{
    atomic_store_explicit(&s_dispatch_ready, true, memory_order_release);
    static const char ready[] = "FOF_READY\n";
    (void)badge_usb_transport_emit(ready, sizeof(ready) - 1U,
                                   BADGE_USB_FRAME_OPTIONAL,
                                   pdMS_TO_TICKS(250));
}

static bool begin_binary_upload(badge_usb_binary_target_t target,
                                uint32_t exact_size,
                                bool scanner_credit_v1)
{
    bool begun = badge_usb_stream_begin_binary(&s_stream, target, exact_size,
                                                (uint32_t)now_ms());
    bool policy_begun = false;
    if (begun) {
        policy_begun = scanner_credit_v1
            ? badge_usb_upload_begin_credit_v1(
                &s_upload_policy, target, exact_size)
            : badge_usb_upload_begin(
                &s_upload_policy, target, exact_size);
    }
    if (begun && !policy_begun) {
        badge_usb_stream_result_t aborted;
        (void)badge_usb_stream_abort(&s_stream, "upload_policy_busy", &aborted);
        begun = false;
    }
    if (begun) {
        portENTER_CRITICAL(&s_health_lock);
        s_health.parser_target = target;
        s_health.upload_received = 0;
        s_health.upload_size = exact_size;
        s_health.last_upload_progress_ms = now_ms();
        portEXIT_CRITICAL(&s_health_lock);
    }
    return begun;
}

bool badge_usb_transport_begin_binary(badge_usb_binary_target_t target,
                                      uint32_t exact_size)
{
    return begin_binary_upload(target, exact_size, false);
}

bool badge_usb_transport_begin_scanner_binary(uint32_t exact_size,
                                              bool credit_v1)
{
    return begin_binary_upload(
        BADGE_USB_BINARY_SCANNER, exact_size, credit_v1);
}

bool badge_usb_transport_reject_uplink_ota_begin(const char *error)
{
    uplink_usb_ota_result_t result = {
        .ok = false,
        .retryable = false,
        .emit_required = true,
        .reboot_required = false,
        .phase = UPLINK_USB_OTA_PHASE_ABORTED,
    };
    snprintf(result.error, sizeof(result.error), "%s",
             error && error[0] ? error : "invalid_manifest");
    return emit_uplink_ota_result(&result, pdMS_TO_TICKS(1000));
}

bool badge_usb_transport_handle_uplink_ota_begin(
    const uplink_ota_manifest_t *manifest)
{
    uplink_usb_ota_result_t result = {0};
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (!badge_runtime_update_maintenance_active()) {
        badge_usb_uplink_ota_maintenance_required_result(&result);
        return emit_uplink_ota_result(&result, pdMS_TO_TICKS(1000));
    }
#endif
    badge_usb_uplink_ota_flow_init(&s_uplink_flow);
    s_uplink_finish_pending = false;
    s_uplink_abort_pending = false;
    s_uplink_terminal_pending = false;
    s_uplink_recovery_after_cleanup = false;
    s_uplink_suppress_terminal = false;
    s_uplink_abort_reason[0] = '\0';
    s_uplink_terminal_length = 0U;
    if (!uplink_usb_ota_begin(manifest, &result)) {
        bool emitted = emit_uplink_ota_result(&result, pdMS_TO_TICKS(1000));
        badge_usb_uplink_action_t failed_action =
            badge_usb_uplink_ota_begin_failure_action(&result);
        if (failed_action == BADGE_USB_UPLINK_ACTION_RETRY_CLEANUP) {
            latch_uplink_abort("operation_release_failed");
        }
        return emitted;
    }

    badge_usb_uplink_action_t action =
        badge_usb_uplink_ota_flow_begin_result(&s_uplink_flow, &result);
    if (action != BADGE_USB_UPLINK_ACTION_WAIT_RECEIPT) {
        (void)abort_uplink_ota("invalid_ready_result");
        return false;
    }

    badge_usb_uplink_receipt_decision_t receipt =
        deliver_uplink_receipt(&result, "ready_receipt_failed");
    if (receipt != BADGE_USB_UPLINK_RECEIPT_ACKNOWLEDGED) {
        return false;
    }

    if (!badge_usb_stream_begin_binary(
            &s_stream, BADGE_USB_BINARY_UPLINK, manifest->size,
            (uint32_t)now_ms())) {
        (void)abort_uplink_ota("usb_parser_busy");
        return false;
    }

    portENTER_CRITICAL(&s_health_lock);
    s_health.parser_target = BADGE_USB_BINARY_UPLINK;
    s_health.upload_received = 0U;
    s_health.upload_size = manifest->size;
    s_health.last_upload_progress_ms = now_ms();
    portEXIT_CRITICAL(&s_health_lock);
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    badge_runtime_update_keepalive(
        (uint32_t)(esp_timer_get_time() / 1000));
#endif
    return true;
}

void badge_usb_transport_snapshot(badge_usb_health_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_health_lock);
    *out = s_health;
    portEXIT_CRITICAL(&s_health_lock);
}

bool badge_usb_transport_host_active(uint32_t sample_window_ms)
{
    int64_t deadline_us = esp_timer_get_time() +
                          ((int64_t)sample_window_ms * 1000);
    do {
        if (usb_serial_jtag_is_connected()) {
            return true;
        }
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            break;
        }
        esp_rom_delay_us((uint32_t)(remaining_us < 1000
            ? remaining_us : 1000));
    } while (true);
    return false;
}
