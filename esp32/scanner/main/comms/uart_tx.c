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
#include "detection_policy.h"
#include "time_sync_policy.h"
#include "uart_protocol.h"
#include "task_priorities.h"
#include "version.h"
#include "wifi_scanner.h"
#include "calibration_mode.h"
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
#include <stdlib.h>
#include "esp_timer.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_uart_tx";

#define UART_PORT_NUM       UART_NUM_1
#define TX_BUF_SIZE         UART_BUF_SIZE
#define RX_BUF_SIZE         UART_BUF_SIZE
#define UART_TX_STACK_WARN_BYTES 1024
#define LOW_PRIORITY_RATE_SLOTS 64
#define BLE_FINGERPRINT_REEMIT_MS 60000
#define WIFI_ASSOC_REEMIT_MS     30000
#define WIFI_PROBE_REEMIT_MS     60000

/* Queue receive timeout -- short enough to allow periodic maintenance. */
#define QUEUE_RX_TIMEOUT_MS 100

/* ── Counters for status messages ───────────────────────────────────────── */

static int s_ble_count  = 0;
static int s_wifi_count = 0;
static uint8_t s_current_channel = 0;
static uint32_t s_seq = 0;
static uint32_t s_uart_tx_dropped = 0;
static uint32_t s_uart_tx_queue_high_water = 0;
static uint32_t s_uart_tx_queue_depth = 0;
static uint32_t s_uart_tx_queue_capacity = 0;
static uint32_t s_uart_tx_queue_pressure_pct = 0;
static uint32_t s_noise_drop_ble = 0;
static uint32_t s_noise_drop_wifi = 0;
static uint32_t s_probe_seen = 0;
static uint32_t s_probe_sent = 0;
static uint32_t s_probe_drop_low_value = 0;
static uint32_t s_probe_drop_rate_limit = 0;
static uint32_t s_probe_drop_pressure = 0;

typedef struct {
    bool    in_use;
    char    key[64];
    char    aux[32];
    int64_t last_sent_ms;
    float   last_confidence;
    int8_t  last_rssi;
} rate_limit_entry_t;

static rate_limit_entry_t s_ble_fp_rate[LOW_PRIORITY_RATE_SLOTS];
static rate_limit_entry_t s_wifi_assoc_rate[LOW_PRIORITY_RATE_SLOTS];
static rate_limit_entry_t s_wifi_probe_rate[LOW_PRIORITY_RATE_SLOTS];

/* UART write mutex — prevents interleaved writes from multiple tasks */
static SemaphoreHandle_t s_uart_mutex = NULL;
static QueueHandle_t s_detection_queue = NULL;

/* ── Detection cache for OLED scoreboard ────────────────────────────────── */

static scanner_detection_summary_t s_det_cache[DETECTION_CACHE_SIZE];
static int s_det_cache_count = 0;
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;

/* Deferred identity — set before TX task starts, sent after uplink ready */
static char s_identity_board[24] = {0};
static char s_identity_chip[16]  = {0};
static char s_identity_caps[16]  = {0};

/* TX enable flag — controlled by uplink start/stop commands.
 * Scanner starts DISABLED, enabled when uplink sends "ready" or "start". */
static volatile bool s_tx_enabled = false;
static bool s_uart_tx_stack_warned = false;

bool uart_tx_is_enabled(void) { return s_tx_enabled; }
void uart_tx_set_enabled(bool enabled) {
    s_tx_enabled = enabled;
    ESP_LOGI("fof_uart_tx", "TX %s by uplink command", enabled ? "ENABLED" : "DISABLED");
}

void uart_tx_flush_detection_queue(void)
{
    QueueHandle_t q = s_detection_queue;
    if (!q) {
        return;
    }
    drone_detection_t dropped;
    uint32_t count = 0;
    while (xQueueReceive(q, &dropped, 0) == pdTRUE) {
        count++;
    }
    if (count > 0) {
        s_uart_tx_queue_depth = 0;
        ESP_LOGI(TAG, "Flushed %lu queued detections for mode transition",
                 (unsigned long)count);
    }
}

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

