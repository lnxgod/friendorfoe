/**
 * Friend or Foe -- Uplink HTTP Upload Implementation
 *
 * Collects drone detections from a FreeRTOS queue, batches them into
 * JSON payloads, and POSTs to the FastAPI backend.  When WiFi is down,
 * batches are buffered in a ring buffer and drained on reconnect.
 */

#include "http_upload.h"
#include "uart_rx.h"
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
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_up";

static QueueHandle_t   s_detection_queue   = NULL;
static ring_buffer_t  *s_offline_buffer    = NULL;
static int             s_success_count     = 0;
static int             s_fail_count        = 0;
static int64_t         s_last_success_epoch_ms = 0;

/* Persistent HTTP client handle (avoids socket exhaustion from rapid open/close) */
static esp_http_client_handle_t s_http_client = NULL;

/* Maximum JSON payload size for a batch */
#define MAX_PAYLOAD_SIZE    4096

/* Retry config — keep total blocking time well under 30s WDT timeout */
#define HTTP_TIMEOUT_MS     3000    /* 3s per attempt (was 10s) */
#define MAX_RETRIES         1       /* 1 retry max — fail fast, try next batch */
#define BACKOFF_BASE_MS     500     /* 500ms base (was 1000) */
#define MAX_DRAIN_PER_CYCLE 1       /* drain 1 offline batch per loop — don't block */
#define HEALTH_RESET_SEC    30      /* force reset client if no success for 30s */

/* ── Source integer to string mapping ──────────────────────────────────── */

static const char *source_to_string(uint8_t src)
{
    switch (src) {
        case DETECTION_SRC_BLE_RID:            return "ble_rid";
        case DETECTION_SRC_WIFI_SSID:          return "wifi_ssid";
        case DETECTION_SRC_WIFI_DJI_IE:        return "wifi_dji_ie";
        case DETECTION_SRC_WIFI_BEACON:        return "wifi_beacon_rid";
        case DETECTION_SRC_WIFI_OUI:           return "wifi_oui";
        case DETECTION_SRC_WIFI_PROBE_REQUEST: return "wifi_probe_request";
        default:                               return "unknown";
    }
}

static size_t estimate_detection_json_size(const drone_detection_t *det)
{
    size_t estimate = 120; /* fixed keys + punctuation */

    if (det->drone_id[0] != '\0') estimate += strlen(det->drone_id);
    if (det->manufacturer[0] != '\0') estimate += strlen(det->manufacturer);
    if (det->model[0] != '\0') estimate += strlen(det->model);
    if (det->ssid[0] != '\0') estimate += strlen(det->ssid);
    if (det->bssid[0] != '\0') estimate += strlen(det->bssid);
    if (det->operator_id[0] != '\0') estimate += strlen(det->operator_id);

    if (det->latitude != 0.0 || det->longitude != 0.0) estimate += 32;
    if (det->operator_lat != 0.0 || det->operator_lon != 0.0) estimate += 32;
    if (det->altitude_m != 0.0) estimate += 12;
    if (det->speed_mps != 0.0) estimate += 10;
    if (det->heading_deg != 0.0) estimate += 10;
    if (det->last_updated_ms > 0) estimate += 18;
    return estimate;
}

/* ── Build JSON payload from a batch of detections ─────────────────────── */

static char *build_payload(const drone_detection_t *batch, int count, int64_t scan_ts_ms)
{
    /* Get device identity */
    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));

    /* Get device GPS position */
    gps_position_t gps_pos = {0};
    gps_get_position(&gps_pos);

    /* Use scan timestamp if provided, else current time */
    int64_t ts_ms = (scan_ts_ms > 0) ? scan_ts_ms : time_sync_get_epoch_ms();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "device_lat", gps_pos.latitude);
    cJSON_AddNumberToObject(root, "device_lon", gps_pos.longitude);
    cJSON_AddNumberToObject(root, "device_alt", gps_pos.altitude_m);
    cJSON_AddNumberToObject(root, "timestamp", (double)(ts_ms / 1000));

    /* Firmware version + board type */
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        cJSON_AddStringToObject(root, "firmware_version", app->version);
    }
