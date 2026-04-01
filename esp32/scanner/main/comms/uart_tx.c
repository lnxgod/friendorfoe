/**
 * Friend or Foe -- Scanner UART TX Implementation
 *
 * Serializes drone detections to newline-delimited JSON and sends them
 * over UART1 to the Uplink board.  Bayesian fusion is applied inline
 * as detections arrive from the shared queue.
 */

#include "uart_tx.h"

#include "bayesian_fusion.h"
#include "constants.h"
#include "detection_types.h"
#include "uart_protocol.h"
#include "task_priorities.h"
#include "led_status.h"

#include "cJSON.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string.h>
#include <math.h>
#include "esp_timer.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_uart_tx";

#define UART_PORT_NUM       UART_NUM_1
#define TX_BUF_SIZE         UART_BUF_SIZE
#define RX_BUF_SIZE         UART_BUF_SIZE

/* Queue receive timeout -- short enough to allow periodic maintenance. */
#define QUEUE_RX_TIMEOUT_MS 100

/* ── Counters for status messages ───────────────────────────────────────── */

static int s_ble_count  = 0;
static int s_wifi_count = 0;
static uint8_t s_current_channel = 0;
static uint32_t s_seq = 0;

/* UART write mutex — prevents interleaved writes from multiple tasks */
static SemaphoreHandle_t s_uart_mutex = NULL;

/* ── Detection cache for OLED scoreboard ────────────────────────────────── */

static scanner_detection_summary_t s_det_cache[DETECTION_CACHE_SIZE];
static int s_det_cache_count = 0;
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Detection cache helpers ────────────────────────────────────────────── */

/**
 * Find an existing cache entry by drone_id, or allocate a new slot.
 * Must be called inside portENTER_CRITICAL / portEXIT_CRITICAL.
 * Returns a pointer to the slot, or NULL if the cache is full and we
 * need to evict (in which case we evict the oldest entry).
 */
static scanner_detection_summary_t *cache_find_or_alloc(const char *drone_id)
{
    /* Search for existing entry */
    for (int i = 0; i < s_det_cache_count; i++) {
        if (strcmp(s_det_cache[i].drone_id, drone_id) == 0) {
            return &s_det_cache[i];
        }
    }

    /* Append if room */
    if (s_det_cache_count < DETECTION_CACHE_SIZE) {
        return &s_det_cache[s_det_cache_count++];
    }

    /* Evict oldest entry */
    int oldest_idx = 0;
    int64_t oldest_ts = s_det_cache[0].timestamp_ms;
    for (int i = 1; i < s_det_cache_count; i++) {
        if (s_det_cache[i].timestamp_ms < oldest_ts) {
            oldest_ts = s_det_cache[i].timestamp_ms;
            oldest_idx = i;
        }
    }
    return &s_det_cache[oldest_idx];
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

/**
 * Add a number to a cJSON object only if the value is non-zero / meaningful.
 * Keeps the JSON compact by omitting default / empty fields.
 */
static void cjson_add_double_if(cJSON *obj, const char *key, double val)
{
    if (val != 0.0) {
        cJSON_AddNumberToObject(obj, key, val);
    }
}

static void cjson_add_string_if(cJSON *obj, const char *key, const char *val)
{
    if (val && val[0] != '\0') {
        cJSON_AddStringToObject(obj, key, val);
    }
}

/**
 * Transmit a raw null-terminated string over UART, appending the newline
 * delimiter.  The caller is responsible for providing valid JSON.
 */
static void uart_send_line(const char *json_str)
{
    size_t len = strlen(json_str);
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT_NUM, json_str, len);
    uart_write_bytes(UART_PORT_NUM, "\n", 1);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void uart_tx_init(void)
{
    s_uart_mutex = xSemaphoreCreateMutex();

    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM,
                                 SCANNER_UART_TX_PIN,
                                 SCANNER_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM,
                                        RX_BUF_SIZE,
                                        TX_BUF_SIZE,
                                        0, NULL, 0));

    ESP_LOGI(TAG, "UART%d initialised: %d baud, TX=GPIO%d, RX=GPIO%d",
             UART_PORT_NUM, UART_BAUD_RATE,
             SCANNER_UART_TX_PIN, SCANNER_UART_RX_PIN);
}