static void cjson_add_csv_array_if(cJSON *obj,
                                   const char *key,
                                   const char *csv,
                                   const char *fallback)
{
    const char *src = (csv && csv[0] != '\0') ? csv : fallback;
    if (!src || src[0] == '\0') {
        return;
    }

    cJSON *arr = cJSON_AddArrayToObject(obj, key);
    if (!arr) {
        return;
    }

    char token[33];
    size_t pos = 0;
    bool emitted = false;
    for (const char *p = src;; ++p) {
        if (*p == ',' || *p == '\0') {
            token[pos] = '\0';
            if (pos > 0) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(token));
                emitted = true;
            }
            pos = 0;
            if (*p == '\0') {
                break;
            }
            continue;
        }
        if (pos < sizeof(token) - 1) {
            token[pos++] = *p;
        }
    }

    if (!emitted) {
        cJSON_DeleteItemFromObject(obj, key);
    }
}

static bool is_ble_source(uint8_t source)
{
    return source == DETECTION_SRC_BLE_RID ||
           source == DETECTION_SRC_BLE_FINGERPRINT;
}

static const char *detection_rate_key(const drone_detection_t *detection)
{
    if (!detection) {
        return "";
    }
    if (detection->bssid[0] != '\0') {
        return detection->bssid;
    }
    if (detection->drone_id[0] != '\0') {
        return detection->drone_id;
    }
    return "";
}

static rate_limit_entry_t *find_or_alloc_rate_slot(rate_limit_entry_t *table,
                                                   size_t count,
                                                   const char *key)
{
    int oldest_idx = 0;
    int64_t oldest_ms = INT64_MAX;

    for (size_t i = 0; i < count; i++) {
        if (table[i].in_use && strcmp(table[i].key, key) == 0) {
            return &table[i];
        }
        if (!table[i].in_use) {
            return &table[i];
        }
        if (table[i].last_sent_ms < oldest_ms) {
            oldest_ms = table[i].last_sent_ms;
            oldest_idx = (int)i;
        }
    }

    return &table[oldest_idx];
}

