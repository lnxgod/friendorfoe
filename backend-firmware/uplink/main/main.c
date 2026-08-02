#include <inttypes.h>
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "backend_ap_policy.h"
#include "backend_ble_investigation.h"
#include "backend_command_client.h"
#include "backend_config_portal.h"
#include "backend_coordinator.h"
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
#include "backend_led_pattern.h"
#include "backend_nvs_config.h"
#include "backend_ota_maintenance.h"
#include "backend_scanner_control_codec.h"
#include "backend_scanner_relay.h"
#include "backend_scanner_status_codec.h"
#include "backend_scanner_topology.h"
#include "backend_self_ota.h"
#include "backend_threat_policy.h"
#include "backend_time_sync.h"
#include "backend_uart_investigation.h"
#include "backend_uart_slot.h"
#include "backend_upload_batch.h"
#include "backend_upload_fifo.h"
#include "backend_uploader.h"
#include "backend_wifi_manager.h"
#include "backend_status_led.h"

#define UPLINK_FACTORY_URL "http://192.168.4.2:8000"
#define UPLINK_SCANNER_STALE_MS INT64_C(15000)
#define UPLINK_COORDINATOR_PERIOD_MS 100
#define UPLINK_NETWORK_PERIOD_MS 500
#define UPLINK_UPLOAD_PERIOD_MS 250
#define UPLINK_TIME_PERIOD_MS 10000
#define UPLINK_OTA_PERIOD_MS 1000
#define UPLINK_RELAY_WAIT_MS 180000
#define UPLINK_USB_LINE_CAPACITY 512U
#define UPLINK_STATUS_CAPACITY 768U
#define UPLINK_HTTP_RESPONSE_CAPACITY (BACKEND_HTTP_MAX_JSON_BODY + 1U)
#define UPLINK_JOURNAL_NAMESPACE "fof_backend"
#define UPLINK_JOURNAL_KEY "ota_journal"

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

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t http_lock;
    SemaphoreHandle_t usb_lock;
    SemaphoreHandle_t ota_lock;
    SemaphoreHandle_t uart_tx_lock[BACKEND_UART_SLOT_COUNT];
    SemaphoreHandle_t relay_complete;

    backend_config_record_t config;
    backend_config_portal_t portal;
    backend_ap_policy_t ap_policy;
    backend_wifi_manager_t wifi;
    backend_uart_slots_t uarts;
    backend_coordinator_t coordinator;
    backend_threat_state_t threats;
    backend_scanner_status_tracker_t scanner_tracker[2];
    backend_scanner_health_t scanner_health[2];
    backend_scanner_plan_t scanner_plan;
    int64_t scanner_last_seen_ms[2];
    backend_upload_fifo_t upload_fifo;
    backend_upload_batch_t *upload_storage;
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
    int64_t boot_monotonic_ms;
    int64_t epoch_anchor_ms;
    int64_t epoch_anchor_monotonic_ms;
    int8_t wifi_rssi;
    char wifi_ssid[33];
    bool epoch_valid;
    bool config_loaded;
    bool portal_started;
    bool usb_ap_start_requested;
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
    bool ota_worker_live;
    bool usb_worker_live;
    bool fatal_runtime;
    bool flow_sent_paused;
    bool flow_state_known;
    bool ota_ready;
    bool relay_active;
    bool relay_result;
    bool target_claimed[3];
    char boot_line[UPLINK_STATUS_CAPACITY];
    char health_line[UPLINK_STATUS_CAPACITY];
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
    const int result = printf("%s\n", line);
    fflush(stdout);
    (void)xSemaphoreGive(s_runtime.usb_lock);
    return result >= 0;
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
    copy_text(context->capabilities[0], sizeof(context->capabilities[0]),
              "scanner_uart");
    copy_text(context->capabilities[1], sizeof(context->capabilities[1]),
              "http_uplink");
    copy_text(context->capabilities[2], sizeof(context->capabilities[2]),
              "threat_led");
    context->capability_count = 3U;
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
    if (s_runtime.uploader.last_backend_success_ms >= 0 &&
        now_ms >= s_runtime.uploader.last_backend_success_ms) {
        context->upload.has_last_success_age = true;
        context->upload.last_success_age_s = (uint32_t)(
            (now_ms - s_runtime.uploader.last_backend_success_ms) / 1000);
    }
    context->sequence = sequence;
    return true;
}

