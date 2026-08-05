#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "comms/backend_uart_rx.h"
#include "comms/backend_uart_tx.h"
#include "comms/uart_ota.h"
#include "core/backend_detection_sink.h"
#include "core/backend_investigation_sink.h"
#include "core/backend_scanner_runtime.h"
#include "core/backend_task_priorities.h"
#include "core/scanner_rollback.h"
#include "detection/bayesian_fusion.h"
#include "detection/ble_investigator.h"
#include "detection/ble_remote_id.h"
#include "detection/wifi_scanner.h"
#include "backend_status_led.h"

#include "backend_detection_codec.h"
#include "backend_identity.h"
#include "backend_led_protocol.h"
#include "backend_scanner_status_codec.h"
#include "backend_version.h"
#include "psram_alloc.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define APP_DETECTION_QUEUE_LENGTH 16U
#define APP_CONTROL_QUEUE_LENGTH 8U
#define APP_UART_BUFFER_BYTES 8192U
#define APP_UART_READ_BYTES 1024U
#define APP_USB_COMMAND_BYTES 64U
#define APP_BOOT_RECORD_BYTES 512U
#define APP_HEALTH_RECORD_BYTES 512U
#define APP_SUPERVISOR_PERIOD_MS 250U

typedef struct {
    drone_detection_t detection;
    int64_t observed_monotonic_ms;
} app_detection_item_t;

typedef enum {
    APP_COMMAND_CONTROL = 0,
    APP_COMMAND_INVESTIGATE,
    APP_COMMAND_CANCEL,
} app_command_kind_t;

typedef struct {
    app_command_kind_t kind;
    int64_t received_monotonic_ms;
    union {
        backend_scanner_control_t control;
        ble_investigation_request_t investigation;
        char cancel_id[BLE_INV_REQUEST_ID_LEN];
    } payload;
} app_command_item_t;

typedef struct {
    backend_scanner_runtime_t runtime;
    backend_uart_rx_t uart_rx;
    backend_uart_tx_t investigation_tx;
    uart_ota_t ota;
    scanner_rollback_policy_t rollback;
    backend_led_mirror_t led_mirror;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t uart_mutex;
    QueueHandle_t detection_queue;
    QueueHandle_t control_queue;
    const esp_partition_t *update_partition;
    esp_ota_handle_t update_handle;
    size_t update_written;
    uint32_t boot_id;
    uint32_t command_ingress_boot_id;
    uint32_t status_sequence;
    uint32_t detection_sequence;
    int64_t time_anchor_monotonic_ms;
    int8_t component_slot;
    char mac[18];
    char boot_record[APP_BOOT_RECORD_BYTES];
    char health_record[APP_HEALTH_RECORD_BYTES];
    bool nvs_erased;
    bool command_ingress_healthy;
    bool running_pending_verify;
    bool rollback_clear;
    bool role_acked;
    bool health_available;
    bool ble_initialized;
    bool wifi_initialized;
    bool wifi_task_started;
    bool update_open;
} scanner_app_t;

static const char *TAG = "backend_scanner";
static scanner_app_t s_app;
static atomic_uint_fast32_t s_rx_errors;
static atomic_uint_fast32_t s_tx_drops;

