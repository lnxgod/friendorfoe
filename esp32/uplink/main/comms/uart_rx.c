/**
 * Friend or Foe -- Uplink UART RX Implementation
 *
 * Receives newline-delimited JSON from the Scanner board over UART1,
 * parses detection and status messages, and enqueues drone_detection_t
 * structs for the HTTP upload task.
 */

#include "uart_rx.h"
#include "uart_protocol.h"
#include "config.h"

#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart_rx";

#define UART_NUM            UART_NUM_1
#define LINE_BUF_SIZE       1024
#define READ_BUF_SIZE       256

static QueueHandle_t s_detection_queue = NULL;
static int           s_detection_count = 0;

/* ── Source string to DETECTION_SRC mapping ────────────────────────────── */

static uint8_t parse_source(int src_int)
{
    switch (src_int) {
        case DETECTION_SRC_BLE_RID:     return DETECTION_SRC_BLE_RID;
        case DETECTION_SRC_WIFI_SSID:   return DETECTION_SRC_WIFI_SSID;
        case DETECTION_SRC_WIFI_DJI_IE: return DETECTION_SRC_WIFI_DJI_IE;
        case DETECTION_SRC_WIFI_BEACON: return DETECTION_SRC_WIFI_BEACON;
        case DETECTION_SRC_WIFI_OUI:    return DETECTION_SRC_WIFI_OUI;
        default:                        return DETECTION_SRC_BLE_RID;
    }
}

/* ── JSON helpers ──────────────────────────────────────────────────────── */

static double json_get_double(const cJSON *obj, const char *key, double def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return def;
}

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return def;
}

static const char *json_get_string(const cJSON *obj, const char *key,
                                   const char *def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return def;
}

/* ── Parse a detection message ─────────────────────────────────────────── */

static bool parse_detection(const cJSON *root, drone_detection_t *det)
{
    memset(det, 0, sizeof(*det));

    /* Required field: drone_id */
    const char *drone_id = json_get_string(root, JSON_KEY_DRONE_ID, NULL);
    if (!drone_id) {
        ESP_LOGW(TAG, "Detection missing drone_id");
        return false;
    }
    strncpy(det->drone_id, drone_id, sizeof(det->drone_id) - 1);

    /* Source */
    det->source = parse_source(json_get_int(root, JSON_KEY_SOURCE, 0));

    /* Confidence */
    det->confidence = (float)json_get_double(root, JSON_KEY_CONFIDENCE, 0.0);

    /* Position */
    det->latitude    = json_get_double(root, JSON_KEY_LATITUDE, 0.0);
    det->longitude   = json_get_double(root, JSON_KEY_LONGITUDE, 0.0);
    det->altitude_m  = json_get_double(root, JSON_KEY_ALTITUDE, 0.0);

    /* RF */
    det->rssi = (int8_t)json_get_int(root, JSON_KEY_RSSI, 0);

    /* Kinematics */
    det->speed_mps   = (float)json_get_double(root, JSON_KEY_SPEED, 0.0);
    det->heading_deg  = (float)json_get_double(root, JSON_KEY_HEADING, 0.0);
    det->vertical_speed_mps = (float)json_get_double(root, JSON_KEY_VSPEED, 0.0);

    /* Distance estimate */
    det->estimated_distance_m = json_get_double(root, JSON_KEY_DISTANCE, 0.0);

    /* Metadata */
    const char *mfr = json_get_string(root, JSON_KEY_MANUFACTURER, "");
    strncpy(det->manufacturer, mfr, sizeof(det->manufacturer) - 1);

    const char *model = json_get_string(root, JSON_KEY_MODEL, "");
    strncpy(det->model, model, sizeof(det->model) - 1);

    /* Operator info */
    det->operator_lat = json_get_double(root, JSON_KEY_OPERATOR_LAT, 0.0);
    det->operator_lon = json_get_double(root, JSON_KEY_OPERATOR_LON, 0.0);
    const char *op_id = json_get_string(root, JSON_KEY_OPERATOR_ID, "");
    strncpy(det->operator_id, op_id, sizeof(det->operator_id) - 1);

    /* ASTM fields */
    det->ua_type       = (uint8_t)json_get_int(root, JSON_KEY_UA_TYPE, 0);
    det->id_type       = (uint8_t)json_get_int(root, JSON_KEY_ID_TYPE, 0);
    const char *self_id = json_get_string(root, JSON_KEY_SELF_ID, "");
    strncpy(det->self_id_text, self_id, sizeof(det->self_id_text) - 1);
    det->height_agl_m   = json_get_double(root, JSON_KEY_HEIGHT_AGL, 0.0);
    det->geodetic_alt_m = json_get_double(root, JSON_KEY_GEODETIC_ALT, 0.0);
    det->h_accuracy_m   = (float)json_get_double(root, JSON_KEY_H_ACCURACY, 0.0);
    det->v_accuracy_m   = (float)json_get_double(root, JSON_KEY_V_ACCURACY, 0.0);

    /* WiFi-specific */
    const char *ssid = json_get_string(root, JSON_KEY_SSID, "");
    strncpy(det->ssid, ssid, sizeof(det->ssid) - 1);
    const char *bssid = json_get_string(root, JSON_KEY_BSSID, "");
    strncpy(det->bssid, bssid, sizeof(det->bssid) - 1);
    det->freq_mhz          = json_get_int(root, JSON_KEY_FREQ, 0);
    det->channel_width_mhz = json_get_int(root, JSON_KEY_CHANNEL_WIDTH, 0);

    /* Timestamps */
    det->first_seen_ms    = (int64_t)json_get_double(root, JSON_KEY_FIRST_SEEN, 0.0);
    det->last_updated_ms  = (int64_t)json_get_double(root, JSON_KEY_LAST_UPDATED, 0.0);

    /* Fused confidence */
    det->fused_confidence = (float)json_get_double(root, JSON_KEY_FUSED_CONFIDENCE, 0.0);

    /* Timestamp fallback */
    if (det->last_updated_ms == 0) {
        det->last_updated_ms = (int64_t)json_get_double(root, JSON_KEY_TIMESTAMP, 0.0);
    }

    return true;
}