void uart_tx_send_detection(const drone_detection_t *detection)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "cJSON alloc failed for detection");
        return;
    }

    /* Required fields */
    cJSON_AddStringToObject(root, JSON_KEY_TYPE, MSG_TYPE_DETECTION);
    cJSON_AddStringToObject(root, JSON_KEY_DRONE_ID, detection->drone_id);
    cJSON_AddNumberToObject(root, JSON_KEY_SOURCE, detection->source);
    cJSON_AddNumberToObject(root, JSON_KEY_CONFIDENCE, detection->confidence);
    cJSON_AddNumberToObject(root, JSON_KEY_FUSED_CONFIDENCE, detection->fused_confidence);
    cJSON_AddNumberToObject(root, JSON_KEY_RSSI, detection->rssi);
    cJSON_AddNumberToObject(root, JSON_KEY_TIMESTAMP, (double)detection->last_updated_ms);
    cJSON_AddNumberToObject(root, JSON_KEY_SEQ, s_seq++);

    /* Position -- only include if we have a fix */
    cjson_add_double_if(root, JSON_KEY_LATITUDE,  detection->latitude);
    cjson_add_double_if(root, JSON_KEY_LONGITUDE, detection->longitude);
    cjson_add_double_if(root, JSON_KEY_ALTITUDE,  detection->altitude_m);

    /* Kinematics */
    cjson_add_double_if(root, JSON_KEY_HEADING, detection->heading_deg);
    cjson_add_double_if(root, JSON_KEY_SPEED,   detection->speed_mps);
    cjson_add_double_if(root, JSON_KEY_VSPEED,  detection->vertical_speed_mps);

    /* Distance estimate */
    cjson_add_double_if(root, JSON_KEY_DISTANCE, detection->estimated_distance_m);

    /* Metadata */
    cjson_add_string_if(root, JSON_KEY_MANUFACTURER, detection->manufacturer);
    cjson_add_string_if(root, JSON_KEY_MODEL,        detection->model);

    /* Operator info */
    cjson_add_double_if(root, JSON_KEY_OPERATOR_LAT, detection->operator_lat);
    cjson_add_double_if(root, JSON_KEY_OPERATOR_LON, detection->operator_lon);
    cjson_add_string_if(root, JSON_KEY_OPERATOR_ID,  detection->operator_id);

    /* ASTM / OpenDroneID fields */
    if (detection->ua_type != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_UA_TYPE, detection->ua_type);
    }
    if (detection->id_type != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_ID_TYPE, detection->id_type);
    }
    cjson_add_string_if(root, JSON_KEY_SELF_ID, detection->self_id_text);
    cjson_add_double_if(root, JSON_KEY_HEIGHT_AGL,    detection->height_agl_m);
    cjson_add_double_if(root, JSON_KEY_GEODETIC_ALT,  detection->geodetic_alt_m);
    cjson_add_double_if(root, JSON_KEY_H_ACCURACY,    detection->h_accuracy_m);
    cjson_add_double_if(root, JSON_KEY_V_ACCURACY,    detection->v_accuracy_m);

    /* WiFi-specific */
    cjson_add_string_if(root, JSON_KEY_SSID,  detection->ssid);
    cjson_add_string_if(root, JSON_KEY_BSSID, detection->bssid);
    if (detection->freq_mhz != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_FREQ, detection->freq_mhz);
    }
    if (detection->channel_width_mhz != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_CHANNEL_WIDTH, detection->channel_width_mhz);
    }

    /* Probe request: include probed SSID as a JSON array */
    if (detection->source == DETECTION_SRC_WIFI_PROBE_REQUEST &&
        detection->ssid[0] != '\0') {
        cJSON *probed = cJSON_AddArrayToObject(root, JSON_KEY_PROBED_SSIDS);
        if (probed) {
            cJSON_AddItemToArray(probed, cJSON_CreateString(detection->ssid));
        }
    }

    /* BLE fingerprinting fields */
    if (detection->ble_company_id != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_COMPANY_ID, detection->ble_company_id);
    }
    if (detection->ble_apple_type != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_APPLE_TYPE, detection->ble_apple_type);
    }
    if (detection->ble_ad_type_count != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_AD_TYPES, detection->ble_ad_type_count);
    }
    if (detection->ble_payload_len != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_PAYLOAD_LEN, detection->ble_payload_len);
    }
    if (detection->ble_addr_type != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_ADDR_TYPE, detection->ble_addr_type);
    }
    if (detection->ble_ja3_hash != 0) {
        char ja3_hex[9];
        snprintf(ja3_hex, sizeof(ja3_hex), "%08lx", (unsigned long)detection->ble_ja3_hash);
        cJSON_AddStringToObject(root, JSON_KEY_BLE_JA3, ja3_hex);
    }

    /* Apple Continuity deep fields */
    if (detection->ble_apple_auth[0] || detection->ble_apple_auth[1] || detection->ble_apple_auth[2]) {
        char auth_hex[7];
        snprintf(auth_hex, sizeof(auth_hex), "%02x%02x%02x",
                 detection->ble_apple_auth[0], detection->ble_apple_auth[1], detection->ble_apple_auth[2]);
        cJSON_AddStringToObject(root, JSON_KEY_BLE_APPLE_AUTH, auth_hex);
    }
    if (detection->ble_apple_activity != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_ACTIVITY, detection->ble_apple_activity);
    }
    if (detection->ble_raw_mfr_len > 0) {
        char mfr_hex[41];  /* 20 bytes * 2 + null */
        for (int i = 0; i < detection->ble_raw_mfr_len && i < 20; i++) {
            snprintf(&mfr_hex[i*2], 3, "%02x", detection->ble_raw_mfr[i]);
        }
        mfr_hex[detection->ble_raw_mfr_len * 2] = '\0';
        cJSON_AddStringToObject(root, JSON_KEY_BLE_RAW_MFR, mfr_hex);
    }
    if (detection->ble_adv_interval_us > 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_BLE_ADV_INTERVAL, (double)(detection->ble_adv_interval_us / 1000));  /* ms */
    }

    /* Timestamps */
    if (detection->first_seen_ms != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_FIRST_SEEN, (double)detection->first_seen_ms);
    }
    cJSON_AddNumberToObject(root, JSON_KEY_LAST_UPDATED, (double)detection->last_updated_ms);

    /* Serialize */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        uart_send_line(json_str);
        cJSON_free(json_str);
    } else {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
    }
}