static void increment_saturated(atomic_uint_fast32_t *value)
{
    uint_fast32_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (current < UINT32_MAX &&
           !atomic_compare_exchange_weak_explicit(
               value, &current, current + 1U,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static bool app_lock(void)
{
    return s_app.state_mutex != NULL &&
           xSemaphoreTake(s_app.state_mutex, portMAX_DELAY) == pdTRUE;
}

static void app_unlock(void)
{
    xSemaphoreGive(s_app.state_mutex);
}

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / INT64_C(1000);
}

static const char *profile_name(backend_scan_profile_t profile)
{
    switch (profile) {
    case BACKEND_SCAN_PROFILE_BLE_PRIMARY:
        return "ble_primary";
    case BACKEND_SCAN_PROFILE_WIFI_PRIMARY:
        return "wifi_primary";
    case BACKEND_SCAN_PROFILE_HYBRID_FAILOVER:
        return "hybrid_failover";
    case BACKEND_SCAN_PROFILE_QUIESCENT:
    default:
        return "quiescent";
    }
}

static bool uart_write_raw(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL || length == 0U || s_app.uart_mutex == NULL ||
        xSemaphoreTake(s_app.uart_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        increment_saturated(&s_tx_drops);
        return false;
    }
    const int written = uart_write_bytes(
        (uart_port_t)BACKEND_SCANNER_UART_PORT, bytes, length);
    const bool complete = written == (int)length &&
        uart_wait_tx_done(
            (uart_port_t)BACKEND_SCANNER_UART_PORT,
            pdMS_TO_TICKS(500)) == ESP_OK;
    xSemaphoreGive(s_app.uart_mutex);
    if (!complete) {
        increment_saturated(&s_tx_drops);
    }
    return complete;
}

static bool uart_write_line(char *line, size_t length, size_t capacity)
{
    if (line == NULL || length == 0U || length + 1U >= capacity) {
        increment_saturated(&s_tx_drops);
        return false;
    }
    line[length] = '\n';
    line[length + 1U] = '\0';
    return uart_write_raw((const uint8_t *)line, length + 1U);
}

static bool investigation_uart_write(
    void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    return uart_write_raw(bytes, length);
}

static bool investigation_consumer(
    void *context, const ble_investigation_chunk_t *chunk)
{
    (void)context;
    return backend_uart_tx_send_investigation(
        &s_app.investigation_tx, chunk);
}

static bool detection_consumer(
    void *context,
    const drone_detection_t *detection,
    int64_t observed_monotonic_ms)
{
    (void)context;
    if (detection == NULL || observed_monotonic_ms < 0 ||
        s_app.detection_queue == NULL || !app_lock()) {
        return false;
    }
    const bool accepted = backend_scanner_runtime_enqueue_detection(
        &s_app.runtime);
    app_unlock();
    if (!accepted) {
        return false;
    }

    const app_detection_item_t item = {
        .detection = *detection,
        .observed_monotonic_ms = observed_monotonic_ms,
    };
    if (xQueueSend(s_app.detection_queue, &item, 0) != pdTRUE) {
        if (app_lock()) {
            (void)backend_scanner_runtime_complete_detection(&s_app.runtime);
            app_unlock();
        }
        increment_saturated(&s_tx_drops);
        return false;
    }
    return true;
}

static bool ota_read_binding(void *context, uart_ota_local_binding_t *out)
{
    scanner_app_t *app = context;
    if (app == NULL || out == NULL || app->component_slot < 0 ||
        app->component_slot > 1 || app->runtime.role.generation == 0U) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->component_slot = (uint8_t)app->component_slot;
    memcpy(out->mac, app->mac, sizeof(out->mac));
    out->boot_id = app->boot_id;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    out->topology_generation = app->runtime.role.topology_generation;
#else
    out->topology_generation = app->runtime.role.generation;
#endif
    return true;
}

static uint8_t *ota_psram_acquire(void *context, size_t size)
{
    (void)context;
    return psram_alloc_strict(size);
}

static void ota_psram_release(
    void *context, uint8_t *buffer, size_t size)
{
    (void)context;
    (void)size;
    psram_free(buffer);
}

static bool ota_emit_receipt(
    void *context, const uart_ota_receipt_t *receipt)
{
    (void)context;
    char line[UART_JSON_MAX_SIZE + 2U];
    const size_t length = uart_ota_receipt_to_json(
        receipt, line, UART_JSON_MAX_SIZE);
    return uart_write_line(line, length, sizeof(line));
}

static bool ota_inactive_begin(void *context, size_t size)
{
    scanner_app_t *app = context;
    if (app == NULL || app->update_open || size == 0U ||
        size > UART_OTA_INACTIVE_SLOT_CAPACITY) {
        return false;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || next == NULL || next == running ||
        next->size != UART_OTA_INACTIVE_SLOT_CAPACITY || size > next->size) {
        return false;
    }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(next, size, &handle) != ESP_OK) {
        return false;
    }
    app->update_partition = next;
    app->update_handle = handle;
    app->update_written = 0U;
    app->update_open = true;
    return true;
}

static bool ota_inactive_write(
    void *context,
    size_t offset,
    const uint8_t *bytes,
    size_t size)
{
    scanner_app_t *app = context;
    if (app == NULL || !app->update_open || bytes == NULL || size == 0U ||
        offset != app->update_written ||
        offset > UART_OTA_INACTIVE_SLOT_CAPACITY ||
        size > UART_OTA_INACTIVE_SLOT_CAPACITY - offset ||
        esp_ota_write(app->update_handle, bytes, size) != ESP_OK) {
        return false;
    }
    app->update_written += size;
    return true;
}

static bool ota_inactive_finish(void *context)
{
    scanner_app_t *app = context;
    if (app == NULL || !app->update_open) {
        return false;
    }
    const esp_err_t result = esp_ota_end(app->update_handle);
    app->update_open = false;
    app->update_handle = 0;
    return result == ESP_OK;
}

static void ota_inactive_abort(void *context)
{
    scanner_app_t *app = context;
    if (app != NULL && app->update_open) {
        (void)esp_ota_abort(app->update_handle);
        app->update_open = false;
        app->update_handle = 0;
    }
    if (app != NULL) {
        app->update_partition = NULL;
        app->update_written = 0U;
    }
}

static bool ota_activate_pending_verify(void *context)
{
    scanner_app_t *app = context;
    return app != NULL && !app->update_open &&
           app->update_partition != NULL &&
           esp_ota_set_boot_partition(app->update_partition) == ESP_OK;
}

static bool ota_request_reboot(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
    return true;
}

static bool ota_binary_active(void *context)
{
    (void)context;
    if (!app_lock()) {
        return false;
    }
    const bool active = uart_ota_is_receiving_binary(&s_app.ota);
    app_unlock();
    return active;
}

static bool radios_quiesced(void)
{
    const bool ble_quiet = !s_app.ble_initialized ||
        ble_remote_id_is_quiesced();
    const bool wifi_quiet = !s_app.wifi_initialized ||
        wifi_scanner_is_quiesced();
    return ble_quiet && wifi_quiet &&
           !ble_investigator_runtime_is_busy();
}

static bool handle_ota_begin(
    const backend_scanner_ota_begin_control_t *begin)
{
    if (begin == NULL || !app_lock()) {
        return false;
    }
    if ((s_app.ota.state == UART_OTA_STATE_FAILED ||
         s_app.ota.state == UART_OTA_STATE_DRY_RUN_COMPLETE) &&
        begin->generation > s_app.ota.highest_generation) {
        uart_ota_reset(&s_app.ota);
    }
    if (backend_scanner_runtime_profile(&s_app.runtime) !=
            BACKEND_SCAN_PROFILE_QUIESCENT ||
        !radios_quiesced()) {
        app_unlock();
        return false;
    }
    const uart_ota_result_t result = uart_ota_begin(&s_app.ota, begin);
    if (result == UART_OTA_RESULT_OK) {
        backend_scanner_runtime_set_ota_active(&s_app.runtime, true);
    }
    app_unlock();
    return result == UART_OTA_RESULT_OK;
}

static bool handle_ota_end(
    const backend_scanner_ota_finish_control_t *finish)
{
    if (finish == NULL || !app_lock()) {
        return false;
    }
    const uart_ota_result_t result = uart_ota_end(&s_app.ota, finish);
    if (s_app.ota.state == UART_OTA_STATE_DRY_RUN_COMPLETE ||
        s_app.ota.state == UART_OTA_STATE_FAILED) {
        backend_scanner_runtime_set_ota_active(&s_app.runtime, false);
    }
    app_unlock();
    return result == UART_OTA_RESULT_OK;
}

static bool handle_ota_abort(
    const backend_scanner_ota_finish_control_t *finish)
{
    if (finish == NULL || !app_lock()) {
        return false;
    }
    const uart_ota_result_t result = uart_ota_abort(&s_app.ota, finish);
    backend_scanner_runtime_set_ota_active(&s_app.runtime, false);
    app_unlock();
    return result == UART_OTA_RESULT_OK;
}

static bool enqueue_command(const app_command_item_t *item)
{
    if (item == NULL || s_app.control_queue == NULL || !app_lock()) {
        return false;
    }
    const bool admitted = backend_scanner_runtime_enqueue_control(
        &s_app.runtime);
    app_unlock();
    if (!admitted) {
        return false;
    }
    if (xQueueSend(s_app.control_queue, item, 0) != pdTRUE) {
        if (app_lock()) {
            (void)backend_scanner_runtime_complete_control(&s_app.runtime);
            app_unlock();
        }
        return false;
    }
    return true;
}

static bool control_callback(
    void *context,
    const backend_scanner_control_t *control,
    int64_t now_ms)
{
    (void)context;
    if (control == NULL || now_ms < 0) {
        return false;
    }
    switch (control->type) {
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
        return handle_ota_begin(&control->payload.ota_begin);
    case BACKEND_SCANNER_CONTROL_OTA_END:
        return handle_ota_end(&control->payload.ota_finish);
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        return handle_ota_abort(&control->payload.ota_finish);
    default: {
        const app_command_item_t item = {
            .kind = APP_COMMAND_CONTROL,
            .received_monotonic_ms = now_ms,
            .payload.control = *control,
        };
        return enqueue_command(&item);
    }
    }
}

static bool investigate_callback(
    void *context,
    const ble_investigation_request_t *request,
    int64_t now_ms)
{
    (void)context;
    if (request == NULL || now_ms < 0) {
        return false;
    }
    const app_command_item_t item = {
        .kind = APP_COMMAND_INVESTIGATE,
        .received_monotonic_ms = now_ms,
        .payload.investigation = *request,
    };
    return enqueue_command(&item);
}

static bool cancel_callback(
    void *context, const char *command_id, int64_t now_ms)
{
    (void)context;
    if (command_id == NULL || now_ms < 0 ||
        strlen(command_id) >= BLE_INV_REQUEST_ID_LEN) {
        return false;
    }
    app_command_item_t item = {
        .kind = APP_COMMAND_CANCEL,
        .received_monotonic_ms = now_ms,
    };
    memcpy(item.payload.cancel_id, command_id, strlen(command_id) + 1U);
    return enqueue_command(&item);
}

static backend_led_state_t led_state_from_name(
    const char *name, bool *valid)
{
    *valid = true;
    if (strcmp(name, "healthy") == 0) {
        return BACKEND_LED_HEALTHY;
    }
    if (strcmp(name, "network_degraded") == 0) {
        return BACKEND_LED_NETWORK_DEGRADED;
    }
    if (strcmp(name, "drone") == 0) {
        return BACKEND_LED_DRONE;
    }
    if (strcmp(name, "meta") == 0) {
        return BACKEND_LED_META;
    }
    if (strcmp(name, "drone_meta") == 0) {
        return BACKEND_LED_DRONE_META;
    }
    if (strcmp(name, "fatal") == 0) {
        return BACKEND_LED_FATAL;
    }
    *valid = false;
    return BACKEND_LED_UART_LOST;
}

static void initialize_ble_if_needed(void)
{
    if (!s_app.ble_initialized) {
        ble_remote_id_init();
        s_app.ble_initialized = ble_remote_id_is_initialized();
    }
}

static void initialize_wifi_if_needed(void)
{
    if (!s_app.wifi_initialized) {
        wifi_scanner_init();
        s_app.wifi_initialized = wifi_scanner_is_initialized();
    }
    if (s_app.wifi_initialized && !s_app.wifi_task_started) {
        wifi_scanner_start();
        s_app.wifi_task_started = true;
    }
}

static void reconcile_radios(backend_scan_profile_t profile, int64_t now_ms)
{
    const bool want_ble = profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY ||
        profile == BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    const bool want_wifi = profile == BACKEND_SCAN_PROFILE_WIFI_PRIMARY ||
        profile == BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;

    if (!want_ble) {
        ble_investigator_runtime_quiesce(now_ms);
        if (s_app.ble_initialized) {
            ble_remote_id_stop();
        }
    } else {
        initialize_ble_if_needed();
        if (s_app.ble_initialized) {
            ble_remote_id_start();
        }
    }

    if (!want_wifi) {
        if (s_app.wifi_initialized) {
            wifi_scanner_pause();
        }
    } else {
        initialize_wifi_if_needed();
        if (s_app.wifi_initialized && wifi_scanner_is_paused()) {
            (void)wifi_scanner_resume();
        }
    }
}

static const char *time_source_name(
    backend_scanner_time_source_t source)
{
    switch (source) {
    case BACKEND_SCANNER_TIME_SNTP:
        return "sntp";
    case BACKEND_SCANNER_TIME_BACKEND:
        return "backend";
    case BACKEND_SCANNER_TIME_NONE:
    default:
        return "none";
    }
}

static bool send_status(void)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    backend_scanner_status_t status;
    memset(&status, 0, sizeof(status));
    if (identity == NULL || !app_lock()) {
        return false;
    }
    if (s_app.status_sequence != UINT32_MAX) {
        ++s_app.status_sequence;
    }
    if (s_app.status_sequence == 0U) {
        s_app.status_sequence = 1U;
    }
    status.schema = BACKEND_SCANNER_STATUS_SCHEMA;
    status.sequence = s_app.status_sequence;
    status.boot_id = s_app.boot_id;
    memcpy(status.mac, s_app.mac, sizeof(status.mac));
    memcpy(status.target, identity->target, strlen(identity->target) + 1U);
    memcpy(status.project, identity->project, strlen(identity->project) + 1U);
    memcpy(status.hardware, identity->hardware,
           strlen(identity->hardware) + 1U);
    memcpy(status.version, identity->version, strlen(identity->version) + 1U);
    status.profile = backend_scanner_runtime_profile(&s_app.runtime);
    status.role_generation = s_app.runtime.role.generation;
    status.role_acked = s_app.role_acked;
    status.command_ingress = s_app.command_ingress_healthy &&
        s_app.command_ingress_boot_id == s_app.boot_id;
    status.ble_healthy = s_app.runtime.ble_healthy;
    status.wifi_healthy = s_app.runtime.wifi_healthy;
    status.flow_paused = s_app.runtime.flow.paused;
    const char *ota_state = "idle";
    if (s_app.running_pending_verify) {
        ota_state = "pending_verify";
    } else if (s_app.ota.state == UART_OTA_STATE_STAGING) {
        ota_state = "staging";
    } else if (s_app.ota.state == UART_OTA_STATE_IMAGE_STAGED) {
        ota_state = "staged";
    } else if (s_app.ota.state == UART_OTA_STATE_FAILED) {
        ota_state = "failed";
    }
    snprintf(status.ota_state, sizeof(status.ota_state), "%s", ota_state);
    snprintf(status.rollback_state, sizeof(status.rollback_state), "%s",
             s_app.rollback_clear ? "valid" : "pending_verify");
    status.rx_errors = (uint32_t)atomic_load_explicit(
        &s_rx_errors, memory_order_relaxed);
    status.tx_drops = (uint32_t)atomic_load_explicit(
        &s_tx_drops, memory_order_relaxed);
    const int64_t now = monotonic_ms();
    status.uptime_ms = now < 0 ? 0U : (uint64_t)now;
    app_unlock();

    char line[BACKEND_SCANNER_STATUS_MAX_LINE + 2U];
    const size_t length = backend_scanner_status_encode(
        &status, line, BACKEND_SCANNER_STATUS_MAX_LINE + 1U);
    return uart_write_line(line, length, sizeof(line));
}

