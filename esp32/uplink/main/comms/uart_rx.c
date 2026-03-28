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
#include "led_status.h"

#include <string.h>
#include <stdatomic.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

static const char *TAG = "uart_rx";

#define LINE_BUF_SIZE       1024
#define READ_BUF_SIZE       256

/* Scanner connection tracking */
#define SCANNER_TIMEOUT_MS  5000
static atomic_int_fast64_t s_last_rx_time_ble = 0;
static atomic_int_fast64_t s_last_rx_time_wifi = 0;
static bool s_first_status_received = false;

static QueueHandle_t s_detection_queue = NULL;
static int           s_detection_count = 0;

/* ── Recent detections ring buffer ─────────────────────────────────────── */

#define RECENT_RING_SIZE  8

static detection_summary_t s_recent_ring[RECENT_RING_SIZE];
static int                 s_recent_head = 0;   /* next write index */
static int                 s_recent_count = 0;
static portMUX_TYPE        s_recent_lock = portMUX_INITIALIZER_UNLOCKED;

static void push_recent(const drone_detection_t *det)
{
    portENTER_CRITICAL(&s_recent_lock);
    detection_summary_t *slot = &s_recent_ring[s_recent_head];
    strncpy(slot->drone_id, det->drone_id, sizeof(slot->drone_id) - 1);
    slot->drone_id[sizeof(slot->drone_id) - 1] = '\0';
    slot->source       = det->source;
    slot->confidence   = det->confidence;
    slot->rssi         = det->rssi;
    slot->timestamp_ms = esp_timer_get_time() / 1000;
    s_recent_head = (s_recent_head + 1) % RECENT_RING_SIZE;
    if (s_recent_count < RECENT_RING_SIZE) {
        s_recent_count++;
    }
    portEXIT_CRITICAL(&s_recent_lock);
}

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

    /* First status message from scanner: flash "connected!" */
    if (!s_first_status_received) {
        s_first_status_received = true;
        led_set_pattern(LED_DETECTION);
        ESP_LOGI(TAG, "Scanner connected (first status received)");
    }
}

/* ── Process one complete JSON line ────────────────────────────────────── */

static void process_line(const char *line, size_t len, int scanner_id)
{
    /* Update last-received timestamp for the correct scanner */
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);
    if (scanner_id == 0) {
        atomic_store(&s_last_rx_time_ble, now_ms);
    } else {
        atomic_store(&s_last_rx_time_wifi, now_ms);
    }

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
            push_recent(&det);
        }
    } else if (strcmp(msg_type, MSG_TYPE_STATUS) == 0) {
        handle_status(root);
    } else {
        ESP_LOGD(TAG, "Ignoring message type: %s", msg_type);
    }

    cJSON_Delete(root);
}

/* ── UART RX task (parameterized for dual-scanner support) ─────────────── */

typedef struct {
    int uart_num;
    int scanner_id;     /* 0=BLE scanner, 1=WiFi scanner */
    const char *label;
} uart_rx_task_params_t;

static void uart_rx_task(void *arg)
{
    uart_rx_task_params_t *params = (uart_rx_task_params_t *)arg;
    int uart_num = params->uart_num;
    int scanner_id = params->scanner_id;

    char line_buf[LINE_BUF_SIZE];
    int  line_pos = 0;
    uint8_t read_buf[READ_BUF_SIZE];

    ESP_LOGI(TAG, "UART RX task started: %s (UART%d)", params->label, uart_num);

    while (1) {
        int bytes_read = uart_read_bytes(uart_num, read_buf, sizeof(read_buf),
                                         pdMS_TO_TICKS(100));
        if (bytes_read <= 0) {
            continue;
        }

        for (int i = 0; i < bytes_read; i++) {
            char c = (char)read_buf[i];

            if (c == UART_MSG_DELIMITER) {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    process_line(line_buf, line_pos, scanner_id);
                    line_pos = 0;
                }
            } else {
                if (line_pos < LINE_BUF_SIZE - 1) {
                    line_buf[line_pos++] = c;
                } else {
                    ESP_LOGW(TAG, "[%s] Line buffer overflow, discarding", params->label);
                    line_pos = 0;
                }
            }
        }
    }
}

