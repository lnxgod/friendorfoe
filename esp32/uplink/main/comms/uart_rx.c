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
#define SCANNER_TIMEOUT_MS  15000
static atomic_int_fast64_t s_last_rx_time_ble = 0;
static atomic_int_fast64_t s_last_rx_time_wifi = 0;
static bool s_first_status_received = false;

static QueueHandle_t s_detection_queue = NULL;
static int           s_detection_count = 0;

/* Scanner identity (populated from scanner_info UART messages) */
static scanner_info_t s_ble_scanner_info = {0};
static scanner_info_t s_wifi_scanner_info = {0};

/* OTA response tracking (set by UART RX, read by relay handler) */
static volatile ota_response_t s_last_ota_response = {0};
static portMUX_TYPE s_ota_response_lock = portMUX_INITIALIZER_UNLOCKED;

/* Pause flags for OTA relay — lets relay handler read UART directly */
static volatile bool s_rx_paused_ble = false;
static volatile bool s_rx_paused_wifi = false;

/* Backpressure flag — prevents stop/start oscillation between RX tasks */
static _Atomic bool s_backpressure_active = false;

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
        case DETECTION_SRC_BLE_RID:            return DETECTION_SRC_BLE_RID;
        case DETECTION_SRC_WIFI_SSID:          return DETECTION_SRC_WIFI_SSID;
        case DETECTION_SRC_WIFI_DJI_IE:        return DETECTION_SRC_WIFI_DJI_IE;
        case DETECTION_SRC_WIFI_BEACON:        return DETECTION_SRC_WIFI_BEACON;
        case DETECTION_SRC_WIFI_OUI:           return DETECTION_SRC_WIFI_OUI;
        case DETECTION_SRC_WIFI_PROBE_REQUEST: return DETECTION_SRC_WIFI_PROBE_REQUEST;
        default:                               return DETECTION_SRC_BLE_RID;
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
    det->wifi_auth_mode    = (uint8_t)json_get_int(root, JSON_KEY_WIFI_AUTH_MODE, 0xFF);

    /* Probe request: extract probed SSIDs and fingerprint */
    const char *probed_str = json_get_string(root, JSON_KEY_PROBED_SSIDS, NULL);
    if (probed_str) {
        strncpy(det->probed_ssids, probed_str, sizeof(det->probed_ssids) - 1);
    }
    const char *ie_hash_str = json_get_string(root, "ie_hash", NULL);
    if (ie_hash_str) {
        det->probe_ie_hash = (uint32_t)strtoul(ie_hash_str, NULL, 16);
    }
    det->wifi_generation = (uint8_t)json_get_int(root, "wifi_gen", 0);

    /* Timestamps */
    det->first_seen_ms    = (int64_t)json_get_double(root, JSON_KEY_FIRST_SEEN, 0.0);
    det->last_updated_ms  = (int64_t)json_get_double(root, JSON_KEY_LAST_UPDATED, 0.0);

    /* Fused confidence */
    det->fused_confidence = (float)json_get_double(root, JSON_KEY_FUSED_CONFIDENCE, 0.0);

    /* BLE fingerprinting fields */
    det->ble_company_id = (uint16_t)json_get_double(root, JSON_KEY_BLE_COMPANY_ID, 0);
    det->ble_apple_type = (uint8_t)json_get_double(root, JSON_KEY_BLE_APPLE_TYPE, 0);
    det->ble_ad_type_count = (uint8_t)json_get_double(root, JSON_KEY_BLE_AD_TYPES, 0);
    det->ble_payload_len = (uint8_t)json_get_double(root, JSON_KEY_BLE_PAYLOAD_LEN, 0);
    det->ble_addr_type = (uint8_t)json_get_double(root, JSON_KEY_BLE_ADDR_TYPE, 0);

    /* BLE-JA3 structural profile hash */
    const char *ja3_str = json_get_string(root, JSON_KEY_BLE_JA3, NULL);
    if (ja3_str) {
        det->ble_ja3_hash = (uint32_t)strtoul(ja3_str, NULL, 16);
    }

    /* Apple Continuity deep fields (previously dropped — fixes entity resolution) */
    const char *auth_str = json_get_string(root, JSON_KEY_BLE_APPLE_AUTH, NULL);
    if (auth_str && strlen(auth_str) == 6) {
        for (int i = 0; i < 3; i++) {
            char hex2[3] = { auth_str[i*2], auth_str[i*2+1], '\0' };
            det->ble_apple_auth[i] = (uint8_t)strtoul(hex2, NULL, 16);
        }
    }
    det->ble_apple_activity = (uint8_t)json_get_int(root, JSON_KEY_BLE_ACTIVITY, 0);
    det->ble_apple_flags = (uint8_t)json_get_int(root, JSON_KEY_BLE_APPLE_FLAGS, 0);

    /* Raw manufacturer data (hex string → byte array) */
    const char *mfr_hex = json_get_string(root, JSON_KEY_BLE_RAW_MFR, NULL);
    if (mfr_hex) {
        int hex_len = strlen(mfr_hex);
        int byte_count = hex_len / 2;
        if (byte_count > 20) byte_count = 20;
        for (int i = 0; i < byte_count; i++) {
            char hex2[3] = { mfr_hex[i*2], mfr_hex[i*2+1], '\0' };
            det->ble_raw_mfr[i] = (uint8_t)strtoul(hex2, NULL, 16);
        }
        det->ble_raw_mfr_len = (uint8_t)byte_count;
    }

    /* Advertisement interval */
    double ival_ms = json_get_double(root, JSON_KEY_BLE_ADV_INTERVAL, 0.0);
    if (ival_ms > 0) {
        det->ble_adv_interval_us = (int64_t)(ival_ms * 1000);
    }

    /* BLE Service UUIDs (comma-separated hex → uint16 array) */
    const char *svc_str = json_get_string(root, JSON_KEY_BLE_SVC_UUIDS, NULL);
    if (svc_str) {
        char svc_buf[36];
        strncpy(svc_buf, svc_str, sizeof(svc_buf) - 1);
        svc_buf[sizeof(svc_buf) - 1] = '\0';
        char *tok = svc_buf;
        while (*tok && det->ble_svc_uuid_count < 4) {
            det->ble_service_uuids[det->ble_svc_uuid_count++] =
                (uint16_t)strtoul(tok, NULL, 16);
            char *comma = strchr(tok, ',');
            if (comma) { tok = comma + 1; } else { break; }
        }
    }

    /* Timestamp fallback */
    if (det->last_updated_ms == 0) {
        det->last_updated_ms = (int64_t)json_get_double(root, JSON_KEY_TIMESTAMP, 0.0);
    }

    return true;
}