static void restart_required_radios(
    backend_scan_profile_t profile, int64_t now_ms)
{
    if (profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY ||
        profile == BACKEND_SCAN_PROFILE_HYBRID_FAILOVER) {
        ble_investigator_runtime_quiesce(now_ms);
        if (s_app.ble_initialized) {
            ble_remote_id_stop();
        }
    }
    if (profile == BACKEND_SCAN_PROFILE_WIFI_PRIMARY ||
        profile == BACKEND_SCAN_PROFILE_HYBRID_FAILOVER) {
        if (s_app.wifi_initialized) {
            wifi_scanner_pause();
        }
    }
    reconcile_radios(profile, now_ms);
}

static void process_control(
    const backend_scanner_control_t *control, int64_t now_ms)
{
    switch (control->type) {
    case BACKEND_SCANNER_CONTROL_ROLE: {
        const backend_scanner_role_control_t *role = &control->payload.role;
        backend_scanner_role_result_t result = BACKEND_ROLE_INVALID_BOOT;
        if (app_lock()) {
            result = backend_scanner_runtime_apply_role(
                &s_app.runtime, role->boot_id,
                role->generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
                role->topology_generation,
#endif
                role->profile);
            if (result == BACKEND_ROLE_APPLIED) {
                s_app.role_acked = false;
                if (s_app.component_slot < 0 &&
                    role->profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY) {
                    s_app.component_slot = 0;
                } else if (s_app.component_slot < 0 &&
                           role->profile == BACKEND_SCAN_PROFILE_WIFI_PRIMARY) {
                    s_app.component_slot = 1;
                }
            }
            app_unlock();
        }
        if (result == BACKEND_ROLE_APPLIED ||
            result == BACKEND_ROLE_REFRESHED) {
            reconcile_radios(role->profile, now_ms);
        }
        break;
    }
    case BACKEND_SCANNER_CONTROL_TIME:
        if (app_lock()) {
            const backend_scanner_time_result_t result =
                backend_scanner_runtime_apply_time(
                    &s_app.runtime,
                    control->payload.time.generation,
                    control->payload.time.valid,
                    control->payload.time.epoch_ms,
                    time_source_name(control->payload.time.source));
            if (result == BACKEND_SCANNER_TIME_APPLIED) {
                s_app.time_anchor_monotonic_ms = now_ms;
            }
            app_unlock();
        }
        break;
    case BACKEND_SCANNER_CONTROL_FLOW:
        if (app_lock()) {
            (void)backend_scanner_runtime_apply_flow(
                &s_app.runtime,
                control->payload.flow.generation,
                control->payload.flow.paused);
            app_unlock();
        }
        break;
    case BACKEND_SCANNER_CONTROL_LED_STATE: {
        bool valid = false;
        backend_led_command_t command = {
            .state = led_state_from_name(
                control->payload.led.state, &valid),
            .generation = control->payload.led.generation,
            .ttl_ms = control->payload.led.ttl_ms,
        };
        if (valid && app_lock()) {
            (void)backend_led_mirror_accept(
                &s_app.led_mirror, &command, now_ms);
            app_unlock();
        }
        break;
    }
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
        (void)send_status();
        break;
    case BACKEND_SCANNER_CONTROL_RECOVERY: {
        backend_scan_profile_t profile = BACKEND_SCAN_PROFILE_QUIESCENT;
        bool permitted = false;
        if (app_lock()) {
            permitted = control->payload.recovery.boot_id == s_app.boot_id &&
                control->payload.recovery.generation >=
                    s_app.runtime.role.generation &&
                !s_app.runtime.ota_active;
            profile = backend_scanner_runtime_profile(&s_app.runtime);
            app_unlock();
        }
        if (permitted) {
            restart_required_radios(profile, now_ms);
        }
        break;
    }
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
    case BACKEND_SCANNER_CONTROL_INVESTIGATE:
    case BACKEND_SCANNER_CONTROL_CANCEL:
    default:
        break;
    }
}