/* Static params (must outlive the tasks) */
static uart_rx_task_params_t s_ble_task_params = { .uart_num = 0, .scanner_id = 0, .label = "BLE" };
#if CONFIG_DUAL_SCANNER
static uart_rx_task_params_t s_wifi_task_params = { .uart_num = 0, .scanner_id = 1, .label = "WiFi" };
#endif

/* ── Public API ────────────────────────────────────────────────────────── */

static void init_uart_port(int uart_num, int rx_pin, int tx_pin, const char *label)
{
    const uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(uart_num, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE * 2, 0, NULL, 0));

    ESP_LOGI(TAG, "%s scanner UART%d: %d baud, RX=GPIO%d, TX=GPIO%d",
             label, uart_num, UART_BAUD_RATE, rx_pin, tx_pin);
}

void uart_rx_init(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;

    /* BLE scanner on UART1 (always) */
    s_ble_task_params.uart_num = CONFIG_BLE_SCANNER_UART;
    init_uart_port(CONFIG_BLE_SCANNER_UART,
                   CONFIG_BLE_SCANNER_RX_PIN, CONFIG_BLE_SCANNER_TX_PIN, "BLE");

#if CONFIG_DUAL_SCANNER
    /* WiFi scanner on UART2 (plain ESP32 only) */
    s_wifi_task_params.uart_num = CONFIG_WIFI_SCANNER_UART;
    init_uart_port(CONFIG_WIFI_SCANNER_UART,
                   CONFIG_WIFI_SCANNER_RX_PIN, CONFIG_WIFI_SCANNER_TX_PIN, "WiFi");
#endif
}

void uart_rx_start(void)
{
    xTaskCreate(uart_rx_task, "uart_rx_ble", CONFIG_UART_RX_STACK,
                &s_ble_task_params, CONFIG_UART_RX_PRIORITY, NULL);
    ESP_LOGI(TAG, "BLE scanner RX task created");

#if CONFIG_DUAL_SCANNER
    xTaskCreate(uart_rx_task, "uart_rx_wifi", CONFIG_UART_RX_STACK,
                &s_wifi_task_params, CONFIG_UART_RX_PRIORITY, NULL);
    ESP_LOGI(TAG, "WiFi scanner RX task created");
#endif
}

int uart_rx_get_detection_count(void)
{
    return s_detection_count;
}

int uart_rx_get_recent_detections(detection_summary_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }

    int copied = 0;
    portENTER_CRITICAL(&s_recent_lock);
    int count = s_recent_count < max ? s_recent_count : max;
    /* Copy newest first */
    for (int i = 0; i < count; i++) {
        int idx = (s_recent_head - 1 - i + RECENT_RING_SIZE) % RECENT_RING_SIZE;
        out[copied++] = s_recent_ring[idx];
    }
    portEXIT_CRITICAL(&s_recent_lock);
    return copied;
}

static bool _check_connected(atomic_int_fast64_t *ts)
{
    int_fast64_t last = atomic_load(ts);
    if (last == 0) return false;
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);
    return (now_ms - last) < SCANNER_TIMEOUT_MS;
}

bool uart_rx_is_scanner_connected(void)
{
    return _check_connected(&s_last_rx_time_ble)
#if CONFIG_DUAL_SCANNER
        || _check_connected(&s_last_rx_time_wifi)
#endif
        ;
}

bool uart_rx_is_ble_scanner_connected(void)
{
    return _check_connected(&s_last_rx_time_ble);
}

bool uart_rx_is_wifi_scanner_connected(void)
{
#if CONFIG_DUAL_SCANNER
    return _check_connected(&s_last_rx_time_wifi);
#else
    return false;
#endif
}