#ifdef UPLINK_ESP32
    cJSON_AddStringToObject(root, "board_type", "uplink-esp32");
#else
    cJSON_AddStringToObject(root, "board_type", "uplink-c3");
#endif

    /* Connected scanner identity (from UART scanner_info messages) */
    cJSON *scanners = cJSON_AddArrayToObject(root, "scanners");
    if (scanners) {
        const scanner_info_t *ble_info = uart_rx_get_ble_scanner_info();
        if (ble_info) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "uart", "ble");
            cJSON_AddStringToObject(s, "ver", ble_info->version);
            cJSON_AddStringToObject(s, "board", ble_info->board);
            cJSON_AddStringToObject(s, "chip", ble_info->chip);
            cJSON_AddStringToObject(s, "caps", ble_info->caps);
            cJSON_AddItemToArray(scanners, s);
        }
#if CONFIG_DUAL_SCANNER
        const scanner_info_t *wifi_info = uart_rx_get_wifi_scanner_info();
        if (wifi_info) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "uart", "wifi");
            cJSON_AddStringToObject(s, "ver", wifi_info->version);
            cJSON_AddStringToObject(s, "board", wifi_info->board);
            cJSON_AddStringToObject(s, "chip", wifi_info->chip);
            cJSON_AddStringToObject(s, "caps", wifi_info->caps);
            cJSON_AddItemToArray(scanners, s);
        }