static void process_investigation(
    const ble_investigation_request_t *request, int64_t now_ms)
{
    bool allowed = false;
    if (app_lock()) {
        const backend_scan_profile_t profile =
            backend_scanner_runtime_profile(&s_app.runtime);
        allowed = !s_app.runtime.ota_active &&
            (profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY ||
             profile == BACKEND_SCAN_PROFILE_HYBRID_FAILOVER);
        app_unlock();
    }
    if (!allowed) {
        ble_investigator_runtime_emit_rejection(
            request->request_id, request->mode,
            request->target_mac, "ble_unavailable");
        return;
    }
    const ble_investigator_request_decision_t decision =
        ble_investigator_runtime_decide_request(request->request_id);
    if (decision == BLE_INV_REQUEST_RETRANSMIT) {
        return;
    }
    if (decision != BLE_INV_REQUEST_AVAILABLE ||
        !ble_investigator_runtime_start(request, now_ms)) {
        ble_investigator_runtime_emit_rejection(
            request->request_id, request->mode,
            request->target_mac,
            decision == BLE_INV_REQUEST_BUSY_REJECTION
                ? "busy" : "start_failed");
    }
}

static void control_task(void *argument)
{
    (void)argument;
    (void)backend_scanner_runtime_wdt_register_current();
    for (;;) {
        app_command_item_t item;
        if (xQueueReceive(
                s_app.control_queue, &item,
                pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item.kind == APP_COMMAND_CONTROL) {
                process_control(
                    &item.payload.control,
                    item.received_monotonic_ms);
            } else if (item.kind == APP_COMMAND_INVESTIGATE) {
                process_investigation(
                    &item.payload.investigation,
                    item.received_monotonic_ms);
            } else if (item.kind == APP_COMMAND_CANCEL) {
                (void)ble_investigator_runtime_cancel(
                    item.payload.cancel_id,
                    item.received_monotonic_ms);
            }
            if (app_lock()) {
                (void)backend_scanner_runtime_complete_control(
                    &s_app.runtime);
                app_unlock();
            }
        }
        ble_investigator_runtime_tick(monotonic_ms());
        if (app_lock()) {
            (void)backend_scanner_runtime_worker_iteration(
                &s_app.runtime, BACKEND_WORKER_UART_RX_CONTROL);
            app_unlock();
        }
    }
}