static bool queue_upload_locked(
    const backend_detection_observation_t *observation)
{
    backend_batch_context_t context;
    const uint32_t sequence = s_runtime.next_batch_sequence;
    if (!upload_context_locked(sequence, &context)) {
        return false;
    }
    backend_upload_builder_init(
        &s_runtime.upload_builder, &context, monotonic_ms());
    if (observation != NULL &&
        backend_upload_builder_add(
            &s_runtime.upload_builder, observation, monotonic_ms()) !=
            BACKEND_ENCODE_OK) {
        return false;
    }
    backend_upload_batch_t batch;
    if (!backend_upload_builder_finish(&s_runtime.upload_builder, &batch)) {
        return false;
    }
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
            &s_runtime.upload_fifo, &batch, &dropped)) {
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
    return true;
}

static bool coordinator_upload_sink(
    void *context, const backend_detection_observation_t *observation)
{
    (void)context;
    return queue_upload_locked(observation);
}

static bool portal_commit(
    void *context, const backend_config_record_t *candidate)
{
    (void)context;
    if (!backend_config_commit(candidate)) {
        return false;
    }
    lock_runtime();
    s_runtime.config = *candidate;
    backend_ap_policy_note_config_commit(
        &s_runtime.ap_policy, candidate->generation, monotonic_ms());
    unlock_runtime();
    return true;
}

