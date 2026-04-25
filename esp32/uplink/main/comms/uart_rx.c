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
#include "detection_policy.h"
#include "led_status.h"

#include <string.h>
#include <stdio.h>
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

/* Per-scanner backpressure flags — BLE noise should not be able to pause the
 * WiFi scanner, and each scanner must be able to resume independently. */
static _Atomic bool s_backpressure_ble = false;
static _Atomic bool s_backpressure_wifi = false;

static bool s_node_calibration_mode = false;
static char s_node_scan_mode[16] = "normal";
static char s_node_calibration_session_id[24] = {0};
static char s_node_calibration_uuid[48] = {0};

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

static _Atomic bool *backpressure_flag_for_scanner(int scanner_id)
{
    return (scanner_id == 0) ? &s_backpressure_ble : &s_backpressure_wifi;
}

static uart_port_t scanner_uart_for_id(int scanner_id)
{
#if CONFIG_DUAL_SCANNER
    return (scanner_id == 0) ? CONFIG_BLE_SCANNER_UART : CONFIG_WIFI_SCANNER_UART;
#else
    (void)scanner_id;
    return CONFIG_BLE_SCANNER_UART;
#endif
}

static void send_scanner_flow_cmd(int scanner_id, const char *type)
{
    char cmd[24];
    int n = snprintf(cmd, sizeof(cmd), "{\"type\":\"%s\"}\n", type);
    if (n > 0) {
        uart_write_bytes(scanner_uart_for_id(scanner_id), cmd, n);
    }
}

static bool is_low_value_ble_detection(const drone_detection_t *det)
{
    if (det->source == DETECTION_SRC_BLE_FINGERPRINT) {
        return det->confidence < 0.10f;
    }
    return det->source == DETECTION_SRC_BLE_RID &&
           det->confidence < 0.10f &&
           strncmp(det->drone_id, "rid_", 4) != 0 &&
           det->latitude == 0.0 &&
           det->longitude == 0.0 &&
           det->operator_lat == 0.0 &&
           det->operator_lon == 0.0;
}

static void maybe_resume_scanner(int scanner_id)
{
    _Atomic bool *flag = backpressure_flag_for_scanner(scanner_id);
    if (!atomic_load(flag) || s_detection_queue == NULL) {
        return;
    }

    UBaseType_t queue_count = uxQueueMessagesWaiting(s_detection_queue);
    if (queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)) {
        send_scanner_flow_cmd(scanner_id, "start");
        ESP_LOGI(TAG, "Queue drained %d/%d — resuming scanner[%d]",
                 (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE, scanner_id);
        atomic_store(flag, false);
    }
}

static void note_scanner_activity(int scanner_id, int_fast64_t now_ms)
{
    if (scanner_id == 0) {
        atomic_store(&s_last_rx_time_ble, now_ms);
    } else {
        atomic_store(&s_last_rx_time_wifi, now_ms);
    }
}

static bool msg_type_is_scanner_originated(const char *msg_type)
{
    if (!msg_type) {
        return false;
    }
    return strcmp(msg_type, MSG_TYPE_DETECTION) == 0 ||
           strcmp(msg_type, MSG_TYPE_STATUS) == 0 ||
           strcmp(msg_type, "scanner_info") == 0 ||
           strcmp(msg_type, MSG_TYPE_CAL_MODE_ACK) == 0 ||
           strncmp(msg_type, "ota_", 4) == 0;
}

void uart_rx_set_node_calibration_mode(bool active,
                                       const char *session_id,
                                       const char *calibration_uuid)
{
    s_node_calibration_mode = active;
    strncpy(s_node_scan_mode, active ? "calibration" : "normal", sizeof(s_node_scan_mode) - 1);
    s_node_scan_mode[sizeof(s_node_scan_mode) - 1] = '\0';

    strncpy(s_node_calibration_session_id, session_id ? session_id : "", sizeof(s_node_calibration_session_id) - 1);
    s_node_calibration_session_id[sizeof(s_node_calibration_session_id) - 1] = '\0';
    strncpy(s_node_calibration_uuid, calibration_uuid ? calibration_uuid : "", sizeof(s_node_calibration_uuid) - 1);
    s_node_calibration_uuid[sizeof(s_node_calibration_uuid) - 1] = '\0';

    s_ble_scanner_info.calibration_mode_acked = false;
    s_wifi_scanner_info.calibration_mode_acked = false;
}

bool uart_rx_is_node_calibration_mode(void)
{
    return s_node_calibration_mode;
}

const char *uart_rx_get_node_scan_mode(void)
{
    return s_node_scan_mode;
}

const char *uart_rx_get_node_calibration_uuid(void)
{
    return s_node_calibration_uuid;
}