static void detection_tx_task(void *argument)
{
    (void)argument;
    char line[BACKEND_DETECTION_UART_MAX_LINE + 2U];
    for (;;) {
        app_detection_item_t item;
        if (xQueueReceive(
                s_app.detection_queue, &item,
                portMAX_DELAY) != pdTRUE) {
            continue;
        }
        backend_scanner_stamp_t stamp = {0};
        if (app_lock()) {
            if (s_app.detection_sequence != UINT32_MAX) {
                ++s_app.detection_sequence;
            }
            if (s_app.detection_sequence == 0U) {
                s_app.detection_sequence = 1U;
            }
            stamp.sequence = s_app.detection_sequence;
            if (s_app.runtime.time_valid &&
                item.observed_monotonic_ms >=
                    s_app.time_anchor_monotonic_ms) {
                stamp.time_valid = true;
                stamp.observed_epoch_ms = s_app.runtime.epoch_ms +
                    item.observed_monotonic_ms -
                    s_app.time_anchor_monotonic_ms;
            }
            app_unlock();
        }
        const size_t length = backend_detection_uart_encode(
            &item.detection, &stamp, line,
            BACKEND_DETECTION_UART_MAX_LINE + 1U);
        if (length == 0U ||
            !uart_write_line(line, length, sizeof(line))) {
            increment_saturated(&s_tx_drops);
        }
        if (app_lock()) {
            (void)backend_scanner_runtime_complete_detection(&s_app.runtime);
            app_unlock();
        }
    }
}

static void account_rx_result(const backend_uart_rx_result_t *result)
{
    if (result->invalid_argument || result->semantic_rejection) {
        increment_saturated(&s_rx_errors);
    }
    for (size_t index = 0U;
         index < result->rejected_frames + result->dispatch_failures;
         ++index) {
        increment_saturated(&s_rx_errors);
    }
}