static bool allow_rate_limited_detection(rate_limit_entry_t *table,
                                         size_t count,
                                         const char *key,
                                         const char *aux,
                                         int64_t now_ms,
                                         int window_ms,
                                         float confidence,
                                         int8_t rssi,
                                         bool allow_aux_change,
                                         bool allow_confidence_bump,
                                         bool allow_rssi_jump)
{
    if (!key || key[0] == '\0') {
        return true;
    }

    rate_limit_entry_t *slot = find_or_alloc_rate_slot(table, count, key);
    if (!slot->in_use) {
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        strncpy(slot->key, key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';
        strncpy(slot->aux, aux ? aux : "", sizeof(slot->aux) - 1);
        slot->aux[sizeof(slot->aux) - 1] = '\0';
        slot->last_sent_ms = now_ms;
        slot->last_confidence = confidence;
        slot->last_rssi = rssi;
        return true;
    }

    bool allow = false;
    if ((now_ms - slot->last_sent_ms) >= window_ms) {
        allow = true;
    }
    if (!allow && allow_aux_change && aux && aux[0] != '\0' &&
        strcmp(slot->aux, aux) != 0) {
        allow = true;
    }
    if (!allow && allow_confidence_bump &&
        confidence >= (slot->last_confidence + 0.10f)) {
        allow = true;
    }
    if (!allow && allow_rssi_jump &&
        abs((int)rssi - (int)slot->last_rssi) >= 8) {
        allow = true;
    }
    if (!allow) {
        return false;
    }

    slot->key[0] = '\0';
    slot->aux[0] = '\0';
    strncpy(slot->key, key, sizeof(slot->key) - 1);
    slot->key[sizeof(slot->key) - 1] = '\0';
    strncpy(slot->aux, aux ? aux : "", sizeof(slot->aux) - 1);
    slot->aux[sizeof(slot->aux) - 1] = '\0';
    slot->last_sent_ms = now_ms;
    slot->last_confidence = confidence;
    slot->last_rssi = rssi;
    return true;
}

typedef enum {
    UART_DROP_LOW_VALUE = 0,
    UART_DROP_RATE_LIMIT = 1,
    UART_DROP_PRESSURE = 2,
} uart_drop_reason_t;

static void note_uart_drop(const drone_detection_t *detection,
                           uart_drop_reason_t reason)
{
    s_uart_tx_dropped++;
    if (is_ble_source(detection->source)) {
        s_noise_drop_ble++;
    } else {
        s_noise_drop_wifi++;
    }
    if (detection->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
        if (reason == UART_DROP_LOW_VALUE) {
            s_probe_drop_low_value++;
        } else if (reason == UART_DROP_RATE_LIMIT) {
            s_probe_drop_rate_limit++;
        } else if (reason == UART_DROP_PRESSURE) {
            s_probe_drop_pressure++;
        }
    }
}

static bool should_rate_limit_detection(const drone_detection_t *detection,
                                        int64_t now_ms)
{
    if (detection &&
        detection->source == DETECTION_SRC_BLE_FINGERPRINT &&
        fof_policy_ble_has_calibration_uuid_le(
            detection->ble_service_uuids_128,
            detection->ble_svc_uuid_128_count
        )) {
        return false;
    }

    if (detection->source == DETECTION_SRC_BLE_FINGERPRINT) {
        const char *key = detection->bssid[0] ? detection->bssid : detection->drone_id;
        const char *aux = detection->manufacturer[0] ? detection->manufacturer :
                          (detection->model[0] ? detection->model : "");
        return !allow_rate_limited_detection(
            s_ble_fp_rate, LOW_PRIORITY_RATE_SLOTS,
            key, aux, now_ms, BLE_FINGERPRINT_REEMIT_MS,
            detection->confidence, detection->rssi, true, true, false
        );
    }
    if (detection->source == DETECTION_SRC_WIFI_ASSOC) {
        const char *key = detection->drone_id;
        return !allow_rate_limited_detection(
            s_wifi_assoc_rate, LOW_PRIORITY_RATE_SLOTS,
            key, detection->bssid, now_ms, WIFI_ASSOC_REEMIT_MS,
            detection->confidence, detection->rssi, true, false, true
        );
    }
    if (detection->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
        const char *key = detection_rate_key(detection);
        char aux[16];
        fof_policy_probe_rate_aux(
            detection->probe_ie_hash,
            detection->probed_ssids[0] ? detection->probed_ssids : detection->ssid,
            aux,
            sizeof(aux)
        );
        return !allow_rate_limited_detection(
            s_wifi_probe_rate, LOW_PRIORITY_RATE_SLOTS,
            key, aux, now_ms, WIFI_PROBE_REEMIT_MS,
            detection->confidence, detection->rssi, true, false, false
        );
    }
    return false;
}

static bool should_shed_low_priority_detection(const drone_detection_t *detection,
                                               UBaseType_t queue_depth,
                                               UBaseType_t queue_capacity)
{
    if (!detection) {
        return false;
    }
    return fof_policy_should_shed_low_priority(
        detection->source,
        detection->manufacturer,
        detection->ble_service_uuids_128,
        detection->ble_svc_uuid_128_count,
        (uint32_t)queue_depth,
        (uint32_t)queue_capacity
    );
}

/**
 * Transmit a raw null-terminated string over UART, appending the newline
 * delimiter.  The caller is responsible for providing valid JSON.
 */
void uart_tx_send_raw_json(const char *json_str)
{
    if (!json_str) return;
    size_t len = strlen(json_str);
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT_NUM, json_str, len);
    uart_write_bytes(UART_PORT_NUM, "\n", 1);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
}

static void uart_send_line(const char *json_str)
{
    size_t len = strlen(json_str);
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT_NUM, json_str, len);
    uart_write_bytes(UART_PORT_NUM, "\n", 1);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
}

