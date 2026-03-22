/**
 * Friend or Foe -- Uplink HTTP Upload Implementation
 *
 * Collects drone detections from a FreeRTOS queue, batches them into
 * JSON payloads, and POSTs to the FastAPI backend.  When WiFi is down,
 * batches are buffered in a ring buffer and drained on reconnect.
 */

#include "http_upload.h"
#include "wifi_sta.h"
#include "ring_buffer.h"
#include "nvs_config.h"
#include "config.h"
#include "gps.h"
#include "time_sync.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_up";

static QueueHandle_t   s_detection_queue   = NULL;
static ring_buffer_t  *s_offline_buffer    = NULL;
static int             s_success_count     = 0;
static int             s_fail_count        = 0;

/* Maximum JSON payload size for a batch */
#define MAX_PAYLOAD_SIZE    8192

/* Exponential backoff base delay */
#define BACKOFF_BASE_MS     1000

/* ── Source integer to string mapping ──────────────────────────────────── */

static const char *source_to_string(uint8_t src)
{
    switch (src) {
        case DETECTION_SRC_BLE_RID:     return "ble_rid";
        case DETECTION_SRC_WIFI_SSID:   return "wifi_ssid";
        case DETECTION_SRC_WIFI_DJI_IE: return "wifi_dji_ie";
        case DETECTION_SRC_WIFI_BEACON: return "wifi_beacon";
        case DETECTION_SRC_WIFI_OUI:    return "wifi_oui";
        default:                        return "unknown";
    }
}

/* ── Build JSON payload from a batch of detections ─────────────────────── */

