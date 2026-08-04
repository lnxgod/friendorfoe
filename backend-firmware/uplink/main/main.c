#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#include "freertos/queue.h"
#endif
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "backend_ap_policy.h"
#include "backend_ble_investigation.h"
#include "backend_command_client.h"
#include "backend_config_portal.h"
#include "backend_coordinator.h"
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
#include "backend_dashboard_event.h"
#include "backend_event_ring.h"
#include "backend_lite_ap_policy.h"
#endif
#include "backend_detection_codec.h"
#include "backend_detection_router.h"
#include "backend_firmware_buffer.h"
#include "backend_firmware_store.h"
#include "backend_health.h"
#include "backend_http_policy.h"
#include "backend_http_transport.h"
#include "backend_identity.h"
#include "backend_ingest_ack.h"
#include "backend_json_reader.h"
#include "backend_json_writer.h"
#include "backend_led_pattern.h"
#include "backend_nvs_config.h"
#include "backend_ota_maintenance.h"
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#include "backend_ota_command_client.h"
#include "backend_ota_event_outbox.h"
#include "backend_ota_workflow.h"
#endif
#include "backend_scanner_control_codec.h"
#include "backend_scanner_relay.h"
#include "backend_scanner_status_codec.h"
#include "backend_scanner_topology.h"
#include "backend_self_ota.h"
#include "backend_threat_policy.h"
#include "backend_time_sync.h"
#include "backend_uart_investigation.h"
#include "backend_uart_slot.h"
#include "production_scanner_uart.h"
#include "backend_upload_batch.h"
#include "backend_upload_fifo.h"
#include "backend_uploader.h"
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
#include "backend_usb_config.h"
#include "backend_usb_protocol.h"
#include "backend_usb_service.h"
#endif
#include "backend_wifi_manager.h"
#include "backend_status_led.h"
#include "psram_alloc.h"

#define UPLINK_FACTORY_URL "http://192.168.4.2:8000"
#define UPLINK_SCANNER_STALE_MS INT64_C(15000)
#define UPLINK_COORDINATOR_PERIOD_MS 100
#define UPLINK_NETWORK_PERIOD_MS 500
#define UPLINK_UPLOAD_PERIOD_MS 250
#define UPLINK_TIME_PERIOD_MS 10000
#define UPLINK_OTA_PERIOD_MS 1000
#define UPLINK_RELAY_WAIT_MS 180000
#define UPLINK_UART_TASK_STACK_DEPTH 12288U
#define UPLINK_PRODUCTION_BOOTSTRAP_STACK_DEPTH 3072U
#define UPLINK_PRODUCTION_BOOTSTRAP_WINDOW_MS INT64_C(8000)
#define UPLINK_PRODUCTION_BOOTSTRAP_PERIOD_MS 250U
#define UPLINK_USB_LINE_CAPACITY 512U
#define UPLINK_STATUS_CAPACITY 768U
#define UPLINK_HTTP_RESPONSE_CAPACITY (BACKEND_HTTP_MAX_JSON_BODY + 1U)
#define UPLINK_JOURNAL_NAMESPACE "fof_backend"
#define UPLINK_JOURNAL_KEY BACKEND_OTA_JOURNAL_STORAGE_KEY
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#define UPLINK_OTA_EVENT_SLOT0_KEY "ota_evt0"
#define UPLINK_OTA_EVENT_SLOT1_KEY "ota_evt1"
#define UPLINK_TOPOLOGY_EPOCH_KEY "topo_epoch"
#define UPLINK_OTA_RESULT_QUEUE_LENGTH 4U
#define UPLINK_OTA_PROGRESS_QUEUE_LENGTH 1U
#define UPLINK_OTA_CLIENT_PERIOD_MS 500U
#define UPLINK_OTA_CLIENT_PATH_CAPACITY 192U
#endif

typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    size_t expected_size;
    bool begun;
    bool ended;
} uplink_self_ota_context_t;

typedef struct {
    uint8_t *destination;
    size_t capacity;
    size_t length;
} uplink_download_sink_t;

typedef struct {
    const uint8_t *bytes;
    size_t length;
} uplink_memory_image_t;

typedef enum {
    UPLINK_SCANNER_WIRE_UNKNOWN = 0,
    UPLINK_SCANNER_WIRE_BACKEND,
    UPLINK_SCANNER_WIRE_PRODUCTION,
} uplink_scanner_wire_t;

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
typedef struct {
    backend_ota_command_envelope_t command;
    bool resume;
    bool restart;
} uplink_ota_work_item_t;

typedef struct {
    backend_ota_command_envelope_t command;
    backend_ota_evidence_t evidence;
    fof_firmware_version_relation_t relation;
    char running_version[65];
    bool call_succeeded;
} uplink_ota_result_item_t;
#endif

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t coordinator_lock;
    SemaphoreHandle_t upload_build_lock;
    SemaphoreHandle_t http_lock;
    SemaphoreHandle_t usb_lock;
    SemaphoreHandle_t ota_lock;
    SemaphoreHandle_t uart_tx_lock[BACKEND_UART_SLOT_COUNT];
    SemaphoreHandle_t relay_complete;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    SemaphoreHandle_t event_ring_lock;
    SemaphoreHandle_t config_transaction_lock;
#endif
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    QueueHandle_t ota_work_queue;
    QueueHandle_t ota_result_queue;
    QueueHandle_t ota_progress_queue;
    SemaphoreHandle_t ota_progress_ack;
#endif

    backend_config_record_t config;
    backend_config_portal_t portal;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    backend_lite_ap_policy_t lite_ap_policy;
    backend_usb_service_t usb;
    backend_usb_config_t usb_config;
    backend_event_ring_t event_ring;
    backend_dashboard_event_t *event_storage;
    atomic_uint_fast64_t event_ring_contention_drops;
#else
    backend_ap_policy_t ap_policy;
#endif
    backend_wifi_manager_t wifi;
    backend_uart_slots_t uarts;
    backend_coordinator_t coordinator;
    backend_threat_state_t threats;
    backend_scanner_status_tracker_t scanner_tracker[2];
    backend_scanner_health_t scanner_health[2];
    production_scanner_message_t production_scanner[2];
    uplink_scanner_wire_t scanner_wire[2];
    backend_scanner_plan_t scanner_plan;
    int64_t scanner_last_seen_ms[2];
    int64_t production_ready_sent_ms[2];
    uint32_t production_boot_id[2];
    uint32_t production_status_sequence[2];
    backend_upload_fifo_t upload_fifo;
    backend_upload_batch_t *upload_storage;
    backend_batch_context_t upload_context_scratch;
    backend_upload_batch_t upload_batch_scratch;
    backend_upload_batch_t upload_request_scratch;
    backend_upload_builder_t upload_builder;
    backend_uploader_state_t uploader;
    backend_heartbeat_state_t heartbeat;
    backend_ble_investigation_state_t investigation;
    backend_command_client_state_t command_client;
    backend_command_http_state_t command_http;
    backend_self_ota_t self_ota;
    uplink_self_ota_context_t self_ota_context;
    backend_firmware_buffer_t firmware_buffer;
    backend_firmware_store_t firmware_store;
    backend_ota_maintenance_t maintenance;
    backend_ota_poll_state_t ota_poll[3];
    backend_scanner_relay_t relay;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_event_outbox_storage_t ota_outbox_storage;
    backend_ota_event_outbox_snapshot_t ota_startup_outbox;
    backend_ota_journal_record_t ota_startup_journal;
    backend_ota_journal_startup_action_t ota_startup_action;
    backend_ota_workflow_t ota_workflow;
    uplink_ota_result_item_t ota_result_scratch;
    char ota_client_http_response[UPLINK_HTTP_RESPONSE_CAPACITY];
    bool ota_terminal_event_pending;
    backend_ota_terminal_outcome_t ota_terminal_outcome;
    char ota_terminal_receipt_sha256[65];
    bool ota_progress_event_pending;
    bool ota_progress_waiter;
    backend_ota_progress_update_t ota_pending_progress;
#endif

    uint8_t mac[6];
    char mac_text[18];
    uint32_t boot_id;
    uint32_t next_batch_sequence;
    uint32_t role_generation;
    uint32_t topology_generation;
    uint32_t time_generation;
    uint32_t flow_generation;
    uint32_t command_success_count;
    uint32_t command_failure_count;
    uint32_t catalog_generation;
    uint32_t relay_quiet_generation;
    uint32_t relay_expected_role_generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    uint32_t relay_session_generation;
#endif
    int64_t boot_monotonic_ms;
    int64_t epoch_anchor_ms;
    int64_t epoch_anchor_monotonic_ms;
    int8_t wifi_rssi;
    char wifi_ssid[33];
    bool epoch_valid;
    bool config_loaded;
    bool portal_started;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    bool history_available;
    bool usb_available;
#else
    bool usb_ap_start_requested;
#endif
    bool wifi_initialized;
    bool wifi_connected;
    bool backend_reachable;
    bool led_started;
    bool uart_started[2];
    bool uart_worker_live[2];
    bool coordinator_worker_live;
    bool network_worker_live;
    bool uploader_worker_live;
    bool time_worker_live;
    bool command_worker_live;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool ota_client_worker_live;
#endif
    bool ota_worker_live;
    bool usb_worker_live;
    bool fatal_runtime;
    bool flow_sent_paused;
    bool flow_state_known;
    bool ota_ready;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool ota_client_blocked;
#endif
    bool relay_active;
    bool relay_result;
    bool target_claimed[3];
    char boot_line[UPLINK_STATUS_CAPACITY];
    char health_line[UPLINK_STATUS_CAPACITY];
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    char usb_status_scratch[BACKEND_USB_STATUS_MAX];
    char usb_print_scratch[BACKEND_USB_STATUS_MAX];
#endif
    char portal_http_response[UPLINK_HTTP_RESPONSE_CAPACITY];
    char uploader_http_response[UPLINK_HTTP_RESPONSE_CAPACITY];
    char time_http_response[UPLINK_HTTP_RESPONSE_CAPACITY];
    char command_http_response[UPLINK_HTTP_RESPONSE_CAPACITY];
    bool health_line_ready;
    char uart_control_line[2][BACKEND_SCANNER_WIRE_MAX_LINE + 1U];
} uplink_runtime_t;

static EXT_RAM_BSS_ATTR uplink_runtime_t s_runtime;

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / INT64_C(1000);
}

static void lock_runtime(void)
{
    (void)xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
}

static void unlock_runtime(void)
{
    (void)xSemaphoreGive(s_runtime.lock);
}

static void lock_coordinator(void)
{
    (void)xSemaphoreTake(s_runtime.coordinator_lock, portMAX_DELAY);
}

static void unlock_coordinator(void)
{
    (void)xSemaphoreGive(s_runtime.coordinator_lock);
}

static void copy_text(char *output, size_t capacity, const char *value)
{
    if (output == NULL || capacity == 0U) {
        return;
    }
    if (value == NULL) {
        output[0] = '\0';
        return;
    }
    (void)snprintf(output, capacity, "%s", value);
}

