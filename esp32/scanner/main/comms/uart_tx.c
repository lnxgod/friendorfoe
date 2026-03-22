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
#include "freertos/task.h"
#include "freertos/queue.h"

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
    uart_write_bytes(UART_PORT_NUM, json_str, len);
    uart_write_bytes(UART_PORT_NUM, "\n", 1);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void uart_tx_init(void)
{
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

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        uart_send_line(json_str);
        cJSON_free(json_str);
    }
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

            /* Compute uptime in seconds */
            uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() /
                                           configTICK_RATE_HZ);

            uart_tx_send_status(s_ble_count, s_wifi_count,
                                s_current_channel, uptime_s);
            led_set_pattern(LED_SCANNING);

            ESP_LOGD(TAG, "Status: ble=%d wifi=%d ch=%d uptime=%lus",
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