/* Scanner identity — set by uart_tx_send_scanner_info, included in every status */
static const char *s_scanner_ver   = NULL;
static const char *s_scanner_board = NULL;
static const char *s_scanner_chip  = NULL;
static const char *s_scanner_caps  = NULL;

void uart_tx_send_status(int ble_count, int wifi_count,
                         uint8_t current_channel, uint32_t uptime_s)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "cJSON alloc failed for status");
        return;
    }

    cJSON_AddStringToObject(root, JSON_KEY_TYPE, MSG_TYPE_STATUS);
    cJSON_AddNumberToObject(root, "ble_count",  ble_count);
    cJSON_AddNumberToObject(root, "wifi_count", wifi_count);
    cJSON_AddNumberToObject(root, "ch",         current_channel);
    cJSON_AddNumberToObject(root, "uptime_s",   uptime_s);
    cJSON_AddNumberToObject(root, JSON_KEY_SEQ, s_seq++);

    /* Include scanner identity in every status message */
    if (s_scanner_ver) {
        cJSON_AddStringToObject(root, "ver", s_scanner_ver);
        cJSON_AddStringToObject(root, "board", s_scanner_board);
        cJSON_AddStringToObject(root, "chip", s_scanner_chip);
        cJSON_AddStringToObject(root, "caps", s_scanner_caps);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        uart_send_line(json_str);
        cJSON_free(json_str);
    }
}

