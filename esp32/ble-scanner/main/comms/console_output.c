/**
 * Friend or Foe — BLE Scanner Console Output
 *
 * Reads drone detections from the shared queue, applies Bayesian fusion,
 * serializes to newline-delimited JSON, and prints to stdout (serial console).
 * Fork of scanner/uart_tx.c with UART hardware replaced by printf.
 */

#include "console_output.h"

#include "bayesian_fusion.h"
#include "constants.h"
#include "detection_types.h"
#include "uart_protocol.h"
#include "core/task_priorities.h"
#include "led_status.h"

#include "cJSON.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include "esp_timer.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_console";

/* Queue receive timeout -- short enough to allow periodic maintenance. */
#define QUEUE_RX_TIMEOUT_MS 100

/* ── Counters for status messages ───────────────────────────────────────── */

static int s_ble_count = 0;
static uint32_t s_seq = 0;

/* ── Detection cache for OLED scoreboard ────────────────────────────────── */

static scanner_detection_summary_t s_det_cache[DETECTION_CACHE_SIZE];
static int s_det_cache_count = 0;
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Detection cache helpers ────────────────────────────────────────────── */

static scanner_detection_summary_t *cache_find_or_alloc(const char *drone_id)
{
    for (int i = 0; i < s_det_cache_count; i++) {
        if (strcmp(s_det_cache[i].drone_id, drone_id) == 0) {
            return &s_det_cache[i];
        }
    }

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
 * Print a JSON detection line to stdout (console).
 */
static void console_send_detection(const drone_detection_t *detection)
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

    /* Position */
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

    /* WiFi-specific (mostly empty for BLE scanner, but keep for completeness) */
    cjson_add_string_if(root, JSON_KEY_SSID,  detection->ssid);
    cjson_add_string_if(root, JSON_KEY_BSSID, detection->bssid);

    /* Timestamps */
    if (detection->first_seen_ms != 0) {
        cJSON_AddNumberToObject(root, JSON_KEY_FIRST_SEEN, (double)detection->first_seen_ms);
    }
    cJSON_AddNumberToObject(root, JSON_KEY_LAST_UPDATED, (double)detection->last_updated_ms);

    /* Serialize and print */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        printf("%s\n", json_str);
        cJSON_free(json_str);
    } else {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
    }
}

static void console_send_status(int ble_count, uint32_t uptime_s)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, JSON_KEY_TYPE, MSG_TYPE_STATUS);
    cJSON_AddNumberToObject(root, "ble_count", ble_count);
    cJSON_AddNumberToObject(root, "uptime_s",  uptime_s);
    cJSON_AddNumberToObject(root, JSON_KEY_SEQ, s_seq++);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        printf("%s\n", json_str);
        cJSON_free(json_str);
    }
}

/* ── Console output task ────────────────────────────────────────────────── */

static void console_output_task(void *arg)
{
    QueueHandle_t detection_queue = (QueueHandle_t)arg;
    drone_detection_t det;

    TickType_t last_prune_tick = xTaskGetTickCount();
    const TickType_t prune_interval = pdMS_TO_TICKS(PRUNE_INTERVAL_MS);
    const TickType_t queue_timeout  = pdMS_TO_TICKS(QUEUE_RX_TIMEOUT_MS);

    ESP_LOGI(TAG, "Console output task started (prune every %d ms)", PRUNE_INTERVAL_MS);

    for (;;) {
        if (xQueueReceive(detection_queue, &det, queue_timeout) == pdTRUE) {
            s_ble_count++;

            /* Run Bayesian sensor fusion */
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
                slot->latitude = det.latitude;
                slot->longitude = det.longitude;
                slot->altitude_m = det.altitude_m;
                slot->speed_mps = det.speed_mps;
            }
            portEXIT_CRITICAL(&s_cache_lock);

            /* Print to console */
            console_send_detection(&det);
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

            uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() /
                                           configTICK_RATE_HZ);
            console_send_status(s_ble_count, uptime_s);
            led_set_pattern(LED_SCANNING);
        }
    }
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void console_output_start(QueueHandle_t detection_queue)
{
    xTaskCreatePinnedToCore(
        console_output_task,
        "console_tx",
        CONSOLE_TX_TASK_STACK_SIZE,
        (void *)detection_queue,
        CONSOLE_TX_TASK_PRIORITY,
        NULL,
        CONSOLE_TX_TASK_CORE
    );

    ESP_LOGI(TAG, "Console output task created on core %d, priority %d",
             CONSOLE_TX_TASK_CORE, CONSOLE_TX_TASK_PRIORITY);

#if CONFIG_FOF_GLASSES_DETECTION
    /* Start glasses output task if queue is attached */
    if (s_glasses_queue != NULL) {
        xTaskCreatePinnedToCore(
            glasses_output_task,
            "glasses_tx",
            3072,
            NULL,
            CONSOLE_TX_TASK_PRIORITY,
            NULL,
            CONSOLE_TX_TASK_CORE
        );
        ESP_LOGI(TAG, "Glasses output task created");
    }
#endif
}