#endif
    }

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
        cJSON_AddNumberToObject(det, "latitude", d->latitude);
        cJSON_AddNumberToObject(det, "longitude", d->longitude);
        cJSON_AddNumberToObject(det, "altitude_m", d->altitude_m);
        cJSON_AddNumberToObject(det, "rssi", d->rssi);
        cJSON_AddNumberToObject(det, "speed_mps", d->speed_mps);
        cJSON_AddNumberToObject(det, "heading_deg", d->heading_deg);
        cJSON_AddStringToObject(det, "manufacturer", d->manufacturer);
        cJSON_AddStringToObject(det, "model", d->model);

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

        /* Probe request: include probed_ssids array for backend */
        if (d->source == DETECTION_SRC_WIFI_PROBE_REQUEST && d->ssid[0] != '\0') {
            cJSON *probed = cJSON_AddArrayToObject(det, "probed_ssids");
            if (probed) {
                cJSON_AddItemToArray(probed, cJSON_CreateString(d->ssid));
            }
        }

        /* BLE fingerprinting fields */
        if (d->ble_company_id != 0) {
            cJSON_AddNumberToObject(det, "ble_company_id", d->ble_company_id);
        }
        if (d->ble_apple_type != 0) {
            cJSON_AddNumberToObject(det, "ble_apple_type", d->ble_apple_type);
        }
        if (d->ble_ad_type_count != 0) {
            cJSON_AddNumberToObject(det, "ble_ad_type_count", d->ble_ad_type_count);
        }
        if (d->ble_payload_len != 0) {
            cJSON_AddNumberToObject(det, "ble_payload_len", d->ble_payload_len);
        }
        if (d->ble_addr_type != 0) {
            cJSON_AddNumberToObject(det, "ble_addr_type", d->ble_addr_type);
        }
        if (d->ble_ja3_hash != 0) {
            char ja3_hex[9];
            snprintf(ja3_hex, sizeof(ja3_hex), "%08lx", (unsigned long)d->ble_ja3_hash);
            cJSON_AddStringToObject(det, "ble_ja3", ja3_hex);
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

/**
 * Ensure the persistent HTTP client is initialized.
 * Reuses the same TCP connection (keep-alive) to avoid socket exhaustion.
 */
static bool s_using_fallback_url = false;

static bool ensure_http_client(void)
{
    if (s_http_client) {
        return true;
    }

    char backend_url[128] = {0};
    if (s_using_fallback_url) {
        strncpy(backend_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(backend_url) - 1);
        ESP_LOGI(TAG, "Using fallback backend URL: %s", backend_url);
    } else {
        nvs_config_get_backend_url(backend_url, sizeof(backend_url));
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);

    esp_http_client_config_t config = {
        .url              = url,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = HTTP_TIMEOUT_MS,
        .buffer_size      = 2048,
        .keep_alive_enable = true,
    };

    s_http_client = esp_http_client_init(&config);
    if (!s_http_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_http_client_set_header(s_http_client, "Content-Type", "application/json");
    ESP_LOGI(TAG, "HTTP client created: %s (timeout=%dms, keep-alive)", url, HTTP_TIMEOUT_MS);
    return true;
}

/**
 * Destroy and recreate the HTTP client (call after persistent errors).
 */
static void reset_http_client(void)
{
    if (s_http_client) {
        esp_http_client_close(s_http_client);
        esp_http_client_cleanup(s_http_client);
        s_http_client = NULL;
    }
}

/**
 * POST payload to backend using persistent keep-alive connection.
 *
 * If the connection is stale (server closed keep-alive), we reset the
 * client and retry ONCE with a fresh connection.  This handles the
 * common case where uvicorn's keep-alive timeout expires between batches.
 */
static bool http_post_payload(const char *payload)
{
    if (!wifi_sta_is_connected()) {
        return false;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_http_client()) {
            return false;
        }

        esp_http_client_set_post_field(s_http_client, payload, strlen(payload));
        esp_err_t err = esp_http_client_perform(s_http_client);

        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(s_http_client);
            if (status >= 200 && status < 300) {
                ESP_LOGD(TAG, "Upload OK (HTTP %d)", status);
                return true;
            }
            /* Server error (5xx) — don't reset client, just fail this attempt */
            ESP_LOGW(TAG, "Upload failed: HTTP %d", status);
            return false;
        }

        /* Connection error — reset client and retry once with fresh socket */
        if (attempt == 0) {
            ESP_LOGW(TAG, "Connection error: %s — reconnecting", esp_err_to_name(err));
            reset_http_client();
            /* Immediate retry with fresh client (stale keep-alive recovery) */
            continue;
        }

        ESP_LOGW(TAG, "HTTP failed after reconnect: %s", esp_err_to_name(err));
        reset_http_client();
    }

    return false;
}

/* ── Upload a payload with exponential backoff retry ───────────────────── */