void uart_tx_send_scanner_info(const char *ver, const char *board,
                                const char *chip, const char *caps)
{
    /* Store for inclusion in every future status message */
    s_scanner_ver   = ver;
    s_scanner_board = board;
    s_scanner_chip  = chip;
    s_scanner_caps  = caps;

    /* Also send as standalone message */
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"scanner_info\",\"ver\":\"%s\",\"board\":\"%s\","
             "\"chip\":\"%s\",\"caps\":\"%s\"}",
             ver ? ver : "?", board ? board : "?",
             chip ? chip : "?", caps ? caps : "?");
    uart_send_line(buf);
    ESP_LOGI(TAG, "Scanner info TX: %s v%s (%s)", board, ver, caps);
}

/* ── UART TX Task ───────────────────────────────────────────────────────── */

/**
 * FreeRTOS task: reads detections from the shared queue, fuses, and sends.
 *
 * Also performs periodic maintenance:
 *  - Prunes stale Bayesian tracks every PRUNE_INTERVAL_MS
 *  - Sends a status heartbeat alongside the prune
 */
static void uart_tx_task(void *arg)
{
    QueueHandle_t detection_queue = (QueueHandle_t)arg;
    drone_detection_t det;

    TickType_t last_prune_tick = xTaskGetTickCount();
    const TickType_t prune_interval = pdMS_TO_TICKS(PRUNE_INTERVAL_MS);
    const TickType_t queue_timeout  = pdMS_TO_TICKS(QUEUE_RX_TIMEOUT_MS);

    ESP_LOGI(TAG, "UART TX task started (prune every %d ms)", PRUNE_INTERVAL_MS);

    for (;;) {
        /* Block on detection queue with a short timeout so we can do
         * periodic maintenance even when no detections are flowing. */
        if (xQueueReceive(detection_queue, &det, queue_timeout) == pdTRUE) {

            /* Track per-source counters for status messages */
            if (det.source == DETECTION_SRC_BLE_RID) {
                s_ble_count++;
            } else {
                s_wifi_count++;
            }

            /* Keep track of WiFi channel if available */
            if (det.freq_mhz > 0) {
                /* 2.4 GHz channel derivation: (freq - 2407) / 5 */
                if (det.freq_mhz >= 2412 && det.freq_mhz <= 2484) {
                    s_current_channel = (uint8_t)((det.freq_mhz - 2407) / 5);
                }
            }

            /* Run Bayesian sensor fusion -- updates internal state and
             * returns the fused probability for this drone_id. */
            int64_t now_ms = esp_timer_get_time() / 1000;
            float fused = bayesian_fusion_update(det.drone_id,
                                                 det.source,
                                                 det.confidence,
                                                 now_ms);
            det.fused_confidence = fused;

            /* Cache for OLED scoreboard */
            portENTER_CRITICAL(&s_cache_lock);
            {
                scanner_detection_summary_t *slot = cache_find_or_alloc(det.drone_id);
                strncpy(slot->drone_id, det.drone_id, sizeof(slot->drone_id) - 1);
                slot->drone_id[sizeof(slot->drone_id) - 1] = '\0';
                strncpy(slot->manufacturer, det.manufacturer, sizeof(slot->manufacturer) - 1);
                slot->manufacturer[sizeof(slot->manufacturer) - 1] = '\0';
                slot->confidence = det.fused_confidence;
                slot->rssi = det.rssi;
                slot->timestamp_ms = det.last_updated_ms;
            }
            portEXIT_CRITICAL(&s_cache_lock);

            /* Serialize and transmit */
            uart_tx_send_detection(&det);
            led_set_pattern(LED_DETECTION);

            ESP_LOGD(TAG, "TX: %s src=%d raw=%.2f fused=%.2f rssi=%d",
                     det.drone_id, det.source,
                     det.confidence, det.fused_confidence, det.rssi);
        }

        /* Periodic maintenance */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_prune_tick) >= prune_interval) {
            last_prune_tick = now;

            int64_t prune_now_ms = esp_timer_get_time() / 1000;
            bayesian_fusion_prune(prune_now_ms);

            /* Prune stale entries from display cache */
            portENTER_CRITICAL(&s_cache_lock);
            {
                int dst = 0;
                for (int src = 0; src < s_det_cache_count; src++) {
                    int64_t age_ms = prune_now_ms - s_det_cache[src].timestamp_ms;
                    if (age_ms <= DETECTION_STALE_MS) {
                        if (dst != src) {
                            s_det_cache[dst] = s_det_cache[src];
                        }
                        dst++;
                    }
                }
                s_det_cache_count = dst;
            }
            portEXIT_CRITICAL(&s_cache_lock);

            /* Compute uptime in seconds */
            uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() /
                                           configTICK_RATE_HZ);

            uart_tx_send_status(s_ble_count, s_wifi_count,
                                s_current_channel, uptime_s);
            led_set_pattern(LED_SCANNING);

            ESP_LOGI(TAG, "Status TX: ble=%d wifi=%d ch=%d uptime=%lus",
                     s_ble_count, s_wifi_count,
                     s_current_channel, (unsigned long)uptime_s);
        }
    }
    /* Task should never return; if it does, clean up. */
    vTaskDelete(NULL);
}