int console_output_get_ble_count(void)
{
    return s_ble_count;
}

int console_output_get_total_count(void)
{
    return s_ble_count;
}

int console_output_get_cached_detections(scanner_detection_summary_t *out, int max_count)
{
    if (!out || max_count <= 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_cache_lock);
    int count = s_det_cache_count < max_count ? s_det_cache_count : max_count;
    memcpy(out, s_det_cache, count * sizeof(scanner_detection_summary_t));
    portEXIT_CRITICAL(&s_cache_lock);

    /* Insertion sort by timestamp descending (most recent first) */
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

/* ── Glasses detection support ─────────────────────────────────────────── */

#if CONFIG_FOF_GLASSES_DETECTION

static QueueHandle_t s_glasses_queue = NULL;
static glasses_detection_t s_glasses_cache[GLASSES_CACHE_SIZE];
static int s_glasses_cache_count = 0;
static portMUX_TYPE s_glasses_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_glasses_count = 0;

void console_output_set_glasses_queue(QueueHandle_t glasses_queue)
{
    s_glasses_queue = glasses_queue;
}

static void console_send_glasses(const glasses_detection_t *g)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "type", "glasses");
    cJSON_AddStringToObject(root, "device", g->device_name[0] ? g->device_name : g->device_type);
    cJSON_AddStringToObject(root, "device_type", g->device_type);
    cJSON_AddStringToObject(root, "manufacturer", g->manufacturer);
    cJSON_AddBoolToObject(root, "has_camera", g->has_camera);
    cJSON_AddNumberToObject(root, "rssi", g->rssi);
    cJSON_AddNumberToObject(root, "confidence", g->confidence);
    cJSON_AddStringToObject(root, "match", g->match_reason);
    cJSON_AddNumberToObject(root, "ts", (double)g->last_seen_ms);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             g->mac[5], g->mac[4], g->mac[3], g->mac[2], g->mac[1], g->mac[0]);
    cJSON_AddStringToObject(root, "mac", mac_str);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        printf("%s\n", json_str);
        cJSON_free(json_str);
    }
}

/**
 * Glasses output task — polls the glasses queue and outputs JSON.
 * Runs as a lightweight task alongside the main console_output_task.
 */
static void glasses_output_task(void *arg)
{
    glasses_detection_t g;
    const TickType_t timeout = pdMS_TO_TICKS(200);

    for (;;) {
        if (s_glasses_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (xQueueReceive(s_glasses_queue, &g, timeout) == pdTRUE) {
#if defined(CONFIG_FOF_GLASSES_ALERT_RSSI)
            if (g.rssi < CONFIG_FOF_GLASSES_ALERT_RSSI) {
                continue;  /* Too far away — skip */
            }
#endif
            s_glasses_count++;

            /* Cache for OLED display */
            portENTER_CRITICAL(&s_glasses_lock);
            {
                /* Find existing entry by MAC or allocate new */
                int idx = -1;
                for (int i = 0; i < s_glasses_cache_count; i++) {
                    if (memcmp(s_glasses_cache[i].mac, g.mac, 6) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx < 0) {
                    if (s_glasses_cache_count < GLASSES_CACHE_SIZE) {
                        idx = s_glasses_cache_count++;
                    } else {
                        /* Evict oldest */
                        int oldest = 0;
                        for (int i = 1; i < s_glasses_cache_count; i++) {
                            if (s_glasses_cache[i].last_seen_ms < s_glasses_cache[oldest].last_seen_ms) {
                                oldest = i;
                            }
                        }
                        idx = oldest;
                    }
                }
                s_glasses_cache[idx] = g;
            }
            portEXIT_CRITICAL(&s_glasses_lock);

            console_send_glasses(&g);
            led_set_pattern(LED_DETECTION);

            ESP_LOGI(TAG, "GLASSES: %s (%s) RSSI=%d cam=%s [%s]",
                     g.device_type, g.manufacturer, g.rssi,
                     g.has_camera ? "YES" : "no", g.match_reason);
        }
    }
}

int console_output_get_cached_glasses(glasses_detection_t *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    portENTER_CRITICAL(&s_glasses_lock);
    int count = s_glasses_cache_count < max_count ? s_glasses_cache_count : max_count;
    memcpy(out, s_glasses_cache, count * sizeof(glasses_detection_t));
    portEXIT_CRITICAL(&s_glasses_lock);

    return count;
}

int console_output_get_glasses_count(void)
{
    return s_glasses_count;
}

#endif /* CONFIG_FOF_GLASSES_DETECTION */