/* ── Handle a parsed status message ────────────────────────────────────── */

static void handle_status(const cJSON *root)
{
    int ble_count  = json_get_int(root, "ble_count", 0);
    int wifi_count = json_get_int(root, "wifi_count", 0);
    int channel    = json_get_int(root, "ch", 0);
    int uptime     = json_get_int(root, "uptime_s", 0);

    ESP_LOGI(TAG, "Scanner status: BLE=%d WiFi=%d ch=%d uptime=%ds",
             ble_count, wifi_count, channel, uptime);
}

/* ── Process one complete JSON line ────────────────────────────────────── */

static void process_line(const char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse error: %.64s...", line);
        return;
    }

    const char *msg_type = json_get_string(root, JSON_KEY_TYPE, NULL);
    if (!msg_type) {
        ESP_LOGW(TAG, "Message missing 'type' field");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(msg_type, MSG_TYPE_DETECTION) == 0) {
        drone_detection_t det;
        if (parse_detection(root, &det)) {
            if (xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_detection_count++;
                ESP_LOGD(TAG, "Enqueued detection: %s (src=%d conf=%.2f)",
                         det.drone_id, det.source, det.confidence);
            } else {
                ESP_LOGW(TAG, "Detection queue full, dropping: %s", det.drone_id);
            }
        }
    } else if (strcmp(msg_type, MSG_TYPE_STATUS) == 0) {
        handle_status(root);
    } else {
        ESP_LOGD(TAG, "Ignoring message type: %s", msg_type);
    }

    cJSON_Delete(root);
}

/* ── UART RX task ──────────────────────────────────────────────────────── */

static void uart_rx_task(void *arg)
{
    char line_buf[LINE_BUF_SIZE];
    int  line_pos = 0;
    uint8_t read_buf[READ_BUF_SIZE];

    ESP_LOGI(TAG, "UART RX task started");

    while (1) {
        int bytes_read = uart_read_bytes(UART_NUM, read_buf, sizeof(read_buf),
                                         pdMS_TO_TICKS(100));
        if (bytes_read <= 0) {
            continue;
        }

        for (int i = 0; i < bytes_read; i++) {
            char c = (char)read_buf[i];

            if (c == UART_MSG_DELIMITER) {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    process_line(line_buf, line_pos);
                    line_pos = 0;
                }
            } else {
                if (line_pos < LINE_BUF_SIZE - 1) {
                    line_buf[line_pos++] = c;
                } else {
                    /* Line too long -- discard and reset */
                    ESP_LOGW(TAG, "Line buffer overflow, discarding");
                    line_pos = 0;
                }
            }
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void uart_rx_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;

    const uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UPLINK_UART_TX_PIN,
                                 UPLINK_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE * 2, 0, NULL, 0));

    ESP_LOGI(TAG, "UART%d initialized: %d baud, RX=GPIO%d, TX=GPIO%d",
             UART_NUM, UART_BAUD_RATE, UPLINK_UART_RX_PIN, UPLINK_UART_TX_PIN);
}

void uart_rx_start(void)
{
    xTaskCreate(uart_rx_task, "uart_rx", CONFIG_UART_RX_STACK,
                NULL, CONFIG_UART_RX_PRIORITY, NULL);
    ESP_LOGI(TAG, "UART RX task created (priority=%d, stack=%d)",
             CONFIG_UART_RX_PRIORITY, CONFIG_UART_RX_STACK);
}

int uart_rx_get_detection_count(void)
{
    return s_detection_count;
}