static void format_mac(const uint8_t mac[6], char output[18])
{
    (void)snprintf(
        output,
        18U,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_mac(const char *value, uint8_t output[6])
{
    unsigned octets[6];
    char trailing = '\0';
    if (value == NULL || output == NULL ||
        sscanf(
            value,
            "%2x:%2x:%2x:%2x:%2x:%2x%c",
            &octets[0], &octets[1], &octets[2],
            &octets[3], &octets[4], &octets[5], &trailing) != 6) {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        output[index] = (uint8_t)octets[index];
    }
    return true;
}

static int64_t current_epoch_ms_locked(int64_t now_ms)
{
    if (!s_runtime.epoch_valid || now_ms < s_runtime.epoch_anchor_monotonic_ms) {
        return 0;
    }
    return s_runtime.epoch_anchor_ms +
           (now_ms - s_runtime.epoch_anchor_monotonic_ms);
}

static const char *network_state_locked(void)
{
    return s_runtime.wifi_connected ? "sta" :
           s_runtime.portal_started ? "ap" : "down";
}

static const char *led_state_name(backend_led_state_t state)
{
    switch (state) {
    case BACKEND_LED_HEALTHY: return "healthy";
    case BACKEND_LED_NETWORK_DEGRADED: return "network_degraded";
    case BACKEND_LED_DRONE: return "drone";
    case BACKEND_LED_META: return "meta";
    case BACKEND_LED_DRONE_META: return "drone_meta";
    case BACKEND_LED_FATAL: return "fatal";
    case BACKEND_LED_UART_LOST: return "uart_lost";
    default: return NULL;
    }
}

static bool print_line(const char *line)
{
    if (line == NULL || s_runtime.usb_lock == NULL ||
        xSemaphoreTake(s_runtime.usb_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    const size_t length = strlen(line);
    bool emitted = false;
    if (s_runtime.usb_available &&
        length + 1U <= sizeof(s_runtime.usb_print_scratch)) {
        memcpy(s_runtime.usb_print_scratch, line, length);
        s_runtime.usb_print_scratch[length] = '\n';
        emitted = backend_usb_service_emit(
            &s_runtime.usb,
            BACKEND_USB_FRAME_REQUIRED,
            s_runtime.usb_print_scratch,
            length + 1U);
    }
    (void)xSemaphoreGive(s_runtime.usb_lock);
    return emitted;
#else
    const int result = printf("%s\n", line);
    fflush(stdout);
    (void)xSemaphoreGive(s_runtime.usb_lock);
    return result >= 0;
#endif
}

static bool uart_write_locked(size_t slot, const void *bytes, size_t length)
{
    if (slot >= BACKEND_UART_SLOT_COUNT || bytes == NULL || length == 0U ||
        s_runtime.uart_tx_lock[slot] == NULL ||
        xSemaphoreTake(s_runtime.uart_tx_lock[slot], portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const uart_port_t port =
        (uart_port_t)s_runtime.uarts.slot[slot].config.uart;
    const int written = uart_write_bytes(port, bytes, length);
    (void)xSemaphoreGive(s_runtime.uart_tx_lock[slot]);
    return written == (int)length;
}

static bool uart_send_line(size_t slot, const char *line, size_t length)
{
    if (slot >= BACKEND_UART_SLOT_COUNT || line == NULL || length == 0U ||
        length >= BACKEND_SCANNER_WIRE_MAX_LINE) {
        return false;
    }
    if (xSemaphoreTake(s_runtime.uart_tx_lock[slot], portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const uart_port_t port =
        (uart_port_t)s_runtime.uarts.slot[slot].config.uart;
    const int body_written = uart_write_bytes(port, line, length);
    const int newline_written = body_written == (int)length
        ? uart_write_bytes(port, "\n", 1U) : -1;
    (void)xSemaphoreGive(s_runtime.uart_tx_lock[slot]);
    return body_written == (int)length && newline_written == 1;
}

static bool uart_send_control(
    size_t slot, const backend_scanner_control_t *control)
{
    if (slot >= BACKEND_UART_SLOT_COUNT || control == NULL) {
        return false;
    }
    char *line = s_runtime.uart_control_line[slot];
    const size_t length = backend_scanner_control_encode(
        control, line, sizeof(s_runtime.uart_control_line[slot]));
    return length != 0U && uart_send_line(slot, line, length);
}

static bool uart_send_production_ready(size_t slot, int64_t now_ms)
{
    if (slot >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
    char *line = s_runtime.uart_control_line[slot];
    const size_t length = production_scanner_encode_ready(
        line, sizeof(s_runtime.uart_control_line[slot]));
    const bool sent = length != 0U && uart_send_line(slot, line, length);
    if (sent) {
        s_runtime.production_ready_sent_ms[slot] = now_ms;
    }
    return sent;
}

static bool uart_send_production_flow(size_t slot, bool paused, int64_t now_ms)
{
    if (slot >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
    char *line = s_runtime.uart_control_line[slot];
    const size_t length = paused
        ? production_scanner_encode_stop(
              line, sizeof(s_runtime.uart_control_line[slot]))
        : production_scanner_encode_ready(
              line, sizeof(s_runtime.uart_control_line[slot]));
    const bool sent = length != 0U && uart_send_line(slot, line, length);
    if (sent && !paused) {
        s_runtime.production_ready_sent_ms[slot] = now_ms;
    }
    return sent;
}

static backend_scan_profile_t production_profile_for_slot(size_t slot)
{
    return slot == 0U
        ? BACKEND_SCAN_PROFILE_BLE_PRIMARY
        : BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
}

static bool uart_send_production_assignment(size_t slot)
{
    if (slot >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
    const backend_scan_profile_t profile = production_profile_for_slot(slot);
    char *line = s_runtime.uart_control_line[slot];
    const size_t length = production_scanner_encode_profile(
        profile, line, sizeof(s_runtime.uart_control_line[slot]));
    return length != 0U && uart_send_line(slot, line, length);
}

static bool uart_send_production_profile(size_t slot, int64_t now_ms)
{
    return uart_send_production_assignment(slot) &&
        uart_send_production_ready(slot, now_ms);
}

static bool uart_send_production_time(
    size_t slot, int64_t epoch_ms, backend_time_source_t source)
{
    if (slot >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
    const char *source_name = source == BACKEND_TIME_SOURCE_SNTP
        ? "local" : "backend";
    char *line = s_runtime.uart_control_line[slot];
    const size_t length = production_scanner_encode_time(
        epoch_ms, source_name, line,
        sizeof(s_runtime.uart_control_line[slot]));
    return length != 0U && uart_send_line(slot, line, length);
}

static void production_scanner_bootstrap_worker(void *argument)
{
    (void)argument;
    const int64_t started_ms = monotonic_ms();
    uint32_t iteration = 0U;
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        if (now_ms < started_ms ||
            now_ms - started_ms >= UPLINK_PRODUCTION_BOOTSTRAP_WINDOW_MS) {
            break;
        }
        lock_runtime();
        for (size_t slot = 0U; slot < BACKEND_UART_SLOT_COUNT; ++slot) {
            if (s_runtime.uart_started[slot] &&
                s_runtime.scanner_wire[slot] == UPLINK_SCANNER_WIRE_UNKNOWN) {
                const bool assigned = uart_send_production_assignment(slot);
                if (assigned && (iteration % 4U) == 0U) {
                    (void)uart_send_production_ready(slot, now_ms);
                }
            }
        }
        unlock_runtime();
        ++iteration;
        vTaskDelay(pdMS_TO_TICKS(UPLINK_PRODUCTION_BOOTSTRAP_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

static bool scanner_identity_exact(const backend_scanner_status_t *status)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    return status != NULL && identity != NULL &&
           backend_identity_matches(
               identity, status->target, status->project, status->hardware) &&
           strcmp(status->version, FOF_VERSION_BACKEND) == 0;
}

static bool upload_context_locked(
    uint32_t sequence, backend_batch_context_t *context)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    if (context == NULL || identity == NULL || sequence == 0U) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    copy_text(context->device_id, sizeof(context->device_id),
              s_runtime.config.device_id);
    context->boot_id = s_runtime.boot_id;
    context->topology_generation = s_runtime.topology_generation;
    copy_text(context->product_family, sizeof(context->product_family),
              identity->product_family);
    copy_text(context->firmware_line, sizeof(context->firmware_line),
              identity->firmware_line);
    copy_text(context->component, sizeof(context->component),
              identity->component);
    copy_text(context->firmware_version, sizeof(context->firmware_version),
              identity->version);
    copy_text(context->firmware_target, sizeof(context->firmware_target),
              identity->target);
    copy_text(context->app_project, sizeof(context->app_project),
              identity->project);
    copy_text(context->hardware_type, sizeof(context->hardware_type),
              identity->hardware);
    copy_text(context->hardware_mac, sizeof(context->hardware_mac),
              s_runtime.mac_text);
    copy_text(context->node_name, sizeof(context->node_name),
              s_runtime.config.display_name[0] != '\0'
                  ? s_runtime.config.display_name : s_runtime.config.device_id);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    static const char *const capabilities[] = {
        "display_none", "rgb_led", "scanner_uart", "http_uplink",
        "config_ap", "remote_ota", "uart_relay_ota",
    };
#else
    static const char *const capabilities[] = {
        "display_none", "yellow_led", "scanner_uart", "http_uplink",
        "config_ap", "remote_ota", "uart_relay_ota",
    };
#endif
    for (size_t index = 0U;
         index < sizeof(capabilities) / sizeof(capabilities[0]); ++index) {
        copy_text(context->capabilities[index],
                  sizeof(context->capabilities[index]), capabilities[index]);
    }
    context->capability_count = sizeof(capabilities) / sizeof(capabilities[0]);
    context->has_device_location = s_runtime.config.has_location;
    context->device_lat = s_runtime.config.latitude;
    context->device_lon = s_runtime.config.longitude;
    context->device_alt = s_runtime.config.altitude_m;
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (s_runtime.scanner_tracker[slot].initialized) {
            context->scanner_present[slot] = true;
            context->scanners[slot] =
                s_runtime.scanner_tracker[slot].status;
        }
    }
    const int64_t now_ms = monotonic_ms();
    context->epoch_ms = current_epoch_ms_locked(now_ms);
    context->clock_valid =
        context->epoch_ms > BACKEND_DETECTION_EPOCH_MIN_MS;
    copy_text(context->wifi_ssid, sizeof(context->wifi_ssid),
              s_runtime.wifi_ssid);
    context->wifi_rssi = s_runtime.wifi_rssi;
    context->ap_active = s_runtime.portal_started;
    context->config_generation = s_runtime.config.generation;
    context->command_success_count = s_runtime.command_success_count;
    context->command_failure_count = s_runtime.command_failure_count;
    context->uptime_ms = now_ms >= s_runtime.boot_monotonic_ms
        ? (uint64_t)(now_ms - s_runtime.boot_monotonic_ms) : 0U;
    context->led_state = backend_status_led_state();
    context->upload_queue.depth_batches = s_runtime.upload_fifo.count;
    context->upload_queue.capacity_batches = s_runtime.upload_fifo.capacity;
    context->upload_queue.overflow_dropped_batches =
        s_runtime.upload_fifo.dropped_batches;
    context->upload_queue.quarantined_batches =
        s_runtime.uploader.quarantine_count;
    context->upload.ok = s_runtime.uploader.ack_count;
    context->upload.failed = s_runtime.uploader.quarantine_count;
    context->upload.retry_count = s_runtime.uploader.retry_count;
    if (s_runtime.uploader.ack_count != 0U &&
        s_runtime.uploader.last_backend_success_ms >= 0 &&
        now_ms >= s_runtime.uploader.last_backend_success_ms) {
        context->upload.has_last_success_age = true;
        context->upload.last_success_age_s = (uint32_t)(
            (now_ms - s_runtime.uploader.last_backend_success_ms) / 1000);
    }
    context->sequence = sequence;
    return true;
}

static bool queue_upload(
    const backend_detection_observation_t *observation)
{
    if (xSemaphoreTake(
            s_runtime.upload_build_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    lock_runtime();
    const uint32_t sequence = s_runtime.next_batch_sequence;
    if (!upload_context_locked(sequence, &s_runtime.upload_context_scratch)) {
        unlock_runtime();
        (void)xSemaphoreGive(s_runtime.upload_build_lock);
        return false;
    }
    unlock_runtime();
    backend_upload_builder_init(
        &s_runtime.upload_builder, &s_runtime.upload_context_scratch,
        monotonic_ms());
    if (observation != NULL &&
        backend_upload_builder_add(
            &s_runtime.upload_builder, observation, monotonic_ms()) !=
            BACKEND_ENCODE_OK) {
        (void)xSemaphoreGive(s_runtime.upload_build_lock);
        return false;
    }
    if (!backend_upload_builder_finish(
            &s_runtime.upload_builder, &s_runtime.upload_batch_scratch)) {
        (void)xSemaphoreGive(s_runtime.upload_build_lock);
        return false;
    }
    lock_runtime();
    uint32_t dropped_sequence = 0U;
    uint32_t dropped_crc = 0U;
    const backend_upload_batch_t *old_head =
        backend_upload_fifo_peek(&s_runtime.upload_fifo);
    if (s_runtime.upload_fifo.count == s_runtime.upload_fifo.capacity &&
        old_head != NULL) {
        dropped_sequence = old_head->sequence;
        dropped_crc = old_head->json_crc32;
    }
    bool dropped = false;
    if (!backend_upload_fifo_push(
            &s_runtime.upload_fifo, &s_runtime.upload_batch_scratch, &dropped)) {
        unlock_runtime();
        (void)xSemaphoreGive(s_runtime.upload_build_lock);
        return false;
    }
    (void)backend_uploader_note_enqueued(
        &s_runtime.uploader,
        s_runtime.upload_fifo.count,
        dropped,
        dropped_sequence,
        dropped_crc);
    s_runtime.next_batch_sequence = sequence == UINT32_MAX
        ? 1U : sequence + 1U;
    unlock_runtime();
    (void)xSemaphoreGive(s_runtime.upload_build_lock);
    return true;
}

static bool coordinator_upload_sink(
    void *context, const backend_detection_observation_t *observation)
{
    (void)context;
    return queue_upload(observation);
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static void coordinator_canonical_sink(
    void *context, const backend_detection_observation_t *observation)
{
    (void)context;
    backend_dashboard_event_t event;
    if (!backend_dashboard_event_project(observation, &event)) {
        return;
    }
    if (s_runtime.history_available &&
        xSemaphoreTake(s_runtime.event_ring_lock, 0) == pdTRUE) {
        (void)backend_event_ring_append(&s_runtime.event_ring, &event);
        (void)xSemaphoreGive(s_runtime.event_ring_lock);
    } else if (s_runtime.history_available) {
        atomic_fetch_add_explicit(
            &s_runtime.event_ring_contention_drops,
            1U,
            memory_order_relaxed);
    }

    char frame[BACKEND_USB_DET_MAX];
    const size_t length = backend_dashboard_event_encode_fof_det(
        &event, frame, sizeof(frame));
    if (length != 0U && s_runtime.usb_available) {
        (void)backend_usb_service_emit(
            &s_runtime.usb,
            BACKEND_USB_FRAME_OPTIONAL,
            frame,
            length);
    }
}
#endif

static bool portal_commit(
    void *context, const backend_config_record_t *candidate)
{
    (void)context;
    if (!backend_config_commit(candidate)) {
        return false;
    }
    lock_runtime();
    s_runtime.config = *candidate;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ap_policy_note_config_commit(
        &s_runtime.ap_policy, candidate->generation, monotonic_ms());
#endif
    unlock_runtime();
    return true;
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
static bool begin_config_transaction(void *context)
{
    (void)context;
    return xSemaphoreTakeRecursive(
        s_runtime.config_transaction_lock, portMAX_DELAY) == pdTRUE;
}

static void end_config_transaction(void *context)
{
    (void)context;
    (void)xSemaphoreGiveRecursive(s_runtime.config_transaction_lock);
}

static bool disconnect_lite_station(void)
{
    const esp_err_t result = esp_wifi_disconnect();
    return result == ESP_OK || result == ESP_ERR_WIFI_NOT_CONNECT;
}

static void reset_lite_wifi_state_locked(void)
{
    backend_wifi_manager_reset(&s_runtime.wifi);
    s_runtime.wifi_initialized = false;
    s_runtime.wifi_connected = false;
    s_runtime.backend_reachable = false;
    s_runtime.wifi_rssi = 0;
    s_runtime.wifi_ssid[0] = '\0';
}

static bool acquire_lite_http_if_sta_usable(void)
{
    if (xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    lock_runtime();
    const bool station_usable = backend_lite_network_can_use_sta(
        s_runtime.config.network_count,
        s_runtime.wifi_initialized,
        s_runtime.wifi_connected);
    unlock_runtime();
    if (!station_usable) {
        (void)xSemaphoreGive(s_runtime.http_lock);
        return false;
    }
    return true;
}

static void release_lite_http(void)
{
    (void)xSemaphoreGive(s_runtime.http_lock);
}
#endif

static bool portal_reconnect(
    void *context,
    const backend_config_record_t *committed,
    int64_t now_ms)
{
    (void)context;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (committed->network_count == 0U) {
        /* Lite HTTP and zero-network reconnects share http -> runtime order. */
        (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
        lock_runtime();
        const bool result = disconnect_lite_station();
        reset_lite_wifi_state_locked();
        unlock_runtime();
        (void)xSemaphoreGive(s_runtime.http_lock);
        return result;
    }
#endif
    lock_runtime();
    bool result = true;
    if (committed->network_count == 0U) {
        s_runtime.wifi_initialized = false;
        s_runtime.wifi_connected = false;
    } else if (s_runtime.wifi_initialized) {
        result = backend_wifi_manager_apply_committed_config(
            &s_runtime.wifi, committed, now_ms);
    } else {
        result = backend_wifi_manager_init(
            &s_runtime.wifi, committed, now_ms);
        s_runtime.wifi_initialized = result;
    }
    unlock_runtime();
    return result;
}

static bool portal_backend_get(
    void *context,
    const char *base_url,
    const char *path,
    uint32_t timeout_ms,
    int *status_code)
{
    (void)context;
    (void)timeout_ms;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (!acquire_lite_http_if_sta_usable()) {
        if (status_code != NULL) {
            *status_code = 0;
        }
        return false;
    }
#else
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
    const backend_http_result_t result = backend_http_get_json(
        base_url, path, s_runtime.portal_http_response,
        sizeof(s_runtime.portal_http_response));
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (result.transport_complete && result.status_code >= 200 &&
        result.status_code < 300) {
        lock_runtime();
        if (backend_lite_network_can_use_sta(
                s_runtime.config.network_count,
                s_runtime.wifi_initialized,
                s_runtime.wifi_connected)) {
            s_runtime.backend_reachable = true;
        }
        unlock_runtime();
    }
    if (status_code != NULL) {
        *status_code = result.status_code;
    }
    release_lite_http();
#else
    (void)xSemaphoreGive(s_runtime.http_lock);
    if (status_code != NULL) {
        *status_code = result.status_code;
    }
    if (result.transport_complete && result.status_code >= 200 &&
        result.status_code < 300) {
        lock_runtime();
        backend_ap_policy_note_backend_success(
            &s_runtime.ap_policy,
            s_runtime.config.generation,
            monotonic_ms());
        s_runtime.backend_reachable = true;
        unlock_runtime();
    }
#endif
    return result.transport_complete;
}

static void wifi_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)context;
    const int64_t now_ms = monotonic_ms();
    lock_runtime();
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        const bool station_allowed = backend_lite_network_can_use_sta(
            s_runtime.config.network_count,
            s_runtime.wifi_initialized,
            true);
        if (!station_allowed) {
            (void)disconnect_lite_station();
            reset_lite_wifi_state_locked();
        } else {
#endif
        s_runtime.wifi_connected = true;
        if (s_runtime.wifi_initialized) {
            (void)backend_wifi_manager_handle_event(
                &s_runtime.wifi, BACKEND_WIFI_EVENT_CONNECTED, now_ms);
        }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        }
#endif
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        backend_wifi_event_t event = BACKEND_WIFI_EVENT_DISCONNECTED;
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        if (disconnected != NULL &&
            (disconnected->reason == WIFI_REASON_AUTH_FAIL ||
             disconnected->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)) {
            event = BACKEND_WIFI_EVENT_AUTH_FAILED;
        } else if (disconnected != NULL &&
                   disconnected->reason == WIFI_REASON_NO_AP_FOUND) {
            event = BACKEND_WIFI_EVENT_NO_AP;
        }
        s_runtime.wifi_connected = false;
        s_runtime.backend_reachable = false;
        s_runtime.wifi_ssid[0] = '\0';
        if (s_runtime.wifi_initialized) {
            (void)backend_wifi_manager_handle_event(
                &s_runtime.wifi, event, now_ms);
        }
    }
    unlock_runtime();
}

static bool initialize_wifi_platform(void)
{
    const esp_err_t netif_result = esp_netif_init();
    if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    const esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL &&
        esp_netif_create_default_wifi_sta() == NULL) {
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        return true;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    return esp_wifi_init(&init_config) == ESP_OK &&
           esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK;
}

static bool self_ota_begin_adapter(void *context, size_t image_size)
{
    uplink_self_ota_context_t *adapter = context;
    if (adapter == NULL || image_size == 0U) {
        return false;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->partition = esp_ota_get_next_update_partition(NULL);
    if (adapter->partition == NULL || image_size > adapter->partition->size ||
        esp_ota_begin(adapter->partition, image_size, &adapter->handle) !=
            ESP_OK) {
        return false;
    }
    adapter->expected_size = image_size;
    adapter->begun = true;
    return true;
}

static bool self_ota_write_adapter(
    void *context, size_t offset, const uint8_t *bytes, size_t length)
{
    uplink_self_ota_context_t *adapter = context;
    return adapter != NULL && adapter->begun && !adapter->ended &&
           bytes != NULL && length != 0U &&
           offset <= adapter->expected_size &&
           length <= adapter->expected_size - offset &&
           esp_ota_write_with_offset(adapter->handle, bytes, length, offset) ==
               ESP_OK;
}

static bool self_ota_end_adapter(void *context)
{
    uplink_self_ota_context_t *adapter = context;
    if (adapter == NULL || !adapter->begun || adapter->ended ||
        esp_ota_end(adapter->handle) != ESP_OK) {
        return false;
    }
    adapter->ended = true;
    return true;
}

static bool self_ota_select_adapter(void *context)
{
    uplink_self_ota_context_t *adapter = context;
    return adapter != NULL && adapter->ended && adapter->partition != NULL &&
           esp_ota_set_boot_partition(adapter->partition) == ESP_OK;
}

static bool self_ota_mark_valid_adapter(void *context)
{
    (void)context;
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

static bool memory_image_read(
    void *context, size_t offset, uint8_t *output, size_t length)
{
    const uplink_memory_image_t *image = context;
    if (image == NULL || output == NULL || length == 0U ||
        offset >= image->length || length > image->length - offset) {
        return false;
    }
    memcpy(output, image->bytes + offset, length);
    return true;
}

static bool store_partition_erase(
    void *context, const char *label, size_t capacity)
{
    (void)context;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    return partition != NULL && capacity <= partition->size &&
           esp_partition_erase_range(partition, 0U, capacity) == ESP_OK;
}

static bool store_partition_write(
    void *context,
    const char *label,
    size_t offset,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    return partition != NULL && bytes != NULL && offset <= partition->size &&
           length <= partition->size - offset &&
           esp_partition_write(partition, offset, bytes, length) == ESP_OK;
}

static bool store_partition_read(
    void *context,
    const char *label,
    size_t offset,
    uint8_t *output,
    size_t length)
{
    (void)context;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    return partition != NULL && output != NULL && offset <= partition->size &&
           length <= partition->size - offset &&
           esp_partition_read(partition, offset, output, length) == ESP_OK;
}

static backend_ota_journal_io_result_t journal_load(
    void *context, uint8_t *output, size_t capacity, size_t *out_length)
{
    (void)context;
    if (output == NULL || out_length == NULL) {
        return BACKEND_OTA_JOURNAL_IO_ERROR;
    }
    nvs_handle_t handle;
    if (nvs_open(UPLINK_JOURNAL_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return BACKEND_OTA_JOURNAL_IO_NOT_FOUND;
    }
    size_t required = 0U;
    esp_err_t result = nvs_get_blob(handle, UPLINK_JOURNAL_KEY, NULL, &required);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return BACKEND_OTA_JOURNAL_IO_NOT_FOUND;
    }
    if (result != ESP_OK || required == 0U || required > capacity) {
        nvs_close(handle);
        return BACKEND_OTA_JOURNAL_IO_ERROR;
    }
    size_t actual = required;
    result = nvs_get_blob(handle, UPLINK_JOURNAL_KEY, output, &actual);
    nvs_close(handle);
    if (result != ESP_OK || actual != required) {
        return BACKEND_OTA_JOURNAL_IO_ERROR;
    }
    *out_length = actual;
    return BACKEND_OTA_JOURNAL_IO_OK;
}

static bool journal_store(
    void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    nvs_handle_t handle;
    if (bytes == NULL || length == 0U ||
        nvs_open(UPLINK_JOURNAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool result =
        nvs_set_blob(handle, UPLINK_JOURNAL_KEY, bytes, length) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return result;
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
static bool nvs_erase_exact_key(const char *key)
{
    nvs_handle_t handle;
    if (key == NULL ||
        nvs_open(UPLINK_JOURNAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const esp_err_t erased = nvs_erase_key(handle, key);
    const bool result =
        (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return result;
}

typedef enum {
    UPLINK_TOPOLOGY_EPOCH_OK = 0,
    UPLINK_TOPOLOGY_EPOCH_MISSING,
    UPLINK_TOPOLOGY_EPOCH_ERROR,
} uplink_topology_epoch_result_t;

static uplink_topology_epoch_result_t topology_epoch_load(uint32_t *out)
{
    if (out == NULL) {
        return UPLINK_TOPOLOGY_EPOCH_ERROR;
    }
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(
        UPLINK_JOURNAL_NAMESPACE, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        return UPLINK_TOPOLOGY_EPOCH_MISSING;
    }
    if (opened != ESP_OK) {
        return UPLINK_TOPOLOGY_EPOCH_ERROR;
    }
    const esp_err_t loaded = nvs_get_u32(
        handle, UPLINK_TOPOLOGY_EPOCH_KEY, out);
    nvs_close(handle);
    if (loaded == ESP_ERR_NVS_NOT_FOUND) {
        return UPLINK_TOPOLOGY_EPOCH_MISSING;
    }
    return loaded == ESP_OK && *out != 0U
        ? UPLINK_TOPOLOGY_EPOCH_OK : UPLINK_TOPOLOGY_EPOCH_ERROR;
}

static bool topology_epoch_store(uint32_t value)
{
    nvs_handle_t handle;
    if (value == 0U || nvs_open(
            UPLINK_JOURNAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool stored = nvs_set_u32(
            handle, UPLINK_TOPOLOGY_EPOCH_KEY, value) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return stored;
}

static bool topology_epoch_restore(
    backend_ota_journal_startup_action_t action,
    const backend_ota_journal_record_t *journal)
{
    uint32_t stored = 0U;
    const uplink_topology_epoch_result_t loaded = topology_epoch_load(&stored);
    if (loaded == UPLINK_TOPOLOGY_EPOCH_ERROR) {
        return false;
    }
    const bool active_recovery =
        action == BACKEND_OTA_JOURNAL_STARTUP_RESTART_METADATA ||
        action == BACKEND_OTA_JOURNAL_STARTUP_RESTART_DOWNLOAD ||
        action == BACKEND_OTA_JOURNAL_STARTUP_ROLL_BACK ||
        action == BACKEND_OTA_JOURNAL_STARTUP_WAIT_REBOOT ||
        action == BACKEND_OTA_JOURNAL_STARTUP_CHECK_CONVERGENCE;
    uint32_t expected = 0U;
    if (active_recovery) {
        if (journal == NULL ||
            journal->expected_topology_generation == 0U) {
            s_runtime.ota_startup_action =
                BACKEND_OTA_JOURNAL_STARTUP_BLOCKED;
            expected = loaded == UPLINK_TOPOLOGY_EPOCH_OK ? stored : 1U;
        } else if (loaded == UPLINK_TOPOLOGY_EPOCH_OK) {
            expected = stored;
            if (stored != journal->expected_topology_generation) {
                s_runtime.ota_startup_action =
                    BACKEND_OTA_JOURNAL_STARTUP_BLOCKED;
            }
        } else {
            expected = journal->expected_topology_generation;
        }
        if (loaded == UPLINK_TOPOLOGY_EPOCH_MISSING &&
            !topology_epoch_store(expected)) {
            return false;
        }
    } else if (loaded == UPLINK_TOPOLOGY_EPOCH_OK) {
        expected = stored;
    } else {
        expected = action == BACKEND_OTA_JOURNAL_STARTUP_TERMINAL &&
                journal != NULL &&
                journal->expected_topology_generation != 0U
            ? journal->expected_topology_generation : 1U;
        if (!topology_epoch_store(expected)) {
            return false;
        }
    }
    s_runtime.topology_generation = expected;
    return true;
}

static bool journal_erase_exact(void *context, const char *key)
{
    (void)context;
    return key != NULL && strcmp(key, UPLINK_JOURNAL_KEY) == 0 &&
           nvs_erase_exact_key(UPLINK_JOURNAL_KEY);
}

static const char *ota_outbox_key(backend_ota_event_outbox_slot_t slot)
{
    switch (slot) {
    case BACKEND_OTA_EVENT_OUTBOX_SLOT_0:
        return UPLINK_OTA_EVENT_SLOT0_KEY;
    case BACKEND_OTA_EVENT_OUTBOX_SLOT_1:
        return UPLINK_OTA_EVENT_SLOT1_KEY;
    default:
        return NULL;
    }
}

static backend_ota_event_outbox_io_result_t ota_outbox_load_slot(
    void *context,
    backend_ota_event_outbox_slot_t slot,
    uint8_t *output,
    size_t capacity,
    size_t *out_length)
{
    (void)context;
    const char *key = ota_outbox_key(slot);
    if (key == NULL || output == NULL || out_length == NULL) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_ERROR;
    }
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(
        UPLINK_JOURNAL_NAMESPACE, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_NOT_FOUND;
    }
    if (opened != ESP_OK) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_ERROR;
    }
    size_t required = 0U;
    esp_err_t result = nvs_get_blob(handle, key, NULL, &required);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return BACKEND_OTA_EVENT_OUTBOX_IO_NOT_FOUND;
    }
    if (result != ESP_OK || required == 0U || required > capacity) {
        nvs_close(handle);
        return BACKEND_OTA_EVENT_OUTBOX_IO_ERROR;
    }
    size_t actual = required;
    result = nvs_get_blob(handle, key, output, &actual);
    nvs_close(handle);
    if (result != ESP_OK || actual != required) {
        return BACKEND_OTA_EVENT_OUTBOX_IO_ERROR;
    }
    *out_length = actual;
    return BACKEND_OTA_EVENT_OUTBOX_IO_OK;
}

static bool ota_outbox_store_slot(
    void *context,
    backend_ota_event_outbox_slot_t slot,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
    const char *key = ota_outbox_key(slot);
    nvs_handle_t handle;
    if (key == NULL || bytes == NULL || length == 0U ||
        length > BACKEND_OTA_EVENT_OUTBOX_SLOT_MAX_BYTES ||
        nvs_open(UPLINK_JOURNAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool result = nvs_set_blob(handle, key, bytes, length) == ESP_OK &&
                        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return result;
}

static bool ota_outbox_clear_exact_slot(
    void *context, backend_ota_event_outbox_slot_t slot)
{
    (void)context;
    const char *key = ota_outbox_key(slot);
    return key != NULL && nvs_erase_exact_key(key);
}
#endif

static void *firmware_buffer_alloc(size_t size, void *context)
{
    (void)context;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool binary_download_sink(
    void *context, const uint8_t *bytes, size_t length)
{
    uplink_download_sink_t *sink = context;
    if (sink == NULL || bytes == NULL || length == 0U ||
        sink->length > sink->capacity ||
        length > sink->capacity - sink->length) {
        return false;
    }
    memcpy(sink->destination + sink->length, bytes, length);
    sink->length += length;
    return true;
}

static bool ota_fetch_metadata(
    void *context,
    const char *catalog_name,
    char *json,
    size_t capacity,
    size_t *out_length,
    uint32_t *out_catalog_generation)
{
    (void)context;
    if (catalog_name == NULL || json == NULL || capacity == 0U ||
        out_length == NULL || out_catalog_generation == NULL) {
        return false;
    }
    char endpoint[128];
    if (snprintf(
            endpoint, sizeof(endpoint),
            "/nodes/firmware/latest/%s", catalog_name) >=
        (int)sizeof(endpoint)) {
        return false;
    }
    char base_url[sizeof(s_runtime.config.backend_url)];
    lock_runtime();
    copy_text(base_url, sizeof(base_url), s_runtime.config.backend_url);
    unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (!acquire_lite_http_if_sta_usable()) {
        return false;
    }
#else
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
    const backend_http_result_t result = backend_http_get_json(
        base_url, endpoint, json, capacity);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    (void)xSemaphoreGive(s_runtime.http_lock);
#endif
    const bool valid = result.transport_complete &&
        result.status_code == 200 && result.body_length > 0U &&
        result.body_length < capacity;
    if (valid) {
        lock_runtime();
        s_runtime.catalog_generation =
            s_runtime.catalog_generation == UINT32_MAX
                ? 1U : s_runtime.catalog_generation + 1U;
        *out_catalog_generation = s_runtime.catalog_generation;
        unlock_runtime();
        *out_length = result.body_length;
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    release_lite_http();
#endif
    return valid;
}

static bool ota_download_image(
    void *context,
    const char *catalog_name,
    uint8_t *destination,
    size_t capacity,
    size_t expected_size,
    size_t *out_size)
{
    (void)context;
    if (catalog_name == NULL || destination == NULL || out_size == NULL ||
        expected_size == 0U || expected_size > capacity) {
        return false;
    }
    char endpoint[128];
    if (snprintf(
            endpoint, sizeof(endpoint),
            "/nodes/firmware/download/%s", catalog_name) >=
        (int)sizeof(endpoint)) {
        return false;
    }
    char base_url[sizeof(s_runtime.config.backend_url)];
    lock_runtime();
    copy_text(base_url, sizeof(base_url), s_runtime.config.backend_url);
    unlock_runtime();
    uplink_download_sink_t sink = {
        .destination = destination,
        .capacity = capacity,
        .length = 0U,
    };
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (!acquire_lite_http_if_sta_usable()) {
        return false;
    }
#else
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
    const backend_http_result_t result = backend_http_get_binary(
        base_url,
        endpoint,
        expected_size,
        binary_download_sink,
        &sink);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    (void)xSemaphoreGive(s_runtime.http_lock);
#endif
    const bool valid = result.transport_complete &&
        result.status_code == 200 && sink.length == expected_size;
    if (valid) {
        *out_size = sink.length;
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    release_lite_http();
#endif
    return valid;
}

static const char *ota_running_version(
    void *context, backend_ota_component_t component)
{
    (void)context;
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        return FOF_VERSION_BACKEND;
    }
    const int slot = backend_ota_component_slot(component);
    if (slot < 0 || slot >= 2) {
        return NULL;
    }
    lock_runtime();
    const char *version = s_runtime.scanner_tracker[slot].initialized
        ? s_runtime.scanner_tracker[slot].status.version : NULL;
    unlock_runtime();
    return version;
}

static size_t ota_partition_capacity(
    void *context, backend_ota_component_t component)
{
    (void)context;
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        const esp_partition_t *partition =
            esp_ota_get_next_update_partition(NULL);
        return partition == NULL ? 0U : partition->size;
    }
    return BACKEND_FIRMWARE_STORE_CAPACITY;
}

static uint32_t ota_image_write_count(void *context)
{
    (void)context;
    lock_runtime();
    const uint32_t self_count =
        backend_self_ota_image_write_count(&s_runtime.self_ota);
    const uint32_t store_count =
        backend_firmware_store_image_mutation_count(&s_runtime.firmware_store);
    unlock_runtime();
    return UINT32_MAX - self_count < store_count
        ? UINT32_MAX : self_count + store_count;
}

static bool ota_snapshot_binding(
    void *context,
    backend_ota_component_t component,
    backend_ota_target_binding_t *out)
{
    (void)context;
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->component = component;
    out->component_slot = backend_ota_component_slot(component);
    lock_runtime();
    out->topology_generation = s_runtime.topology_generation;
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        memcpy(out->target_mac, s_runtime.mac, sizeof(out->target_mac));
        out->target_boot_id = s_runtime.boot_id;
        unlock_runtime();
        return true;
    }
    const int slot = out->component_slot;
    if (slot < 0 || slot >= 2 ||
        !s_runtime.scanner_tracker[slot].initialized ||
        !parse_mac(
            s_runtime.scanner_tracker[slot].status.mac,
            out->target_mac)) {
        unlock_runtime();
        return false;
    }
    out->target_boot_id = s_runtime.scanner_tracker[slot].status.boot_id;
    unlock_runtime();
    return out->target_boot_id != 0U;
}

static bool ota_acquire_target(
    void *context, backend_ota_component_t component)
{
    (void)context;
    if (component < BACKEND_OTA_COMPONENT_UPLINK ||
        component > BACKEND_OTA_COMPONENT_SCANNER1) {
        return false;
    }
    lock_runtime();
    bool available = true;
    for (size_t index = 0U; index < 3U; ++index) {
        available = available && !s_runtime.target_claimed[index];
    }
    if (available) {
        s_runtime.target_claimed[component] = true;
    }
    unlock_runtime();
    return available;
}

static void ota_release_target(
    void *context, backend_ota_component_t component)
{
    (void)context;
    if (component >= BACKEND_OTA_COMPONENT_UPLINK &&
        component <= BACKEND_OTA_COMPONENT_SCANNER1) {
        lock_runtime();
        s_runtime.target_claimed[component] = false;
        unlock_runtime();
    }
}

static backend_ota_image_result_t ota_validate_staged(
    void *context,
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
    uplink_memory_image_t image = {.bytes = bytes, .length = length};
    return backend_ota_image_validate(
        manifest, expected_kind, memory_image_read, &image);
}

static bool relay_image(
    backend_ota_component_t component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length,
    bool dry_run)
{
    const int slot = backend_ota_component_slot(component);
    if (slot < 0 || slot >= 2 || manifest == NULL || bytes == NULL ||
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        !has_operation_id || operation_id == NULL ||
#endif
        length != manifest->image_size) {
        return false;
    }
    uplink_memory_image_t image = {.bytes = bytes, .length = length};
    const backend_firmware_store_result_t stage =
        backend_firmware_store_stage(
            &s_runtime.firmware_store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            has_operation_id,
            operation_id,
#endif
            manifest,
            memory_image_read,
            &image,
            !dry_run);
    if (stage != BACKEND_FIRMWARE_STORE_OK) {
        return false;
    }

    while (xSemaphoreTake(s_runtime.relay_complete, 0) == pdTRUE) {
    }
    lock_runtime();
    if (!s_runtime.scanner_tracker[slot].initialized ||
        s_runtime.relay_active || s_runtime.role_generation > UINT32_MAX - 2U
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        || s_runtime.relay_session_generation == UINT32_MAX
#endif
        ) {
        unlock_runtime();
        return false;
    }
    uint8_t target_mac[6];
    if (!parse_mac(
            s_runtime.scanner_tracker[slot].status.mac, target_mac)) {
        unlock_runtime();
        return false;
    }
    s_runtime.relay_quiet_generation = ++s_runtime.role_generation;
    s_runtime.relay_expected_role_generation = ++s_runtime.role_generation;
    const backend_scan_profile_t expected_profile =
        s_runtime.scanner_health[slot].commanded_profile ==
                BACKEND_SCAN_PROFILE_QUIESCENT
            ? (slot == 0 ? BACKEND_SCAN_PROFILE_BLE_PRIMARY
                         : BACKEND_SCAN_PROFILE_WIFI_PRIMARY)
            : s_runtime.scanner_health[slot].commanded_profile;
    uint32_t relay_session_id = esp_random();
    if (relay_session_id == 0U) {
        relay_session_id = 1U;
    }
    backend_scanner_relay_init(&s_runtime.relay);
    const bool begun = backend_scanner_relay_begin(
        &s_runtime.relay,
        &s_runtime.firmware_store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        has_operation_id,
        operation_id,
        ++s_runtime.relay_session_generation,
#endif
        (backend_scanner_slot_t)slot,
        manifest,
        target_mac,
        relay_session_id,
        manifest->generation,
        s_runtime.scanner_tracker[slot].status.boot_id,
        s_runtime.topology_generation,
        expected_profile,
        s_runtime.relay_expected_role_generation,
        dry_run);
    if (begun) {
        s_runtime.relay_active = true;
        s_runtime.relay_result = false;
    }
    unlock_runtime();
    if (!begun ||
        xSemaphoreTake(
            s_runtime.relay_complete,
            pdMS_TO_TICKS(UPLINK_RELAY_WAIT_MS)) != pdTRUE) {
        return false;
    }
    lock_runtime();
    const bool result = s_runtime.relay_result;
    unlock_runtime();
    return result;
}

static bool ota_scanner_dry_run(
    void *context,
    backend_ota_component_t component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
    return relay_image(
        component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        has_operation_id, operation_id,
#endif
        manifest, bytes, length, true);
}

static bool ota_mutate_staged(
    void *context,
    backend_ota_component_t component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id,
#endif
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    if (!has_operation_id || operation_id == NULL) {
        return false;
    }
#endif
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        if (backend_self_ota_begin(&s_runtime.self_ota, manifest) !=
            BACKEND_SELF_OTA_READY) {
            return false;
        }
        size_t offset = 0U;
        while (offset < length) {
            const size_t chunk = length - offset > 4096U
                ? 4096U : length - offset;
            if (!backend_self_ota_write(
                    &s_runtime.self_ota,
                    offset,
                    bytes + offset,
                    chunk)) {
                return false;
            }
            offset += chunk;
        }
        return backend_self_ota_finish(&s_runtime.self_ota) ==
               BACKEND_SELF_OTA_READY_TO_REBOOT;
    }
    return relay_image(
        component,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        has_operation_id, operation_id,
#endif
        manifest, bytes, length, false);
}

static bool ota_request_reboot(
    void *context, backend_ota_component_t component)
{
    (void)context;
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        print_line("FOF_BACKEND_OTA_REBOOT {\"component\":\"uplink\"}");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    return component == BACKEND_OTA_COMPONENT_SCANNER0 ||
           component == BACKEND_OTA_COMPONENT_SCANNER1;
}

static bool ota_read_convergence(
    void *context,
    backend_ota_component_t component,
    const backend_ota_manifest_t *manifest,
    backend_ota_convergence_t *out)
{
    (void)context;
    if (out == NULL || manifest == NULL ||
        !ota_snapshot_binding(NULL, component, &out->binding)) {
        return false;
    }
    memset((uint8_t *)out + sizeof(out->binding), 0,
           sizeof(*out) - sizeof(out->binding));
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        out->identity_exact = strcmp(manifest->version, FOF_VERSION_BACKEND) == 0;
        out->command_ingress_healthy = true;
        out->role_acked = true;
        out->profile_correct = true;
        out->radio_healthy = true;
        out->rollback_clear = backend_self_ota_rollback_clear(
            &s_runtime.self_ota);
        return true;
    }
    const int slot = backend_ota_component_slot(component);
    lock_runtime();
    const backend_scanner_status_t *status =
        &s_runtime.scanner_tracker[slot].status;
    out->identity_exact = scanner_identity_exact(status) &&
                          strcmp(status->version, manifest->version) == 0;
    out->command_ingress_healthy = status->command_ingress;
    out->role_acked = status->role_acked;
    out->profile_correct =
        status->profile == s_runtime.scanner_health[slot].commanded_profile;
    out->radio_healthy = backend_scanner_required_radio_healthy(
        status->profile, status->ble_healthy, status->wifi_healthy);
    out->rollback_clear = strcmp(status->rollback_state, "valid") == 0;
    unlock_runtime();
    return true;
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
static bool ota_report_progress(
    void *context, const backend_ota_progress_update_t *update)
{
    (void)context;
    if (update == NULL || s_runtime.ota_progress_queue == NULL ||
        s_runtime.ota_progress_ack == NULL ||
        xQueueSend(
            s_runtime.ota_progress_queue, update, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    return xSemaphoreTake(
        s_runtime.ota_progress_ack, portMAX_DELAY) == pdTRUE;
}

static uint32_t ota_relay_retry_count(
    void *context, backend_ota_component_t component)
{
    (void)context;
    return component == BACKEND_OTA_COMPONENT_UPLINK
        ? 0U : (uint32_t)s_runtime.relay.retry_count;
}
#endif

static bool ota_emit(void *context, const char *line, size_t length)
{
    (void)context;
    if (line == NULL || length == 0U ||
        memchr(line, '\0', length) != NULL) {
        return false;
    }
    char output[BACKEND_OTA_MAINTENANCE_EVIDENCE_CAPACITY];
    if (length >= sizeof(output)) {
        return false;
    }
    memcpy(output, line, length);
    output[length] = '\0';
    return print_line(output);
}

static bool json_find(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    const char *key,
    size_t *index)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, key, index);
}

static bool relay_receipt_decode(
    const uint8_t *bytes,
    size_t length,
    backend_scanner_relay_receipt_t *out)
{
    if (bytes == NULL || out == NULL || length == 0U || length > 1024U) {
        return false;
    }
    const char *json = (const char *)bytes;
    backend_json_token_t tokens[40];
    size_t token_count = 0U;
    if (backend_json_parse(
            json, length, tokens, 40U, &token_count) != BACKEND_JSON_OK ||
        token_count == 0U || tokens[0].kind != BACKEND_JSON_OBJECT) {
        return false;
    }
    size_t index = 0U;
    char type[24];
    if (!json_find(json, tokens, token_count, "type", &index) ||
        !backend_json_copy_string(json, &tokens[index], type, sizeof(type))) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (strcmp(type, "scanner_quiet_ack") == 0) {
        out->kind = BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK;
    } else if (strcmp(type, "ota_ack") == 0) {
        out->kind = BACKEND_SCANNER_RELAY_RECEIPT_ACK;
    } else if (strcmp(type, "ota_nack") == 0) {
        out->kind = BACKEND_SCANNER_RELAY_RECEIPT_NACK;
    } else if (strcmp(type, "ota_staged") == 0) {
        out->kind = BACKEND_SCANNER_RELAY_RECEIPT_STAGED;
    } else if (strcmp(type, "ota_done") == 0) {
        out->kind = BACKEND_SCANNER_RELAY_RECEIPT_DONE;
    } else if (strcmp(type, "ota_error") == 0) {
        out->kind = BACKEND_SCANNER_RELAY_RECEIPT_ERROR;
    } else {
        return false;
    }
    uint64_t value = 0U;
#define READ_RECEIPT_U32(key, member)                                      \
    do {                                                                   \
        if (!json_find(json, tokens, token_count, key, &index) ||           \
            !backend_json_get_u64(json, &tokens[index], &value) ||         \
            value > UINT32_MAX) {                                          \
            return false;                                                  \
        }                                                                  \
        out->member = (uint32_t)value;                                     \
    } while (0)
    READ_RECEIPT_U32("session_id", session_id);
    READ_RECEIPT_U32("generation", generation);
    READ_RECEIPT_U32("sequence", sequence);
    READ_RECEIPT_U32("next_sequence", next_sequence);
    READ_RECEIPT_U32("received", received);
#undef READ_RECEIPT_U32
    if (!json_find(json, tokens, token_count, "dry_run", &index) ||
        !backend_json_get_bool(json, &tokens[index], &out->dry_run)) {
        return false;
    }
    if (json_find(json, tokens, token_count, "reason", &index) &&
        tokens[index].kind == BACKEND_JSON_STRING &&
        !backend_json_copy_string(
            json, &tokens[index], out->reason, sizeof(out->reason))) {
        return false;
    }
    return out->session_id != 0U && out->generation != 0U;
}

static void signal_relay_if_terminal_locked(
    backend_scanner_relay_event_result_t result)
{
    if (!s_runtime.relay_active ||
        (result != BACKEND_SCANNER_RELAY_EVENT_COMPLETE &&
         result != BACKEND_SCANNER_RELAY_EVENT_FAILED)) {
        return;
    }
    s_runtime.relay_result =
        result == BACKEND_SCANNER_RELAY_EVENT_COMPLETE;
    s_runtime.relay_active = false;
    (void)xSemaphoreGive(s_runtime.relay_complete);
}

static void update_scanner_status_locked(
    size_t slot, const backend_scanner_status_t *status, int64_t now_ms)
{
    const uint32_t previous_boot =
        s_runtime.scanner_tracker[slot].initialized
            ? s_runtime.scanner_tracker[slot].status.boot_id : 0U;
    const backend_scanner_status_accept_result_t accepted =
        backend_scanner_status_tracker_accept(
            &s_runtime.scanner_tracker[slot], status);
    if (accepted == BACKEND_SCANNER_STATUS_STALE ||
        accepted == BACKEND_SCANNER_STATUS_CONFLICT ||
        accepted == BACKEND_SCANNER_STATUS_INVALID) {
        return;
    }
    s_runtime.scanner_last_seen_ms[slot] = now_ms;
    backend_scanner_health_t *health = &s_runtime.scanner_health[slot];
    health->connected = true;
    health->identity_valid = scanner_identity_exact(status);
    health->command_healthy = status->command_ingress;
    health->boot_id = status->boot_id;
    health->acknowledged_generation = status->role_generation;
    health->reported_profile = status->profile;
    health->role_acked = status->role_acked;
    health->radio_healthy = backend_scanner_required_radio_healthy(
        status->profile, status->ble_healthy, status->wifi_healthy);
    const bool expected_relay_reboot =
        s_runtime.relay_active &&
        s_runtime.relay.slot == (backend_scanner_slot_t)slot &&
        (s_runtime.relay.state == BACKEND_SCANNER_RELAY_REBOOT_WAIT ||
         s_runtime.relay.state == BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT);
    if (previous_boot != 0U && previous_boot != status->boot_id) {
        /*
         * A scanner relay binds the authorized operation to the current
         * topology generation.  Its expected reboot is part of that same
         * operation, not a topology replacement, so keep the binding stable
         * until the relay state machine validates convergence.
         */
        if (!expected_relay_reboot) {
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            if (s_runtime.topology_generation == UINT32_MAX) {
                s_runtime.ota_client_blocked = true;
            } else {
                const uint32_t next_generation =
                    s_runtime.topology_generation + 1U;
                if (topology_epoch_store(next_generation)) {
                    s_runtime.topology_generation = next_generation;
                } else {
                    s_runtime.ota_client_blocked = true;
                }
            }
#else
            const uint32_t next_generation =
                s_runtime.topology_generation == UINT32_MAX
                    ? UINT32_MAX : s_runtime.topology_generation + 1U;
            s_runtime.topology_generation = next_generation;
#endif
        }
        health->commanded_generation = 0U;
        health->role_acked = false;
    }

    if (s_runtime.relay_active &&
        s_runtime.relay.slot == (backend_scanner_slot_t)slot) {
        if (s_runtime.relay.state == BACKEND_SCANNER_RELAY_QUIET_REQUESTED &&
            status->role_acked &&
            status->role_generation == s_runtime.relay_quiet_generation &&
            status->profile == BACKEND_SCAN_PROFILE_QUIESCENT) {
            backend_scanner_relay_receipt_t receipt = {
                .kind = BACKEND_SCANNER_RELAY_RECEIPT_QUIET_ACK,
                .session_id = s_runtime.relay.session_id,
                .generation = s_runtime.relay.generation,
                .dry_run = s_runtime.relay.dry_run,
            };
            signal_relay_if_terminal_locked(
                backend_scanner_relay_receive(
                    &s_runtime.relay, &receipt, now_ms));
        }
        if ((s_runtime.relay.state == BACKEND_SCANNER_RELAY_REBOOT_WAIT ||
             s_runtime.relay.state ==
                 BACKEND_SCANNER_RELAY_CONVERGENCE_WAIT) &&
            status->boot_id != s_runtime.relay.old_boot_id &&
            status->role_generation !=
                s_runtime.relay_expected_role_generation) {
            backend_scanner_control_t role = {
                .type = BACKEND_SCANNER_CONTROL_ROLE,
                .payload.role = {
                    .boot_id = status->boot_id,
                    .generation = s_runtime.relay_expected_role_generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
                    .topology_generation = s_runtime.topology_generation,
#endif
                    .profile = s_runtime.relay.expected_profile,
                },
            };
            (void)uart_send_control(slot, &role);
            health->commanded_generation =
                s_runtime.relay_expected_role_generation;
            health->commanded_profile = s_runtime.relay.expected_profile;
        }
        signal_relay_if_terminal_locked(
            backend_scanner_relay_on_status(
                &s_runtime.relay,
                status,
                s_runtime.topology_generation,
                now_ms));
    }
}

static uint32_t production_boot_id_for_slot(size_t slot)
{
    uint32_t value = s_runtime.boot_id ^ UINT32_C(0x50524f44) ^
        (uint32_t)((slot + 1U) * UINT32_C(0x01010101));
    return value == 0U ? (uint32_t)(slot + 1U) : value;
}

static void update_production_scanner_locked(
    size_t slot,
    const production_scanner_message_t *message,
    int64_t now_ms)
{
    if (slot >= BACKEND_UART_SLOT_COUNT || message == NULL) {
        return;
    }
    backend_scanner_health_t *health = &s_runtime.scanner_health[slot];
    production_scanner_message_t *stored =
        &s_runtime.production_scanner[slot];
    const bool reconnect = !health->connected &&
        s_runtime.scanner_wire[slot] == UPLINK_SCANNER_WIRE_PRODUCTION &&
        s_runtime.scanner_last_seen_ms[slot] != 0;
    s_runtime.scanner_wire[slot] = UPLINK_SCANNER_WIRE_PRODUCTION;
    s_runtime.scanner_last_seen_ms[slot] = now_ms;
    health->connected = true;
    health->command_healthy = true;
    const bool changed_reported_boot = message->boot_id_present &&
        s_runtime.production_boot_id[slot] != 0U &&
        s_runtime.production_boot_id[slot] != message->boot_id;
    if (message->boot_id_present) {
        s_runtime.production_boot_id[slot] = message->boot_id;
    } else if (s_runtime.production_boot_id[slot] == 0U) {
        s_runtime.production_boot_id[slot] =
            production_boot_id_for_slot(slot);
    } else if (reconnect && !stored->boot_id_present) {
        if (++s_runtime.production_boot_id[slot] == 0U) {
            s_runtime.production_boot_id[slot] = 1U;
        }
    }
    if (reconnect || changed_reported_boot) {
        health->commanded_generation = 0U;
        health->acknowledged_generation = 0U;
        health->role_acked = false;
        health->radio_healthy = false;
    }
    health->boot_id = s_runtime.production_boot_id[slot];

    if (message->identity_present) {
        copy_text(stored->version, sizeof(stored->version), message->version);
        copy_text(stored->board, sizeof(stored->board), message->board);
        copy_text(stored->chip, sizeof(stored->chip), message->chip);
        copy_text(
            stored->capabilities, sizeof(stored->capabilities),
            message->capabilities);
        stored->identity_present = true;
        stored->identity_valid = message->identity_valid;
        health->identity_valid = message->identity_valid;
        if (message->management_identity_present) {
            copy_text(
                stored->firmware_name, sizeof(stored->firmware_name),
                message->firmware_name);
            copy_text(
                stored->app_project, sizeof(stored->app_project),
                message->app_project);
            copy_text(
                stored->hardware_type, sizeof(stored->hardware_type),
                message->hardware_type);
            copy_text(
                stored->hardware_id, sizeof(stored->hardware_id),
                message->hardware_id);
            stored->management_identity_present = true;
            stored->management_identity_valid =
                message->management_identity_valid;
        }
        if (message->boot_id_present) {
            stored->boot_id = message->boot_id;
            stored->boot_id_present = true;
        }
        /* Production identity is projected for USB inventory, but it is not
         * admitted to the backend-native scanner tracker or relay. */
        s_runtime.scanner_tracker[slot].initialized = false;
    }
    if (message->ota_state[0] != '\0') {
        copy_text(
            stored->ota_state, sizeof(stored->ota_state),
            message->ota_state);
    }
    if (message->rollback_state[0] != '\0') {
        copy_text(
            stored->rollback_state, sizeof(stored->rollback_state),
            message->rollback_state);
    }
    if (message->sequence != 0U) {
        stored->sequence = message->sequence;
    }
    if (message->uptime_ms != 0U) {
        if (!stored->boot_id_present && stored->uptime_ms != 0U &&
            message->uptime_ms < stored->uptime_ms) {
            uint32_t next = s_runtime.production_boot_id[slot] + 1U;
            if (next == 0U) {
                next = 1U;
            }
            s_runtime.production_boot_id[slot] = next;
            health->boot_id = next;
            health->commanded_generation = 0U;
            health->acknowledged_generation = 0U;
            health->role_acked = false;
            health->radio_healthy = false;
        }
        stored->uptime_ms = message->uptime_ms;
    }
    stored->command_rx_count = message->command_rx_count;
    stored->tx_drops = message->tx_drops;
    if (message->ble_initialized_present) {
        stored->ble_initialized = message->ble_initialized;
    }
    if (message->ble_scanning_present) {
        stored->ble_scanning = message->ble_scanning;
    }
    if (message->ble_host_active_present) {
        stored->ble_host_active = message->ble_host_active;
    }
    if (message->ble_host_synced_present) {
        stored->ble_host_synced = message->ble_host_synced;
    }
    if (message->wifi_initialized_present) {
        stored->wifi_initialized = message->wifi_initialized;
    }
    if (message->wifi_active_present) {
        stored->wifi_active = message->wifi_active;
    }
    if (message->wifi_paused_present) {
        stored->wifi_paused = message->wifi_paused;
    }

    if (message->profile_present) {
        stored->profile = message->profile;
        stored->profile_present = true;
        health->reported_profile = message->profile;
    }
    const bool explicit_profile_ack =
        message->kind == PRODUCTION_SCANNER_MESSAGE_PROFILE_ACK &&
        (!message->slot_role_ok_present || message->slot_role_ok);
    const bool matching_status = message->profile_present &&
        health->commanded_generation != 0U &&
        health->commanded_profile == message->profile &&
        message->command_rx_count != 0U;
    if ((explicit_profile_ack || matching_status) &&
        health->commanded_generation != 0U &&
        health->commanded_profile == health->reported_profile) {
        health->acknowledged_generation = health->commanded_generation;
        health->role_acked = true;
        if (explicit_profile_ack) {
            /* Current badge production firmware sets slot_role_ok only after
             * accepting the exact durable role/profile pair.  Older deployed
             * combo images omit that additive field; their profile ACK is the
             * authoritative success signal.  An explicit false is never
             * promoted to radio health. */
            health->radio_healthy = true;
        }
    }
    if (message->profile_present &&
        (message->kind == PRODUCTION_SCANNER_MESSAGE_INFO ||
         message->kind == PRODUCTION_SCANNER_MESSAGE_STATUS)) {
        if (message->profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY &&
            (message->ble_initialized_present ||
             message->ble_scanning_present ||
             message->ble_host_active_present ||
             message->ble_host_synced_present)) {
            const bool observed_healthy = message->ble_initialized &&
                (message->ble_scanning || message->ble_host_active ||
                 message->ble_host_synced);
            health->radio_healthy = health->radio_healthy || observed_healthy;
        } else if (message->profile == BACKEND_SCAN_PROFILE_WIFI_PRIMARY &&
                   (message->wifi_initialized_present ||
                    message->wifi_active_present ||
                    message->wifi_paused_present)) {
            const bool observed_healthy = message->wifi_initialized &&
                (!message->wifi_paused || message->wifi_active);
            health->radio_healthy = health->radio_healthy || observed_healthy;
        }
    }
    if (++s_runtime.production_status_sequence[slot] == 0U) {
        s_runtime.production_status_sequence[slot] = 1U;
    }
}

static void ingest_decoded_detection(
    size_t slot,
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *stamp,
    int64_t now_ms)
{
    lock_runtime();
    const int64_t epoch_ms = current_epoch_ms_locked(now_ms);
    unlock_runtime();
    backend_detection_observation_t observation;
    backend_observation_resolve(
        detection, stamp, epoch_ms, &observation);
    lock_coordinator();
    const backend_coordinator_ingest_result_t result =
        backend_coordinator_ingest_detection(
            &s_runtime.coordinator,
            (uint8_t)slot,
            &observation,
            now_ms);
    unlock_coordinator();
    if (result.update_local_threat) {
        lock_runtime();
        backend_threat_ingest(
            &s_runtime.threats, &observation.detection, now_ms);
        unlock_runtime();
    }
}

static void dispatch_uart_frame(
    size_t slot, const uint8_t *bytes, size_t length)
{
    const int64_t now_ms = monotonic_ms();
    drone_detection_t detection;
    backend_scanner_stamp_t stamp;
    if (backend_detection_uart_decode(
            (const char *)bytes,
            length,
            (backend_scanner_slot_t)slot,
            &detection,
            &stamp) == BACKEND_DECODE_OK) {
        lock_runtime();
        s_runtime.scanner_wire[slot] = UPLINK_SCANNER_WIRE_BACKEND;
        unlock_runtime();
        ingest_decoded_detection(slot, &detection, &stamp, now_ms);
        return;
    }

    if (backend_detection_uart_decode_production(
            (const char *)bytes,
            length,
            (backend_scanner_slot_t)slot,
            &detection,
            &stamp) == BACKEND_DECODE_OK) {
        lock_runtime();
        if (s_runtime.scanner_wire[slot] != UPLINK_SCANNER_WIRE_BACKEND) {
            s_runtime.scanner_wire[slot] = UPLINK_SCANNER_WIRE_PRODUCTION;
            s_runtime.scanner_last_seen_ms[slot] = now_ms;
            s_runtime.scanner_health[slot].connected = true;
            s_runtime.scanner_health[slot].command_healthy = true;
        }
        unlock_runtime();
        ingest_decoded_detection(slot, &detection, &stamp, now_ms);
        return;
    }

    backend_scanner_status_t status;
    if (backend_scanner_status_decode(
            (const char *)bytes, length, &status) ==
        BACKEND_SCANNER_STATUS_DECODE_OK) {
        lock_runtime();
        s_runtime.scanner_wire[slot] = UPLINK_SCANNER_WIRE_BACKEND;
        update_scanner_status_locked(slot, &status, now_ms);
        unlock_runtime();
        return;
    }

    production_scanner_message_t production;
    if (production_scanner_uart_decode(
            (const char *)bytes, length, &production)) {
        lock_runtime();
        if (s_runtime.scanner_wire[slot] != UPLINK_SCANNER_WIRE_BACKEND) {
            update_production_scanner_locked(
                slot, &production, now_ms);
        }
        unlock_runtime();
        return;
    }

    ble_investigation_chunk_t chunk;
    if (backend_uart_investigation_decode(bytes, length, &chunk) ==
        BACKEND_UART_INVESTIGATION_DECODE_OK) {
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        char completed_json[BACKEND_COMMAND_RESULT_MAX_JSON + 1U];
        size_t completed_length = 0U;
#endif
        lock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        const uint8_t queue_count_before =
            s_runtime.investigation.queue_count;
#endif
        (void)backend_ble_investigation_accept_chunk(
            &s_runtime.investigation,
            (backend_scanner_slot_t)slot,
            &chunk);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        for (uint8_t index = queue_count_before;
             index < s_runtime.investigation.queue_count; ++index) {
            const uint8_t queue_index = (uint8_t)(
                (s_runtime.investigation.queue_head + index) %
                BACKEND_COMMAND_RESULT_QUEUE_CAPACITY);
            const backend_command_result_t *result =
                &s_runtime.investigation.queue[queue_index];
            if (strcmp(result->type, "ble_inv_end") == 0 &&
                result->json_length < sizeof(completed_json)) {
                completed_length = result->json_length;
                memcpy(
                    completed_json,
                    result->json,
                    completed_length + 1U);
                break;
            }
        }
#endif
        unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        if (completed_length != 0U && s_runtime.usb_available) {
            char frame[BACKEND_USB_DET_MAX];
            const size_t frame_length =
                backend_usb_protocol_encode_investigation(
                    completed_json,
                    completed_length,
                    frame,
                    sizeof(frame));
            if (frame_length != 0U) {
                (void)backend_usb_service_emit(
                    &s_runtime.usb,
                    BACKEND_USB_FRAME_OPTIONAL,
                    frame,
                    frame_length);
            }
        }
#endif
        return;
    }

    backend_scanner_relay_receipt_t receipt;
    if (relay_receipt_decode(bytes, length, &receipt)) {
        lock_runtime();
        if (s_runtime.relay_active &&
            s_runtime.relay.slot == (backend_scanner_slot_t)slot) {
            signal_relay_if_terminal_locked(
                backend_scanner_relay_receive(
                    &s_runtime.relay, &receipt, now_ms));
        }
        unlock_runtime();
    }
}

static void uart_worker(void *argument)
{
    const size_t slot = (size_t)(uintptr_t)argument;
    uint8_t input[512];
    lock_runtime();
    s_runtime.uart_worker_live[slot] = true;
    unlock_runtime();
    for (;;) {
        const uart_port_t port =
            (uart_port_t)s_runtime.uarts.slot[slot].config.uart;
        const int received = uart_read_bytes(
            port, input, sizeof(input), pdMS_TO_TICKS(100));
        size_t offset = 0U;
        while (received > 0 && offset < (size_t)received) {
            size_t consumed = 0U;
            const scanner_uart_line_event_t event =
                backend_uart_slot_consume(
                    &s_runtime.uarts,
                    slot,
                    input + offset,
                    (size_t)received - offset,
                    &consumed);
            if (consumed == 0U) {
                break;
            }
            offset += consumed;
            if (event.kind == SCANNER_UART_LINE_EVENT_FRAME_READY) {
                dispatch_uart_frame(slot, event.bytes, event.byte_len);
            }
        }
    }
}

static void send_role_locked(
    size_t slot, backend_scan_profile_t profile, int64_t now_ms)
{
    backend_scanner_health_t *health = &s_runtime.scanner_health[slot];
    if (!health->connected || health->boot_id == 0U ||
        s_runtime.role_generation == UINT32_MAX) {
        return;
    }
    const uint32_t generation = ++s_runtime.role_generation;
    if (s_runtime.scanner_wire[slot] ==
        UPLINK_SCANNER_WIRE_PRODUCTION) {
        const backend_scan_profile_t production_profile =
            production_profile_for_slot(slot);
        if (uart_send_production_profile(slot, now_ms)) {
            health->commanded_generation = generation;
            health->commanded_profile = production_profile;
            health->convergence_started_ms = now_ms;
            health->convergence_started = true;
            health->command_healthy = true;
            health->role_acked = false;
            health->radio_healthy = false;
        } else {
            health->command_healthy = false;
        }
        return;
    }
    backend_scanner_control_t control = {
        .type = BACKEND_SCANNER_CONTROL_ROLE,
        .payload.role = {
            .boot_id = health->boot_id,
            .generation = generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            .topology_generation = s_runtime.topology_generation,
#endif
            .profile = profile,
        },
    };
    if (uart_send_control(slot, &control)) {
        health->commanded_generation = generation;
        health->commanded_profile = profile;
        health->convergence_started_ms = now_ms;
        health->convergence_started = true;
    }
}

static void send_led_mirror_locked(
    const backend_led_mirror_output_t *mirror)
{
    const char *state = led_state_name(mirror->command.state);
    if (state == NULL) {
        return;
    }
    backend_scanner_control_t control = {
        .type = BACKEND_SCANNER_CONTROL_LED_STATE,
    };
    copy_text(control.payload.led.state,
              sizeof(control.payload.led.state), state);
    control.payload.led.generation = mirror->command.generation;
    control.payload.led.ttl_ms = mirror->command.ttl_ms;
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if ((mirror->send_mask & (UINT8_C(1) << slot)) != 0U) {
            if (s_runtime.scanner_wire[slot] ==
                UPLINK_SCANNER_WIRE_BACKEND) {
                (void)uart_send_control(slot, &control);
            }
        }
    }
}

static void service_relay_locked(int64_t now_ms)
{
    if (!s_runtime.relay_active) {
        return;
    }
    signal_relay_if_terminal_locked(
        backend_scanner_relay_tick(&s_runtime.relay, now_ms));
    if (!s_runtime.relay_active) {
        return;
    }
    backend_scanner_relay_action_t action;
    if (!backend_scanner_relay_take_action(
            &s_runtime.relay, now_ms, &action)) {
        return;
    }
    bool sent = false;
    if (action.kind == BACKEND_SCANNER_RELAY_ACTION_SEND_QUIET) {
        backend_scanner_control_t quiet = {
            .type = BACKEND_SCANNER_CONTROL_ROLE,
            .payload.role = {
                .boot_id = s_runtime.relay.old_boot_id,
                .generation = s_runtime.relay_quiet_generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
                .topology_generation = s_runtime.topology_generation,
#endif
                .profile = BACKEND_SCAN_PROFILE_QUIESCENT,
            },
        };
        sent = uart_send_control(action.slot, &quiet);
    } else if (action.kind == BACKEND_SCANNER_RELAY_ACTION_SEND_CHUNK) {
        sent = uart_write_locked(
            action.slot, action.frame, action.frame_length);
    } else {
        sent = uart_send_control(action.slot, &action.control);
    }
    if (!sent) {
        s_runtime.scanner_health[action.slot].command_healthy = false;
    }
}

static void coordinator_worker(void *argument)
{
    (void)argument;
    lock_runtime();
    s_runtime.coordinator_worker_live = true;
    unlock_runtime();
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        uint8_t retry_slot = 0U;
        backend_detection_observation_t retry_threat;
        lock_coordinator();
        (void)backend_coordinator_tick_detections(
            &s_runtime.coordinator, now_ms);
        const bool retried = backend_coordinator_retry_one(
            &s_runtime.coordinator, &retry_slot, &retry_threat);
        unlock_coordinator();
        if (retried) {
            lock_runtime();
            backend_threat_ingest(
                &s_runtime.threats, &retry_threat.detection, now_ms);
            unlock_runtime();
        }

        lock_runtime();
        for (size_t slot = 0U; slot < 2U; ++slot) {
            if (s_runtime.scanner_wire[slot] ==
                    UPLINK_SCANNER_WIRE_UNKNOWN &&
                now_ms - s_runtime.production_ready_sent_ms[slot] >=
                    INT64_C(1000)) {
                (void)uart_send_production_profile(slot, now_ms);
            }
            if (s_runtime.scanner_health[slot].connected &&
                now_ms - s_runtime.scanner_last_seen_ms[slot] >
                    UPLINK_SCANNER_STALE_MS) {
                s_runtime.scanner_health[slot].connected = false;
                s_runtime.scanner_health[slot].role_acked = false;
                s_runtime.scanner_health[slot].radio_healthy = false;
            }
            if (s_runtime.scanner_health[slot].connected &&
                s_runtime.scanner_wire[slot] ==
                    UPLINK_SCANNER_WIRE_PRODUCTION &&
                now_ms - s_runtime.production_ready_sent_ms[slot] >=
                    INT64_C(30000)) {
                (void)uart_send_production_ready(slot, now_ms);
            }
        }
        backend_scanner_plan_compute(
            s_runtime.scanner_health,
            s_runtime.boot_monotonic_ms,
            now_ms,
            &s_runtime.scanner_plan);
        for (size_t slot = 0U; slot < 2U; ++slot) {
            if (s_runtime.target_claimed[slot + 1U]) {
                continue;
            }
            const backend_scan_profile_t desired =
                s_runtime.scanner_wire[slot] == UPLINK_SCANNER_WIRE_PRODUCTION
                    ? production_profile_for_slot(slot)
                    : s_runtime.scanner_plan.desired[slot];
            backend_scanner_health_t *health =
                &s_runtime.scanner_health[slot];
            if (health->connected &&
                (health->commanded_generation == 0U ||
                 health->commanded_profile != desired ||
                 (!health->role_acked &&
                  now_ms - health->convergence_started_ms >= 2000))) {
                send_role_locked(slot, desired, now_ms);
            }
        }

        backend_threat_snapshot_t threats;
        backend_threat_snapshot(&s_runtime.threats, now_ms, &threats);
        backend_health_inputs_t inputs = {
            .scanner_usable = {
                s_runtime.scanner_health[0].connected &&
                    s_runtime.scanner_health[0].identity_valid,
                s_runtime.scanner_health[1].connected &&
                    s_runtime.scanner_health[1].identity_valid,
            },
            .wifi_connected = s_runtime.wifi_connected,
            .backend_reachable = s_runtime.backend_reachable,
            .fatal_runtime = s_runtime.fatal_runtime,
            .threats = threats,
        };
        uint8_t connected_mask = 0U;
        for (size_t slot = 0U; slot < 2U; ++slot) {
            if (s_runtime.scanner_health[slot].connected) {
                connected_mask |= (uint8_t)(UINT8_C(1) << slot);
            }
        }
        service_relay_locked(now_ms);
        unlock_runtime();

        backend_health_snapshot_t health_snapshot;
        backend_health_evaluate(&inputs, &health_snapshot);
        (void)backend_status_led_set_state(health_snapshot.led_state);
        backend_led_mirror_output_t mirror;
        lock_coordinator();
        backend_coordinator_update_led(
            &s_runtime.coordinator,
            health_snapshot.led_state,
            connected_mask,
            now_ms,
            &mirror);
        const bool paused = backend_coordinator_flow_paused(
            &s_runtime.coordinator);
        unlock_coordinator();

        lock_runtime();
        send_led_mirror_locked(&mirror);
        if (!s_runtime.flow_state_known ||
            paused != s_runtime.flow_sent_paused) {
            if (s_runtime.flow_generation != UINT32_MAX) {
                backend_scanner_control_t flow = {
                    .type = BACKEND_SCANNER_CONTROL_FLOW,
                    .payload.flow = {
                        .generation = ++s_runtime.flow_generation,
                        .paused = paused,
                    },
                };
                for (size_t slot = 0U; slot < 2U; ++slot) {
                    if ((connected_mask & (UINT8_C(1) << slot)) != 0U) {
                        if (s_runtime.scanner_wire[slot] ==
                            UPLINK_SCANNER_WIRE_PRODUCTION) {
                            (void)uart_send_production_flow(
                                slot, paused, now_ms);
                        } else {
                            (void)uart_send_control(slot, &flow);
                        }
                    }
                }
                s_runtime.flow_state_known = true;
                s_runtime.flow_sent_paused = paused;
            }
        }
        unlock_runtime();
        vTaskDelay(pdMS_TO_TICKS(UPLINK_COORDINATOR_PERIOD_MS));
    }
}

static void network_worker(void *argument)
{
    (void)argument;
    lock_runtime();
    s_runtime.network_worker_live = true;
    unlock_runtime();
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        backend_ap_action_t ap_action = BACKEND_AP_NO_CHANGE;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        const bool usb_live_confirmed = s_runtime.usb_available &&
            backend_usb_service_live_confirmed(&s_runtime.usb, now_ms);
#endif
        lock_runtime();
        if (s_runtime.wifi_initialized) {
            (void)backend_wifi_manager_handle_event(
                &s_runtime.wifi,
                BACKEND_WIFI_EVENT_TICK,
                now_ms);
        }
        if (s_runtime.wifi_connected) {
            wifi_ap_record_t record;
            if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
                s_runtime.wifi_rssi = record.rssi;
                copy_text(
                    s_runtime.wifi_ssid,
                    sizeof(s_runtime.wifi_ssid),
                    (const char *)record.ssid);
            }
        }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        const bool config_valid = s_runtime.config_loaded &&
            s_runtime.config.network_count > 0U &&
            backend_config_validate(&s_runtime.config) ==
                BACKEND_CONFIG_VALID;
        const backend_lite_ap_input_t ap_input = {
            .wifi_configured = config_valid,
            .wifi_connected = s_runtime.wifi_connected,
            .wifi_join_failed = s_runtime.wifi_initialized &&
                backend_wifi_manager_join_failed(&s_runtime.wifi),
            .usb_live_confirmed = usb_live_confirmed,
        };
        ap_action = backend_lite_ap_policy_tick(
            &s_runtime.lite_ap_policy, ap_input);
#else
        const backend_ap_input_t ap_input = {
            .config_valid = s_runtime.config_loaded &&
                s_runtime.config.network_count > 0U &&
                backend_config_validate(&s_runtime.config) ==
                    BACKEND_CONFIG_VALID,
            .config_generation = s_runtime.config.generation,
            .backend_connected = s_runtime.backend_reachable,
            .usb_start_requested = s_runtime.usb_ap_start_requested,
        };
        s_runtime.usb_ap_start_requested = false;
        ap_action = backend_ap_policy_tick(
            &s_runtime.ap_policy, ap_input, monotonic_ms());
#endif
        unlock_runtime();
        if (ap_action == BACKEND_AP_START) {
            const bool started = backend_config_portal_start(
                &s_runtime.portal, s_runtime.mac);
            lock_runtime();
            s_runtime.portal_started = started;
            if (!started) {
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
                s_runtime.lite_ap_policy.running = false;
#else
                s_runtime.ap_policy.running = false;
#endif
            }
            unlock_runtime();
        } else if (ap_action == BACKEND_AP_STOP) {
            const bool stopped = backend_config_portal_stop(
                &s_runtime.portal);
            lock_runtime();
            s_runtime.portal_started = !stopped;
            if (!stopped) {
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
                s_runtime.lite_ap_policy.running = true;
#else
                s_runtime.ap_policy.running = true;
#endif
            }
            unlock_runtime();
        }
        vTaskDelay(pdMS_TO_TICKS(UPLINK_NETWORK_PERIOD_MS));
    }
}

static void uploader_worker(void *argument)
{
    (void)argument;
    backend_upload_batch_t *request = &s_runtime.upload_request_scratch;
    lock_runtime();
    s_runtime.uploader_worker_live = true;
    unlock_runtime();
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        bool have_request = false;
        char base_url[sizeof(s_runtime.config.backend_url)];
        char device_id[sizeof(s_runtime.config.device_id)];
        lock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        const bool heartbeat_due = backend_lite_network_can_use_sta(
            s_runtime.config.network_count,
            s_runtime.wifi_initialized,
            s_runtime.wifi_connected) && backend_heartbeat_due(
                &s_runtime.heartbeat, now_ms);
#else
        const bool heartbeat_due = backend_heartbeat_due(
            &s_runtime.heartbeat, now_ms);
#endif
        unlock_runtime();
        if (heartbeat_due && queue_upload(NULL)) {
            lock_runtime();
            backend_heartbeat_mark_queued(&s_runtime.heartbeat, now_ms);
            unlock_runtime();
        }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        /* Hold HTTP across admission, POST, and response bookkeeping. */
        if (!acquire_lite_http_if_sta_usable()) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_UPLOAD_PERIOD_MS));
            continue;
        }
        lock_runtime();
        const backend_upload_batch_t *head =
            backend_upload_fifo_peek(&s_runtime.upload_fifo);
        if (head != NULL && backend_uploader_begin_head(
                &s_runtime.uploader,
                head,
                s_runtime.upload_fifo.count,
                now_ms)) {
            *request = *head;
            copy_text(base_url, sizeof(base_url),
                      s_runtime.config.backend_url);
            copy_text(device_id, sizeof(device_id),
                      s_runtime.config.device_id);
            have_request = true;
        }
        unlock_runtime();
        if (!have_request) {
            release_lite_http();
            vTaskDelay(pdMS_TO_TICKS(UPLINK_UPLOAD_PERIOD_MS));
            continue;
        }
#else
        lock_runtime();
        const backend_upload_batch_t *head =
            backend_upload_fifo_peek(&s_runtime.upload_fifo);
        if (head != NULL && backend_uploader_begin_head(
                &s_runtime.uploader,
                head,
                s_runtime.upload_fifo.count,
                now_ms)) {
            *request = *head;
            copy_text(base_url, sizeof(base_url),
                      s_runtime.config.backend_url);
            copy_text(device_id, sizeof(device_id),
                      s_runtime.config.device_id);
            have_request = true;
        }
        unlock_runtime();
        if (!have_request) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_UPLOAD_PERIOD_MS));
            continue;
        }

        (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
        const backend_http_result_t http_result = backend_http_post_json(
            base_url,
            "/detections/drones",
            request->json,
            request->json_len,
            s_runtime.uploader_http_response,
            sizeof(s_runtime.uploader_http_response));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        (void)xSemaphoreGive(s_runtime.http_lock);
#endif
        const bool ack_valid = http_result.transport_complete &&
            backend_ingest_ack_validate(
                s_runtime.uploader_http_response,
                http_result.body_length,
                device_id,
                request->item_count);
        const backend_http_disposition_t disposition = backend_http_classify(
            http_result.transport_complete,
            http_result.status_code,
            ack_valid);
        lock_runtime();
        backend_uploader_queue_result_t queue_result =
            BACKEND_UPLOADER_QUEUE_UNCHANGED;
        const backend_upload_batch_t *live_head =
            backend_upload_fifo_peek(&s_runtime.upload_fifo);
        if (live_head != NULL &&
            live_head->sequence == request->sequence &&
            live_head->json_crc32 == request->json_crc32 &&
            (disposition == BACKEND_HTTP_ACK ||
             disposition == BACKEND_HTTP_QUARANTINE) &&
            backend_upload_fifo_pop_acked(
                &s_runtime.upload_fifo, request->sequence)) {
            queue_result = disposition == BACKEND_HTTP_ACK
                ? BACKEND_UPLOADER_QUEUE_POPPED
                : BACKEND_UPLOADER_QUEUE_QUARANTINED;
        }
        const backend_uploader_outcome_t outcome =
            backend_uploader_note_response(
                &s_runtime.uploader,
                request->sequence,
                request->json_crc32,
                disposition,
                http_result.status_code,
                queue_result,
                s_runtime.upload_fifo.count,
                esp_random(),
                now_ms);
        if (outcome == BACKEND_UPLOADER_ACKED) {
            s_runtime.backend_reachable = true;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            backend_ap_policy_note_backend_success(
                &s_runtime.ap_policy,
                s_runtime.config.generation,
                now_ms);
#endif
        } else if (!http_result.transport_complete) {
            s_runtime.backend_reachable = false;
        }
        unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        release_lite_http();
#endif
    }
}

static void broadcast_time(
    int64_t epoch_ms, backend_time_source_t source)
{
    lock_runtime();
    if (s_runtime.time_generation == UINT32_MAX) {
        unlock_runtime();
        return;
    }
    const uint32_t generation = ++s_runtime.time_generation;
    s_runtime.epoch_valid = epoch_ms > BACKEND_DETECTION_EPOCH_MIN_MS;
    s_runtime.epoch_anchor_ms = epoch_ms;
    s_runtime.epoch_anchor_monotonic_ms = monotonic_ms();
    backend_scanner_control_t control = {
        .type = BACKEND_SCANNER_CONTROL_TIME,
        .payload.time = {
            .generation = generation,
            .valid = s_runtime.epoch_valid,
            .epoch_ms = epoch_ms,
            .source = source == BACKEND_TIME_SOURCE_SNTP
                ? BACKEND_SCANNER_TIME_SNTP
                : source == BACKEND_TIME_SOURCE_BACKEND
                    ? BACKEND_SCANNER_TIME_BACKEND
                    : BACKEND_SCANNER_TIME_NONE,
        },
    };
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (s_runtime.scanner_wire[slot] ==
                UPLINK_SCANNER_WIRE_PRODUCTION ||
            s_runtime.scanner_wire[slot] ==
                UPLINK_SCANNER_WIRE_UNKNOWN) {
            (void)uart_send_production_time(slot, epoch_ms, source);
        } else if (s_runtime.scanner_health[slot].connected) {
            (void)uart_send_control(slot, &control);
        }
    }
    unlock_runtime();
}

static void time_worker(void *argument)
{
    (void)argument;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    lock_runtime();
    s_runtime.time_worker_live = true;
    unlock_runtime();
    for (;;) {
        const time_t seconds = time(NULL);
        const int64_t sntp_epoch_ms = seconds > 0
            ? (int64_t)seconds * INT64_C(1000) : 0;
        const bool sntp_valid =
            esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET &&
            sntp_epoch_ms > BACKEND_DETECTION_EPOCH_MIN_MS;
        bool backend_valid = false;
        int64_t backend_epoch_ms = 0;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        bool lite_http_held = false;
#endif
        char base_url[sizeof(s_runtime.config.backend_url)];
        bool connected = false;
        lock_runtime();
        connected = s_runtime.wifi_connected;
        copy_text(base_url, sizeof(base_url), s_runtime.config.backend_url);
        unlock_runtime();
        if (!sntp_valid && connected) {
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
            if (acquire_lite_http_if_sta_usable()) {
                lite_http_held = true;
#else
            (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
            const backend_http_result_t result = backend_http_get_json(
                base_url,
                "/detections/time",
                s_runtime.time_http_response,
                sizeof(s_runtime.time_http_response));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            (void)xSemaphoreGive(s_runtime.http_lock);
#endif
            backend_valid = result.transport_complete &&
                result.status_code == 200 &&
                backend_time_parse_response(
                    s_runtime.time_http_response, result.body_length,
                    &backend_epoch_ms);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
            }
#endif
        }
        int64_t selected_epoch_ms = 0;
        const backend_time_source_t source = backend_time_select_source(
            sntp_valid,
            sntp_epoch_ms,
            backend_valid,
            backend_epoch_ms,
            &selected_epoch_ms);
        if (source != BACKEND_TIME_SOURCE_NONE) {
            broadcast_time(selected_epoch_ms, source);
        }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        if (lite_http_held) {
            release_lite_http();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(UPLINK_TIME_PERIOD_MS));
    }
}

static void command_send_result(
    const char *base_url,
    const char *device_id,
    int64_t now_ms)
{
    backend_command_result_t result;
    char path[BACKEND_COMMAND_PATH_CAPACITY];
    char body[BACKEND_COMMAND_RESULT_MAX_JSON + 1U];
    size_t body_length = 0U;
    lock_runtime();
    if (!backend_ble_investigation_next_result(
            &s_runtime.investigation, &result) ||
        !backend_command_result_prepare(&s_runtime.command_client, &result)) {
        unlock_runtime();
        return;
    }
    copy_text(path, sizeof(path), s_runtime.command_client.result_path);
    body_length = s_runtime.command_client.post_body_length;
    memcpy(body, s_runtime.command_client.post_body, body_length + 1U);
    unlock_runtime();

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    if (!acquire_lite_http_if_sta_usable()) {
        return;
    }
#else
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
    const backend_http_result_t http_result = backend_http_post_json(
        base_url, path, body, body_length, s_runtime.command_http_response,
        sizeof(s_runtime.command_http_response));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    (void)xSemaphoreGive(s_runtime.http_lock);
#endif
    lock_runtime();
    backend_command_result_ack_t ack;
    const bool ack_valid = http_result.transport_complete &&
        backend_command_result_ack_validate(
            &s_runtime.command_client,
            s_runtime.command_http_response,
            http_result.body_length,
            &ack);
    const backend_command_http_action_t action =
        backend_command_result_http_action(
            http_result.transport_complete,
            http_result.status_code,
            ack_valid);
    backend_command_http_note(
        &s_runtime.command_http, action, http_result.status_code, true);
    if (action == BACKEND_COMMAND_HTTP_ACK &&
        backend_ble_investigation_mark_acked(
            &s_runtime.investigation,
            ack.command_id,
            ack.accepted_sequence) &&
        backend_command_result_ack_commit(&s_runtime.command_client, &ack)) {
        s_runtime.command_success_count++;
    } else if (action == BACKEND_COMMAND_HTTP_QUARANTINE) {
        s_runtime.command_failure_count++;
    }
    (void)device_id;
    (void)now_ms;
    unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    release_lite_http();
#endif
}

static void command_worker(void *argument)
{
    (void)argument;
    lock_runtime();
    s_runtime.command_worker_live = true;
    unlock_runtime();
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        char base_url[sizeof(s_runtime.config.backend_url)];
        char device_id[sizeof(s_runtime.config.device_id)];
        bool connected;
        lock_runtime();
        connected = s_runtime.wifi_connected;
        copy_text(base_url, sizeof(base_url), s_runtime.config.backend_url);
        copy_text(device_id, sizeof(device_id), s_runtime.config.device_id);
        (void)backend_ble_investigation_check_timeout(
            &s_runtime.investigation, now_ms);
        const bool due = backend_command_poll_due(
            &s_runtime.command_http, now_ms);
        unlock_runtime();
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        command_send_result(base_url, device_id, now_ms);
        if (!due) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        char path[BACKEND_COMMAND_PATH_CAPACITY];
        if (!backend_command_build_poll_path(
                device_id, path, sizeof(path))) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        if (!acquire_lite_http_if_sta_usable()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
#endif
        lock_runtime();
        (void)backend_command_poll_started(&s_runtime.command_http, now_ms);
        unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
#endif
        const backend_http_result_t http_result = backend_http_get_json(
            base_url, path, s_runtime.command_http_response,
            sizeof(s_runtime.command_http_response));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        (void)xSemaphoreGive(s_runtime.http_lock);
#endif
        const backend_command_http_action_t action =
            backend_command_poll_http_action(
                http_result.transport_complete, http_result.status_code);
        lock_runtime();
        backend_command_http_note(
            &s_runtime.command_http, action, http_result.status_code, false);
        if (action == BACKEND_COMMAND_HTTP_BODY) {
            backend_command_envelope_t envelope;
            if (backend_command_envelope_decode(
                    s_runtime.command_http_response, http_result.body_length,
                    &envelope) ==
                BACKEND_COMMAND_DECODE_OK) {
                const backend_command_intent_t intent =
                    backend_command_select_intent(
                        &envelope, &s_runtime.investigation);
                int owner = backend_scanner_ble_owner(
                    &s_runtime.scanner_plan);
                bool accepted = false;
                if (intent == BACKEND_COMMAND_INTENT_START && owner >= 0) {
                    accepted = backend_ble_investigation_start(
                        &s_runtime.investigation,
                        envelope.command_id,
                        &envelope.request,
                        (backend_scanner_slot_t)owner,
                        now_ms);
                } else if (intent ==
                           BACKEND_COMMAND_INTENT_CANCEL_FIRST_SEEN) {
                    accepted = backend_ble_investigation_cancel_first_seen(
                        &s_runtime.investigation,
                        envelope.command_id,
                        &envelope.request,
                        now_ms);
                } else if (intent == BACKEND_COMMAND_INTENT_CANCEL_ACTIVE) {
                    accepted = backend_ble_investigation_request_cancel(
                        &s_runtime.investigation, envelope.command_id);
                    owner = s_runtime.investigation.scanner_slot;
                } else if (intent ==
                           BACKEND_COMMAND_INTENT_ALREADY_ACTIVE) {
                    accepted = true;
                    owner = s_runtime.investigation.scanner_slot;
                }
                char line[BACKEND_SCANNER_WIRE_MAX_LINE + 1U];
                const size_t line_length =
                    backend_command_scanner_line_encode(
                        &envelope, intent, line, sizeof(line));
                if (accepted && owner >= 0 && line_length != 0U) {
                    accepted = uart_send_line(
                        (size_t)owner, line, line_length);
                }
                if (accepted && backend_command_client_bind(
                        &s_runtime.command_client, device_id, &envelope)) {
                    s_runtime.command_success_count++;
                } else if (!accepted) {
                    s_runtime.command_failure_count++;
                }
            } else {
                s_runtime.command_failure_count++;
            }
        }
        unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        release_lite_http();
#endif
    }
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
static bool ota_lower_hex_decode_32(const char *text, uint8_t output[32])
{
    if (text == NULL || output == NULL || text[64] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        const char high = text[index * 2U];
        const char low = text[index * 2U + 1U];
        const int high_value = high >= '0' && high <= '9'
            ? high - '0' : high >= 'a' && high <= 'f'
                ? high - 'a' + 10 : -1;
        const int low_value = low >= '0' && low <= '9'
            ? low - '0' : low >= 'a' && low <= 'f'
                ? low - 'a' + 10 : -1;
        if (high_value < 0 || low_value < 0) {
            memset(output, 0, 32U);
            return false;
        }
        output[index] =
            (uint8_t)((unsigned)high_value * 16U + (unsigned)low_value);
    }
    return true;
}

static bool ota_command_request(
    const backend_ota_command_envelope_t *command,
    backend_ota_request_t *out)
{
    if (command == NULL || out == NULL || !command->has_operation_id) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->probe = !command->is_apply;
    out->has_operation_id = true;
    out->operation_id = command->operation_id;
    out->expected_size = command->expected_size;
    out->has_accepted_probe_receipt = command->is_apply;
    out->command_next_sequence = command->next_sequence;
    out->component = command->component;
    out->apply_mode = command->apply_mode;
    memcpy(out->catalog_name, command->catalog_name,
           sizeof(out->catalog_name));
    memcpy(out->expected_sha256, command->expected_sha256,
           sizeof(out->expected_sha256));
    memcpy(out->expected_mac, command->binding.target_mac, 6U);
    out->expected_boot_id = command->binding.target_boot_id;
    out->expected_topology_generation =
        command->binding.topology_generation;
    return !command->is_apply || ota_lower_hex_decode_32(
        command->probe_receipt_sha256,
        out->accepted_probe_receipt_sha256);
}

static bool ota_progress_event(
    const backend_ota_progress_update_t *update,
    uint32_t sequence,
    backend_ota_progress_event_t *out)
{
    const backend_ota_command_envelope_t *command =
        &s_runtime.ota_workflow.command;
    if (update == NULL || out == NULL ||
        !s_runtime.ota_workflow.command_active ||
        !update->has_operation_id ||
        !backend_ota_operation_id_equal(
            &update->operation_id, &command->operation_id) ||
        update->probe == command->is_apply ||
        update->component != command->component ||
        strcmp(update->catalog_name, command->catalog_name) != 0 ||
        strcmp(update->manifest.sha256, command->expected_sha256) != 0 ||
        update->manifest.image_size != command->expected_size ||
        update->stage > BACKEND_OTA_JOURNAL_PROGRESS_CONVERGENCE) {
        return false;
    }
    *out = (backend_ota_progress_event_t) {
        .prefix = {
            .has_operation_id = true,
            .operation_id = update->operation_id,
            .is_apply = command->is_apply,
            .sequence = sequence,
            .component = update->component,
            .catalog_name = command->catalog_name,
        },
        .stage = (backend_ota_progress_stage_t)update->stage,
        .received = update->received,
        .total = update->total,
        .retry_count = update->retry_count,
    };
    return true;
}

static bool ota_command_local(
    const backend_ota_workflow_t *workflow,
    backend_ota_command_local_t *out)
{
    if (workflow == NULL || out == NULL) {
        return false;
    }
    const backend_ota_component_t component = workflow->expected_component;
    const backend_firmware_identity_t *identity = backend_identity_for_image(
        component == BACKEND_OTA_COMPONENT_UPLINK
            ? BACKEND_IMAGE_UPLINK : BACKEND_IMAGE_SCANNER);
    backend_ota_target_binding_t target;
    if (identity == NULL || !ota_snapshot_binding(NULL, component, &target)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->component = component;
    out->catalog_name = identity->target;
    out->target = identity->target;
    out->project = identity->project;
    out->hardware = identity->hardware;
    memcpy(out->binding.uplink_mac, s_runtime.mac, 6U);
    out->binding.uplink_boot_id = s_runtime.boot_id;
    memcpy(out->binding.target_mac, target.target_mac, 6U);
    out->binding.target_boot_id = target.target_boot_id;
    out->binding.topology_generation = target.topology_generation;
    out->max_expected_size = (uint32_t)ota_partition_capacity(NULL, component);
    out->has_expected_next_sequence = workflow->has_expected_sequence;
    out->expected_next_sequence = workflow->expected_sequence;
    out->has_accepted_probe = workflow->has_accepted_probe;
    out->accepted_probe = workflow->accepted_probe;
    return out->max_expected_size != 0U;
}

static bool ota_drain_pending_outbox(
    const char *base_url, const char *device_id,
    const backend_ota_event_outbox_snapshot_t *pending)
{
    if (base_url == NULL || device_id == NULL || pending == NULL ||
        !s_runtime.ota_workflow.command_active) {
        return false;
    }
    backend_ota_workflow_ack_prediction_t prediction;
    const bool terminal = s_runtime.ota_terminal_event_pending;
    const bool progress = s_runtime.ota_progress_event_pending;
    const bool predicted = terminal
        ? backend_ota_workflow_predict_terminal_ack(
              &s_runtime.ota_workflow, s_runtime.ota_terminal_outcome,
              &prediction)
        : progress
            ? backend_ota_workflow_predict_progress_ack(
                  &s_runtime.ota_workflow, &prediction)
        : backend_ota_workflow_predict_begin_ack(
              &s_runtime.ota_workflow, &prediction);
    char operation_id[BACKEND_OTA_OPERATION_ID_HEX_LENGTH + 1U];
    char path[UPLINK_OTA_CLIENT_PATH_CAPACITY];
    if (!predicted || !backend_ota_operation_id_encode(
            &pending->pending.operation_id, operation_id,
            sizeof(operation_id)) ||
        snprintf(
            path, sizeof(path), "/nodes/%s/backend-ota/%s/events",
            device_id, operation_id) >= (int)sizeof(path)) {
        return false;
    }
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
    const backend_http_result_t http = backend_http_post_json(
        base_url, path, (const char *)pending->pending.body,
        pending->pending.body_length, s_runtime.ota_client_http_response,
        sizeof(s_runtime.ota_client_http_response));
    (void)xSemaphoreGive(s_runtime.http_lock);
    const char *action = prediction.is_apply ? "apply" : "probe";
    backend_ota_command_ack_t ack;
    if (!http.transport_complete || http.status_code < 200 ||
        http.status_code >= 300 ||
        !backend_ota_command_ack_decode(
            s_runtime.ota_client_http_response, http.body_length,
            &pending->pending.operation_id, pending->pending.sequence,
            prediction.component, action, prediction.terminal, &ack)) {
        return false;
    }
    const backend_ota_event_outbox_ack_t outbox_ack = {
        .strict_decoded = true,
        .http_status = (uint16_t)http.status_code,
        .ok = ack.ok,
        .has_operation_id = ack.has_operation_id,
        .operation_id = ack.operation_id,
        .accepted_sequence = ack.accepted_sequence,
        .next_sequence = ack.next_sequence,
    };
    backend_ota_progress_event_t progress_event;
    if (progress) {
        backend_ota_request_t request;
        if (!ota_progress_event(
                &s_runtime.ota_pending_progress,
                pending->pending.sequence, &progress_event) ||
            !ota_command_request(
                &s_runtime.ota_workflow.command, &request) ||
            !backend_ota_maintenance_ack_fullsize_progress(
                &s_runtime.maintenance, &request,
                &s_runtime.ota_pending_progress,
                ack.accepted_sequence, ack.next_sequence)) {
            s_runtime.ota_client_blocked = true;
            return false;
        }
    }
    if (backend_ota_event_outbox_acknowledge(
            &s_runtime.ota_outbox_storage, &outbox_ack) !=
        BACKEND_OTA_EVENT_OUTBOX_ACK_CLEARED) {
        return false;
    }
    if (terminal) {
        if (!backend_ota_workflow_note_terminal_ack(
                &s_runtime.ota_workflow, s_runtime.ota_terminal_outcome,
                s_runtime.ota_terminal_receipt_sha256, &ack)) {
            s_runtime.ota_client_blocked = true;
            return false;
        }
        s_runtime.ota_terminal_event_pending = false;
        memset(s_runtime.ota_terminal_receipt_sha256, 0,
               sizeof(s_runtime.ota_terminal_receipt_sha256));
    } else if (progress) {
        if (!backend_ota_workflow_note_progress_ack(
                &s_runtime.ota_workflow, &progress_event, &ack)) {
            s_runtime.ota_client_blocked = true;
            return false;
        }
        s_runtime.ota_progress_event_pending = false;
        memset(&s_runtime.ota_pending_progress, 0,
               sizeof(s_runtime.ota_pending_progress));
        if (s_runtime.ota_progress_waiter) {
            s_runtime.ota_progress_waiter = false;
            (void)xSemaphoreGive(s_runtime.ota_progress_ack);
        }
    } else if (!backend_ota_workflow_note_begin_ack(
                   &s_runtime.ota_workflow, &ack)) {
        s_runtime.ota_client_blocked = true;
        return false;
    }
    return true;
}

static backend_ota_terminal_error_t ota_terminal_error(
    backend_ota_decision_t decision)
{
    switch (decision) {
    case BACKEND_OTA_DECISION_REJECT_IDENTITY:
        return BACKEND_OTA_TERMINAL_ERROR_IDENTITY_MISMATCH;
    case BACKEND_OTA_DECISION_REJECT_TARGET_BINDING:
        return BACKEND_OTA_TERMINAL_ERROR_STALE_BINDING;
    case BACKEND_OTA_DECISION_REJECT_CAPACITY:
        return BACKEND_OTA_TERMINAL_ERROR_CAPACITY;
    case BACKEND_OTA_DECISION_REJECT_DIGEST:
        return BACKEND_OTA_TERMINAL_ERROR_HASH_MISMATCH;
    case BACKEND_OTA_DECISION_REJECT_SIZE:
    case BACKEND_OTA_DECISION_REJECT_BUSY:
    case BACKEND_OTA_DECISION_REJECT_VERSION:
    case BACKEND_OTA_DECISION_FAILED:
    default:
        return BACKEND_OTA_TERMINAL_ERROR_INTERNAL;
    }
}

static bool ota_terminal_health(
    backend_ota_component_t component,
    backend_ota_terminal_evidence_t *terminal)
{
    if (terminal == NULL || !ota_snapshot_binding(
            NULL, component, &terminal->actual_binding)) {
        return false;
    }
    if (component == BACKEND_OTA_COMPONENT_UPLINK) {
        terminal->identity_exact = true;
        terminal->command_ingress_healthy = true;
        terminal->role_acked = true;
        terminal->profile_correct = true;
        terminal->radio_healthy = true;
        terminal->rollback_clear = backend_self_ota_rollback_clear(
            &s_runtime.self_ota);
        return true;
    }
    const int slot = backend_ota_component_slot(component);
    if (slot < 0 || slot >= 2) {
        return false;
    }
    lock_runtime();
    const backend_scanner_status_t *status =
        &s_runtime.scanner_tracker[slot].status;
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    terminal->identity_exact = identity != NULL &&
        backend_identity_matches(
            identity, status->target, status->project, status->hardware);
    terminal->command_ingress_healthy = status->command_ingress;
    terminal->role_acked = status->role_acked;
    terminal->profile_correct =
        status->profile == s_runtime.scanner_health[slot].commanded_profile;
    terminal->radio_healthy = backend_scanner_required_radio_healthy(
        status->profile, status->ble_healthy, status->wifi_healthy);
    terminal->rollback_clear = strcmp(status->rollback_state, "valid") == 0;
    unlock_runtime();
    return true;
}

static bool ota_build_terminal_event(
    const uplink_ota_result_item_t *result,
    uint32_t sequence,
    char *body,
    size_t capacity,
    backend_ota_terminal_outcome_t *outcome,
    backend_ota_built_end_t *built)
{
    if (result == NULL || body == NULL || outcome == NULL || built == NULL) {
        return false;
    }
    backend_ota_terminal_evidence_t terminal;
    memset(&terminal, 0, sizeof(terminal));
    terminal.candidate = result->evidence.manifest;
    terminal.relation = result->relation;
    terminal.complete_image_validated =
        result->evidence.complete_image_validated;
    terminal.validated_image_bytes = terminal.complete_image_validated
        ? result->evidence.manifest.image_size : 0U;
    if (!ota_terminal_health(result->command.component, &terminal)) {
        return false;
    }
    if (!result->command.is_apply &&
        result->evidence.decision == BACKEND_OTA_DECISION_ADMIT) {
        terminal.outcome = BACKEND_OTA_TERMINAL_ELIGIBLE;
        terminal.error = BACKEND_OTA_TERMINAL_ERROR_NONE;
    } else if (!result->command.is_apply &&
               result->evidence.decision == BACKEND_OTA_DECISION_NO_UPDATE) {
        terminal.outcome = BACKEND_OTA_TERMINAL_NO_UPDATE;
        terminal.error = BACKEND_OTA_TERMINAL_ERROR_NONE;
    } else if (result->command.is_apply &&
               result->evidence.decision == BACKEND_OTA_DECISION_APPLIED &&
               result->evidence.converged &&
               result->evidence.rollback_clear) {
        terminal.outcome = BACKEND_OTA_TERMINAL_APPLIED;
        terminal.error = BACKEND_OTA_TERMINAL_ERROR_NONE;
        terminal.image_writes = result->command.expected_size;
    } else {
        terminal.outcome = BACKEND_OTA_TERMINAL_FAILED;
        terminal.error = ota_terminal_error(result->evidence.decision);
        if (result->command.is_apply ||
            result->evidence.manifest.target[0] != '\0') {
            const backend_firmware_identity_t *identity =
                backend_identity_for_image(
                    result->command.component == BACKEND_OTA_COMPONENT_UPLINK
                        ? BACKEND_IMAGE_UPLINK : BACKEND_IMAGE_SCANNER);
            if (identity == NULL) {
                return false;
            }
            terminal.has_observed_failure_identity = true;
            copy_text(terminal.observed_target,
                      sizeof(terminal.observed_target), identity->target);
            copy_text(terminal.observed_project,
                      sizeof(terminal.observed_project), identity->project);
            copy_text(terminal.observed_hardware,
                      sizeof(terminal.observed_hardware), identity->hardware);
            copy_text(
                terminal.observed_version,
                sizeof(terminal.observed_version),
                result->evidence.manifest.version[0] != '\0'
                    ? result->evidence.manifest.version
                    : result->running_version);
        }
    }
    *outcome = terminal.outcome;
    return backend_ota_event_end_build(
        &result->command, &s_runtime.ota_workflow.progress, sequence,
        &terminal, body, capacity, built);
}

static void ota_lower_hex_encode_32(
    const uint8_t bytes[32], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0U; index < 32U; ++index) {
        output[index * 2U] = digits[bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[bytes[index] & UINT8_C(0x0f)];
    }
    output[64] = '\0';
}

static bool ota_restore_command_from_journal(
    const backend_ota_journal_record_t *record,
    backend_ota_command_envelope_t *out)
{
    if (record == NULL || out == NULL || !record->has_operation_id) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->is_apply =
        record->action == BACKEND_OTA_JOURNAL_ACTION_APPLY;
    out->has_operation_id = true;
    out->operation_id = record->operation_id;
    out->component = record->component;
    memcpy(out->catalog_name, record->catalog_name,
           sizeof(out->catalog_name));
    memcpy(out->expected_sha256, record->expected_sha256,
           sizeof(out->expected_sha256));
    out->expected_size = record->expected_size;
    memcpy(out->binding.uplink_mac, record->expected_uplink_mac, 6U);
    out->binding.uplink_boot_id = record->expected_uplink_boot_id;
    memcpy(out->binding.target_mac, record->expected_target_mac, 6U);
    out->binding.target_boot_id = record->expected_target_boot_id;
    out->binding.topology_generation =
        record->expected_topology_generation;
    out->apply_mode = record->apply_mode;
    out->next_sequence = record->command_next_sequence;
    if (out->is_apply) {
        if (!record->has_accepted_probe_receipt) {
            return false;
        }
        ota_lower_hex_encode_32(
            record->accepted_probe_receipt_sha256,
            out->probe_receipt_sha256);
    }
    return true;
}

static bool ota_restore_workflow_from_journal(
    const backend_ota_journal_record_t *record,
    backend_ota_command_envelope_t *command)
{
    if (!ota_restore_command_from_journal(record, command)) {
        return false;
    }
    backend_ota_workflow_init(&s_runtime.ota_workflow);
    s_runtime.ota_workflow.has_rollout_operation = true;
    s_runtime.ota_workflow.rollout_operation_id = record->operation_id;
    s_runtime.ota_workflow.expected_component = record->component;
    s_runtime.ota_workflow.expected_apply = command->is_apply;
    s_runtime.ota_workflow.has_expected_sequence = true;
    s_runtime.ota_workflow.expected_sequence = command->next_sequence;
    if (command->is_apply) {
        backend_ota_accepted_probe_t *accepted =
            &s_runtime.ota_workflow.accepted_probe;
        memset(accepted, 0, sizeof(*accepted));
        accepted->probe.operation_id = command->operation_id;
        accepted->probe.component = command->component;
        memcpy(accepted->probe.catalog_name, command->catalog_name,
               sizeof(accepted->probe.catalog_name));
        if (!ota_lower_hex_decode_32(
                command->expected_sha256,
                accepted->probe.expected_sha256)) {
            return false;
        }
        accepted->probe.expected_size = command->expected_size;
        accepted->probe.binding = command->binding;
        accepted->probe.apply_mode = command->apply_mode;
        memcpy(accepted->receipt_sha256,
               record->accepted_probe_receipt_sha256, 32U);
        accepted->apply_start_sequence = command->next_sequence;
        s_runtime.ota_workflow.has_accepted_probe = true;
    }
    if (backend_ota_workflow_admit(
            &s_runtime.ota_workflow, command) !=
        BACKEND_OTA_WORKFLOW_ADMITTED) {
        return false;
    }
    if (record->progress_initialized) {
        s_runtime.ota_workflow.progress =
            (backend_ota_progress_state_t) {
                .initialized = true,
                .stage = (backend_ota_progress_stage_t)
                    record->progress_stage,
                .received = record->progress_received,
                .total = record->progress_total,
                .retry_count = record->progress_retry_count,
            };
        s_runtime.ota_workflow.begin_acked = true;
        s_runtime.ota_workflow.expected_sequence =
            record->event_sequence;
    }
    return true;
}

static bool ota_progress_update_from_journal(
    const backend_ota_journal_record_t *record,
    backend_ota_progress_update_t *update)
{
    if (record == NULL || update == NULL ||
        !record->progress_initialized || !record->has_manifest) {
        return false;
    }
    memset(update, 0, sizeof(*update));
    update->has_operation_id = true;
    update->operation_id = record->operation_id;
    update->probe = record->action == BACKEND_OTA_JOURNAL_ACTION_PROBE;
    update->component = record->component;
    memcpy(update->catalog_name, record->catalog_name,
           sizeof(update->catalog_name));
    update->manifest = record->manifest;
    update->stage = record->progress_stage;
    update->received = record->progress_received;
    update->total = record->progress_total;
    update->retry_count = record->progress_retry_count;
    return true;
}

static bool ota_restore_pending_progress(
    const backend_ota_journal_record_t *record,
    const backend_ota_event_outbox_snapshot_t *outbox)
{
    backend_ota_progress_update_t update;
    if (record == NULL || outbox == NULL ||
        outbox->pending.sequence == UINT32_MAX ||
        !backend_ota_operation_id_equal(
            &record->operation_id, &outbox->pending.operation_id) ||
        (record->event_sequence != outbox->pending.sequence &&
         record->event_sequence != outbox->pending.sequence + 1U) ||
        !ota_progress_update_from_journal(record, &update)) {
        return false;
    }
    backend_ota_progress_event_t event;
    backend_ota_progress_state_t candidate = s_runtime.ota_workflow.progress;
    char body[BACKEND_OTA_EVENT_MAX_BYTES];
    if (!ota_progress_event(
            &update, outbox->pending.sequence, &event)) {
        return false;
    }
    const size_t length = backend_ota_event_progress_encode(
        &candidate, &event, body, sizeof(body));
    if (length == 0U || length != outbox->pending.body_length ||
        memcmp(body, outbox->pending.body, length) != 0) {
        return false;
    }
    s_runtime.ota_workflow.begin_acked = true;
    s_runtime.ota_workflow.expected_sequence = outbox->pending.sequence;
    s_runtime.ota_pending_progress = update;
    s_runtime.ota_progress_event_pending = true;
    s_runtime.ota_progress_waiter = false;
    return true;
}

static bool ota_restore_empty_progress(
    const backend_ota_journal_record_t *record,
    const backend_ota_event_outbox_snapshot_t *outbox)
{
    backend_ota_progress_update_t update;
    if (record == NULL || outbox == NULL ||
        record->event_sequence == UINT32_MAX ||
        !ota_progress_update_from_journal(record, &update)) {
        return false;
    }

    /* A tombstone whose exact progress digest precedes the journal cursor is
     * the ACK-before-tombstone crash cut.  Any other empty snapshot is the
     * persist-before-enqueue cut and must reconstruct the same event. */
    bool acknowledged = false;
    if (outbox->generation != 0U &&
        outbox->pending.sequence != UINT32_MAX &&
        record->event_sequence == outbox->pending.sequence + 1U &&
        backend_ota_operation_id_equal(
            &record->operation_id, &outbox->pending.operation_id)) {
        backend_ota_progress_event_t event;
        backend_ota_progress_state_t candidate =
            s_runtime.ota_workflow.progress;
        char body[BACKEND_OTA_EVENT_MAX_BYTES];
        uint8_t digest[32];
        const size_t length = ota_progress_event(
                &update, outbox->pending.sequence, &event)
            ? backend_ota_event_progress_encode(
                  &candidate, &event, body, sizeof(body)) : 0U;
        acknowledged = length != 0U &&
            backend_ota_sha256(
                (const uint8_t *)body, length, digest) &&
            memcmp(digest, outbox->pending.body_sha256,
                   sizeof(digest)) == 0;
    }
    if (acknowledged) {
        return true;
    }

    backend_ota_progress_event_t event;
    backend_ota_progress_state_t candidate = s_runtime.ota_workflow.progress;
    char body[BACKEND_OTA_EVENT_MAX_BYTES];
    if (!ota_progress_event(
            &update, record->event_sequence, &event)) {
        return false;
    }
    const size_t length = backend_ota_event_progress_encode(
        &candidate, &event, body, sizeof(body));
    if (length == 0U || backend_ota_event_outbox_enqueue(
            &s_runtime.ota_outbox_storage, &record->operation_id,
            record->event_sequence, (const uint8_t *)body, length) !=
            BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED) {
        return false;
    }
    s_runtime.ota_workflow.begin_acked = true;
    s_runtime.ota_workflow.expected_sequence = record->event_sequence;
    s_runtime.ota_pending_progress = update;
    s_runtime.ota_progress_event_pending = true;
    s_runtime.ota_progress_waiter = false;
    return true;
}

static bool ota_startup_restore(
    backend_ota_event_outbox_load_result_t outbox_state)
{
    if (s_runtime.ota_startup_action == BACKEND_OTA_JOURNAL_STARTUP_EMPTY) {
        return outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY;
    }
    if (s_runtime.ota_startup_action == BACKEND_OTA_JOURNAL_STARTUP_BLOCKED) {
        return false;
    }
    backend_ota_command_envelope_t command;
    if (!ota_restore_workflow_from_journal(
            &s_runtime.ota_startup_journal, &command)) {
        return false;
    }
    const bool terminal =
        s_runtime.ota_startup_action == BACKEND_OTA_JOURNAL_STARTUP_TERMINAL;
    if (terminal) {
        s_runtime.ota_workflow.work_available = false;
        s_runtime.ota_workflow.work_inflight = true;
        s_runtime.ota_workflow.begin_acked = true;
        s_runtime.ota_workflow.expected_sequence =
            s_runtime.ota_startup_journal.event_sequence;
        uplink_ota_result_item_t result;
        memset(&result, 0, sizeof(result));
        result.command = command;
        const char *running_version = ota_running_version(
            NULL, command.component);
        copy_text(result.running_version, sizeof(result.running_version),
                  running_version);
        result.evidence.has_operation_id = true;
        result.evidence.operation_id = command.operation_id;
        result.evidence.probe = !command.is_apply;
        result.evidence.component = command.component;
        result.evidence.apply_mode = command.apply_mode;
        result.evidence.manifest = s_runtime.ota_startup_journal.manifest;
        result.evidence.complete_image_validated =
            s_runtime.ota_startup_journal.has_manifest;
        result.evidence.rollback_clear =
            s_runtime.ota_startup_journal.rollback_clear;
        result.evidence.converged = s_runtime.ota_startup_journal.converged;
        if (s_runtime.ota_startup_journal.phase == BACKEND_OTA_PHASE_FAILED) {
            result.evidence.decision = BACKEND_OTA_DECISION_FAILED;
            result.relation = FOF_VERSION_INVALID;
        } else if (!command.is_apply &&
                   s_runtime.ota_startup_journal.has_accepted_probe_receipt) {
            result.evidence.decision = BACKEND_OTA_DECISION_ADMIT;
            result.relation = command.apply_mode ==
                    BACKEND_OTA_SAME_VERSION_RECOVERY
                ? FOF_VERSION_EQUAL : FOF_VERSION_NEWER;
        } else if (!command.is_apply) {
            result.evidence.decision = BACKEND_OTA_DECISION_NO_UPDATE;
            result.relation = FOF_VERSION_EQUAL;
        } else {
            result.evidence.decision = BACKEND_OTA_DECISION_APPLIED;
            result.relation = command.apply_mode ==
                    BACKEND_OTA_SAME_VERSION_RECOVERY
                ? FOF_VERSION_EQUAL : FOF_VERSION_NEWER;
        }
        char body[BACKEND_OTA_EVENT_MAX_BYTES];
        backend_ota_built_end_t built;
        const bool terminal_built = ota_build_terminal_event(
            &result, s_runtime.ota_workflow.expected_sequence,
            body, sizeof(body), &s_runtime.ota_terminal_outcome, &built);
        if (outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING) {
            const bool terminal_matches = terminal_built &&
                backend_ota_operation_id_equal(
                    &command.operation_id,
                    &s_runtime.ota_startup_outbox.pending.operation_id) &&
                s_runtime.ota_workflow.expected_sequence ==
                    s_runtime.ota_startup_outbox.pending.sequence &&
                built.body_length ==
                    s_runtime.ota_startup_outbox.pending.body_length &&
                memcmp(body, s_runtime.ota_startup_outbox.pending.body,
                       built.body_length) == 0;
            if (!terminal_matches) {
                return ota_restore_pending_progress(
                           &s_runtime.ota_startup_journal,
                           &s_runtime.ota_startup_outbox) &&
                    xQueueSend(
                        s_runtime.ota_result_queue, &result, 0) == pdTRUE;
            }
            s_runtime.ota_terminal_event_pending = true;
            copy_text(
                s_runtime.ota_terminal_receipt_sha256,
                sizeof(s_runtime.ota_terminal_receipt_sha256),
                built.receipt_sha256);
            return true;
        }
        if (outbox_state != BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY ||
            !terminal_built) {
            return false;
        }
        uint8_t body_digest[32];
        const bool terminal_tombstoned =
            s_runtime.ota_startup_outbox.generation != 0U &&
            s_runtime.ota_startup_outbox.pending.body_length == 0U &&
            backend_ota_operation_id_equal(
                &command.operation_id,
                &s_runtime.ota_startup_outbox.pending.operation_id) &&
            s_runtime.ota_workflow.expected_sequence ==
                s_runtime.ota_startup_outbox.pending.sequence &&
            backend_ota_sha256(
                (const uint8_t *)body, built.body_length, body_digest) &&
            memcmp(body_digest,
                   s_runtime.ota_startup_outbox.pending.body_sha256,
                   sizeof(body_digest)) == 0;
        if (terminal_tombstoned) {
            backend_ota_workflow_ack_prediction_t prediction;
            backend_ota_command_ack_t ack;
            memset(&ack, 0, sizeof(ack));
            if (!backend_ota_workflow_predict_terminal_ack(
                    &s_runtime.ota_workflow,
                    s_runtime.ota_terminal_outcome, &prediction)) {
                return false;
            }
            ack.ok = true;
            ack.has_operation_id = true;
            ack.operation_id = command.operation_id;
            ack.accepted_sequence =
                s_runtime.ota_workflow.expected_sequence;
            ack.next_sequence = ack.accepted_sequence + 1U;
            ack.current_component = prediction.component;
            copy_text(ack.current_action, sizeof(ack.current_action),
                      prediction.is_apply ? "apply" : "probe");
            ack.terminal = prediction.terminal;
            ack.duplicate = true;
            return backend_ota_workflow_note_terminal_ack(
                &s_runtime.ota_workflow,
                s_runtime.ota_terminal_outcome,
                built.receipt_sha256, &ack);
        }
        return xQueueSend(
            s_runtime.ota_result_queue, &result, 0) == pdTRUE;
    }

    uplink_ota_work_item_t work = {.command = command};
    if (s_runtime.ota_startup_action ==
            BACKEND_OTA_JOURNAL_STARTUP_WAIT_REBOOT ||
        s_runtime.ota_startup_action ==
            BACKEND_OTA_JOURNAL_STARTUP_CHECK_CONVERGENCE ||
        s_runtime.ota_startup_action ==
            BACKEND_OTA_JOURNAL_STARTUP_ROLL_BACK) {
        work.resume = true;
        s_runtime.ota_workflow.work_available = false;
        s_runtime.ota_workflow.work_inflight = true;
        if (s_runtime.ota_startup_journal.progress_initialized) {
            if ((outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING &&
                 !ota_restore_pending_progress(
                     &s_runtime.ota_startup_journal,
                     &s_runtime.ota_startup_outbox)) ||
                (outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY &&
                 !ota_restore_empty_progress(
                     &s_runtime.ota_startup_journal,
                     &s_runtime.ota_startup_outbox)) ||
                (outbox_state != BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING &&
                 outbox_state != BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY)) {
                return false;
            }
            s_runtime.ota_workflow.begin_acked = true;
        } else {
            if (outbox_state != BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY ||
                s_runtime.ota_startup_journal.command_next_sequence >=
                    UINT32_MAX - 1U) {
                return false;
            }
            s_runtime.ota_workflow.begin_acked = true;
            s_runtime.ota_workflow.expected_sequence =
                s_runtime.ota_startup_journal.command_next_sequence + 1U;
        }
    } else {
        /* An accepted/staged uplink command is bound to the pre-reboot uplink
         * boot ID.  Re-downloading or mutating under the new boot would weaken
         * that binding, so preserve event recovery but close it durably via
         * maintenance_resume's ACCEPTED -> FAILED path. */
        const bool stale_uplink_boot =
            command.component == BACKEND_OTA_COMPONENT_UPLINK &&
            command.binding.target_boot_id != s_runtime.boot_id;
        work.resume = stale_uplink_boot;
        work.restart = !stale_uplink_boot;
        backend_ota_command_envelope_t ignored;
        if (!backend_ota_workflow_take_work(
                &s_runtime.ota_workflow, &ignored)) {
            return false;
        }
        if (s_runtime.ota_startup_journal.progress_initialized) {
            if ((outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING &&
                 !ota_restore_pending_progress(
                     &s_runtime.ota_startup_journal,
                     &s_runtime.ota_startup_outbox)) ||
                (outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY &&
                 !ota_restore_empty_progress(
                     &s_runtime.ota_startup_journal,
                     &s_runtime.ota_startup_outbox)) ||
                (outbox_state != BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING &&
                 outbox_state != BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY)) {
                return false;
            }
            s_runtime.ota_workflow.begin_acked = true;
        } else if (outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_EMPTY) {
            char begin_body[BACKEND_OTA_EVENT_MAX_BYTES];
            const backend_ota_event_prefix_t begin = {
                .has_operation_id = true,
                .operation_id = command.operation_id,
                .is_apply = command.is_apply,
                .sequence = command.next_sequence,
                .component = command.component,
                .catalog_name = command.catalog_name,
            };
            const size_t length = backend_ota_event_begin_encode(
                &begin, begin_body, sizeof(begin_body));
            if (length == 0U || backend_ota_event_outbox_enqueue(
                    &s_runtime.ota_outbox_storage, &command.operation_id,
                    command.next_sequence, (const uint8_t *)begin_body,
                    length) != BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED) {
                return false;
            }
        } else if (outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING) {
            char begin_body[BACKEND_OTA_EVENT_MAX_BYTES];
            const backend_ota_event_prefix_t begin = {
                .has_operation_id = true,
                .operation_id = command.operation_id,
                .is_apply = command.is_apply,
                .sequence = command.next_sequence,
                .component = command.component,
                .catalog_name = command.catalog_name,
            };
            const size_t length = backend_ota_event_begin_encode(
                &begin, begin_body, sizeof(begin_body));
            if (length == 0U ||
                length != s_runtime.ota_startup_outbox.pending.body_length ||
                memcmp(begin_body,
                       s_runtime.ota_startup_outbox.pending.body,
                       length) != 0) {
                return false;
            }
        } else {
            return false;
        }
    }
    return xQueueSend(s_runtime.ota_work_queue, &work, 0) == pdTRUE;
}

static void backend_ota_client_worker(void *argument)
{
    (void)argument;
    lock_runtime();
    s_runtime.ota_client_worker_live = true;
    unlock_runtime();
    for (;;) {
        bool connected;
        bool blocked;
        char base_url[sizeof(s_runtime.config.backend_url)];
        char device_id[sizeof(s_runtime.config.device_id)];
        lock_runtime();
        connected = s_runtime.wifi_connected;
        blocked = s_runtime.ota_client_blocked;
        copy_text(base_url, sizeof(base_url), s_runtime.config.backend_url);
        copy_text(device_id, sizeof(device_id), s_runtime.config.device_id);
        unlock_runtime();
        if (!connected || blocked || !s_runtime.ota_ready) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }

        backend_ota_event_outbox_snapshot_t pending;
        const backend_ota_event_outbox_load_result_t outbox =
            backend_ota_event_outbox_load(
                &s_runtime.ota_outbox_storage, &pending);
        if (outbox == BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT ||
            outbox == BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR) {
            lock_runtime();
            s_runtime.ota_client_blocked = true;
            unlock_runtime();
            continue;
        }
        if (outbox == BACKEND_OTA_EVENT_OUTBOX_LOAD_PENDING) {
            (void)ota_drain_pending_outbox(base_url, device_id, &pending);
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        if (s_runtime.ota_workflow.command_active &&
            s_runtime.ota_workflow.begin_acked &&
            !s_runtime.ota_terminal_event_pending &&
            !s_runtime.ota_progress_event_pending) {
            backend_ota_progress_update_t progress_update;
            if (xQueueReceive(
                    s_runtime.ota_progress_queue,
                    &progress_update, 0) == pdTRUE) {
                backend_ota_progress_event_t progress_event;
                backend_ota_progress_state_t progress_candidate =
                    s_runtime.ota_workflow.progress;
                backend_ota_request_t request;
                char progress_body[BACKEND_OTA_EVENT_MAX_BYTES];
                const uint32_t sequence =
                    s_runtime.ota_workflow.expected_sequence;
                const bool progress_ok = ota_progress_event(
                        &progress_update, sequence, &progress_event) &&
                    backend_ota_event_progress_encode(
                        &progress_candidate, &progress_event,
                        progress_body, sizeof(progress_body)) != 0U &&
                    ota_command_request(
                        &s_runtime.ota_workflow.command, &request);
                const size_t progress_length = progress_ok
                    ? strlen(progress_body) : 0U;
                const bool progress_durable = progress_ok &&
                    backend_ota_maintenance_persist_fullsize_progress(
                        &s_runtime.maintenance, &request,
                        &progress_update, sequence);
                if (!progress_durable ||
                    backend_ota_event_outbox_enqueue(
                        &s_runtime.ota_outbox_storage,
                        &progress_update.operation_id, sequence,
                        (const uint8_t *)progress_body,
                        progress_length) !=
                    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED) {
                    lock_runtime();
                    s_runtime.ota_client_blocked = true;
                    unlock_runtime();
                    continue;
                }
                s_runtime.ota_pending_progress = progress_update;
                s_runtime.ota_progress_event_pending = true;
                s_runtime.ota_progress_waiter = true;
                continue;
            }
            uplink_ota_result_item_t result;
            if (xQueueReceive(
                    s_runtime.ota_result_queue, &result, 0) == pdTRUE) {
                char terminal_body[BACKEND_OTA_EVENT_MAX_BYTES];
                backend_ota_built_end_t built;
                backend_ota_terminal_outcome_t outcome =
                    BACKEND_OTA_TERMINAL_FAILED;
                uint8_t receipt_sha256[32];
                const bool built_ok = ota_build_terminal_event(
                        &result, s_runtime.ota_workflow.expected_sequence,
                        terminal_body, sizeof(terminal_body),
                        &outcome, &built);
                const bool receipt_ok = built_ok && ota_lower_hex_decode_32(
                    built.receipt_sha256, receipt_sha256);
                const bool complete =
                    outcome == BACKEND_OTA_TERMINAL_ELIGIBLE ||
                    outcome == BACKEND_OTA_TERMINAL_NO_UPDATE ||
                    outcome == BACKEND_OTA_TERMINAL_APPLIED;
                (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
                const bool terminal_durable = receipt_ok &&
                    backend_ota_maintenance_persist_fullsize_terminal(
                        &s_runtime.maintenance, &result.evidence,
                        s_runtime.ota_workflow.expected_sequence, complete,
                        outcome == BACKEND_OTA_TERMINAL_ELIGIBLE,
                        receipt_sha256);
                (void)xSemaphoreGive(s_runtime.ota_lock);
                if (!terminal_durable ||
                    backend_ota_event_outbox_enqueue(
                        &s_runtime.ota_outbox_storage,
                        &result.command.operation_id,
                        s_runtime.ota_workflow.expected_sequence,
                        (const uint8_t *)terminal_body,
                        built.body_length) !=
                    BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED) {
                    lock_runtime();
                    s_runtime.ota_client_blocked = true;
                    unlock_runtime();
                    continue;
                }
                s_runtime.ota_terminal_outcome = outcome;
                copy_text(
                    s_runtime.ota_terminal_receipt_sha256,
                    sizeof(s_runtime.ota_terminal_receipt_sha256),
                    built.receipt_sha256);
                s_runtime.ota_terminal_event_pending = true;
            }
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        if (s_runtime.ota_workflow.command_active) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }

        backend_ota_command_local_t local;
        if (!ota_command_local(&s_runtime.ota_workflow, &local)) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        char path[UPLINK_OTA_CLIENT_PATH_CAPACITY];
        const int path_length = snprintf(
            path, sizeof(path), "/nodes/%s/backend-ota/next", device_id);
        if (path_length <= 0 || path_length >= (int)sizeof(path)) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
        const backend_http_result_t http = backend_http_get_json(
            base_url, path, s_runtime.ota_client_http_response,
            sizeof(s_runtime.ota_client_http_response));
        (void)xSemaphoreGive(s_runtime.http_lock);
        if (!http.transport_complete || http.status_code != 200) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        backend_ota_command_envelope_t command;
        if (backend_ota_command_decode(
                s_runtime.ota_client_http_response, http.body_length,
                &local, &command) != BACKEND_OTA_COMMAND_DECODE_OK) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        backend_ota_workflow_t candidate = s_runtime.ota_workflow;
        if (backend_ota_workflow_admit(&candidate, &command) !=
            BACKEND_OTA_WORKFLOW_ADMITTED) {
            vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_CLIENT_PERIOD_MS));
            continue;
        }
        backend_ota_request_t request;
        if (!ota_command_request(&command, &request)) {
            continue;
        }
        (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
        const backend_ota_journal_persist_result_t persisted =
            backend_ota_maintenance_accept_fullsize_command(
                &s_runtime.maintenance, &request);
        (void)xSemaphoreGive(s_runtime.ota_lock);
        if (persisted != BACKEND_OTA_JOURNAL_PERSIST_COMMITTED &&
            persisted != BACKEND_OTA_JOURNAL_PERSIST_ALREADY_DURABLE) {
            lock_runtime();
            s_runtime.ota_client_blocked = true;
            unlock_runtime();
            continue;
        }
        char begin_body[BACKEND_OTA_EVENT_MAX_BYTES];
        const backend_ota_event_prefix_t begin = {
            .has_operation_id = true,
            .operation_id = command.operation_id,
            .is_apply = command.is_apply,
            .sequence = command.next_sequence,
            .component = command.component,
            .catalog_name = command.catalog_name,
        };
        const size_t begin_length = backend_ota_event_begin_encode(
            &begin, begin_body, sizeof(begin_body));
        if (begin_length == 0U ||
            backend_ota_event_outbox_enqueue(
                &s_runtime.ota_outbox_storage, &command.operation_id,
                command.next_sequence, (const uint8_t *)begin_body,
                begin_length) != BACKEND_OTA_EVENT_OUTBOX_ENQUEUE_COMMITTED) {
            lock_runtime();
            s_runtime.ota_client_blocked = true;
            unlock_runtime();
            continue;
        }
        backend_ota_command_envelope_t queued;
        if (!backend_ota_workflow_take_work(&candidate, &queued)) {
            continue;
        }
        const uplink_ota_work_item_t item = {.command = queued};
        if (xQueueSend(s_runtime.ota_work_queue, &item, 0) != pdTRUE) {
            lock_runtime();
            s_runtime.ota_client_blocked = true;
            unlock_runtime();
            continue;
        }
        s_runtime.ota_workflow = candidate;
    }
}
#endif

static void ota_worker(void *argument)
{
    (void)argument;
    lock_runtime();
    s_runtime.ota_worker_live = true;
    unlock_runtime();
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    for (;;) {
        uplink_ota_work_item_t item;
        if (xQueueReceive(
                s_runtime.ota_work_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uplink_ota_result_item_t result;
        memset(&result, 0, sizeof(result));
        result.command = item.command;
        char running_version[65];
        memset(running_version, 0, sizeof(running_version));
        const char *running = ota_running_version(
            NULL, item.command.component);
        if (running != NULL) {
            copy_text(running_version, sizeof(running_version), running);
            copy_text(result.running_version,
                      sizeof(result.running_version), running);
        }
        (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
        if (item.resume) {
            const int64_t deadline = monotonic_ms() + UPLINK_RELAY_WAIT_MS;
            while (!backend_ota_maintenance_available(
                       &s_runtime.maintenance) &&
                   monotonic_ms() < deadline) {
                result.call_succeeded = backend_ota_maintenance_resume(
                    &s_runtime.maintenance, false);
                if (!backend_ota_maintenance_available(
                        &s_runtime.maintenance)) {
                    vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_PERIOD_MS));
                }
            }
            if (!backend_ota_maintenance_available(&s_runtime.maintenance)) {
                result.call_succeeded = backend_ota_maintenance_resume(
                    &s_runtime.maintenance, true);
            }
            (void)backend_ota_maintenance_last_evidence(
                &s_runtime.maintenance, &result.evidence);
        } else if (item.restart) {
            backend_ota_request_t request;
            result.call_succeeded = ota_command_request(
                    &item.command, &request) &&
                backend_ota_maintenance_restart_fullsize_command(
                    &s_runtime.maintenance, &request, &result.evidence);
            if (result.call_succeeded && item.command.is_apply &&
                item.command.component != BACKEND_OTA_COMPONENT_UPLINK) {
                const int64_t deadline =
                    monotonic_ms() + UPLINK_RELAY_WAIT_MS;
                while (!backend_ota_maintenance_available(
                           &s_runtime.maintenance) &&
                       monotonic_ms() < deadline) {
                    (void)backend_ota_maintenance_resume(
                        &s_runtime.maintenance, false);
                    if (!backend_ota_maintenance_available(
                            &s_runtime.maintenance)) {
                        vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_PERIOD_MS));
                    }
                }
                if (!backend_ota_maintenance_available(
                        &s_runtime.maintenance)) {
                    (void)backend_ota_maintenance_resume(
                        &s_runtime.maintenance, true);
                }
                (void)backend_ota_maintenance_last_evidence(
                    &s_runtime.maintenance, &result.evidence);
            }
        } else if (!item.command.is_apply) {
            result.call_succeeded = backend_ota_maintenance_run_fullsize_probe(
                &s_runtime.maintenance, item.command.has_operation_id,
                &item.command.operation_id, item.command.expected_size,
                item.command.component, item.command.catalog_name,
                item.command.expected_sha256, item.command.apply_mode,
                &result.evidence);
        } else {
            backend_ota_request_t request;
            result.call_succeeded = ota_command_request(
                    &item.command, &request) &&
                backend_ota_maintenance_request_apply(
                    &s_runtime.maintenance, &request);
            if (result.call_succeeded &&
                item.command.component != BACKEND_OTA_COMPONENT_UPLINK) {
                const int64_t deadline =
                    monotonic_ms() + UPLINK_RELAY_WAIT_MS;
                while (!backend_ota_maintenance_available(
                           &s_runtime.maintenance) &&
                       monotonic_ms() < deadline) {
                    (void)backend_ota_maintenance_resume(
                        &s_runtime.maintenance, false);
                    if (!backend_ota_maintenance_available(
                            &s_runtime.maintenance)) {
                        vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_PERIOD_MS));
                    }
                }
                if (!backend_ota_maintenance_available(
                        &s_runtime.maintenance)) {
                    (void)backend_ota_maintenance_resume(
                        &s_runtime.maintenance, true);
                }
            }
            (void)backend_ota_maintenance_last_evidence(
                &s_runtime.maintenance, &result.evidence);
        }
        result.relation = running_version[0] == '\0' ||
                result.evidence.manifest.version[0] == '\0'
            ? FOF_VERSION_INVALID
            : fof_firmware_version_compare(
                  result.evidence.manifest.version, running_version);
        (void)xSemaphoreGive(s_runtime.ota_lock);
        (void)xQueueSend(
            s_runtime.ota_result_queue, &result, portMAX_DELAY);
    }
#else
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        bool connected;
        bool enabled;
        lock_runtime();
        connected = s_runtime.wifi_connected;
        enabled = s_runtime.config.auto_update_enabled;
        unlock_runtime();
        if (connected && s_runtime.ota_ready &&
            xSemaphoreTake(s_runtime.ota_lock, 0) == pdTRUE) {
            for (backend_ota_component_t component =
                     BACKEND_OTA_COMPONENT_UPLINK;
                 component <= BACKEND_OTA_COMPONENT_SCANNER1;
                 component++) {
                if (!backend_ota_poll_due(
                        &s_runtime.ota_poll[component], now_ms)) {
                    continue;
                }
                backend_ota_auto_decision_t decision;
                const bool success = backend_ota_maintenance_auto_poll(
                    &s_runtime.maintenance,
                    component,
                    enabled,
                    &decision);
                if (success) {
                    backend_ota_poll_note_success(
                        &s_runtime.ota_poll[component], now_ms);
                } else {
                    backend_ota_poll_note_failure(
                        &s_runtime.ota_poll[component], now_ms);
                }
                break;
            }
            (void)xSemaphoreGive(s_runtime.ota_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(UPLINK_OTA_PERIOD_MS));
    }
#endif
}

static void emit_status_lines(void)
{
    char boot[UPLINK_STATUS_CAPACITY];
    char health[UPLINK_STATUS_CAPACITY];
    bool has_health;
    lock_runtime();
    copy_text(boot, sizeof(boot), s_runtime.boot_line);
    copy_text(health, sizeof(health), s_runtime.health_line);
    has_health = s_runtime.health_line_ready;
    unlock_runtime();
    (void)print_line(boot);
    if (has_health) {
        (void)print_line(health);
    }
}

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
typedef struct {
    char mac[18];
    uint32_t boot_id;
    uint32_t config_generation;
    bool wifi_configured;
    bool wifi_connected;
    bool wifi_full_pass_failed;
    bool backend_reachable;
    bool ap_running;
    backend_lite_ap_reason_t recovery_reason;
    bool dashboard_routes_enabled;
    const char *dashboard_failure_reason;
    backend_scanner_health_t scanners[2];
    uplink_scanner_wire_t scanner_wire[2];
    backend_scanner_status_t scanner_status[2];
    bool scanner_status_available[2];
    backend_threat_snapshot_t threats;
    bool backend_has_last_success_age;
    uint32_t backend_last_success_age_s;
    backend_led_state_t led_state;
    bool ota_ready;
    size_t upload_depth;
    size_t upload_capacity;
    uint64_t upload_drops;
    uint64_t upload_ok;
    uint64_t upload_failed;
    uint64_t upload_retries;
    bool history_available;
    size_t history_count;
    uint64_t history_contention_drops;
    backend_usb_service_snapshot_t usb;
} lite_status_snapshot_t;

static const char *lite_recovery_reason_name(
    backend_lite_ap_reason_t reason)
{
    switch (reason) {
    case BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED:
        return "wifi_unconfigured";
    case BACKEND_LITE_AP_REASON_WIFI_JOIN_FAILED:
        return "wifi_join_failed";
    default:
        return "none";
    }
}

static const char *scanner_wire_name(uplink_scanner_wire_t wire)
{
    switch (wire) {
    case UPLINK_SCANNER_WIRE_BACKEND:
        return "backend_uart";
    case UPLINK_SCANNER_WIRE_PRODUCTION:
        return "production_uart";
    default:
        return "unknown";
    }
}

static bool take_lite_status_snapshot(
    int64_t now_ms, lite_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    lock_runtime();
    copy_text(snapshot->mac, sizeof(snapshot->mac), s_runtime.mac_text);
    snapshot->boot_id = s_runtime.boot_id;
    snapshot->config_generation = s_runtime.config.generation;
    snapshot->wifi_configured = s_runtime.config_loaded &&
        s_runtime.config.network_count > 0U &&
        backend_config_validate(&s_runtime.config) == BACKEND_CONFIG_VALID;
    snapshot->wifi_connected = s_runtime.wifi_connected;
    snapshot->wifi_full_pass_failed = s_runtime.wifi_initialized &&
        backend_wifi_manager_join_failed(&s_runtime.wifi);
    snapshot->backend_reachable = s_runtime.backend_reachable;
    snapshot->ap_running = s_runtime.portal_started;
    snapshot->recovery_reason = backend_lite_ap_policy_reason(
        &s_runtime.lite_ap_policy);
    snapshot->dashboard_routes_enabled =
        s_runtime.portal.dashboard_routes_enabled;
    snapshot->dashboard_failure_reason =
        s_runtime.portal.dashboard_failure_reason;
    memcpy(
        snapshot->scanners,
        s_runtime.scanner_health,
        sizeof(snapshot->scanners));
    memcpy(
        snapshot->scanner_wire,
        s_runtime.scanner_wire,
        sizeof(snapshot->scanner_wire));
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (s_runtime.scanner_tracker[slot].initialized) {
            snapshot->scanner_status_available[slot] = true;
            snapshot->scanner_status[slot] =
                s_runtime.scanner_tracker[slot].status;
        } else if (s_runtime.scanner_wire[slot] ==
                       UPLINK_SCANNER_WIRE_PRODUCTION &&
                   s_runtime.production_scanner[slot].identity_valid) {
            const production_scanner_message_t *production =
                &s_runtime.production_scanner[slot];
            backend_scanner_status_t *scanner =
                &snapshot->scanner_status[slot];
            memset(scanner, 0, sizeof(*scanner));
            scanner->schema = BACKEND_SCANNER_STATUS_SCHEMA;
            scanner->sequence =
                s_runtime.production_status_sequence[slot];
            scanner->boot_id = s_runtime.production_boot_id[slot];
            copy_text(scanner->mac, sizeof(scanner->mac),
                      production->management_identity_valid
                          ? production->hardware_id : "");
            copy_text(scanner->target, sizeof(scanner->target),
                      production->management_identity_valid
                          ? production->firmware_name : production->board);
            copy_text(scanner->project, sizeof(scanner->project),
                      production->management_identity_valid
                          ? production->app_project : "production_combo");
            copy_text(scanner->hardware, sizeof(scanner->hardware),
                      production->management_identity_valid
                          ? production->hardware_type : production->chip);
            copy_text(scanner->version, sizeof(scanner->version),
                      production->version);
            scanner->profile = production->profile_present
                ? production->profile
                : s_runtime.scanner_health[slot].reported_profile;
            scanner->role_generation =
                s_runtime.scanner_health[slot].acknowledged_generation;
            scanner->role_acked =
                s_runtime.scanner_health[slot].role_acked;
            scanner->command_ingress =
                s_runtime.scanner_health[slot].command_healthy;
            scanner->ble_healthy = production->ble_scanning ||
                production->ble_host_active ||
                production->ble_host_synced;
            scanner->wifi_healthy = !production->wifi_paused;
            copy_text(scanner->ota_state, sizeof(scanner->ota_state),
                      production->ota_state[0] != '\0'
                          ? production->ota_state : "native");
            copy_text(
                scanner->rollback_state,
                sizeof(scanner->rollback_state),
                production->rollback_state[0] != '\0'
                    ? production->rollback_state : "native");
            scanner->tx_drops = production->tx_drops;
            scanner->uptime_ms = production->uptime_ms;
            snapshot->scanner_status_available[slot] = true;
        }
    }
    backend_threat_snapshot(
        &s_runtime.threats, now_ms, &snapshot->threats);
    snapshot->led_state = backend_status_led_state();
    snapshot->ota_ready = s_runtime.ota_ready;
    snapshot->upload_depth = s_runtime.upload_fifo.count;
    snapshot->upload_capacity = s_runtime.upload_fifo.capacity;
    snapshot->upload_drops = s_runtime.upload_fifo.dropped_batches;
    snapshot->upload_ok = s_runtime.uploader.ack_count;
    snapshot->upload_failed = s_runtime.uploader.quarantine_count;
    snapshot->upload_retries = s_runtime.uploader.retry_count;
    if (s_runtime.uploader.ack_count != 0U &&
        s_runtime.uploader.last_backend_success_ms >= 0 &&
        now_ms >= s_runtime.uploader.last_backend_success_ms) {
        const int64_t age_s =
            (now_ms - s_runtime.uploader.last_backend_success_ms) / 1000;
        snapshot->backend_has_last_success_age = true;
        snapshot->backend_last_success_age_s = age_s > UINT32_MAX
            ? UINT32_MAX : (uint32_t)age_s;
    }
    snapshot->history_available = s_runtime.history_available;
    const bool usb_available = s_runtime.usb_available;
    unlock_runtime();

    snapshot->history_contention_drops = atomic_load_explicit(
        &s_runtime.event_ring_contention_drops, memory_order_relaxed);
    if (snapshot->history_available &&
        xSemaphoreTake(
            s_runtime.event_ring_lock, pdMS_TO_TICKS(20U)) == pdTRUE) {
        snapshot->history_count = s_runtime.event_ring.count;
        (void)xSemaphoreGive(s_runtime.event_ring_lock);
    }
    if (usb_available) {
        (void)backend_usb_service_snapshot(
            &s_runtime.usb, now_ms, &snapshot->usb);
    }
    return true;
}

static bool append_lite_backend_status(
    backend_json_writer_t *writer,
    const lite_status_snapshot_t *status)
{
    if (writer == NULL || status == NULL ||
        !backend_json_append_format(
            writer,
            ",\"backend\":{\"reachable\":%s,"
            "\"last_success_age_s\":",
            status->backend_reachable ? "true" : "false")) {
        return false;
    }
    if (status->backend_has_last_success_age) {
        if (!backend_json_append_format(
                writer,
                "%lu",
                (unsigned long)status->backend_last_success_age_s)) {
            return false;
        }
    } else if (!backend_json_append(writer, "null")) {
        return false;
    }
    return backend_json_append(writer, "}");
}

static bool append_lite_scanner_summary(
    backend_json_writer_t *writer,
    size_t slot,
    const lite_status_snapshot_t *status)
{
    if (writer == NULL || status == NULL || slot >= 2U) {
        return false;
    }
    const backend_scanner_health_t *health = &status->scanners[slot];
    const bool available = status->scanner_status_available[slot];
    const backend_scanner_status_t *scanner =
        &status->scanner_status[slot];
    if (!backend_json_append_format(
            writer,
            "{\"slot\":%u,\"connected\":%s,"
            "\"identity_valid\":%s,\"status_available\":%s,"
            "\"protocol\":\"%s\","
            "\"identity\":",
            (unsigned)slot,
            health->connected ? "true" : "false",
            health->identity_valid ? "true" : "false",
            available ? "true" : "false",
            scanner_wire_name(status->scanner_wire[slot]))) {
        return false;
    }
    if (available) {
        if (!backend_json_append(writer, "{\"target\":") ||
            !backend_json_append_escaped(writer, scanner->target) ||
            !backend_json_append(writer, ",\"project\":") ||
            !backend_json_append_escaped(writer, scanner->project) ||
            !backend_json_append(writer, ",\"hardware\":") ||
            !backend_json_append_escaped(writer, scanner->hardware) ||
            !backend_json_append(writer, ",\"version\":") ||
            !backend_json_append_escaped(writer, scanner->version) ||
            !backend_json_append(writer, ",\"hardware_id\":") ||
            !backend_json_append_escaped(writer, scanner->mac) ||
            !backend_json_append_format(
                writer, ",\"boot_id\":%lu",
                (unsigned long)scanner->boot_id) ||
            !backend_json_append(writer, "}")) {
            return false;
        }
    } else if (!backend_json_append(writer, "null")) {
        return false;
    }
    if (!backend_json_append(writer, ",\"profile\":")) {
        return false;
    }
    if (available) {
        if (!backend_json_append_format(
                writer, "%u", (unsigned)scanner->profile)) {
            return false;
        }
    } else if (!backend_json_append(writer, "null")) {
        return false;
    }
    if (!backend_json_append_format(
            writer,
            ",\"health\":{\"command\":%s,\"radio\":%s,"
            "\"role_acked\":%s},\"errors\":",
            health->command_healthy ? "true" : "false",
            health->radio_healthy ? "true" : "false",
            health->role_acked ? "true" : "false")) {
        return false;
    }
    if (available) {
        if (!backend_json_append_format(
                writer,
                "{\"rx\":%lu,\"tx_drops\":%lu}",
                (unsigned long)scanner->rx_errors,
                (unsigned long)scanner->tx_drops)) {
            return false;
        }
    } else if (!backend_json_append(writer, "null")) {
        return false;
    }
    if (!backend_json_append(writer, ",\"uptime_ms\":")) {
        return false;
    }
    if (available) {
        if (!backend_json_append_format(
                writer,
                "%llu",
                (unsigned long long)scanner->uptime_ms)) {
            return false;
        }
    } else if (!backend_json_append(writer, "null")) {
        return false;
    }
    return backend_json_append(writer, "}");
}

static bool append_lite_badge_compatibility(
    backend_json_writer_t *writer,
    const lite_status_snapshot_t *status)
{
    if (writer == NULL || status == NULL ||
        !backend_json_append_format(
            writer,
            ",\"counts\":{\"drone\":%u,\"meta\":%u,"
            "\"tracker\":0,\"wifi_anomaly\":0,\"ble\":0,\"other\":0},"
            "\"scanners\":[",
            (unsigned)status->threats.drone_count,
            (unsigned)status->threats.meta_count) ||
        !append_lite_scanner_summary(writer, 0U, status) ||
        !backend_json_append(writer, ",") ||
        !append_lite_scanner_summary(writer, 1U, status)) {
        return false;
    }
    return backend_json_append(writer, "]");
}

static size_t build_lite_status(
    bool usb_frame, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    output[0] = '\0';
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    lite_status_snapshot_t status;
    if (identity == NULL ||
        !take_lite_status_snapshot(monotonic_ms(), &status)) {
        return 0U;
    }
    const char *led = led_state_name(status.led_state);
    if (led == NULL) {
        led = "unknown";
    }

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    bool ok = !usb_frame || backend_json_append(&writer, "FOF_STATUS:");
    ok = ok && backend_json_append(&writer, "{\"product_family\":") &&
        backend_json_append_escaped(&writer, identity->product_family) &&
        backend_json_append(&writer, ",\"target\":") &&
        backend_json_append_escaped(&writer, identity->target) &&
        backend_json_append(&writer, ",\"project\":") &&
        backend_json_append_escaped(&writer, identity->project) &&
        backend_json_append(&writer, ",\"hardware\":") &&
        backend_json_append_escaped(&writer, identity->hardware) &&
        backend_json_append(&writer, ",\"version\":") &&
        backend_json_append_escaped(&writer, identity->version) &&
        backend_json_append(&writer, ",\"firmware_name\":") &&
        backend_json_append_escaped(&writer, identity->target) &&
        backend_json_append(&writer, ",\"app_project\":") &&
        backend_json_append_escaped(&writer, identity->project) &&
        backend_json_append(&writer, ",\"hardware_type\":") &&
        backend_json_append_escaped(&writer, identity->hardware) &&
        backend_json_append(&writer, ",\"hardware_id\":") &&
        backend_json_append_escaped(&writer, status.mac) &&
        backend_json_append(&writer, ",\"mac\":") &&
        backend_json_append_escaped(&writer, status.mac) &&
        backend_json_append_format(
            &writer,
            ",\"boot_id\":%lu,\"mode\":\"headless\","
            "\"mode_label\":\"Backend Badge Lite\","
            "\"config_generation\":%lu,"
            "\"capabilities\":[\"display_none\",\"usb_live\","
            "\"usb_live_ack\",\"usb_buffered\",\"usb_config\","
            "\"http_uplink\",\"config_ap\",\"ap_dashboard\","
            "\"remote_ota\",\"production_scanner_uart\"],"
            "\"wifi\":{\"configured\":%s,\"connected\":%s,"
            "\"full_pass_failed\":%s},"
            "\"recovery\":{\"reason\":\"%s\",\"ap_running\":%s},"
            "\"scanner\":[{\"slot\":0,\"connected\":%s,"
            "\"identity_valid\":%s},{\"slot\":1,\"connected\":%s,"
            "\"identity_valid\":%s}],"
            "\"threats\":{\"drone_active\":%s,\"meta_active\":%s,"
            "\"drone_count\":%u,\"meta_count\":%u,"
            "\"drone_last_seen_age_ms\":%lld,"
            "\"meta_last_seen_age_ms\":%lld},"
            "\"led\":\"%s\",\"ota_ready\":%s,"
            "\"upload\":{\"depth\":%u,\"capacity\":%u,"
            "\"dropped\":%llu,\"ok\":%llu,\"failed\":%llu,"
            "\"retries\":%llu},"
            "\"usb\":{\"available\":%s,\"host_connected\":%s,"
            "\"required_depth\":%u,\"optional_depth\":%u,"
            "\"optional_drops\":%llu,\"required_failures\":%llu,"
            "\"bytes_transmitted\":%llu,\"bytes_received\":%llu,"
            "\"output_poisoned\":%s},"
            "\"live\":{\"started\":%s,\"session_id\":\"%s\","
            "\"last_ack_sequence\":%llu,\"confirmed\":%s,"
            "\"lease_remaining_ms\":%lld},"
            "\"history\":{\"available\":%s,\"count\":%u,"
            "\"contention_drops\":%llu},"
            "\"dashboard\":{\"enabled\":%s,\"degraded_reason\":",
            (unsigned long)status.boot_id,
            (unsigned long)status.config_generation,
            status.wifi_configured ? "true" : "false",
            status.wifi_connected ? "true" : "false",
            status.wifi_full_pass_failed ? "true" : "false",
            lite_recovery_reason_name(status.recovery_reason),
            status.ap_running ? "true" : "false",
            status.scanners[0].connected ? "true" : "false",
            status.scanners[0].identity_valid ? "true" : "false",
            status.scanners[1].connected ? "true" : "false",
            status.scanners[1].identity_valid ? "true" : "false",
            status.threats.drone_live ? "true" : "false",
            status.threats.meta_live ? "true" : "false",
            (unsigned)status.threats.drone_count,
            (unsigned)status.threats.meta_count,
            (long long)status.threats.drone_last_seen_age_ms,
            (long long)status.threats.meta_last_seen_age_ms,
            led,
            status.ota_ready ? "true" : "false",
            (unsigned)status.upload_depth,
            (unsigned)status.upload_capacity,
            (unsigned long long)status.upload_drops,
            (unsigned long long)status.upload_ok,
            (unsigned long long)status.upload_failed,
            (unsigned long long)status.upload_retries,
            status.usb.available ? "true" : "false",
            status.usb.host_connected ? "true" : "false",
            (unsigned)status.usb.required_queue_depth,
            (unsigned)status.usb.optional_queue_depth,
            (unsigned long long)status.usb.optional_drops,
            (unsigned long long)status.usb.required_failures,
            (unsigned long long)status.usb.bytes_transmitted,
            (unsigned long long)status.usb.bytes_received,
            status.usb.output_poisoned ? "true" : "false",
            status.usb.live_started ? "true" : "false",
            status.usb.live_session_id,
            (unsigned long long)status.usb.last_ack_sequence,
            status.usb.live_confirmed ? "true" : "false",
            (long long)status.usb.live_lease_remaining_ms,
            status.history_available ? "true" : "false",
            (unsigned)status.history_count,
            (unsigned long long)status.history_contention_drops,
            status.dashboard_routes_enabled ? "true" : "false");
    if (!ok) {
        output[0] = '\0';
        return 0U;
    }
    const char *degraded_reason = status.dashboard_failure_reason;
    if (!status.history_available) {
        degraded_reason = "history_psram_unavailable";
    }
    if (degraded_reason == NULL) {
        ok = backend_json_append(&writer, "null");
    } else {
        ok = backend_json_append_escaped(&writer, degraded_reason);
    }
    ok = ok && backend_json_append(&writer, "}") &&
        append_lite_backend_status(&writer, &status) &&
        append_lite_badge_compatibility(&writer, &status) &&
        backend_json_append(&writer, ",\"scanner_summaries\":[") &&
        append_lite_scanner_summary(&writer, 0U, &status) &&
        backend_json_append(&writer, ",") &&
        append_lite_scanner_summary(&writer, 1U, &status) &&
        backend_json_append(&writer, "]}");
    if (usb_frame) {
        ok = ok && backend_json_append(&writer, "\n");
    }
    if (!ok) {
        output[0] = '\0';
        return 0U;
    }
    return backend_json_writer_finish(&writer);
}

static bool portal_dashboard_status(
    void *context,
    char *output,
    size_t capacity,
    size_t *out_length)
{
    (void)context;
    if (out_length == NULL) {
        return false;
    }
    *out_length = build_lite_status(false, output, capacity);
    return *out_length != 0U;
}

static bool portal_event_snapshot(
    void *context,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *events,
    size_t event_capacity,
    backend_event_ring_snapshot_t *snapshot)
{
    (void)context;
    if (events == NULL || snapshot == NULL || limit == 0U ||
        limit > BACKEND_DASHBOARD_MAX_LIMIT) {
        return false;
    }
    if (!s_runtime.history_available) {
        memset(snapshot, 0, sizeof(*snapshot));
        return true;
    }
    if (xSemaphoreTake(
            s_runtime.event_ring_lock, pdMS_TO_TICKS(20U)) != pdTRUE) {
        return false;
    }
    const bool copied = backend_event_ring_snapshot(
        &s_runtime.event_ring,
        after,
        limit,
        events,
        event_capacity,
        snapshot);
    (void)xSemaphoreGive(s_runtime.event_ring_lock);
    return copied;
}

static bool emit_required_frame(const char *frame, size_t length)
{
    return backend_usb_service_emit(
        &s_runtime.usb,
        BACKEND_USB_FRAME_REQUIRED,
        frame,
        length);
}

static void emit_control_error(const char *reason)
{
    char frame[160];
    const int written = snprintf(
        frame,
        sizeof(frame),
        "FOF_CTL_ERROR:{\"reason\":\"%s\"}\n",
        reason == NULL ? "unknown" : reason);
    if (written > 0 && (size_t)written < sizeof(frame)) {
        (void)emit_required_frame(frame, (size_t)written);
    }
}

static bool line_starts_with(
    const char *line, size_t length, const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    return length >= prefix_length &&
        memcmp(line, prefix, prefix_length) == 0;
}

static bool screen_only_command(const char *line, size_t length)
{
    static const char *const prefixes[] = {
        "FOF_SCREEN",
        "FOF_NAV",
        "FOF_THEME",
        "FOF_DISPLAY",
        "FOF_SET:theme",
        "FOF_SET:navigation",
        "FOF_SET:display",
    };
    for (size_t index = 0U;
         index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
        if (line_starts_with(line, length, prefixes[index])) {
            return true;
        }
    }
    return false;
}

static const char *portal_update_reason(
    backend_portal_update_result_t result)
{
    switch (result) {
    case BACKEND_PORTAL_UPDATE_UNKNOWN_FIELD:
        return "unknown_field";
    case BACKEND_PORTAL_UPDATE_CONFIRMATION_REQUIRED:
        return "confirmation_required";
    case BACKEND_PORTAL_UPDATE_COMMIT_FAILED:
        return "commit_failed";
    case BACKEND_PORTAL_UPDATE_RECONNECT_FAILED:
        return "reconnect_failed";
    case BACKEND_PORTAL_UPDATE_STALE_GENERATION:
        return "stale_generation";
    case BACKEND_PORTAL_UPDATE_INVALID_ARGUMENT:
        return "invalid_argument";
    case BACKEND_PORTAL_UPDATE_INVALID_JSON:
        return "invalid_json";
    case BACKEND_PORTAL_UPDATE_INVALID_CONFIG:
        return "invalid_config";
    default:
        return "unknown";
    }
}

static void emit_lite_status(void)
{
    if (xSemaphoreTake(s_runtime.usb_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const size_t length = build_lite_status(
        true,
        s_runtime.usb_status_scratch,
        sizeof(s_runtime.usb_status_scratch));
    if (length != 0U) {
        (void)emit_required_frame(s_runtime.usb_status_scratch, length);
    }
    (void)xSemaphoreGive(s_runtime.usb_lock);
}

static void emit_redacted_config(void)
{
    backend_config_record_t config;
    lock_runtime();
    config = s_runtime.config;
    unlock_runtime();
    if (xSemaphoreTake(s_runtime.usb_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    static const char prefix[] = "FOF_CONFIG:";
    memcpy(
        s_runtime.usb_status_scratch,
        prefix,
        sizeof(prefix) - 1U);
    const size_t json_length = backend_portal_render_redacted_config(
        &config,
        s_runtime.usb_status_scratch + sizeof(prefix) - 1U,
        sizeof(s_runtime.usb_status_scratch) - sizeof(prefix));
    if (json_length != 0U) {
        const size_t length = sizeof(prefix) - 1U + json_length;
        s_runtime.usb_status_scratch[length] = '\n';
        (void)emit_required_frame(
            s_runtime.usb_status_scratch, length + 1U);
    }
    (void)xSemaphoreGive(s_runtime.usb_lock);
}

static void emit_staged_result(
    const char *prefix,
    const char *value)
{
    char frame[128];
    const int written = snprintf(
        frame, sizeof(frame), "%s%s\n", prefix, value);
    if (written > 0 && (size_t)written < sizeof(frame)) {
        (void)emit_required_frame(frame, (size_t)written);
    }
}

static void handle_lite_config_set(
    const backend_usb_command_t *command, int64_t now_ms)
{
    if (!begin_config_transaction(NULL)) {
        emit_control_error("config_transaction_failed");
        return;
    }
    const backend_portal_update_result_t result =
        backend_config_portal_apply_update(
            &s_runtime.portal,
            command->json,
            command->json_length,
            now_ms);
    if (result == BACKEND_PORTAL_UPDATE_OK ||
        result == BACKEND_PORTAL_UPDATE_RECONNECT_FAILED) {
        backend_config_record_t config;
        lock_runtime();
        config = s_runtime.config;
        unlock_runtime();
        backend_usb_config_init(&s_runtime.usb_config, &config);
        end_config_transaction(NULL);
        char frame[128];
        const int written = snprintf(
            frame,
            sizeof(frame),
            "FOF_CONFIG_OK:{\"generation\":%lu,\"reconnect\":%s}\n",
            (unsigned long)config.generation,
            result == BACKEND_PORTAL_UPDATE_OK ? "true" : "false");
        if (written > 0 && (size_t)written < sizeof(frame)) {
            (void)emit_required_frame(frame, (size_t)written);
        }
        return;
    }
    end_config_transaction(NULL);
    char frame[128];
    const int written = snprintf(
        frame,
        sizeof(frame),
        "FOF_CONFIG_ERROR:{\"reason\":\"%s\"}\n",
        portal_update_reason(result));
    if (written > 0 && (size_t)written < sizeof(frame)) {
        (void)emit_required_frame(frame, (size_t)written);
    }
}

static void handle_lite_save(int64_t now_ms)
{
    if (!begin_config_transaction(NULL)) {
        emit_staged_result("FOF_ERROR:", "config_transaction_failed");
        return;
    }
    lock_runtime();
    const backend_config_record_t current = s_runtime.config;
    unlock_runtime();
    uint32_t generation = 0U;
    const backend_portal_update_result_t result = backend_usb_config_save(
        &s_runtime.usb_config,
        &current,
        portal_commit,
        portal_reconnect,
        NULL,
        now_ms,
        &generation);
    if (result == BACKEND_PORTAL_UPDATE_OK ||
        result == BACKEND_PORTAL_UPDATE_RECONNECT_FAILED) {
        lock_runtime();
        s_runtime.portal.config = s_runtime.config;
        const backend_config_record_t config = s_runtime.config;
        unlock_runtime();
        backend_usb_config_init(&s_runtime.usb_config, &config);
        end_config_transaction(NULL);
        if (result == BACKEND_PORTAL_UPDATE_OK) {
            emit_staged_result("", "FOF_SAVED");
        } else {
            emit_staged_result("FOF_ERROR:", "reconnect_failed");
        }
        (void)generation;
        return;
    }
    end_config_transaction(NULL);
    emit_staged_result("FOF_ERROR:", portal_update_reason(result));
}

static void handle_lite_ap_start(int64_t now_ms)
{
    const bool live_confirmed = s_runtime.usb_available &&
        backend_usb_service_live_confirmed(&s_runtime.usb, now_ms);
    lock_runtime();
    const bool configured = s_runtime.config_loaded &&
        s_runtime.config.network_count > 0U &&
        backend_config_validate(&s_runtime.config) == BACKEND_CONFIG_VALID;
    const bool join_failed = s_runtime.wifi_initialized &&
        backend_wifi_manager_join_failed(&s_runtime.wifi);
    const bool eligible = !configured || join_failed;
    unlock_runtime();
    if (live_confirmed) {
        emit_control_error("usb_live_confirmed");
    } else if (!eligible) {
        emit_control_error("recovery_ap_not_needed");
    } else {
        static const char ok[] =
            "FOF_CTL_OK:{\"command\":\"ap_start\"}\n";
        (void)emit_required_frame(ok, sizeof(ok) - 1U);
    }
}

static bool dispatch_lite_ota_alias(
    const char *line, size_t length)
{
    if (s_runtime.ota_ready &&
        backend_ota_maintenance_is_status_usb(line, length)) {
        (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
        (void)backend_ota_maintenance_emit_status(&s_runtime.maintenance);
        (void)xSemaphoreGive(s_runtime.ota_lock);
        return true;
    }
    backend_ota_request_t request;
    if (!s_runtime.ota_ready ||
        !backend_ota_maintenance_parse_usb(line, length, &request)) {
        return false;
    }
    (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
    if (request.probe) {
        backend_ota_evidence_t evidence;
        (void)backend_ota_maintenance_run_probe(
            &s_runtime.maintenance,
            request.component,
            request.catalog_name,
            request.expected_sha256[0] == '\0'
                ? NULL : request.expected_sha256,
            &evidence);
    } else {
        (void)backend_ota_maintenance_request_apply(
            &s_runtime.maintenance, &request);
    }
    (void)xSemaphoreGive(s_runtime.ota_lock);
    return true;
}

static void lite_usb_line(
    void *context,
    const char *line,
    size_t length,
    int64_t now_ms)
{
    (void)context;
    if (screen_only_command(line, length)) {
        emit_control_error("unsupported_capability");
        return;
    }
    backend_usb_command_t command;
    if (!backend_usb_protocol_parse_line(line, length, &command)) {
        emit_control_error("malformed_command");
        return;
    }
    char frame[256];
    size_t frame_length = 0U;
    switch (command.kind) {
    case BACKEND_USB_COMMAND_PING:
        frame_length = backend_usb_protocol_encode_pong(
            backend_identity_for_image(BACKEND_IMAGE_UPLINK),
            frame,
            sizeof(frame));
        if (frame_length != 0U) {
            (void)emit_required_frame(frame, frame_length);
        }
        return;
    case BACKEND_USB_COMMAND_STATUS:
        emit_lite_status();
        return;
    case BACKEND_USB_COMMAND_LIVE_START: {
        char session_id[33];
        if (!backend_usb_service_live_start(
                &s_runtime.usb, now_ms, session_id)) {
            emit_control_error("live_start_failed");
            return;
        }
        frame_length = backend_usb_protocol_encode_live_ready(
            session_id, frame, sizeof(frame));
        if (frame_length == 0U ||
            !backend_usb_service_emit_live_ready(
                &s_runtime.usb,
                session_id,
                frame,
                frame_length)) {
            (void)backend_usb_service_live_stop(
                &s_runtime.usb, session_id);
            emit_control_error("required_queue_full");
        }
        return;
    }
    case BACKEND_USB_COMMAND_LIVE_ACK:
        if (!backend_usb_service_live_acknowledge(
                &s_runtime.usb,
                command.session_id,
                command.sequence,
                now_ms)) {
            emit_control_error("invalid_live_ack");
        }
        return;
    case BACKEND_USB_COMMAND_LIVE_STOP:
        if (!backend_usb_service_live_stop(
                &s_runtime.usb, command.session_id)) {
            emit_control_error("stale_live_session");
            return;
        }
        {
            const int written = snprintf(
                frame,
                sizeof(frame),
                "FOF_LIVE_STOPPED:{\"session_id\":\"%s\"}\n",
                command.session_id);
            if (written > 0 && (size_t)written < sizeof(frame)) {
                (void)emit_required_frame(frame, (size_t)written);
            }
        }
        return;
    case BACKEND_USB_COMMAND_CONFIG_GET:
        emit_redacted_config();
        return;
    case BACKEND_USB_COMMAND_CONFIG_SET:
        handle_lite_config_set(&command, now_ms);
        return;
    case BACKEND_USB_COMMAND_SET: {
        backend_portal_update_result_t result =
            BACKEND_PORTAL_UPDATE_COMMIT_FAILED;
        if (begin_config_transaction(NULL)) {
            result = backend_usb_config_stage(
                &s_runtime.usb_config, command.key, command.value);
            end_config_transaction(NULL);
        }
        if (result == BACKEND_PORTAL_UPDATE_OK) {
            emit_staged_result("FOF_OK:", command.key);
        } else {
            emit_staged_result("FOF_ERROR:", portal_update_reason(result));
        }
        return;
    }
    case BACKEND_USB_COMMAND_SAVE:
        handle_lite_save(now_ms);
        return;
    case BACKEND_USB_COMMAND_BACKEND_STATUS:
        emit_status_lines();
        return;
    case BACKEND_USB_COMMAND_AP_START:
        handle_lite_ap_start(now_ms);
        return;
    case BACKEND_USB_COMMAND_UNKNOWN:
        if (!dispatch_lite_ota_alias(line, length)) {
            emit_control_error("unknown_command");
        }
        return;
    default:
        emit_control_error("malformed_command");
        return;
    }
}
#endif

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
static void usb_worker(void *argument)
{
    (void)argument;
    char line[UPLINK_USB_LINE_CAPACITY];
    lock_runtime();
    s_runtime.usb_worker_live = true;
    unlock_runtime();
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t length = strlen(line);
        while (length > 0U &&
               (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
            line[--length] = '\0';
        }
        if (strcmp(line, "FOF_BACKEND_STATUS") == 0) {
            emit_status_lines();
            continue;
        }
        if (backend_config_portal_handle_usb_line(
                &s_runtime.portal, line, length)) {
            if (backend_config_portal_take_usb_start_request(
                    &s_runtime.portal)) {
                lock_runtime();
                s_runtime.usb_ap_start_requested = true;
                unlock_runtime();
            }
            continue;
        }
        if (s_runtime.ota_ready &&
            backend_ota_maintenance_is_status_usb(line, length)) {
            (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
            (void)backend_ota_maintenance_emit_status(
                &s_runtime.maintenance);
            (void)xSemaphoreGive(s_runtime.ota_lock);
            continue;
        }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        backend_ota_request_t request;
        if (s_runtime.ota_ready &&
            backend_ota_maintenance_parse_usb(line, length, &request)) {
            (void)xSemaphoreTake(s_runtime.ota_lock, portMAX_DELAY);
            if (request.probe) {
                backend_ota_evidence_t evidence;
                (void)backend_ota_maintenance_run_probe(
                    &s_runtime.maintenance,
                    request.component,
                    request.catalog_name,
                    request.expected_sha256[0] == '\0'
                        ? NULL : request.expected_sha256,
                    &evidence);
            } else {
                (void)backend_ota_maintenance_request_apply(
                    &s_runtime.maintenance, &request);
            }
            (void)xSemaphoreGive(s_runtime.ota_lock);
            continue;
        }
#endif
        (void)print_line(
            "FOF_BACKEND_ERROR {\"reason\":\"unknown_command\"}");
    }
}
#endif

static bool create_runtime_tasks(void)
{
    BaseType_t created = xTaskCreate(
        uart_worker, "uart0_backend", UPLINK_UART_TASK_STACK_DEPTH,
        (void *)(uintptr_t)0U, 8U, NULL);
    created = created == pdPASS ? xTaskCreate(
        uart_worker, "uart1_backend", UPLINK_UART_TASK_STACK_DEPTH,
        (void *)(uintptr_t)1U, 8U, NULL) : created;
    created = created == pdPASS ? xTaskCreate(
        coordinator_worker, "coordinator_backend", 7168U,
        NULL, 7U, NULL) : created;
    created = created == pdPASS ? xTaskCreate(
        network_worker, "network_backend", 4096U,
        NULL, 5U, NULL) : created;
    created = created == pdPASS ? xTaskCreate(
        uploader_worker, "uploader_backend", 14336U,
        NULL, 5U, NULL) : created;
    created = created == pdPASS ? xTaskCreate(
        time_worker, "time_backend", 8192U,
        NULL, 4U, NULL) : created;
    created = created == pdPASS ? xTaskCreate(
        command_worker, "commands_backend", 14336U,
        NULL, 5U, NULL) : created;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    created = created == pdPASS ? xTaskCreate(
        backend_ota_client_worker, "ota_client_backend", 14336U,
        NULL, 5U, NULL) : created;
#endif
    created = created == pdPASS ? xTaskCreate(
        ota_worker, "ota_backend", 8192U,
        NULL, 4U, NULL) : created;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    created = created == pdPASS ? xTaskCreate(
        usb_worker, "usb_backend", 8192U,
        NULL, 3U, NULL) : created;
#endif
    return created == pdPASS;
}

static bool workers_ready(void)
{
    lock_runtime();
    const bool ready = s_runtime.uart_worker_live[0] &&
        s_runtime.uart_worker_live[1] &&
        s_runtime.coordinator_worker_live &&
        s_runtime.network_worker_live &&
        s_runtime.uploader_worker_live &&
        s_runtime.time_worker_live &&
        s_runtime.command_worker_live &&
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        s_runtime.ota_client_worker_live &&
#endif
        s_runtime.ota_worker_live
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        &&
        s_runtime.usb_worker_live;
#else
        ;
#endif
    unlock_runtime();
    return ready;
}

static bool init_sync_objects(void)
{
    s_runtime.lock = xSemaphoreCreateMutex();
    s_runtime.coordinator_lock = xSemaphoreCreateMutex();
    s_runtime.upload_build_lock = xSemaphoreCreateMutex();
    s_runtime.http_lock = xSemaphoreCreateMutex();
    s_runtime.usb_lock = xSemaphoreCreateMutex();
    s_runtime.ota_lock = xSemaphoreCreateMutex();
    s_runtime.uart_tx_lock[0] = xSemaphoreCreateMutex();
    s_runtime.uart_tx_lock[1] = xSemaphoreCreateMutex();
    s_runtime.relay_complete = xSemaphoreCreateBinary();
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    s_runtime.event_ring_lock = xSemaphoreCreateMutex();
    s_runtime.config_transaction_lock = xSemaphoreCreateRecursiveMutex();
#endif
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    s_runtime.ota_work_queue = xQueueCreate(
        1U, sizeof(uplink_ota_work_item_t));
    s_runtime.ota_result_queue = xQueueCreate(
        UPLINK_OTA_RESULT_QUEUE_LENGTH, sizeof(uplink_ota_result_item_t));
    s_runtime.ota_progress_queue = xQueueCreate(
        UPLINK_OTA_PROGRESS_QUEUE_LENGTH,
        sizeof(backend_ota_progress_update_t));
    s_runtime.ota_progress_ack = xSemaphoreCreateBinary();
#endif
    return s_runtime.lock != NULL && s_runtime.coordinator_lock != NULL &&
           s_runtime.upload_build_lock != NULL &&
           s_runtime.http_lock != NULL &&
           s_runtime.usb_lock != NULL && s_runtime.ota_lock != NULL &&
           s_runtime.uart_tx_lock[0] != NULL &&
           s_runtime.uart_tx_lock[1] != NULL &&
           s_runtime.relay_complete != NULL
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
           && s_runtime.event_ring_lock != NULL &&
           s_runtime.config_transaction_lock != NULL
#endif
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
           && s_runtime.ota_work_queue != NULL &&
           s_runtime.ota_result_queue != NULL &&
           s_runtime.ota_progress_queue != NULL &&
           s_runtime.ota_progress_ack != NULL
#endif
           ;
}

static bool load_or_create_config(void)
{
    if (backend_config_load_or_migrate(&s_runtime.config)) {
        s_runtime.config_loaded = true;
        return true;
    }
    if (backend_nvs_config_storage_health() != BACKEND_NVS_STORAGE_READY) {
        return false;
    }
    backend_config_record_t factory;
    memset(&factory, 0, sizeof(factory));
    factory.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    factory.generation = 1U;
    copy_text(factory.backend_url, sizeof(factory.backend_url),
              UPLINK_FACTORY_URL);
    (void)snprintf(
        factory.device_id,
        sizeof(factory.device_id),
        "uplink_%02X%02X%02X",
        s_runtime.mac[3], s_runtime.mac[4], s_runtime.mac[5]);
    copy_text(factory.display_name, sizeof(factory.display_name),
              factory.device_id);
    copy_text(factory.ap_password, sizeof(factory.ap_password),
              BACKEND_CONFIG_PORTAL_DEFAULT_PASSWORD);
    factory.auto_update_enabled = false;
    if (!backend_config_commit(&factory)) {
        return false;
    }
    s_runtime.config = factory;
    s_runtime.config_loaded = true;
    return true;
}

static bool init_ota_runtime(void)
{
    const backend_self_ota_adapters_t self_adapters = {
        .context = &s_runtime.self_ota_context,
        .begin = self_ota_begin_adapter,
        .write = self_ota_write_adapter,
        .end = self_ota_end_adapter,
        .select_boot_partition = self_ota_select_adapter,
        .mark_running_valid = self_ota_mark_valid_adapter,
    };
    backend_self_ota_init(&s_runtime.self_ota, &self_adapters);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    const bool state_read = running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    if (!state_read ||
        (ota_state != ESP_OTA_IMG_VALID &&
         ota_state != ESP_OTA_IMG_PENDING_VERIFY)) {
        return false;
    }
    const bool pending = state_read && ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    backend_self_ota_on_boot(&s_runtime.self_ota, s_runtime.boot_id, pending);

    if (!backend_firmware_buffer_init_once(
            &s_runtime.firmware_buffer, firmware_buffer_alloc, NULL)) {
        return false;
    }
    const backend_firmware_store_partition_t store_partition = {
        .context = NULL,
        .erase = store_partition_erase,
        .write = store_partition_write,
        .read = store_partition_read,
    };
    backend_firmware_store_init(
        &s_runtime.firmware_store, &store_partition);
    const backend_ota_journal_storage_t journal = {
        .context = NULL,
        .load = journal_load,
        .store = journal_store,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        .erase_exact = journal_erase_exact,
#endif
    };
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    s_runtime.ota_startup_action = backend_ota_journal_startup_recover(
        &journal, &s_runtime.ota_startup_journal);
    if (!topology_epoch_restore(
            s_runtime.ota_startup_action,
            &s_runtime.ota_startup_journal)) {
        return false;
    }
#endif
    const backend_ota_maintenance_adapters_t adapters = {
        .context = NULL,
        .fetch_metadata = ota_fetch_metadata,
        .download_image = ota_download_image,
        .running_version = ota_running_version,
        .partition_capacity = ota_partition_capacity,
        .image_write_count = ota_image_write_count,
        .snapshot_binding = ota_snapshot_binding,
        .acquire_target_claim = ota_acquire_target,
        .release_target_claim = ota_release_target,
        .validate_staged_image = ota_validate_staged,
        .scanner_dry_run = ota_scanner_dry_run,
        .mutate_staged_image = ota_mutate_staged,
        .request_reboot = ota_request_reboot,
        .read_convergence = ota_read_convergence,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        .report_progress = ota_report_progress,
        .relay_retry_count = ota_relay_retry_count,
#endif
        .emit_and_flush = ota_emit,
    };
    if (!backend_ota_maintenance_init(
            &s_runtime.maintenance,
            &adapters,
            &journal,
            &s_runtime.firmware_buffer,
            s_runtime.mac,
            s_runtime.boot_id)) {
        return false;
    }
    backend_ota_maintenance_on_boot(
        &s_runtime.maintenance, s_runtime.boot_id);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_workflow_init(&s_runtime.ota_workflow);
    s_runtime.ota_outbox_storage =
        (backend_ota_event_outbox_storage_t) {
            .context = NULL,
            .load_slot = ota_outbox_load_slot,
            .store_slot = ota_outbox_store_slot,
            .clear_exact_slot = ota_outbox_clear_exact_slot,
        };
    const backend_ota_event_outbox_load_result_t outbox_state =
        backend_ota_event_outbox_load(
            &s_runtime.ota_outbox_storage, &s_runtime.ota_startup_outbox);
    s_runtime.ota_client_blocked =
        outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_CORRUPT ||
        outbox_state == BACKEND_OTA_EVENT_OUTBOX_LOAD_IO_ERROR ||
        !ota_startup_restore(outbox_state);
#endif
    for (size_t index = 0U; index < 3U; ++index) {
        backend_ota_poll_init(
            &s_runtime.ota_poll[index], s_runtime.boot_monotonic_ms);
    }
    return true;
}

static const char *initial_ota_state(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return "unknown";
    }
    if (state == ESP_OTA_IMG_VALID) {
        return "valid";
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        return "pending_verify";
    }
    return "invalid";
}

static bool running_ota_state_is_safe(void)
{
    const char *state = initial_ota_state();
    return strcmp(state, "valid") == 0 ||
           strcmp(state, "pending_verify") == 0;
}

static void build_boot_line(void)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    lock_runtime();
    (void)snprintf(
        s_runtime.boot_line,
        sizeof(s_runtime.boot_line),
        "FOF_BACKEND_BOOT {\"product_family\":\"%s\","
        "\"firmware_line\":\"%s\",\"component\":\"%s\","
        "\"target\":\"%s\",\"project\":\"%s\","
        "\"hardware\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\","
        "\"boot_id\":%" PRIu32 ",\"device_id\":\"%s\","
        "\"config_state\":\"loaded\",\"config_generation\":%" PRIu32 ","
        "\"nvs_erased\":false,\"auto_update_enabled\":%s,"
        "\"uart0_started\":%s,\"uart1_started\":%s,"
        "\"network_state\":\"%s\",\"ota_state\":\"%s\"}",
        identity->product_family,
        identity->firmware_line,
        identity->component,
        identity->target,
        identity->project,
        identity->hardware,
        identity->version,
        s_runtime.mac_text,
        s_runtime.boot_id,
        s_runtime.config.device_id,
        s_runtime.config.generation,
        s_runtime.config.auto_update_enabled ? "true" : "false",
        s_runtime.uart_started[0] ? "true" : "false",
        s_runtime.uart_started[1] ? "true" : "false",
        network_state_locked(),
        initial_ota_state());
    unlock_runtime();
}

static void build_health_line(void)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    lock_runtime();
    (void)snprintf(
        s_runtime.health_line,
        sizeof(s_runtime.health_line),
        "FOF_BACKEND_HEALTH {\"product_family\":\"%s\","
        "\"firmware_line\":\"%s\",\"component\":\"%s\","
        "\"target\":\"%s\",\"project\":\"%s\","
        "\"hardware\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\","
        "\"boot_id\":%" PRIu32 ",\"device_id\":\"%s\","
        "\"config_state\":\"loaded\",\"config_generation\":%" PRIu32 ","
        "\"nvs_loaded\":true,\"nvs_erased\":false,"
        "\"auto_update_enabled\":%s,\"uart0_started\":%s,"
        "\"uart1_started\":%s,\"coordinator_started\":%s,"
        "\"network_state\":\"%s\",\"rollback_clear\":%s}",
        identity->product_family,
        identity->firmware_line,
        identity->component,
        identity->target,
        identity->project,
        identity->hardware,
        identity->version,
        s_runtime.mac_text,
        s_runtime.boot_id,
        s_runtime.config.device_id,
        s_runtime.config.generation,
        s_runtime.config.auto_update_enabled ? "true" : "false",
        s_runtime.uart_started[0] ? "true" : "false",
        s_runtime.uart_started[1] ? "true" : "false",
        s_runtime.coordinator_worker_live ? "true" : "false",
        network_state_locked(),
        backend_self_ota_rollback_clear(&s_runtime.self_ota)
            ? "true" : "false");
    s_runtime.health_line_ready =
        backend_self_ota_rollback_clear(&s_runtime.self_ota);
    unlock_runtime();
}

void app_main(void)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!backend_identity_record_validate(
            &fof_backend_embedded_identity) ||
        fof_backend_embedded_identity.image_kind != BACKEND_IMAGE_UPLINK) {
        printf("FOF_BACKEND_FATAL {\"reason\":\"identity\"}\n");
        return;
    }
    s_runtime.boot_monotonic_ms = monotonic_ms();
    s_runtime.next_batch_sequence = 1U;
    s_runtime.role_generation = 1U;
    s_runtime.topology_generation = 1U;
    s_runtime.catalog_generation = 1U;
    s_runtime.wifi_rssi = -127;
    s_runtime.scanner_last_seen_ms[0] = -1;
    s_runtime.scanner_last_seen_ms[1] = -1;
    s_runtime.boot_id = esp_random();
    if (s_runtime.boot_id == 0U) {
        s_runtime.boot_id = 1U;
    }
    if (esp_read_mac(s_runtime.mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        printf("FOF_BACKEND_FATAL {\"reason\":\"mac\"}\n");
        return;
    }
    format_mac(s_runtime.mac, s_runtime.mac_text);
    if (!init_sync_objects() || !load_or_create_config()) {
        printf("FOF_BACKEND_FATAL {\"reason\":\"config\"}\n");
        return;
    }

    s_runtime.led_started = backend_status_led_init(BACKEND_LED_NETWORK_DEGRADED);
    if (!backend_uart_slots_init(&s_runtime.uarts)) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"uart_slots\"}");
        return;
    }
    for (size_t slot = 0U; slot < 2U; ++slot) {
        s_runtime.uart_started[slot] = backend_uart_slot_driver_init(slot);
        backend_scanner_status_tracker_init(
            &s_runtime.scanner_tracker[slot]);
    }
    /* Production badge scanners reserve only six seconds at boot for their
     * fixed physical role.  Seed each UART immediately, before Wi-Fi/AP/OTA
     * initialization, and let the coordinator retry while the dialect is
     * still unknown.  Backend-native scanners ignore these native commands. */
    if (xTaskCreate(
            production_scanner_bootstrap_worker,
            "prod_scanner_boot",
            UPLINK_PRODUCTION_BOOTSTRAP_STACK_DEPTH,
            NULL,
            9U,
            NULL) != pdPASS) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"scanner_bootstrap\"}");
        return;
    }
    backend_coordinator_init(&s_runtime.coordinator);
    backend_coordinator_set_upload_sink(
        &s_runtime.coordinator, coordinator_upload_sink, NULL);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    backend_coordinator_set_canonical_sink(
        &s_runtime.coordinator, coordinator_canonical_sink, NULL);
#endif
    backend_threat_init(&s_runtime.threats);
    backend_uploader_state_init(&s_runtime.uploader);
    backend_heartbeat_init(
        &s_runtime.heartbeat, s_runtime.boot_monotonic_ms);
    backend_ble_investigation_init(&s_runtime.investigation);
    backend_command_client_init(&s_runtime.command_client);
    (void)backend_command_http_state_init(
        &s_runtime.command_http, s_runtime.boot_monotonic_ms);

    s_runtime.upload_storage = heap_caps_calloc(
        BACKEND_UPLOAD_FIFO_CAPACITY,
        sizeof(backend_upload_batch_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    backend_upload_fifo_init(
        &s_runtime.upload_fifo,
        s_runtime.upload_storage,
        BACKEND_UPLOAD_FIFO_CAPACITY);
    if (!backend_upload_fifo_is_valid(&s_runtime.upload_fifo)) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"upload_queue\"}");
        return;
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    atomic_init(&s_runtime.event_ring_contention_drops, 0U);
    s_runtime.event_storage = psram_alloc_strict(
        128U * sizeof(backend_dashboard_event_t));
    s_runtime.history_available = backend_event_ring_init(
        &s_runtime.event_ring,
        s_runtime.event_storage,
        BACKEND_EVENT_RING_CAPACITY);
    backend_usb_config_init(&s_runtime.usb_config, &s_runtime.config);
#endif

    const backend_config_portal_ops_t portal_ops = {
        .context = NULL,
        .commit_config = portal_commit,
        .reconnect_wifi = portal_reconnect,
        .backend_get = portal_backend_get,
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
        .begin_config_transaction = begin_config_transaction,
        .end_config_transaction = end_config_transaction,
        .dashboard_status = portal_dashboard_status,
        .event_snapshot = portal_event_snapshot,
#endif
    };
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    backend_lite_ap_policy_init(&s_runtime.lite_ap_policy);
#else
    backend_ap_policy_init(
        &s_runtime.ap_policy, s_runtime.boot_monotonic_ms);
#endif
    const bool boot_config_valid = s_runtime.config_loaded &&
        s_runtime.config.network_count > 0U &&
        backend_config_validate(&s_runtime.config) == BACKEND_CONFIG_VALID;
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    const backend_lite_ap_input_t boot_ap_input = {
        .wifi_configured = boot_config_valid,
        .wifi_connected = false,
        .wifi_join_failed = false,
        .usb_live_confirmed = false,
    };
    const backend_ap_action_t boot_ap_action = backend_lite_ap_policy_tick(
        &s_runtime.lite_ap_policy, boot_ap_input);
#else
    const backend_ap_input_t boot_ap_input = {
        .config_valid = boot_config_valid,
        .config_generation = s_runtime.config.generation,
        .backend_connected = false,
        .usb_start_requested = false,
    };
    const backend_ap_action_t boot_ap_action = backend_ap_policy_tick(
        &s_runtime.ap_policy,
        boot_ap_input,
        s_runtime.boot_monotonic_ms);
#endif
    if (!backend_config_portal_init(
            &s_runtime.portal, &s_runtime.config, &portal_ops) ||
        !initialize_wifi_platform()) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"network_init\"}");
        return;
    }
    (void)esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    (void)esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    if (boot_ap_action == BACKEND_AP_START) {
        if (!backend_config_portal_start(
                &s_runtime.portal, s_runtime.mac)) {
            print_line("FOF_BACKEND_FATAL {\"reason\":\"config_ap\"}");
            return;
        }
        s_runtime.portal_started = true;
    } else if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
               esp_wifi_start() != ESP_OK) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"station_start\"}");
        return;
    }
    if (s_runtime.config.network_count > 0U) {
        s_runtime.wifi_initialized = backend_wifi_manager_init(
            &s_runtime.wifi,
            &s_runtime.config,
            s_runtime.boot_monotonic_ms);
    }
    if (!running_ota_state_is_safe()) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"ota_state\"}");
        return;
    }
    s_runtime.ota_ready = init_ota_runtime();
    if (!s_runtime.ota_ready) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"ota_runtime\"}");
        return;
    }
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    const backend_usb_service_config_t usb_config = {
        .context = NULL,
        .on_line = lite_usb_line,
    };
    s_runtime.usb_available = backend_usb_service_start(
        &s_runtime.usb, &usb_config);
    if (s_runtime.usb_available) {
        char ready[32];
        const size_t ready_length = backend_usb_protocol_encode_ready(
            ready, sizeof(ready));
        if (ready_length != 0U) {
            (void)emit_required_frame(ready, ready_length);
        }
    }
#endif

    if (!s_runtime.led_started || !s_runtime.uart_started[0] ||
        !s_runtime.uart_started[1] || !create_runtime_tasks()) {
        print_line("FOF_BACKEND_FATAL {\"reason\":\"workers\"}");
        return;
    }
    for (size_t attempt = 0U; attempt < 100U && !workers_ready(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (;;) {
        lock_runtime();
        const bool network_ready =
            s_runtime.portal_started || s_runtime.wifi_connected;
        unlock_runtime();
        if (network_ready) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    build_boot_line();
    (void)print_line(s_runtime.boot_line);

    backend_self_ota_health_t rollback_health = {
        .config_loaded = s_runtime.config_loaded,
        .led_worker_running = s_runtime.led_started,
        .uart_worker_running = s_runtime.uart_worker_live[0] &&
                               s_runtime.uart_worker_live[1],
        .coordinator_worker_running = s_runtime.coordinator_worker_live,
        .ap_healthy = s_runtime.portal_started,
        .sta_healthy = s_runtime.wifi_connected,
        .backend_reachable = s_runtime.backend_reachable,
    };
    if (!backend_self_ota_rollback_clear(&s_runtime.self_ota)) {
        (void)backend_self_ota_mark_valid_if_healthy(
            &s_runtime.self_ota, &rollback_health);
    }
    build_health_line();
    if (s_runtime.health_line_ready) {
        (void)print_line(s_runtime.health_line);
    }
}