static bool portal_reconnect(
    void *context,
    const backend_config_record_t *committed,
    int64_t now_ms)
{
    (void)context;
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
    char response[UPLINK_HTTP_RESPONSE_CAPACITY];
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
    const backend_http_result_t result = backend_http_get_json(
        base_url, path, response, sizeof(response));
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
        s_runtime.wifi_connected = true;
        if (s_runtime.wifi_initialized) {
            (void)backend_wifi_manager_handle_event(
                &s_runtime.wifi, BACKEND_WIFI_EVENT_CONNECTED, now_ms);
        }
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
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
    const backend_http_result_t result = backend_http_get_json(
        base_url, endpoint, json, capacity);
    (void)xSemaphoreGive(s_runtime.http_lock);
    if (!result.transport_complete || result.status_code != 200 ||
        result.body_length == 0U || result.body_length >= capacity) {
        return false;
    }
    lock_runtime();
    s_runtime.catalog_generation =
        s_runtime.catalog_generation == UINT32_MAX
            ? 1U : s_runtime.catalog_generation + 1U;
    *out_catalog_generation = s_runtime.catalog_generation;
    unlock_runtime();
    *out_length = result.body_length;
    return true;
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
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
    const backend_http_result_t result = backend_http_get_binary(
        base_url,
        endpoint,
        expected_size,
        binary_download_sink,
        &sink);
    (void)xSemaphoreGive(s_runtime.http_lock);
    if (!result.transport_complete || result.status_code != 200 ||
        sink.length != expected_size) {
        return false;
    }
    *out_size = sink.length;
    return true;
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
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length,
    bool dry_run)
{
    const int slot = backend_ota_component_slot(component);
    if (slot < 0 || slot >= 2 || manifest == NULL || bytes == NULL ||
        length != manifest->image_size) {
        return false;
    }
    uplink_memory_image_t image = {.bytes = bytes, .length = length};
    const backend_firmware_store_result_t stage =
        backend_firmware_store_stage(
            &s_runtime.firmware_store,
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
        s_runtime.relay_active || s_runtime.role_generation > UINT32_MAX - 2U) {
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
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
    return relay_image(component, manifest, bytes, length, true);
}

static bool ota_mutate_staged(
    void *context,
    backend_ota_component_t component,
    const backend_ota_manifest_t *manifest,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
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
    return relay_image(component, manifest, bytes, length, false);
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
            s_runtime.topology_generation =
                s_runtime.topology_generation == UINT32_MAX
                    ? UINT32_MAX : s_runtime.topology_generation + 1U;
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
        backend_detection_observation_t observation;
        backend_observation_resolve(
            &detection,
            &stamp,
            current_epoch_ms_locked(now_ms),
            &observation);
        const backend_coordinator_ingest_result_t result =
            backend_coordinator_ingest_detection(
                &s_runtime.coordinator,
                (uint8_t)slot,
                &observation,
                now_ms);
        if (result.update_local_threat) {
            backend_threat_ingest(
                &s_runtime.threats, &observation.detection, now_ms);
        }
        unlock_runtime();
        return;
    }

    backend_scanner_status_t status;
    if (backend_scanner_status_decode(
            (const char *)bytes, length, &status) ==
        BACKEND_SCANNER_STATUS_DECODE_OK) {
        lock_runtime();
        update_scanner_status_locked(slot, &status, now_ms);
        unlock_runtime();
        return;
    }

    ble_investigation_chunk_t chunk;
    if (backend_uart_investigation_decode(bytes, length, &chunk) ==
        BACKEND_UART_INVESTIGATION_DECODE_OK) {
        lock_runtime();
        (void)backend_ble_investigation_accept_chunk(
            &s_runtime.investigation,
            (backend_scanner_slot_t)slot,
            &chunk);
        unlock_runtime();
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
    backend_scanner_control_t control = {
        .type = BACKEND_SCANNER_CONTROL_ROLE,
        .payload.role = {
            .boot_id = health->boot_id,
            .generation = generation,
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
            (void)uart_send_control(slot, &control);
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
        lock_runtime();
        (void)backend_coordinator_tick_detections(
            &s_runtime.coordinator, now_ms);
        uint8_t retry_slot = 0U;
        backend_detection_observation_t retry_threat;
        if (backend_coordinator_retry_one(
                &s_runtime.coordinator, &retry_slot, &retry_threat)) {
            backend_threat_ingest(
                &s_runtime.threats, &retry_threat.detection, now_ms);
        }
        for (size_t slot = 0U; slot < 2U; ++slot) {
            if (s_runtime.scanner_health[slot].connected &&
                now_ms - s_runtime.scanner_last_seen_ms[slot] >
                    UPLINK_SCANNER_STALE_MS) {
                s_runtime.scanner_health[slot].connected = false;
                s_runtime.scanner_health[slot].role_acked = false;
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
                s_runtime.scanner_plan.desired[slot];
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
        backend_health_snapshot_t health_snapshot;
        backend_health_evaluate(&inputs, &health_snapshot);
        (void)backend_status_led_set_state(health_snapshot.led_state);
        uint8_t connected_mask = 0U;
        for (size_t slot = 0U; slot < 2U; ++slot) {
            if (s_runtime.scanner_health[slot].connected) {
                connected_mask |= (uint8_t)(UINT8_C(1) << slot);
            }
        }
        backend_led_mirror_output_t mirror;
        backend_coordinator_update_led(
            &s_runtime.coordinator,
            health_snapshot.led_state,
            connected_mask,
            now_ms,
            &mirror);
        send_led_mirror_locked(&mirror);

        const bool paused = backend_coordinator_flow_paused(
            &s_runtime.coordinator);
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
                        (void)uart_send_control(slot, &flow);
                    }
                }
                s_runtime.flow_state_known = true;
                s_runtime.flow_sent_paused = paused;
            }
        }
        service_relay_locked(now_ms);
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
        backend_ap_action_t ap_action = BACKEND_AP_NO_CHANGE;
        lock_runtime();
        if (s_runtime.wifi_initialized) {
            (void)backend_wifi_manager_handle_event(
                &s_runtime.wifi,
                BACKEND_WIFI_EVENT_TICK,
                monotonic_ms());
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
        unlock_runtime();
        if (ap_action == BACKEND_AP_START) {
            const bool started = backend_config_portal_start(
                &s_runtime.portal, s_runtime.mac);
            lock_runtime();
            s_runtime.portal_started = started;
            if (!started) {
                s_runtime.ap_policy.running = false;
            }
            unlock_runtime();
        } else if (ap_action == BACKEND_AP_STOP) {
            const bool stopped = backend_config_portal_stop(
                &s_runtime.portal);
            lock_runtime();
            s_runtime.portal_started = !stopped;
            if (!stopped) {
                s_runtime.ap_policy.running = true;
            }
            unlock_runtime();
        }
        vTaskDelay(pdMS_TO_TICKS(UPLINK_NETWORK_PERIOD_MS));
    }
}

static void uploader_worker(void *argument)
{
    (void)argument;
    char response[UPLINK_HTTP_RESPONSE_CAPACITY];
    backend_upload_batch_t request;
    lock_runtime();
    s_runtime.uploader_worker_live = true;
    unlock_runtime();
    for (;;) {
        const int64_t now_ms = monotonic_ms();
        bool have_request = false;
        char base_url[sizeof(s_runtime.config.backend_url)];
        char device_id[sizeof(s_runtime.config.device_id)];
        lock_runtime();
        if (backend_heartbeat_due(&s_runtime.heartbeat, now_ms) &&
            queue_upload_locked(NULL)) {
            backend_heartbeat_mark_queued(&s_runtime.heartbeat, now_ms);
        }
        const backend_upload_batch_t *head =
            backend_upload_fifo_peek(&s_runtime.upload_fifo);
        if (head != NULL && backend_uploader_begin_head(
                &s_runtime.uploader,
                head,
                s_runtime.upload_fifo.count,
                now_ms)) {
            request = *head;
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
        const backend_http_result_t http_result = backend_http_post_json(
            base_url,
            "/detections/drones",
            request.json,
            request.json_len,
            response,
            sizeof(response));
        (void)xSemaphoreGive(s_runtime.http_lock);
        const bool ack_valid = http_result.transport_complete &&
            backend_ingest_ack_validate(
                response,
                http_result.body_length,
                device_id,
                request.item_count);
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
            live_head->sequence == request.sequence &&
            live_head->json_crc32 == request.json_crc32 &&
            (disposition == BACKEND_HTTP_ACK ||
             disposition == BACKEND_HTTP_QUARANTINE) &&
            backend_upload_fifo_pop_acked(
                &s_runtime.upload_fifo, request.sequence)) {
            queue_result = disposition == BACKEND_HTTP_ACK
                ? BACKEND_UPLOADER_QUEUE_POPPED
                : BACKEND_UPLOADER_QUEUE_QUARANTINED;
        }
        const backend_uploader_outcome_t outcome =
            backend_uploader_note_response(
                &s_runtime.uploader,
                request.sequence,
                request.json_crc32,
                disposition,
                http_result.status_code,
                queue_result,
                s_runtime.upload_fifo.count,
                esp_random(),
                now_ms);
        if (outcome == BACKEND_UPLOADER_ACKED) {
            s_runtime.backend_reachable = true;
            backend_ap_policy_note_backend_success(
                &s_runtime.ap_policy,
                s_runtime.config.generation,
                now_ms);
        } else if (!http_result.transport_complete) {
            s_runtime.backend_reachable = false;
        }
        unlock_runtime();
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
        if (s_runtime.scanner_health[slot].connected) {
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
        char base_url[sizeof(s_runtime.config.backend_url)];
        bool connected = false;
        lock_runtime();
        connected = s_runtime.wifi_connected;
        copy_text(base_url, sizeof(base_url), s_runtime.config.backend_url);
        unlock_runtime();
        if (!sntp_valid && connected) {
            char response[UPLINK_HTTP_RESPONSE_CAPACITY];
            (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
            const backend_http_result_t result = backend_http_get_json(
                base_url,
                "/detections/time",
                response,
                sizeof(response));
            (void)xSemaphoreGive(s_runtime.http_lock);
            backend_valid = result.transport_complete &&
                result.status_code == 200 &&
                backend_time_parse_response(
                    response, result.body_length, &backend_epoch_ms);
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

    char response[UPLINK_HTTP_RESPONSE_CAPACITY];
    (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
    const backend_http_result_t http_result = backend_http_post_json(
        base_url, path, body, body_length, response, sizeof(response));
    (void)xSemaphoreGive(s_runtime.http_lock);
    lock_runtime();
    backend_command_result_ack_t ack;
    const bool ack_valid = http_result.transport_complete &&
        backend_command_result_ack_validate(
            &s_runtime.command_client,
            response,
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
        lock_runtime();
        (void)backend_command_poll_started(&s_runtime.command_http, now_ms);
        unlock_runtime();
        char response[UPLINK_HTTP_RESPONSE_CAPACITY];
        (void)xSemaphoreTake(s_runtime.http_lock, portMAX_DELAY);
        const backend_http_result_t http_result = backend_http_get_json(
            base_url, path, response, sizeof(response));
        (void)xSemaphoreGive(s_runtime.http_lock);
        const backend_command_http_action_t action =
            backend_command_poll_http_action(
                http_result.transport_complete, http_result.status_code);
        lock_runtime();
        backend_command_http_note(
            &s_runtime.command_http, action, http_result.status_code, false);
        if (action == BACKEND_COMMAND_HTTP_BODY) {
            backend_command_envelope_t envelope;
            if (backend_command_envelope_decode(
                    response, http_result.body_length, &envelope) ==
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
    }
}

static void ota_worker(void *argument)
{
    (void)argument;
    lock_runtime();
    s_runtime.ota_worker_live = true;
    unlock_runtime();
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
        (void)print_line(
            "FOF_BACKEND_ERROR {\"reason\":\"unknown_command\"}");
    }
}

static bool create_runtime_tasks(void)
{
    BaseType_t created = xTaskCreate(
        uart_worker, "uart0_backend", 6144U,
        (void *)(uintptr_t)0U, 8U, NULL);
    created = created == pdPASS ? xTaskCreate(
        uart_worker, "uart1_backend", 6144U,
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
    created = created == pdPASS ? xTaskCreate(
        ota_worker, "ota_backend", 8192U,
        NULL, 4U, NULL) : created;
    created = created == pdPASS ? xTaskCreate(
        usb_worker, "usb_backend", 8192U,
        NULL, 3U, NULL) : created;
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
        s_runtime.ota_worker_live &&
        s_runtime.usb_worker_live;
    unlock_runtime();
    return ready;
}

static bool init_sync_objects(void)
{
    s_runtime.lock = xSemaphoreCreateMutex();
    s_runtime.http_lock = xSemaphoreCreateMutex();
    s_runtime.usb_lock = xSemaphoreCreateMutex();
    s_runtime.ota_lock = xSemaphoreCreateMutex();
    s_runtime.uart_tx_lock[0] = xSemaphoreCreateMutex();
    s_runtime.uart_tx_lock[1] = xSemaphoreCreateMutex();
    s_runtime.relay_complete = xSemaphoreCreateBinary();
    return s_runtime.lock != NULL && s_runtime.http_lock != NULL &&
           s_runtime.usb_lock != NULL && s_runtime.ota_lock != NULL &&
           s_runtime.uart_tx_lock[0] != NULL &&
           s_runtime.uart_tx_lock[1] != NULL &&
           s_runtime.relay_complete != NULL;
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
    };
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
        "FOF_BACKEND_BOOT {\"target\":\"%s\",\"project\":\"%s\","
        "\"hardware\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\","
        "\"boot_id\":%" PRIu32 ",\"device_id\":\"%s\","
        "\"config_state\":\"loaded\",\"config_generation\":%" PRIu32 ","
        "\"nvs_erased\":false,\"auto_update_enabled\":%s,"
        "\"uart0_started\":%s,\"uart1_started\":%s,"
        "\"network_state\":\"%s\",\"ota_state\":\"%s\"}",
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
        "FOF_BACKEND_HEALTH {\"target\":\"%s\",\"mac\":\"%s\","
        "\"boot_id\":%" PRIu32 ",\"device_id\":\"%s\","
        "\"config_state\":\"loaded\",\"config_generation\":%" PRIu32 ","
        "\"nvs_loaded\":true,\"nvs_erased\":false,"
        "\"auto_update_enabled\":%s,\"uart0_started\":%s,"
        "\"uart1_started\":%s,\"coordinator_started\":%s,"
        "\"network_state\":\"%s\",\"rollback_clear\":%s}",
        identity->target,
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
    backend_coordinator_init(&s_runtime.coordinator);
    backend_coordinator_set_upload_sink(
        &s_runtime.coordinator, coordinator_upload_sink, NULL);
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

    const backend_config_portal_ops_t portal_ops = {
        .context = NULL,
        .commit_config = portal_commit,
        .reconnect_wifi = portal_reconnect,
        .backend_get = portal_backend_get,
    };
    backend_ap_policy_init(
        &s_runtime.ap_policy, s_runtime.boot_monotonic_ms);
    const bool boot_config_valid = s_runtime.config_loaded &&
        s_runtime.config.network_count > 0U &&
        backend_config_validate(&s_runtime.config) == BACKEND_CONFIG_VALID;
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