static void uart_rx_task(void *argument)
{
    (void)argument;
    (void)backend_scanner_runtime_wdt_register_current();
    uint8_t bytes[APP_UART_READ_BYTES];
    for (;;) {
        const int read = uart_read_bytes(
            (uart_port_t)BACKEND_SCANNER_UART_PORT,
            bytes, sizeof(bytes), pdMS_TO_TICKS(100));
        if (read < 0) {
            increment_saturated(&s_rx_errors);
            continue;
        }
        size_t offset = 0U;
        while (offset < (size_t)read) {
            bool binary = false;
            if (app_lock()) {
                binary = uart_ota_is_receiving_binary(&s_app.ota);
                app_unlock();
            }
            if (binary) {
                size_t consumed = 0U;
                uart_ota_result_t result = UART_OTA_RESULT_INVALID_STATE;
                if (app_lock()) {
                    result = uart_ota_consume(
                        &s_app.ota, bytes + offset,
                        (size_t)read - offset, &consumed);
                    backend_scanner_runtime_set_ota_active(
                        &s_app.runtime,
                        s_app.ota.state == UART_OTA_STATE_STAGING ||
                        s_app.ota.state == UART_OTA_STATE_IMAGE_STAGED);
                    (void)backend_scanner_runtime_worker_iteration(
                        &s_app.runtime, BACKEND_WORKER_OTA);
                    app_unlock();
                }
                if (result != UART_OTA_RESULT_OK) {
                    increment_saturated(&s_rx_errors);
                }
                if (consumed == 0U) {
                    break;
                }
                offset += consumed;
                continue;
            }

            const backend_uart_rx_result_t result = backend_uart_rx_consume(
                &s_app.uart_rx, bytes + offset,
                (size_t)read - offset, monotonic_ms());
            account_rx_result(&result);
            if (result.consumed_bytes == 0U) {
                break;
            }
            offset += result.consumed_bytes;
        }
        if (app_lock()) {
            (void)backend_scanner_runtime_worker_iteration(
                &s_app.runtime, BACKEND_WORKER_OTA);
            app_unlock();
        }
    }
}