/* ── Handle a parsed status message ────────────────────────────────────── */

static void handle_status(const cJSON *root, int scanner_id)
{
    int ble_count  = json_get_int(root, "ble_count", 0);
    int wifi_count = json_get_int(root, "wifi_count", 0);
    int channel    = json_get_int(root, "ch", 0);
    int uptime     = json_get_int(root, "uptime_s", 0);

    ESP_LOGI(TAG, "Scanner[%d] status: BLE=%d WiFi=%d ch=%d uptime=%ds",
             scanner_id, ble_count, wifi_count, channel, uptime);

    /* Extract scanner identity if present (piggybacks on status message) */
    const char *ver = json_get_string(root, "ver", NULL);
    if (ver) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *board = json_get_string(root, "board", "?");
        const char *chip = json_get_string(root, "chip", "?");
        const char *caps = json_get_string(root, "caps", "?");
        strncpy(info->version, ver, sizeof(info->version) - 1);
        strncpy(info->board, board, sizeof(info->board) - 1);
        strncpy(info->chip, chip, sizeof(info->chip) - 1);
        strncpy(info->caps, caps, sizeof(info->caps) - 1);
        if (!info->received) {
            info->received = true;
            ESP_LOGI(TAG, "Scanner[%d] identity from status: %s v%s (%s)",
                     scanner_id, board, ver, caps);
        }
    }

    /* Attack / anomaly counters */
    {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        info->deauth_count  = (uint16_t)json_get_int(root, "deauth", 0);
        info->disassoc_count = (uint16_t)json_get_int(root, "disassoc", 0);
        info->auth_count    = (uint16_t)json_get_int(root, "auth_fr", 0);

        const cJSON *flood_item = cJSON_GetObjectItemCaseSensitive(root, "flood");
        info->deauth_flood = (flood_item && cJSON_IsTrue(flood_item));

        const cJSON *spam_item = cJSON_GetObjectItemCaseSensitive(root, "bcn_spam");
        info->beacon_spam = (spam_item && cJSON_IsTrue(spam_item));

        const char *hist = json_get_string(root, "fc_hist", NULL);
        if (hist) {
            strncpy(info->fc_hist, hist, sizeof(info->fc_hist) - 1);
            info->fc_hist[sizeof(info->fc_hist) - 1] = '\0';
        }

        if (info->deauth_flood) {
            ESP_LOGW(TAG, "Scanner[%d] DEAUTH FLOOD detected! deauth=%d",
                     scanner_id, info->deauth_count);
        }
        if (info->beacon_spam) {
            ESP_LOGW(TAG, "Scanner[%d] BEACON SPAM detected!", scanner_id);
        }
    }

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
            /* Skip BLE background noise (0.02 confidence) to reduce queue pressure.
             * WiFi APs (0.05) and phones (0.05) are still useful for the backend. */
            if (det.confidence < 0.04f) {
                push_recent(&det);  /* Still show in recent list */
                return;
            }

            /* Backpressure: if queue is nearly full, tell scanners to pause.
             * Resume when queue drops below 40%. Uses module-level atomics
             * to avoid data races between BLE and WiFi RX tasks. */
            UBaseType_t queue_count = uxQueueMessagesWaiting(s_detection_queue);
            if (queue_count >= (CONFIG_DETECTION_QUEUE_SIZE * 8 / 10)) {
                if (!s_backpressure_active) {
                    const char *stop_cmd = "{\"type\":\"stop\"}\n";
                    uart_write_bytes(CONFIG_BLE_SCANNER_UART, stop_cmd, strlen(stop_cmd));
#if CONFIG_DUAL_SCANNER
                    uart_write_bytes(CONFIG_WIFI_SCANNER_UART, stop_cmd, strlen(stop_cmd));
#endif
                    ESP_LOGW(TAG, "Queue pressure %d/%d — throttling scanners",
                             (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE);
                    s_backpressure_active = true;
                }
            } else if (s_backpressure_active && queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)) {
                const char *start_cmd = "{\"type\":\"start\"}\n";
                uart_write_bytes(CONFIG_BLE_SCANNER_UART, start_cmd, strlen(start_cmd));
#if CONFIG_DUAL_SCANNER
                uart_write_bytes(CONFIG_WIFI_SCANNER_UART, start_cmd, strlen(start_cmd));
#endif
                ESP_LOGI(TAG, "Queue drained %d/%d — resuming scanners",
                         (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE);
                s_backpressure_active = false;
            }

            if (xQueueSend(s_detection_queue, &det, pdMS_TO_TICKS(10)) == pdTRUE) {
                s_detection_count++;
            } else {
                ESP_LOGW(TAG, "Detection queue full, dropping: %s", det.drone_id);
            }
            push_recent(&det);
        }
    } else if (strcmp(msg_type, MSG_TYPE_STATUS) == 0) {
        handle_status(root, scanner_id);
    } else if (strcmp(msg_type, "scanner_info") == 0) {
        /* Scanner identity: version, board type, chip, capabilities, time-offset */
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *ver = json_get_string(root, "ver", "?");
        const char *board = json_get_string(root, "board", "?");
        const char *chip = json_get_string(root, "chip", "?");
        const char *caps = json_get_string(root, "caps", "?");
        strncpy(info->version, ver, sizeof(info->version) - 1);
        strncpy(info->board, board, sizeof(info->board) - 1);
        strncpy(info->chip, chip, sizeof(info->chip) - 1);
        strncpy(info->caps, caps, sizeof(info->caps) - 1);
        info->toff_ms = (int64_t)json_get_double(root, "toff", 0.0);
        info->tcnt    = (uint32_t)json_get_int(root, "tcnt", 0);
        info->received = true;
        ESP_LOGI(TAG, "Scanner[%d] identity: %s v%s (%s) chip=%s toff=%lld tcnt=%u",
                 scanner_id, board, ver, caps, chip,
                 (long long)info->toff_ms, info->tcnt);
    } else if (strncmp(msg_type, "ota_", 4) == 0) {
        /* OTA response from scanner — capture for relay diagnostics */
        portENTER_CRITICAL(&s_ota_response_lock);
        strncpy((char *)s_last_ota_response.type, msg_type, sizeof(s_last_ota_response.type) - 1);
        const char *err = json_get_string(root, "error", "");
        strncpy((char *)s_last_ota_response.error, err, sizeof(s_last_ota_response.error) - 1);
        cJSON *rcv = cJSON_GetObjectItem(root, "received");
        s_last_ota_response.received = rcv ? (uint32_t)rcv->valuedouble : 0;
        cJSON *seq_j = cJSON_GetObjectItem(root, "seq");
        s_last_ota_response.seq = seq_j ? seq_j->valueint : -1;
        s_last_ota_response.timestamp = esp_timer_get_time() / 1000;
        portEXIT_CRITICAL(&s_ota_response_lock);
    } else {
        ESP_LOGW(TAG, "Scanner[%d] msg type='%s' (unhandled)", scanner_id, msg_type);
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

    int debug_dumps = 3;  /* dump first 3 reads for diagnostics */
    int64_t last_heartbeat_ms = esp_timer_get_time() / 1000;
    int total_bytes = 0;

    while (1) {
        /* Periodic heartbeat so we know the task is alive */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_heartbeat_ms > 5000) {
            ESP_LOGI(TAG, "[%s] heartbeat: %d total bytes received", params->label, total_bytes);
            last_heartbeat_ms = now_ms;
        }
        /* During OTA relay, pause reading so relay handler can read ACKs directly */
        volatile bool *paused = (scanner_id == 0) ? &s_rx_paused_ble : &s_rx_paused_wifi;
        if (*paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int bytes_read = uart_read_bytes(uart_num, read_buf, sizeof(read_buf),
                                         pdMS_TO_TICKS(100));
        if (bytes_read <= 0) {
            continue;
        }
        total_bytes += bytes_read;

        if (debug_dumps > 0) {
            debug_dumps--;
            char hex[128];
            int hlen = 0;
            for (int j = 0; j < bytes_read && hlen < 120; j++) {
                hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", read_buf[j]);
            }
            ESP_LOGI(TAG, "[%s] RX %d bytes: %s", params->label, bytes_read, hex);
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
                    /* Dump first 20 bytes for diagnostics */
                    char hex[64];
                    int hlen = 0;
                    for (int j = 0; j < 20 && j < LINE_BUF_SIZE; j++) {
                        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", (uint8_t)line_buf[j]);
                    }
                    ESP_LOGW(TAG, "[%s] Line buffer overflow (%d bytes, no newline). First 20: %s",
                             params->label, line_pos, hex);
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
    /* WiFi scanner on UART2 */
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

/* Route a command to only one UART by scanning the JSON for markers.
 *
 * Scanner delegation rule (default operating mode):
 *   - BLE-specific commands (ble_lockon / ble_lockon_cancel) → BLE UART only.
 *     While the BLE scanner focuses on an RID drone, the WiFi scanner keeps
 *     doing wide passive scanning for other detections.
 *   - WiFi-specific commands (lockon / lockon_cancel, which carry a "ch"
 *     field) → WiFi UART only. Symmetric.
 *   - Everything else (start/stop/ready/ota_*) still broadcasts to both.
 *
 * The backend also passes "type":"ble" / "type":"wifi" hints in per-node
 * lockon commands (see drone_tracker._issue_lockon); we honor those too as
 * a primary signal, falling back to the command-type string.
 */
static bool cmd_is_ble_only(const char *json_cmd)
{
    if (!json_cmd) return false;
    if (strstr(json_cmd, "\"type\":\"ble\"")) return true;           /* backend hint */
    if (strstr(json_cmd, "\"type\":\"ble_lockon\"")) return true;
    if (strstr(json_cmd, "\"type\":\"ble_lockon_cancel\"")) return true;
    return false;
}

static bool cmd_is_wifi_only(const char *json_cmd)
{
    if (!json_cmd) return false;
    if (strstr(json_cmd, "\"type\":\"wifi\"")) return true;          /* backend hint */
    /* Plain "lockon" / "lockon_cancel" commands are WiFi channel locks. */
    if (strstr(json_cmd, "\"type\":\"lockon\"")) return true;
    if (strstr(json_cmd, "\"type\":\"lockon_cancel\"")) return true;
    return false;
}

void uart_rx_send_command(const char *json_cmd)
{
    if (!json_cmd) return;
    size_t len = strlen(json_cmd);

    const bool ble_only  = cmd_is_ble_only(json_cmd);
#if CONFIG_DUAL_SCANNER
    const bool wifi_only = cmd_is_wifi_only(json_cmd);
#else
    const bool wifi_only = false;  /* single-scanner build: always the BLE UART */
#endif

    /* BLE scanner: gets the command unless it's WiFi-specific. */
    if (!wifi_only) {
        uart_write_bytes(CONFIG_BLE_SCANNER_UART, json_cmd, len);
        uart_write_bytes(CONFIG_BLE_SCANNER_UART, "\n", 1);
    }

#if CONFIG_DUAL_SCANNER
    /* WiFi scanner: gets the command unless it's BLE-specific. */
    if (!ble_only) {
        uart_write_bytes(CONFIG_WIFI_SCANNER_UART, json_cmd, len);
        uart_write_bytes(CONFIG_WIFI_SCANNER_UART, "\n", 1);
    }
#endif

    ESP_LOGI("uart_tx_cmd", "cmd → %s (%d bytes)",
             ble_only  ? "BLE only"  :
             wifi_only ? "WiFi only" : "broadcast",
             (int)len);
}

const scanner_info_t *uart_rx_get_ble_scanner_info(void)
{
    return s_ble_scanner_info.received ? &s_ble_scanner_info : NULL;
}

const scanner_info_t *uart_rx_get_wifi_scanner_info(void)
{
    return s_wifi_scanner_info.received ? &s_wifi_scanner_info : NULL;
}

ota_response_t uart_rx_get_last_ota_response(void)
{
    ota_response_t copy;
    portENTER_CRITICAL(&s_ota_response_lock);
    copy = *(const ota_response_t *)&s_last_ota_response;
    portEXIT_CRITICAL(&s_ota_response_lock);
    return copy;
}

void uart_rx_clear_ota_response(void)
{
    portENTER_CRITICAL(&s_ota_response_lock);
    memset((void *)&s_last_ota_response, 0, sizeof(s_last_ota_response));
    portEXIT_CRITICAL(&s_ota_response_lock);
}

void uart_rx_pause_scanner(int scanner_id)
{
    if (scanner_id == 0) s_rx_paused_ble = true;
    else s_rx_paused_wifi = true;
    ESP_LOGW(TAG, "UART RX paused for scanner %d (OTA relay)", scanner_id);
}

void uart_rx_resume_scanner(int scanner_id)
{
    if (scanner_id == 0) s_rx_paused_ble = false;
    else s_rx_paused_wifi = false;
    ESP_LOGW(TAG, "UART RX resumed for scanner %d", scanner_id);
}