const char *uart_rx_get_node_calibration_session_id(void)
{
    return s_node_calibration_session_id;
}

bool uart_rx_node_mode_allows_detection(const drone_detection_t *det)
{
    if (!s_node_calibration_mode) {
        return true;
    }
    if (!det || det->source != DETECTION_SRC_BLE_FINGERPRINT) {
        return false;
    }
    if (fof_policy_ble_svc_raw_contains_uuid(det->ble_svc_uuids_raw, s_node_calibration_uuid)) {
        return true;
    }
    return fof_policy_ble_has_exact_uuid128_le(
        det->ble_service_uuids_128,
        det->ble_svc_uuid_128_count,
        s_node_calibration_uuid
    );
}

/* ── Source string to DETECTION_SRC mapping ────────────────────────────── */

static bool parse_source_value(int src_int, uint8_t *out_source)
{
    if (!out_source) {
        return false;
    }

    switch (src_int) {
        case DETECTION_SRC_BLE_RID:
        case DETECTION_SRC_BLE_FINGERPRINT:
        case DETECTION_SRC_WIFI_SSID:
        case DETECTION_SRC_WIFI_DJI_IE:
        case DETECTION_SRC_WIFI_BEACON:
        case DETECTION_SRC_WIFI_OUI:
        case DETECTION_SRC_WIFI_PROBE_REQUEST:
        case DETECTION_SRC_WIFI_ASSOC:
        case DETECTION_SRC_WIFI_AP_INVENTORY:
            *out_source = (uint8_t)src_int;
            return true;
        default:
            return false;
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

static void json_get_string_or_array_csv(const cJSON *obj,
                                         const char *key,
                                         char *out,
                                         size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(out, item->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    if (!cJSON_IsArray(item)) {
        return;
    }

    size_t off = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, item) {
        if (!cJSON_IsString(entry) || !entry->valuestring ||
            entry->valuestring[0] == '\0') {
            continue;
        }
        if (off > 0 && off < out_len - 1) {
            out[off++] = ',';
        }
        for (const char *p = entry->valuestring; *p && off < out_len - 1; ++p) {
            out[off++] = *p;
        }
        if (off >= out_len - 1) {
            break;
        }
    }
    out[off] = '\0';
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

    /* Source — fail closed. Missing/unknown source codes must not silently
     * downgrade into the BLE RID lane because that makes garbage look
     * authoritative to the backend and operator UI. */
    const cJSON *src_item = cJSON_GetObjectItemCaseSensitive(root, JSON_KEY_SOURCE);
    if (!cJSON_IsNumber(src_item)) {
        ESP_LOGW(TAG, "Detection %s missing numeric src", det->drone_id);
        return false;
    }
    if (!parse_source_value(src_item->valueint, &det->source)) {
        ESP_LOGW(TAG, "Detection %s has unknown src=%d", det->drone_id, src_item->valueint);
        return false;
    }

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
    json_get_string_or_array_csv(
        root,
        JSON_KEY_PROBED_SSIDS,
        det->probed_ssids,
        sizeof(det->probed_ssids)
    );
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
    const char *ble_name = json_get_string(root, JSON_KEY_BLE_NAME, "");
    strncpy(det->ble_name, ble_name, sizeof(det->ble_name) - 1);
    const char *class_reason = json_get_string(root, JSON_KEY_CLASS_REASON, "");
    strncpy(det->class_reason, class_reason, sizeof(det->class_reason) - 1);

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

    /* BLE service UUIDs may include 16-bit hex ("fd5f") and/or 128-bit
     * hyphenated UUIDs ("cafe9a86-0000-1000-8000-..."). Preserve the raw
     * scanner string for backend matching, and mirror 16-bit tokens into
     * det->ble_service_uuids for internal summaries that still read it. */
    const char *svc_str = json_get_string(root, JSON_KEY_BLE_SVC_UUIDS, NULL);
    if (svc_str) {
        /* Pass-through raw copy — this is what http_upload actually sends. */
        strncpy(det->ble_svc_uuids_raw, svc_str, sizeof(det->ble_svc_uuids_raw) - 1);
        det->ble_svc_uuids_raw[sizeof(det->ble_svc_uuids_raw) - 1] = '\0';

        /* Best-effort 16-bit mirror. strtoul stops at '-' for 128-bit tokens. */
        char svc_buf[160];
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
        info->uart_tx_dropped = (uint32_t)json_get_double(root, "uart_tx_dropped", 0);
        info->uart_tx_high_water = (uint32_t)json_get_double(root, "uart_tx_high_water", 0);
        info->tx_queue_depth = (uint32_t)json_get_double(root, "tx_queue_depth", 0);
        info->tx_queue_capacity = (uint32_t)json_get_double(root, "tx_queue_capacity", 0);
        info->tx_queue_pressure_pct = (uint32_t)json_get_double(root, "tx_queue_pressure_pct", 0);
        info->noise_drop_ble = (uint32_t)json_get_double(root, "noise_drop_ble", 0);
        info->noise_drop_wifi = (uint32_t)json_get_double(root, "noise_drop_wifi", 0);
        info->probe_seen = (uint32_t)json_get_double(root, "probe_seen", 0);
        info->probe_sent = (uint32_t)json_get_double(root, "probe_sent", 0);
        info->probe_drop_low_value = (uint32_t)json_get_double(root, "probe_drop_low_value", 0);
        info->probe_drop_rate_limit = (uint32_t)json_get_double(root, "probe_drop_rate_limit", 0);
        info->probe_drop_pressure = (uint32_t)json_get_double(root, "probe_drop_pressure", 0);
        info->toff_ms = (int64_t)json_get_double(root, "toff", (double)info->toff_ms);
        info->tcnt = (uint32_t)json_get_int(root, "tcnt", (int)info->tcnt);
        info->time_valid_count = (uint32_t)json_get_double(root, "time_valid_count", (double)info->time_valid_count);
        info->time_last_valid_age_s = (int64_t)json_get_double(
            root, "time_last_valid_age_s", (double)info->time_last_valid_age_s
        );
        const char *time_state = json_get_string(root, "time_sync_state", info->time_sync_state);
        if (time_state) {
            strncpy(info->time_sync_state, time_state, sizeof(info->time_sync_state) - 1);
            info->time_sync_state[sizeof(info->time_sync_state) - 1] = '\0';
        }
        const char *scan_mode = json_get_string(root, JSON_KEY_SCAN_MODE, info->scan_mode[0] ? info->scan_mode : "normal");
        strncpy(info->scan_mode, scan_mode, sizeof(info->scan_mode) - 1);
        info->scan_mode[sizeof(info->scan_mode) - 1] = '\0';
        const char *cal_uuid = json_get_string(root, JSON_KEY_CALIBRATION_UUID, info->calibration_uuid);
        strncpy(info->calibration_uuid, cal_uuid ? cal_uuid : "", sizeof(info->calibration_uuid) - 1);
        info->calibration_uuid[sizeof(info->calibration_uuid) - 1] = '\0';
        info->calibration_mode_acked =
            strcmp(info->scan_mode, "calibration") == 0 &&
            info->calibration_uuid[0] != '\0';

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
    int_fast64_t now_ms = (int_fast64_t)(esp_timer_get_time() / 1000);

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) {
        ESP_LOGW(TAG, "Scanner[%d] JSON parse error: %.64s...",
                 scanner_id, line);
        return;
    }

    const char *msg_type = json_get_string(root, JSON_KEY_TYPE, NULL);
    if (!msg_type) {
        ESP_LOGW(TAG, "Scanner[%d] message missing 'type' field", scanner_id);
        cJSON_Delete(root);
        return;
    }

    if (!msg_type_is_scanner_originated(msg_type)) {
        ESP_LOGW(TAG, "Scanner[%d] echoed/non-scanner msg type='%s' ignored",
                 scanner_id, msg_type);
        cJSON_Delete(root);
        return;
    }

    note_scanner_activity(scanner_id, now_ms);

    /* A stopped scanner still emits identity / status, so use scanner-originated
     * traffic as a chance to release backpressure once the queue has drained. */
    maybe_resume_scanner(scanner_id);

    if (strcmp(msg_type, MSG_TYPE_DETECTION) == 0) {
        drone_detection_t det;
        if (parse_detection(root, &det)) {
            det.scanner_slot = (uint8_t)scanner_id;
            det.scanner_slots_seen = (uint8_t)(1U << scanner_id);

            /* Skip BLE background noise (0.02 confidence) to reduce queue pressure.
             * WiFi APs (0.05) and phones (0.05) are still useful for the backend. */
            if (det.confidence < 0.04f &&
                det.source != DETECTION_SRC_WIFI_AP_INVENTORY) {
                push_recent(&det);  /* Still show in recent list */
                cJSON_Delete(root);
                return;
            }

            if (!uart_rx_node_mode_allows_detection(&det)) {
                cJSON_Delete(root);
                return;
            }

            UBaseType_t queue_count = uxQueueMessagesWaiting(s_detection_queue);
            bool low_value_ble = is_low_value_ble_detection(&det);

            /* Under sustained queue pressure, shed low-value BLE fingerprints
             * before they can starve WiFi scans or real RID packets. */
            if (low_value_ble &&
                queue_count >= (CONFIG_DETECTION_QUEUE_SIZE / 2)) {
                push_recent(&det);
                cJSON_Delete(root);
                return;
            }

            /* Backpressure: if queue is nearly full, pause only the scanner
             * that is contributing this traffic. Global stop/start caused BLE
             * bursts to starve the WiFi scanner and left scanners stuck until
             * the watchdog re-sent "ready". */
            _Atomic bool *bp_flag = backpressure_flag_for_scanner(scanner_id);
            if (queue_count >= (CONFIG_DETECTION_QUEUE_SIZE * 8 / 10)) {
                if (!atomic_load(bp_flag)) {
                    send_scanner_flow_cmd(scanner_id, "stop");
                    ESP_LOGW(TAG, "Queue pressure %d/%d — throttling scanner[%d]",
                             (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE, scanner_id);
                    atomic_store(bp_flag, true);
                }
            } else if (atomic_load(bp_flag) &&
                       queue_count <= (CONFIG_DETECTION_QUEUE_SIZE * 4 / 10)) {
                send_scanner_flow_cmd(scanner_id, "start");
                ESP_LOGI(TAG, "Queue drained %d/%d — resuming scanner[%d]",
                         (int)queue_count, CONFIG_DETECTION_QUEUE_SIZE, scanner_id);
                atomic_store(bp_flag, false);
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
        info->time_valid_count = (uint32_t)json_get_double(root, "time_valid_count", 0.0);
        info->time_last_valid_age_s = (int64_t)json_get_double(root, "time_last_valid_age_s", -1.0);
        const char *time_state = json_get_string(root, "time_sync_state", "unknown");
        strncpy(info->time_sync_state, time_state, sizeof(info->time_sync_state) - 1);
        info->time_sync_state[sizeof(info->time_sync_state) - 1] = '\0';
        const char *scan_mode = json_get_string(root, JSON_KEY_SCAN_MODE, "normal");
        strncpy(info->scan_mode, scan_mode, sizeof(info->scan_mode) - 1);
        info->scan_mode[sizeof(info->scan_mode) - 1] = '\0';
        const char *cal_uuid = json_get_string(root, JSON_KEY_CALIBRATION_UUID, "");
        strncpy(info->calibration_uuid, cal_uuid, sizeof(info->calibration_uuid) - 1);
        info->calibration_uuid[sizeof(info->calibration_uuid) - 1] = '\0';
        info->calibration_mode_acked =
            strcmp(info->scan_mode, "calibration") == 0 &&
            info->calibration_uuid[0] != '\0';
        info->received = true;
        ESP_LOGI(TAG, "Scanner[%d] identity: %s v%s (%s) chip=%s toff=%lld tcnt=%u valid=%u state=%s mode=%s",
                 scanner_id, board, ver, caps, chip,
                 (long long)info->toff_ms, info->tcnt,
                 info->time_valid_count, info->time_sync_state,
                 info->scan_mode[0] ? info->scan_mode : "normal");
    } else if (strcmp(msg_type, MSG_TYPE_CAL_MODE_ACK) == 0) {
        scanner_info_t *info = (scanner_id == 0) ? &s_ble_scanner_info : &s_wifi_scanner_info;
        const char *scan_mode = json_get_string(root, JSON_KEY_SCAN_MODE, "normal");
        const char *cal_uuid = json_get_string(root, JSON_KEY_CALIBRATION_UUID, "");
        const cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(root, "ok");
        bool ok = (ok_item && cJSON_IsTrue(ok_item)) ||
                  (ok_item && cJSON_IsNumber(ok_item) && ok_item->valueint != 0);
        strncpy(info->scan_mode, scan_mode, sizeof(info->scan_mode) - 1);
        info->scan_mode[sizeof(info->scan_mode) - 1] = '\0';
        strncpy(info->calibration_uuid, cal_uuid, sizeof(info->calibration_uuid) - 1);
        info->calibration_uuid[sizeof(info->calibration_uuid) - 1] = '\0';
        info->calibration_mode_acked = ok;
        info->received = true;
        ESP_LOGW(TAG, "Scanner[%d] calibration ack: ok=%d mode=%s uuid=%s",
                 scanner_id,
                 ok ? 1 : 0,
                 info->scan_mode[0] ? info->scan_mode : "normal",
                 info->calibration_uuid[0] ? info->calibration_uuid : "-");
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
    if (s_node_calibration_mode &&
        (strstr(json_cmd, "\"type\":\"lockon\"") ||
         strstr(json_cmd, "\"type\":\"lockon_cancel\"") ||
         strstr(json_cmd, "\"type\":\"ble_lockon\"") ||
         strstr(json_cmd, "\"type\":\"ble_lockon_cancel\""))) {
        ESP_LOGW(TAG, "Rejecting scan-control command while calibration mode is active");
        return;
    }
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