static bool reset_was_crash(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

static void emit_usb_record(const char *record)
{
    if (record != NULL && record[0] != '\0') {
        printf("%s\n", record);
        fflush(stdout);
    }
}

static void emit_usb_snapshot(void)
{
    char boot[APP_BOOT_RECORD_BYTES];
    char health[APP_HEALTH_RECORD_BYTES];
    bool have_health = false;
    if (!app_lock()) {
        return;
    }
    memcpy(boot, s_app.boot_record, sizeof(boot));
    memcpy(health, s_app.health_record, sizeof(health));
    have_health = s_app.health_available;
    app_unlock();
    emit_usb_record(boot);
    if (have_health) {
        emit_usb_record(health);
    }
}

static void usb_status_task(void *argument)
{
    (void)argument;
    char command[APP_USB_COMMAND_BYTES];
    for (;;) {
        if (fgets(command, sizeof(command), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const size_t length = strcspn(command, "\r\n");
        command[length] = '\0';
        if (strcmp(command, "FOF_BACKEND_STATUS") == 0) {
            emit_usb_snapshot();
        }
    }
}

static void maybe_emit_health(void)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    char record[APP_HEALTH_RECORD_BYTES];
    bool changed = false;
    if (identity == NULL || !app_lock()) {
        return;
    }
    const backend_scan_profile_t profile =
        backend_scanner_runtime_profile(&s_app.runtime);
    const bool radio_healthy = backend_scanner_required_radio_healthy(
        profile, s_app.runtime.ble_healthy, s_app.runtime.wifi_healthy);
    const bool ready = profile != BACKEND_SCAN_PROFILE_QUIESCENT &&
        s_app.command_ingress_healthy &&
        s_app.command_ingress_boot_id == s_app.boot_id &&
        s_app.role_acked && radio_healthy && s_app.rollback_clear &&
        backend_scanner_runtime_rollback_ready(&s_app.runtime);
    if (ready) {
        const int written = snprintf(
            record, sizeof(record),
            "FOF_BACKEND_HEALTH {\"product_family\":\"%s\","
            "\"firmware_line\":\"%s\",\"component\":\"%s\","
            "\"target\":\"%s\",\"project\":\"%s\","
            "\"hardware\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\","
            "\"boot_id\":%" PRIu32 ",\"nvs_erased\":%s,"
            "\"role\":\"%s\",\"command_ingress_boot_id\":%" PRIu32 ","
            "\"radio_healthy\":true,\"rollback_clear\":true}",
            identity->product_family, identity->firmware_line,
            identity->component, identity->target, identity->project,
            identity->hardware, identity->version, s_app.mac, s_app.boot_id,
            s_app.nvs_erased ? "true" : "false", profile_name(profile),
            s_app.command_ingress_boot_id);
        if (written > 0 && (size_t)written < sizeof(record) &&
            (!s_app.health_available ||
             strcmp(record, s_app.health_record) != 0)) {
            memcpy(s_app.health_record, record, (size_t)written + 1U);
            s_app.health_available = true;
            changed = true;
        }
    }
    app_unlock();
    if (changed) {
        emit_usb_record(record);
    }
}

static void evaluate_rollback(int64_t now_ms)
{
    scanner_rollback_action_t action = SCANNER_ROLLBACK_WAIT;
    if (!app_lock()) {
        return;
    }
    const backend_scan_profile_t profile =
        backend_scanner_runtime_profile(&s_app.runtime);
    const scanner_rollback_readiness_t readiness = {
        .boot_id = s_app.boot_id,
        .command_ingress_boot_id = s_app.command_ingress_boot_id,
        .role_boot_id = s_app.runtime.role.boot_id,
        .uptime_ms = now_ms,
        .expected_profile = profile,
        .reported_profile = profile,
        .watchdog_ready_mask = s_app.runtime.watchdog_completed_mask,
        .command_ingress_healthy = s_app.command_ingress_healthy,
        .current_boot_role_acked = s_app.role_acked,
        .required_radio_healthy = backend_scanner_required_radio_healthy(
            profile,
            s_app.runtime.ble_healthy,
            s_app.runtime.wifi_healthy),
    };
    action = scanner_rollback_policy_evaluate(
        &s_app.rollback, &readiness, false);
    app_unlock();

    if (action == SCANNER_ROLLBACK_MARK_VALID &&
        esp_ota_mark_app_valid_cancel_rollback() == ESP_OK && app_lock()) {
        if (scanner_rollback_policy_mark_valid_committed(&s_app.rollback)) {
            s_app.running_pending_verify = false;
            s_app.rollback_clear = true;
        }
        app_unlock();
    } else if (action == SCANNER_ROLLBACK_FORCE_ROLLBACK) {
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void supervisor_task(void *argument)
{
    (void)argument;
    (void)backend_scanner_runtime_wdt_register_current();
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        const bool ble_healthy = s_app.ble_initialized &&
            ble_remote_id_is_active();
        const bool wifi_healthy = s_app.wifi_initialized &&
            wifi_scanner_is_active();
        ble_remote_id_stats_t ble_stats = {0};
        wifi_scanner_stats_t wifi_stats = {0};
        if (s_app.ble_initialized) {
            ble_remote_id_get_stats(&ble_stats);
        }
        if (s_app.wifi_initialized) {
            wifi_scanner_get_stats(&wifi_stats);
        }
        const bool ble_worker_observed = ble_healthy &&
            (ble_stats.ble_scan_start_ok > 0U ||
             ble_stats.ble_any_seen > 0U);
        const bool wifi_worker_observed = wifi_healthy &&
            (wifi_stats.full_scan_count > 0U ||
             wifi_stats.total_frames > 0U);
        bool status_due = false;
        backend_led_state_t led_state = BACKEND_LED_UART_LOST;
        if (app_lock()) {
            backend_scanner_runtime_set_radio_health(
                &s_app.runtime, ble_healthy, wifi_healthy);
            backend_scanner_role_ack_t role_ack;
            if (backend_scanner_role_take_ack(
                    &s_app.runtime.role, ble_healthy,
                    wifi_healthy, &role_ack)) {
                s_app.role_acked = role_ack.boot_id == s_app.boot_id &&
                    role_ack.generation == s_app.runtime.role.generation;
            }
            if (ble_worker_observed) {
                (void)backend_scanner_runtime_worker_iteration(
                    &s_app.runtime, BACKEND_WORKER_BLE_RADIO);
            }
            if (wifi_worker_observed) {
                (void)backend_scanner_runtime_worker_iteration(
                    &s_app.runtime, BACKEND_WORKER_WIFI_RADIO);
            }
            status_due = backend_scanner_runtime_status_due(
                &s_app.runtime, now_ms);
            led_state = backend_led_mirror_effective(
                &s_app.led_mirror, now_ms);
            app_unlock();
        }
        (void)backend_status_led_set_state(led_state);
        evaluate_rollback(now_ms);
        maybe_emit_health();
        if (status_due && send_status() && app_lock()) {
            (void)backend_scanner_runtime_status_sent(
                &s_app.runtime, now_ms);
            app_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(APP_SUPERVISOR_PERIOD_MS));
    }
}

static bool initialize_nvs(bool *erased)
{
    *erased = false;
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) {
            return false;
        }
        *erased = true;
        result = nvs_flash_init();
    }
    return result == ESP_OK;
}

static bool initialize_uart(void)
{
    const uart_config_t config = {
        .baud_rate = BACKEND_SCANNER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    const uart_port_t port = (uart_port_t)BACKEND_SCANNER_UART_PORT;
    return uart_param_config(port, &config) == ESP_OK &&
           uart_set_pin(
               port,
               BACKEND_SCANNER_UART_TX_GPIO,
               BACKEND_SCANNER_UART_RX_GPIO,
               UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE) == ESP_OK &&
           uart_driver_install(
               port, APP_UART_BUFFER_BYTES,
               APP_UART_BUFFER_BYTES, 0, NULL, 0) == ESP_OK;
}

static bool initialize_running_ota_state(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK ||
        (state != ESP_OTA_IMG_VALID &&
         state != ESP_OTA_IMG_PENDING_VERIFY)) {
        return false;
    }
    s_app.running_pending_verify = state == ESP_OTA_IMG_PENDING_VERIFY;
    s_app.rollback_clear = state == ESP_OTA_IMG_VALID;
    return true;
}

static uint32_t generate_boot_id(void)
{
    uint32_t boot_id = 0U;
    while (boot_id == 0U) {
        boot_id = esp_random();
    }
    return boot_id;
}

static bool initialize_identity(void)
{
    if (!backend_identity_record_validate(
            &fof_backend_embedded_identity)) {
        return false;
    }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return false;
    }
    const int written = snprintf(
        s_app.mac, sizeof(s_app.mac),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return written == 17;
}

static bool create_task(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    UBaseType_t priority,
    BaseType_t core)
{
    return xTaskCreatePinnedToCore(
        task, name, stack_depth, NULL, priority,
        NULL, core) == pdPASS;
}

void app_main(void)
{
    memset(&s_app, 0, sizeof(s_app));
    s_app.component_slot = -1;
    atomic_init(&s_rx_errors, 0U);
    atomic_init(&s_tx_drops, 0U);

    if (!initialize_nvs(&s_app.nvs_erased)) {
        ESP_LOGE(TAG, "NVS initialization failed");
        return;
    }
    if (!backend_status_led_init(BACKEND_LED_UART_LOST)) {
        ESP_LOGE(TAG, "GPIO21 yellow LED initialization failed");
        return;
    }
    if (!initialize_running_ota_state()) {
        ESP_LOGE(TAG, "Initial image is not ESP_OTA_IMG_VALID");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }
    s_app.boot_id = generate_boot_id();
    if (!initialize_identity() ||
        !backend_scanner_runtime_init(&s_app.runtime, s_app.boot_id) ||
        !scanner_rollback_policy_init(
            &s_app.rollback, s_app.boot_id,
            s_app.running_pending_verify)) {
        ESP_LOGE(TAG, "Identity or rollback initialization failed");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }
    if (s_app.running_pending_verify &&
        reset_was_crash(esp_reset_reason())) {
        const scanner_rollback_action_t action =
            scanner_rollback_policy_evaluate(
                &s_app.rollback, NULL, true);
        if (action == SCANNER_ROLLBACK_FORCE_ROLLBACK) {
            (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }

    s_app.state_mutex = xSemaphoreCreateMutex();
    s_app.uart_mutex = xSemaphoreCreateMutex();
    s_app.detection_queue = xQueueCreate(
        APP_DETECTION_QUEUE_LENGTH, sizeof(app_detection_item_t));
    s_app.control_queue = xQueueCreate(
        APP_CONTROL_QUEUE_LENGTH, sizeof(app_command_item_t));
    if (s_app.state_mutex == NULL || s_app.uart_mutex == NULL ||
        s_app.detection_queue == NULL || s_app.control_queue == NULL ||
        !initialize_uart()) {
        ESP_LOGE(TAG, "Required queue, mutex, or UART initialization failed");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }

    const backend_uart_rx_callbacks_t callbacks = {
        .control = control_callback,
        .investigate = investigate_callback,
        .cancel = cancel_callback,
        .binary_active = ota_binary_active,
    };
    const uart_ota_config_t ota_config = {
        .running_version = FOF_VERSION_BACKEND,
        .inactive_slot_capacity = FOF_BACKEND_SCANNER_OTA_CAPACITY,
        .ops = {
            .context = &s_app,
            .read_binding = ota_read_binding,
            .psram_acquire = ota_psram_acquire,
            .psram_release = ota_psram_release,
            .emit_receipt = ota_emit_receipt,
            .inactive_slot_begin = ota_inactive_begin,
            .inactive_slot_write = ota_inactive_write,
            .inactive_slot_finish = ota_inactive_finish,
            .inactive_slot_abort = ota_inactive_abort,
            .inactive_slot_activate_pending_verify =
                ota_activate_pending_verify,
            .request_reboot = ota_request_reboot,
        },
    };
    if (!backend_uart_rx_init(&s_app.uart_rx, &callbacks, &s_app) ||
        !backend_uart_tx_init(
            &s_app.investigation_tx,
            investigation_uart_write, &s_app) ||
        !uart_ota_init(&s_app.ota, &ota_config)) {
        ESP_LOGE(TAG, "UART protocol initialization failed");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }

    backend_led_mirror_init(&s_app.led_mirror);
    bayesian_fusion_init();
    backend_detection_sink_register(detection_consumer, &s_app);
    backend_investigation_sink_register(investigation_consumer, &s_app);

    if (!create_task(
            uart_rx_task, "be_uart_rx", UART_CMD_TASK_STACK_SIZE,
            UART_CMD_TASK_PRIORITY, UART_CMD_TASK_CORE)) {
        ESP_LOGE(TAG, "UART ingress task creation failed");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }
    s_app.command_ingress_healthy = true;
    s_app.command_ingress_boot_id = s_app.boot_id;

    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    if (identity == NULL) {
        ESP_LOGE(TAG, "Scanner identity unavailable");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }
    const int boot_written = snprintf(
        s_app.boot_record, sizeof(s_app.boot_record),
        "FOF_BACKEND_BOOT {\"product_family\":\"%s\","
        "\"firmware_line\":\"%s\",\"component\":\"%s\","
        "\"target\":\"%s\",\"project\":\"%s\","
        "\"hardware\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\","
        "\"boot_id\":%" PRIu32 ",\"nvs_erased\":%s,"
        "\"role\":\"%s\",\"uart_ingress\":true,\"ota_state\":\"%s\"}",
        identity->product_family, identity->firmware_line,
        identity->component, identity->target, identity->project,
        identity->hardware, identity->version, s_app.mac,
        s_app.boot_id, s_app.nvs_erased ? "true" : "false",
        profile_name(backend_scanner_runtime_profile(&s_app.runtime)),
        s_app.running_pending_verify ? "pending_verify" : "valid");
    if (boot_written <= 0 ||
        (size_t)boot_written >= sizeof(s_app.boot_record)) {
        ESP_LOGE(TAG, "Boot record encoding failed");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
        return;
    }
    emit_usb_record(s_app.boot_record);

    const bool tasks_created =
        create_task(
            control_task, "be_control", UART_CMD_TASK_STACK_SIZE,
            UART_CMD_TASK_PRIORITY, UART_CMD_TASK_CORE) &&
        create_task(
            detection_tx_task, "be_detect_tx", UART_TX_TASK_STACK_SIZE,
            UART_TX_TASK_PRIORITY, UART_TX_TASK_CORE) &&
        create_task(
            supervisor_task, "be_supervisor", 6144U,
            FUSION_TASK_PRIORITY, CORE_PROCESSING) &&
        create_task(
            usb_status_task, "be_usb_status", 4096U,
            UART_TX_TASK_PRIORITY, CORE_PROCESSING);
    if (!tasks_created) {
        ESP_LOGE(TAG, "Required backend scanner task creation failed");
        (void)backend_status_led_set_state(BACKEND_LED_FATAL);
    }
}