static bool upload_with_retry(const char *payload)
{
    int retry_delay_ms = BACKOFF_BASE_MS;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        if (!wifi_sta_is_connected()) {
            return false;
        }

        if (http_post_payload(payload)) {
            s_success_count++;
            return true;
        }

        s_fail_count++;

        if (attempt < MAX_RETRIES) {
            ESP_LOGW(TAG, "Retry %d/%d in %dms...", attempt + 1, MAX_RETRIES, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            retry_delay_ms *= 2;
            if (retry_delay_ms > 4000) {
                retry_delay_ms = 4000;  /* cap at 4s to stay under WDT */
            }
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

/**
 * Drain at most MAX_DRAIN_PER_CYCLE batches from the offline buffer.
 * Non-blocking: yields back to the main loop quickly to avoid WDT timeout.
 */
static void drain_offline_buffer(void)
{
    if (!s_offline_buffer || ring_buffer_is_empty(s_offline_buffer)) {
        return;
    }

    int remaining = ring_buffer_count(s_offline_buffer);
    int drained = 0;

    offline_batch_t batch;
    while (drained < MAX_DRAIN_PER_CYCLE && ring_buffer_pop(s_offline_buffer, &batch)) {
        if (!wifi_sta_is_connected()) {
            ring_buffer_push(s_offline_buffer, &batch);
            return;
        }

        if (!upload_with_retry(batch.json)) {
            ring_buffer_push(s_offline_buffer, &batch);
            return;
        }

        drained++;
    }

    if (drained > 0) {
        ESP_LOGI(TAG, "Drained %d offline batches (%d remaining)",
                 drained, ring_buffer_count(s_offline_buffer));
    }
}

/* ── Upload task ───────────────────────────────────────────────────────── */

static void http_upload_task(void *arg)
{
    drone_detection_t batch[CONFIG_MAX_BATCH_SIZE];
    int batch_count = 0;
    size_t estimated_payload_bytes = 0;
    TickType_t first_item_tick = 0;
    TickType_t last_item_tick = 0;
    TickType_t last_send   = 0;  /* tick count of last successful send */
    int64_t scan_ts_ms     = 0;  /* timestamp of first detection in batch */

    ESP_LOGI(TAG, "HTTP upload task started");
    bool was_connected = false;
    TickType_t last_success_tick = xTaskGetTickCount();
    int consecutive_fails = 0;

    while (1) {
        /* In standalone mode, just drain the queue without uploading */
        if (wifi_sta_is_standalone()) {
            drone_detection_t det;
            xQueueReceive(s_detection_queue, &det, pdMS_TO_TICKS(500));
            continue;
        }

        /* Reset HTTP client on WiFi reconnect (old socket is dead) */
        bool connected_now = wifi_sta_is_connected();
        if (connected_now && !was_connected) {
            ESP_LOGI(TAG, "WiFi reconnected — resetting HTTP client");
            reset_http_client();
            consecutive_fails = 0;
        }
        was_connected = connected_now;

        /* ── Health watchdog: force reset if stalled ─────────────────── */
        if (connected_now) {
            TickType_t since_success = xTaskGetTickCount() - last_success_tick;
            if (since_success >= pdMS_TO_TICKS(HEALTH_RESET_SEC * 1000)) {
                ESP_LOGW(TAG, "HEALTH RESET: no success for %ds, resetting client + clearing offline buffer",
                         HEALTH_RESET_SEC);
                reset_http_client();
                /* Clear stale offline batches — they're probably the cause */
                if (s_offline_buffer) {
                    offline_batch_t discard;
                    while (ring_buffer_pop(s_offline_buffer, &discard)) {}
                }
                consecutive_fails = 0;
                last_success_tick = xTaskGetTickCount();  /* prevent rapid re-triggers */
            }
        }

        /* Try to drain offline buffer when WiFi is up (non-blocking, max 1) */
        if (connected_now && consecutive_fails == 0 && !ring_buffer_is_empty(s_offline_buffer)) {
            drain_offline_buffer();
        }

        /* Collect detections into batch */
        drone_detection_t det;
        TickType_t wait_ticks = (batch_count > 0)
            ? pdMS_TO_TICKS(25)
            : pdMS_TO_TICKS(100);

        if (xQueueReceive(s_detection_queue, &det, wait_ticks) == pdTRUE) {
            if (batch_count == 0) {
                /* Start the batch age clock with the first detection. */
                first_item_tick = xTaskGetTickCount();
                estimated_payload_bytes = 64; /* batch envelope */
                scan_ts_ms = det.last_updated_ms > 0 ? det.last_updated_ms : time_sync_get_epoch_ms();
            }
            last_item_tick = xTaskGetTickCount();
            batch[batch_count++] = det;
            estimated_payload_bytes += estimate_detection_json_size(&det);
            if (det.last_updated_ms > 0 &&
                (scan_ts_ms == 0 || det.last_updated_ms < scan_ts_ms)) {
                scan_ts_ms = det.last_updated_ms;
            }
        }

        /* Check if batch is ready to send */
        TickType_t now_tick = xTaskGetTickCount();
        TickType_t age = (batch_count > 0) ? (now_tick - first_item_tick) : 0;
        TickType_t idle = (batch_count > 0) ? (now_tick - last_item_tick) : 0;
        bool time_elapsed = (batch_count > 0 &&
                             age >= pdMS_TO_TICKS(CONFIG_BATCH_INTERVAL_MS));
        bool idle_flush = (batch_count > 0 &&
                           idle >= pdMS_TO_TICKS(CONFIG_BATCH_IDLE_FLUSH_MS));
        bool batch_full   = (batch_count >= CONFIG_MAX_BATCH_SIZE);
        bool payload_full = (estimated_payload_bytes >= CONFIG_TARGET_BATCH_BYTES);

        if (batch_count > 0 && (time_elapsed || idle_flush || batch_full || payload_full)) {
            ESP_LOGI(TAG,
                     "Sending batch count=%d age=%" PRIu32 "ms idle=%" PRIu32 "ms bytes=%u",
                     batch_count,
                     (uint32_t)(age * portTICK_PERIOD_MS),
                     (uint32_t)(idle * portTICK_PERIOD_MS),
                     (unsigned)estimated_payload_bytes);

            char *payload = build_payload(batch, batch_count, scan_ts_ms);
            if (payload) {
                if (wifi_sta_is_connected()) {
                    if (!upload_with_retry(payload)) {
                        buffer_batch_offline(payload);
                        consecutive_fails++;
                        /* After 5 consecutive failures, try the other URL */
                        if (consecutive_fails >= 5 && consecutive_fails % 5 == 0) {
                            s_using_fallback_url = !s_using_fallback_url;
                            ESP_LOGW(TAG, "Switching to %s URL after %d failures",
                                     s_using_fallback_url ? "fallback" : "primary",
                                     consecutive_fails);
                            /* Force client recreation with new URL */
                            if (s_http_client) {
                                esp_http_client_cleanup(s_http_client);
                                s_http_client = NULL;
                            }
                        }
                    } else {
                        last_send = xTaskGetTickCount();
                        last_success_tick = last_send;
                        s_last_success_epoch_ms = esp_timer_get_time() / 1000;
                        consecutive_fails = 0;
                    }
                } else {
                    buffer_batch_offline(payload);
                }
                cJSON_free(payload);
            } else {
                ESP_LOGE(TAG, "Failed to build JSON payload");
            }

            /* Reset batch */
            batch_count = 0;
            estimated_payload_bytes = 0;
            scan_ts_ms = 0;
            first_item_tick = 0;
            last_item_tick = 0;
        }

        /* ── Heartbeat: send empty batch if idle for 60s ────────────── */
        TickType_t now = xTaskGetTickCount();
        if (batch_count == 0 && wifi_sta_is_connected() &&
            (now - last_send) >= pdMS_TO_TICKS(CONFIG_HEARTBEAT_INTERVAL_MS)) {
            ESP_LOGI(TAG, "Heartbeat (idle %ds) heap=%lu ok=%d fail=%d",
                     CONFIG_HEARTBEAT_INTERVAL_MS / 1000,
                     (unsigned long)esp_get_free_heap_size(),
                     s_success_count, s_fail_count);
            char *payload = build_payload(NULL, 0, 0);
            if (payload) {
                if (upload_with_retry(payload)) {
                    last_send = xTaskGetTickCount();
                    last_success_tick = last_send;
                    consecutive_fails = 0;
                }
                cJSON_free(payload);
            }
        }

        /* ── Poll lock-on command from backend every 10s ─────────────── */
        {
            static TickType_t last_lockon_poll = 0;
            static bool lockon_was_active = false;
            TickType_t now2 = xTaskGetTickCount();

            if (wifi_sta_is_connected() &&
                (now2 - last_lockon_poll) >= pdMS_TO_TICKS(10000)) {
                last_lockon_poll = now2;

                /* Build lock-on poll URL (per-node: includes device_id) */
                char backend_url[128] = {0};
                if (s_using_fallback_url) {
                    strncpy(backend_url, CONFIG_BACKEND_URL_FALLBACK, sizeof(backend_url) - 1);
                } else {
                    nvs_config_get_backend_url(backend_url, sizeof(backend_url));
                }
                char dev_id[32] = {0};
                nvs_config_get_device_id(dev_id, sizeof(dev_id));
                char url[256];
                snprintf(url, sizeof(url), "%s/detections/lockon?device_id=%s",
                         backend_url, dev_id);

                esp_http_client_config_t cfg = {
                    .url = url,
                    .method = HTTP_METHOD_GET,
                    .timeout_ms = 3000,
                };
                esp_http_client_handle_t client = esp_http_client_init(&cfg);
                if (client) {
                    esp_err_t err = esp_http_client_perform(client);
                    if (err == ESP_OK) {
                        int len = esp_http_client_get_content_length(client);
                        if (len > 0 && len < 512) {
                            char buf[512] = {0};
                            esp_http_client_read(client, buf, sizeof(buf) - 1);
                            /* Parse lock-on response */
                            cJSON *resp = cJSON_Parse(buf);
                            if (resp) {
                                cJSON *active = cJSON_GetObjectItem(resp, "active");
                                if (active && cJSON_IsTrue(active) && !lockon_was_active) {
                                    cJSON *ch = cJSON_GetObjectItem(resp, "channel");
                                    cJSON *dur = cJSON_GetObjectItem(resp, "duration_s");
                                    cJSON *bssid_j = cJSON_GetObjectItem(resp, "bssid");
                                    cJSON *type_j = cJSON_GetObjectItem(resp, "type");
                                    int lock_ch = ch ? ch->valueint : 6;
                                    int lock_dur = dur ? dur->valueint : 45;
                                    const char *lock_type = (type_j && type_j->valuestring) ? type_j->valuestring : "wifi";
                                    const char *lock_bssid = (bssid_j && bssid_j->valuestring) ? bssid_j->valuestring : "";

                                    lockon_was_active = true;
                                    char cmd[160];

                                    if (strcmp(lock_type, "ble") == 0) {
                                        /* BLE lock-on: focus on specific MAC */
                                        ESP_LOGW(TAG, "BLE LOCK-ON: mac=%s dur=%ds", lock_bssid, lock_dur);
                                        snprintf(cmd, sizeof(cmd),
                                                 "{\"type\":\"ble_lockon\",\"mac\":\"%s\",\"dur\":%d}",
                                                 lock_bssid, lock_dur);
                                    } else {
                                        /* WiFi lock-on: fix channel */
                                        ESP_LOGW(TAG, "WiFi LOCK-ON: ch=%d bssid=%s dur=%ds",
                                                 lock_ch, lock_bssid, lock_dur);
                                        snprintf(cmd, sizeof(cmd),
                                                 "{\"type\":\"lockon\",\"ch\":%d,\"dur\":%d,\"bssid\":\"%s\"}",
                                                 lock_ch, lock_dur, lock_bssid);
                                    }
                                    uart_rx_send_command(cmd);
                                } else if (active && !cJSON_IsTrue(active) && lockon_was_active) {
                                    ESP_LOGI(TAG, "LOCK-ON cancelled by backend");
                                    lockon_was_active = false;
                                    uart_rx_send_command("{\"type\":\"lockon_cancel\"}");
                                    uart_rx_send_command("{\"type\":\"ble_lockon_cancel\"}");
                                }
                                cJSON_Delete(resp);
                            }
                        }
                    }
                    esp_http_client_cleanup(client);
                }
            }
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
             "idle_flush=%dms, target_bytes=%d, offline_cap=%d)",
             CONFIG_MAX_BATCH_SIZE, CONFIG_BATCH_INTERVAL_MS,
             CONFIG_BATCH_IDLE_FLUSH_MS, CONFIG_TARGET_BATCH_BYTES,
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

int64_t http_upload_get_last_success_ms(void)
{
    return s_last_success_epoch_ms;
}