void uart_tx_start(QueueHandle_t detection_queue)
{
    xTaskCreatePinnedToCore(
        uart_tx_task,
        "uart_tx",
        UART_TX_TASK_STACK_SIZE,
        (void *)detection_queue,
        UART_TX_TASK_PRIORITY,
        NULL,
        UART_TX_TASK_CORE
    );

    ESP_LOGI(TAG, "UART TX task created on core %d, priority %d",
             UART_TX_TASK_CORE, UART_TX_TASK_PRIORITY);
}

/* ── Display getters ───────────────────────────────────────────────────── */

int uart_tx_get_ble_count(void)
{
    return s_ble_count;
}

int uart_tx_get_wifi_count(void)
{
    return s_wifi_count;
}

int uart_tx_get_total_count(void)
{
    return s_ble_count + s_wifi_count;
}

uint8_t uart_tx_get_current_channel(void)
{
    return s_current_channel;
}

bool uart_tx_get_recent_detection(scanner_detection_summary_t *out)
{
    if (!out) {
        return false;
    }

    portENTER_CRITICAL(&s_cache_lock);
    if (s_det_cache_count == 0) {
        portEXIT_CRITICAL(&s_cache_lock);
        return false;
    }

    /* Find the entry with the highest timestamp */
    int best = 0;
    for (int i = 1; i < s_det_cache_count; i++) {
        if (s_det_cache[i].timestamp_ms > s_det_cache[best].timestamp_ms) {
            best = i;
        }
    }
    *out = s_det_cache[best];
    portEXIT_CRITICAL(&s_cache_lock);
    return true;
}

int uart_tx_get_cached_detections(scanner_detection_summary_t *out, int max_count)
{
    if (!out || max_count <= 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_cache_lock);
    int count = s_det_cache_count < max_count ? s_det_cache_count : max_count;
    memcpy(out, s_det_cache, count * sizeof(scanner_detection_summary_t));
    portEXIT_CRITICAL(&s_cache_lock);

    /* Insertion sort by timestamp descending (most recent first, max 10 items) */
    for (int i = 1; i < count; i++) {
        scanner_detection_summary_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].timestamp_ms < tmp.timestamp_ms) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }

    return count;
}