static void maybe_warn_uart_tx_stack_headroom(void)
{
    UBaseType_t free_words = uxTaskGetStackHighWaterMark(NULL);
    size_t free_bytes = (size_t)free_words * sizeof(StackType_t);

    if (free_bytes <= UART_TX_STACK_WARN_BYTES) {
        if (!s_uart_tx_stack_warned) {
            ESP_LOGW(TAG, "uart_tx stack headroom low: %u bytes free",
                     (unsigned)free_bytes);
            s_uart_tx_stack_warned = true;
        }
    } else if (s_uart_tx_stack_warned &&
               free_bytes > (UART_TX_STACK_WARN_BYTES * 2)) {
        ESP_LOGI(TAG, "uart_tx stack headroom recovered: %u bytes free",
                 (unsigned)free_bytes);
        s_uart_tx_stack_warned = false;
    }
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
    /* RX buffer 4KB (was 2KB) to handle OTA relay bursts without overflow */
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM,
                                        RX_BUF_SIZE * 2,
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
    /* Emit epoch-ms when synced with uplink (v0.60+); otherwise send uptime-ms
     * which the backend ignores via the epoch-validity threshold. */
    extern volatile int64_t g_epoch_offset_ms;
    int64_t ts_ms = detection->last_updated_ms;
    if (g_epoch_offset_ms != 0) ts_ms += g_epoch_offset_ms;
    cJSON_AddNumberToObject(root, JSON_KEY_TIMESTAMP, (double)ts_ms);
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
    if (detection->wifi_auth_mode != 0xFF) {
        cJSON_AddNumberToObject(root, JSON_KEY_WIFI_AUTH_MODE, detection->wifi_auth_mode);
    }

    /* Probe request: include every parsed targeted SSID as a JSON array. */
    if (detection->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
        cjson_add_csv_array_if(
            root,
            JSON_KEY_PROBED_SSIDS,
            detection->probed_ssids,
            detection->ssid
        );
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
    cjson_add_string_if(root, JSON_KEY_BLE_NAME, detection->ble_name);
    cjson_add_string_if(root, JSON_KEY_CLASS_REASON, detection->class_reason);

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

    /* BLE Service UUIDs — v0.63 emits both 16-bit (comma-separated hex)
     * and 128-bit (canonical big-endian hyphenated) in a single field.
     * Backend substring-matches on the UUID it's looking for, so it
     * doesn't need to know the format ordering.
     *   Sizing: up to 4 × 4-char 16-bit + 2 × 36-char 128-bit + 5 commas
     *   + NUL = 16 + 72 + 5 + 1 = 94. 128-byte buffer gives headroom. */
    if (detection->ble_svc_uuid_count > 0 || detection->ble_svc_uuid_128_count > 0) {
        char svc_buf[128];
        int svc_off = 0;
        for (int i = 0; i < detection->ble_svc_uuid_count && i < 4; i++) {
            if (svc_off > 0) svc_buf[svc_off++] = ',';
            svc_off += snprintf(&svc_buf[svc_off], sizeof(svc_buf) - svc_off,
                                "%04x", detection->ble_service_uuids[i]);
        }
        for (int i = 0; i < detection->ble_svc_uuid_128_count && i < 2; i++) {
            if (svc_off > 0) svc_buf[svc_off++] = ',';
            /* UUID bytes are stored LE as transmitted; emit big-endian
             * hyphenated so downstream matches "cafe9a86-0000-..." etc. */
            const uint8_t *u = detection->ble_service_uuids_128[i];
            svc_off += snprintf(&svc_buf[svc_off], sizeof(svc_buf) - svc_off,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
                u[7],  u[6],  u[5],  u[4],  u[3],  u[2],  u[1], u[0]);
        }
        cJSON_AddStringToObject(root, JSON_KEY_BLE_SVC_UUIDS, svc_buf);
    }

    /* Apple Nearby Info data-flags byte — always emitted (even when 0) so
     * the backend can distinguish "all flags false" from "field absent". */
    cJSON_AddNumberToObject(root, JSON_KEY_BLE_APPLE_FLAGS, detection->ble_apple_flags);

    /* WiFi probe fingerprint — only add if not already added as array above */
    if (detection->probed_ssids[0] != '\0' &&
        detection->source != DETECTION_SRC_WIFI_PROBE_REQUEST) {
        cJSON_AddStringToObject(root, JSON_KEY_PROBED_SSIDS, detection->probed_ssids);
    }
    if (detection->probe_ie_hash != 0) {
        char ie_hex[9];
        snprintf(ie_hex, sizeof(ie_hex), "%08lx", (unsigned long)detection->probe_ie_hash);
        cJSON_AddStringToObject(root, "ie_hash", ie_hex);
    }
    if (detection->wifi_generation != 0) {
        cJSON_AddNumberToObject(root, "wifi_gen", detection->wifi_generation);
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

static int64_t scanner_time_last_valid_age_s(void)
{
    extern volatile int64_t g_last_valid_time_local_ms;

    if (g_last_valid_time_local_ms <= 0) {
        return -1;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms <= g_last_valid_time_local_ms) {
        return 0;
    }
    return (now_ms - g_last_valid_time_local_ms) / 1000;
}

static const char *scanner_time_sync_state(void)
{
    extern volatile int64_t g_epoch_offset_ms;
    extern volatile uint32_t g_time_valid_count;
    extern volatile int64_t g_last_valid_time_local_ms;

    return fof_time_sync_state_label(
        g_time_valid_count,
        g_epoch_offset_ms,
        g_last_valid_time_local_ms,
        esp_timer_get_time() / 1000,
        FOF_TIME_SYNC_STALE_AFTER_MS
    );
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
    cJSON_AddNumberToObject(root, "uart_tx_dropped", s_uart_tx_dropped);
    cJSON_AddNumberToObject(root, "uart_tx_high_water", s_uart_tx_queue_high_water);
    cJSON_AddNumberToObject(root, "tx_queue_depth", s_uart_tx_queue_depth);
    cJSON_AddNumberToObject(root, "tx_queue_capacity", s_uart_tx_queue_capacity);
    cJSON_AddNumberToObject(root, "tx_queue_pressure_pct", s_uart_tx_queue_pressure_pct);
    cJSON_AddNumberToObject(root, "noise_drop_ble", s_noise_drop_ble);
    cJSON_AddNumberToObject(root, "noise_drop_wifi", s_noise_drop_wifi);
    cJSON_AddNumberToObject(root, "probe_seen", s_probe_seen);
    cJSON_AddNumberToObject(root, "probe_sent", s_probe_sent);
    cJSON_AddNumberToObject(root, "probe_drop_low_value", s_probe_drop_low_value);
    cJSON_AddNumberToObject(root, "probe_drop_rate_limit", s_probe_drop_rate_limit);
    cJSON_AddNumberToObject(root, "probe_drop_pressure", s_probe_drop_pressure);
    cJSON_AddStringToObject(root, JSON_KEY_SCAN_MODE, scanner_calibration_mode_label());
    cjson_add_string_if(root, JSON_KEY_CALIBRATION_UUID, scanner_calibration_mode_uuid());
    cJSON_AddNumberToObject(root, JSON_KEY_SEQ, s_seq++);

    /* Include scanner identity in every status message */
    if (s_scanner_ver) {
        cJSON_AddStringToObject(root, "ver", s_scanner_ver);
        cJSON_AddStringToObject(root, "board", s_scanner_board);
        cJSON_AddStringToObject(root, "chip", s_scanner_chip);
        cJSON_AddStringToObject(root, "caps", s_scanner_caps);
    }
    {
        extern volatile int64_t g_epoch_offset_ms;
        extern volatile uint32_t g_time_msg_count;
        extern volatile uint32_t g_time_valid_count;
        cJSON_AddNumberToObject(root, "toff", (double)g_epoch_offset_ms);
        cJSON_AddNumberToObject(root, "tcnt", g_time_msg_count);
        cJSON_AddNumberToObject(root, "time_valid_count", g_time_valid_count);
        cJSON_AddNumberToObject(root, "time_last_valid_age_s", (double)scanner_time_last_valid_age_s());
        cJSON_AddStringToObject(root, "time_sync_state", scanner_time_sync_state());
    }

    /* Attack / anomaly counters (delta since last status) */
    {
        uint16_t deauth = 0, disassoc = 0, auth = 0;
        bool flood = false, bcn_spam = false;
        wifi_scanner_get_attack_counters(&deauth, &disassoc, &auth,
                                          &flood, &bcn_spam);
        if (deauth > 0)  cJSON_AddNumberToObject(root, "deauth",  deauth);
        if (disassoc > 0) cJSON_AddNumberToObject(root, "disassoc", disassoc);
        if (auth > 0)    cJSON_AddNumberToObject(root, "auth_fr",  auth);
        if (flood)       cJSON_AddTrueToObject(root, "flood");
        if (bcn_spam)    cJSON_AddTrueToObject(root, "bcn_spam");
        wifi_scanner_reset_attack_counters();
    }

    /* Frame control subtype histogram (comma-separated 16 values) */
    {
        uint32_t hist[16];
        wifi_scanner_get_fc_histogram(hist);
        char hist_str[128];
        int off = 0;
        for (int i = 0; i < 16; i++) {
            if (i > 0) hist_str[off++] = ',';
            off += snprintf(&hist_str[off], sizeof(hist_str) - off,
                            "%lu", (unsigned long)hist[i]);
        }
        cJSON_AddStringToObject(root, "fc_hist", hist_str);
        wifi_scanner_reset_fc_histogram();
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

    /* Also send as standalone message. Includes time-sync diagnostic fields
     * (toff, tcnt) so the uplink can surface them via /api/status — that's
     * what tells us whether the scanner has actually received uplink's time
     * broadcasts (tcnt) and whether the value applied (toff). */
    extern volatile int64_t g_epoch_offset_ms;
    extern volatile uint32_t g_time_msg_count;
    extern volatile uint32_t g_time_valid_count;
    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"scanner_info\",\"ver\":\"%s\",\"board\":\"%s\","
             "\"chip\":\"%s\",\"caps\":\"%s\",\"toff\":%lld,\"tcnt\":%lu,"
             "\"time_valid_count\":%lu,\"time_last_valid_age_s\":%lld,\"time_sync_state\":\"%s\","
             "\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"}",
             ver ? ver : "?", board ? board : "?",
             chip ? chip : "?", caps ? caps : "?",
             (long long)g_epoch_offset_ms,
             (unsigned long)g_time_msg_count,
             (unsigned long)g_time_valid_count,
             (long long)scanner_time_last_valid_age_s(),
             scanner_time_sync_state(),
             scanner_calibration_mode_label(),
             scanner_calibration_mode_uuid());
    uart_send_line(buf);
    ESP_LOGD(TAG, "Scanner info TX: %s v%s (%s) toff=%lld tcnt=%lu valid=%lu state=%s",
             board, ver, caps, (long long)g_epoch_offset_ms,
             (unsigned long)g_time_msg_count,
             (unsigned long)g_time_valid_count,
             scanner_time_sync_state());
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

    /* Wait for uplink to enable TX via the "ready"/"start" command.
     * Auto-start after 30s if no command received (handles wiring issues
     * where uplink→scanner UART direction doesn't work). */
    ESP_LOGI(TAG, "TX paused — waiting for uplink start command (30s timeout)...");
    led_set_pattern(LED_IDLE);

    int wait_count = 0;
    while (!s_tx_enabled) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_count++;
        if (wait_count >= 60) {  /* 60 x 500ms = 30 seconds */
            ESP_LOGW(TAG, "No start command after 30s — auto-enabling TX");
            s_tx_enabled = true;
        }
    }

    /* Send scanner identity now that uplink enabled us */
    if (s_identity_board[0]) {
        /* Keep using the shared Friend or Foe version here. The ESP-IDF app
         * descriptor can lag or collapse to "1", which makes live scanner
         * heartbeats look stale even after a successful OTA. */
        uart_tx_send_scanner_info(FOF_VERSION,
                                  s_identity_board, s_identity_chip, s_identity_caps);
    }
    led_set_pattern(LED_UPLINK_OK);
    ESP_LOGI(TAG, "TX enabled — transmitting detections.");

    for (;;) {
        /* If uplink sent stop command, pause TX until re-enabled */
        if (!s_tx_enabled) {
            led_set_pattern(LED_IDLE);
            ESP_LOGI(TAG, "TX paused by uplink stop command");
            while (!s_tx_enabled) vTaskDelay(pdMS_TO_TICKS(500));
            led_set_pattern(LED_UPLINK_OK);
            ESP_LOGI(TAG, "TX resumed by uplink start command");
        }

        /* Block on detection queue with a short timeout so we can do
         * periodic maintenance even when no detections are flowing. */
        if (xQueueReceive(detection_queue, &det, queue_timeout) == pdTRUE) {
            int64_t tx_now_ms = esp_timer_get_time() / 1000;
            UBaseType_t queue_depth = uxQueueMessagesWaiting(detection_queue);
            UBaseType_t queue_capacity = queue_depth +
                                         uxQueueSpacesAvailable(detection_queue);
            s_uart_tx_queue_depth = (uint32_t)queue_depth;
            s_uart_tx_queue_capacity = (uint32_t)queue_capacity;
            s_uart_tx_queue_pressure_pct = fof_policy_queue_pressure_pct(
                (uint32_t)queue_depth,
                (uint32_t)queue_capacity
            );
            if ((uint32_t)queue_depth > s_uart_tx_queue_high_water) {
                s_uart_tx_queue_high_water = (uint32_t)queue_depth;
            }

            if (det.source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
                s_probe_seen++;
            }

            if (!scanner_calibration_mode_allows_detection(&det)) {
                note_uart_drop(&det, UART_DROP_PRESSURE);
                continue;
            }

            if (fof_policy_should_drop_low_value(det.source,
                                                 det.confidence,
                                                 det.manufacturer,
                                                 det.ble_service_uuids_128,
                                                 det.ble_svc_uuid_128_count)) {
                note_uart_drop(&det, UART_DROP_LOW_VALUE);
                continue;
            }
            if (should_rate_limit_detection(&det, tx_now_ms)) {
                note_uart_drop(&det, UART_DROP_RATE_LIMIT);
                continue;
            }
            if (should_shed_low_priority_detection(&det,
                                                   queue_depth,
                                                   queue_capacity)) {
                note_uart_drop(&det, UART_DROP_PRESSURE);
                continue;
            }

            /* Track per-source counters for status messages */
            if (is_ble_source(det.source)) {
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
            if (det.source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
                s_probe_sent++;
            }
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
            UBaseType_t queue_depth = uxQueueMessagesWaiting(detection_queue);
            UBaseType_t queue_capacity = queue_depth +
                                         uxQueueSpacesAvailable(detection_queue);
            s_uart_tx_queue_depth = (uint32_t)queue_depth;
            s_uart_tx_queue_capacity = (uint32_t)queue_capacity;
            s_uart_tx_queue_pressure_pct = fof_policy_queue_pressure_pct(
                (uint32_t)queue_depth,
                (uint32_t)queue_capacity
            );

            maybe_warn_uart_tx_stack_headroom();
            uart_tx_send_status(s_ble_count, s_wifi_count,
                                s_current_channel, uptime_s);
            led_set_pattern(LED_UPLINK_OK);  /* purple — UART flowing */

            ESP_LOGD(TAG, "Status TX: ble=%d wifi=%d ch=%d uptime=%lus",
                     s_ble_count, s_wifi_count,
                     s_current_channel, (unsigned long)uptime_s);
        }
    }
    /* Task should never return; if it does, clean up. */
    vTaskDelete(NULL);
}

void uart_tx_set_identity(const char *board, const char *chip, const char *caps)
{
    strncpy(s_identity_board, board, sizeof(s_identity_board) - 1);
    strncpy(s_identity_chip,  chip,  sizeof(s_identity_chip) - 1);
    strncpy(s_identity_caps,  caps,  sizeof(s_identity_caps) - 1);
}

void uart_tx_start(QueueHandle_t detection_queue)
{
    s_detection_queue = detection_queue;
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