static char *build_payload(const drone_detection_t *batch, int count)
{
    /* Get device identity */
    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));

    /* Get device GPS position */
    gps_position_t gps_pos = {0};
    gps_get_position(&gps_pos);

    /* Get current timestamp */
    int64_t now_ms = time_sync_get_epoch_ms();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "device_lat", gps_pos.latitude);
    cJSON_AddNumberToObject(root, "device_lon", gps_pos.longitude);
    cJSON_AddNumberToObject(root, "device_alt", gps_pos.altitude_m);
    cJSON_AddNumberToObject(root, "timestamp", (double)(now_ms / 1000));

    cJSON *detections = cJSON_AddArrayToObject(root, "detections");
    if (!detections) {
        cJSON_Delete(root);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        const drone_detection_t *d = &batch[i];
        cJSON *det = cJSON_CreateObject();
        if (!det) {
            continue;
        }

        cJSON_AddStringToObject(det, "drone_id", d->drone_id);
        cJSON_AddStringToObject(det, "source", source_to_string(d->source));
        cJSON_AddNumberToObject(det, "confidence", d->confidence);
        cJSON_AddNumberToObject(det, "lat", d->latitude);
        cJSON_AddNumberToObject(det, "lon", d->longitude);
        cJSON_AddNumberToObject(det, "alt", d->altitude_m);
        cJSON_AddNumberToObject(det, "rssi", d->rssi);
        cJSON_AddNumberToObject(det, "speed", d->speed_mps);
        cJSON_AddNumberToObject(det, "heading", d->heading_deg);
        cJSON_AddNumberToObject(det, "vertical_speed", d->vertical_speed_mps);
        cJSON_AddNumberToObject(det, "distance", d->estimated_distance_m);
        cJSON_AddStringToObject(det, "manufacturer", d->manufacturer);
        cJSON_AddStringToObject(det, "model", d->model);
        cJSON_AddNumberToObject(det, "fused_confidence", d->fused_confidence);

        /* Operator info if present */
        if (d->operator_lat != 0.0 || d->operator_lon != 0.0) {
            cJSON_AddNumberToObject(det, "operator_lat", d->operator_lat);
            cJSON_AddNumberToObject(det, "operator_lon", d->operator_lon);
        }
        if (d->operator_id[0] != '\0') {
            cJSON_AddStringToObject(det, "operator_id", d->operator_id);
        }

        /* ASTM fields if present */
        if (d->ua_type != 0) {
            cJSON_AddNumberToObject(det, "ua_type", d->ua_type);
        }
        if (d->height_agl_m != 0.0) {
            cJSON_AddNumberToObject(det, "height_agl", d->height_agl_m);
        }

        /* WiFi fields if present */
        if (d->ssid[0] != '\0') {
            cJSON_AddStringToObject(det, "ssid", d->ssid);
        }
        if (d->bssid[0] != '\0') {
            cJSON_AddStringToObject(det, "bssid", d->bssid);
        }

        /* Timestamps */
        if (d->last_updated_ms > 0) {
            cJSON_AddNumberToObject(det, "last_updated",
                                    (double)(d->last_updated_ms / 1000));
        }

        cJSON_AddItemToArray(detections, det);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

/* ── HTTP POST with retry ──────────────────────────────────────────────── */

static bool http_post_payload(const char *payload)
{
    if (!wifi_sta_is_connected()) {
        return false;
    }

    /* Build full URL */
    char backend_url[128] = {0};
    nvs_config_get_backend_url(backend_url, sizeof(backend_url));

    char url[256];
    snprintf(url, sizeof(url), "%s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);

    esp_http_client_config_t config = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 10000,
        .buffer_size    = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    bool success = false;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGD(TAG, "Upload OK (HTTP %d, %" PRId64 " bytes)",
                     status, (int64_t)esp_http_client_get_content_length(client));
            success = true;
        } else {
            ESP_LOGW(TAG, "Upload failed: HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

/* ── Upload a payload with exponential backoff retry ───────────────────── */

static bool upload_with_retry(const char *payload)
{
    int retry_delay_ms = BACKOFF_BASE_MS;

    for (int attempt = 0; attempt < 5; attempt++) {
        if (http_post_payload(payload)) {
            s_success_count++;
            return true;
        }

        s_fail_count++;

        if (!wifi_sta_is_connected()) {
            /* WiFi is down -- no point retrying now */
            return false;
        }

        ESP_LOGW(TAG, "Retry %d in %dms...", attempt + 1, retry_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));

        retry_delay_ms *= 2;
        if (retry_delay_ms > CONFIG_MAX_RETRY_DELAY_MS) {
            retry_delay_ms = CONFIG_MAX_RETRY_DELAY_MS;
        }
    }

    return false;
}

/* ── Offline batch storage ─────────────────────────────────────────────── */

/*
 * When WiFi is down, we serialize the batch JSON and store it in the
 * ring buffer.  Each entry is a fixed-size char array.
 */
typedef struct {
    char json[MAX_PAYLOAD_SIZE];
} offline_batch_t;

static void buffer_batch_offline(const char *payload)
{
    if (!s_offline_buffer) {
        return;
    }

    offline_batch_t batch = {0};
    strncpy(batch.json, payload, sizeof(batch.json) - 1);

    bool overwritten = ring_buffer_push(s_offline_buffer, &batch);
    if (overwritten) {
        ESP_LOGW(TAG, "Offline buffer full, oldest batch dropped (count=%d)",
                 ring_buffer_count(s_offline_buffer));
    } else {
        ESP_LOGI(TAG, "Batch stored offline (count=%d)",
                 ring_buffer_count(s_offline_buffer));
    }
}

static void drain_offline_buffer(void)
{
    if (!s_offline_buffer || ring_buffer_is_empty(s_offline_buffer)) {
        return;
    }

    int count = ring_buffer_count(s_offline_buffer);
    ESP_LOGI(TAG, "Draining %d offline batches...", count);

    offline_batch_t batch;
    while (ring_buffer_pop(s_offline_buffer, &batch)) {
        if (!wifi_sta_is_connected()) {
            /* WiFi went down again -- re-buffer and stop */
            ring_buffer_push(s_offline_buffer, &batch);
            ESP_LOGW(TAG, "WiFi lost during drain, %d batches remaining",
                     ring_buffer_count(s_offline_buffer));
            return;
        }

        if (!upload_with_retry(batch.json)) {
            /* Failed even with retry -- put it back */
            ring_buffer_push(s_offline_buffer, &batch);
            ESP_LOGW(TAG, "Drain upload failed, stopping");
            return;
        }

        ESP_LOGD(TAG, "Drained offline batch, %d remaining",
                 ring_buffer_count(s_offline_buffer));
    }

    ESP_LOGI(TAG, "Offline buffer drained successfully");
}

/* ── Upload task ───────────────────────────────────────────────────────── */

static void http_upload_task(void *arg)
{
    drone_detection_t batch[CONFIG_MAX_BATCH_SIZE];
    int batch_count = 0;
    TickType_t batch_start = xTaskGetTickCount();

    ESP_LOGI(TAG, "HTTP upload task started");

    while (1) {
        /* In standalone mode, just drain the queue without uploading */
        if (wifi_sta_is_standalone()) {
            drone_detection_t det;
            xQueueReceive(s_detection_queue, &det, pdMS_TO_TICKS(500));
            continue;
        }

        /* Try to drain offline buffer first when WiFi is up */
        if (wifi_sta_is_connected() && !ring_buffer_is_empty(s_offline_buffer)) {
            drain_offline_buffer();
        }

        /* Collect detections into batch */
        drone_detection_t det;
        TickType_t wait_ticks = pdMS_TO_TICKS(100);

        if (xQueueReceive(s_detection_queue, &det, wait_ticks) == pdTRUE) {
            batch[batch_count++] = det;
        }

        /* Check if batch is ready to send */
        TickType_t elapsed = xTaskGetTickCount() - batch_start;
        bool time_elapsed = (elapsed >= pdMS_TO_TICKS(CONFIG_BATCH_INTERVAL_MS));
        bool batch_full   = (batch_count >= CONFIG_MAX_BATCH_SIZE);

        if (batch_count > 0 && (time_elapsed || batch_full)) {
            ESP_LOGI(TAG, "Sending batch of %d detections", batch_count);

            char *payload = build_payload(batch, batch_count);
            if (payload) {
                if (wifi_sta_is_connected()) {
                    if (!upload_with_retry(payload)) {
                        /* Upload failed -- store offline */
                        buffer_batch_offline(payload);
                    }
                } else {
                    /* WiFi down -- buffer for later */
                    buffer_batch_offline(payload);
                }
                cJSON_free(payload);
            } else {
                ESP_LOGE(TAG, "Failed to build JSON payload");
            }

            /* Reset batch */
            batch_count = 0;
            batch_start = xTaskGetTickCount();
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void http_upload_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;
    s_offline_buffer  = ring_buffer_create(CONFIG_MAX_OFFLINE_BATCHES,
                                           sizeof(offline_batch_t));
    if (!s_offline_buffer) {
        ESP_LOGE(TAG, "Failed to create offline ring buffer");
    }

    ESP_LOGI(TAG, "HTTP upload initialized (batch=%d, interval=%dms, "
             "offline_cap=%d)",
             CONFIG_MAX_BATCH_SIZE, CONFIG_BATCH_INTERVAL_MS,
             CONFIG_MAX_OFFLINE_BATCHES);
}

void http_upload_start(void)
{
    xTaskCreate(http_upload_task, "http_upload", CONFIG_HTTP_UPLOAD_STACK,
                NULL, CONFIG_HTTP_UPLOAD_PRIORITY, NULL);
    ESP_LOGI(TAG, "HTTP upload task created (priority=%d, stack=%d)",
             CONFIG_HTTP_UPLOAD_PRIORITY, CONFIG_HTTP_UPLOAD_STACK);
}

int http_upload_get_success_count(void)
{
    return s_success_count;
}

int http_upload_get_fail_count(void)
{
    return s_fail_count;
}
