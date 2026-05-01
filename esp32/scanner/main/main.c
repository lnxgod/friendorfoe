/**
 * Friend or Foe -- WiFi + BLE Scanner Firmware Entry Point
 *
 * Dual-mode scanner: WiFi promiscuous + BLE Remote ID + privacy device
 * detection. BOOT button double-tap switches OLED between drone view
 * and privacy/glasses view.
 *
 * Architecture:
 *   Core 0 (radio):      WiFi scan task + BLE scan (NimBLE)
 *   Core 1 (processing): UART TX task (fusion runs inline)
 *
 * Detection flow:
 *   WiFi scanner ──▶ detection_queue ──▶ UART TX task
 *                         (50 items)       │
 *                                          ├── Bayesian fusion
 *                                          └── JSON → UART1 → Uplink
 *   BLE scanner  ──▶ detection_queue (drones)
 *                ──▶ glasses_queue   (privacy devices → OLED)
 */

#include "wifi_scanner.h"
#include "bayesian_fusion.h"
#include "uart_tx.h"
#include "detection_types.h"
#include "uart_protocol.h"
#include "task_priorities.h"
#include "calibration_mode.h"
#include "scanner_rollback.h"
#include "led_status.h"
#include "oled_display.h"
#include "ble_remote_id.h"
#include "time_sync_policy.h"
#include "comms/uart_ota.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "psram_alloc.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_scanner";

#include "version.h"

#if defined(WIFI_SCANNER_ONLY) || defined(BLE_SCANNER_ONLY) || !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Supported FoF scanner firmware is ESP32-S3 combo/seed only."
#endif

#define FIRMWARE_NAME "scanner"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   250

/* ── BOOT button ────────────────────────────────────────────────────────── */

#define BOOT_BUTTON_GPIO    GPIO_NUM_9   /* ESP32-S3 BOOT = GPIO9 */
#define LONG_PRESS_MS       1500
#define DOUBLE_TAP_WINDOW_MS 600

/* Button state */
static bool     s_btn_was_pressed = false;
static int64_t  s_btn_press_start = 0;
static int      s_tap_count       = 0;
static int64_t  s_last_tap_time   = 0;

/* Detected events */
static volatile bool s_button_short  = false;
static volatile bool s_button_double = false;

/* ── Wall-clock time offset (v0.60+) ─────────────────────────────────────
 * Scanners don't have a network stack and can't NTP themselves. The uplink
 * pushes its epoch-ms clock over UART every 10 s as {"type":"time","ms":N};
 * the listener below stores the offset vs local esp_timer_get_time(). The
 * serializer in uart_tx adds this offset to every detection's ts field so
 * all nodes emit a common epoch timeline for triangulation clustering.
 * Stays at 0 until the first sync arrives (detections then carry uptime-ms,
 * which the backend ignores via the "> 1700000000" validity check). */
volatile int64_t g_epoch_offset_ms = 0;

/* Counter — increments on every {"type":"time"} message received, regardless
 * of whether the epoch_ms was usable. Reported via scanner_info → uplink
 * → /detections/nodes/status to diagnose UART path vs HTTP-fetch failure
 * without serial console access. */
volatile uint32_t g_time_msg_count = 0;
volatile uint32_t g_time_valid_count = 0;
volatile int64_t g_last_valid_time_local_ms = 0;
volatile uint32_t g_cmd_msg_count = 0;
volatile uint32_t g_cmd_parse_error_count = 0;
volatile uint32_t g_cmd_overflow_count = 0;
volatile uint32_t g_cmd_stale_count = 0;
volatile int64_t g_last_cmd_local_ms = 0;
static volatile int64_t s_uart_cmd_last_loop_ms = 0;
static char s_fw_ready_target[32] = {0};

#define FW_CHECK_DAILY_INTERVAL_MS     (24LL * 60LL * 60LL * 1000LL)
#define FW_CHECK_JITTER_MAX_MS         (60LL * 60LL * 1000LL)

static void maybe_expire_time_sync(int64_t now_ms)
{
    if (g_epoch_offset_ms != 0 &&
        fof_time_offset_is_stale(g_last_valid_time_local_ms,
                                 now_ms,
                                 FOF_TIME_SYNC_STALE_AFTER_MS)) {
        ESP_LOGW(TAG, "TIME SYNC stale after %lld ms — clearing epoch offset",
                 (long long)(now_ms - g_last_valid_time_local_ms));
        g_epoch_offset_ms = 0;
    }
}

/* ── Glasses detection cache ────────────────────────────────────────────── */

#if CONFIG_FOF_GLASSES_DETECTION
#define GLASSES_CACHE_SIZE  10
#define GLASSES_STALE_MS    60000

static QueueHandle_t s_glasses_queue = NULL;

typedef struct {
    glasses_detection_t det;
    bool                occupied;
} glasses_cache_entry_t;

static glasses_cache_entry_t s_glasses_cache[GLASSES_CACHE_SIZE];
static int s_glasses_count = 0;

static void glasses_cache_update(void)
{
    /* Drain queue into cache */
    glasses_detection_t gdet;
    while (xQueueReceive(s_glasses_queue, &gdet, 0) == pdTRUE) {
        /* Find existing entry by MAC or empty slot */
        int slot = -1;
        int oldest_slot = -1;
        int64_t oldest_time = INT64_MAX;

        for (int i = 0; i < GLASSES_CACHE_SIZE; i++) {
            if (s_glasses_cache[i].occupied &&
                memcmp(s_glasses_cache[i].det.mac, gdet.mac, 6) == 0) {
                slot = i;
                break;
            }
            if (!s_glasses_cache[i].occupied && slot < 0) {
                slot = i;
            }
            if (s_glasses_cache[i].occupied &&
                s_glasses_cache[i].det.last_seen_ms < oldest_time) {
                oldest_time = s_glasses_cache[i].det.last_seen_ms;
                oldest_slot = i;
            }
        }

        if (slot < 0) slot = oldest_slot;  /* evict oldest */
        if (slot >= 0) {
            s_glasses_cache[slot].det = gdet;
            s_glasses_cache[slot].occupied = true;
        }
    }

    /* Prune stale entries */
    int64_t now_ms = esp_timer_get_time() / 1000;
    s_glasses_count = 0;
    for (int i = 0; i < GLASSES_CACHE_SIZE; i++) {
        if (s_glasses_cache[i].occupied) {
            if ((now_ms - s_glasses_cache[i].det.last_seen_ms) > GLASSES_STALE_MS) {
                s_glasses_cache[i].occupied = false;
            } else {
                s_glasses_count++;
            }
        }
    }
}

static glasses_detection_t *glasses_cache_get(int index)
{
    int count = 0;
    for (int i = 0; i < GLASSES_CACHE_SIZE; i++) {
        if (s_glasses_cache[i].occupied) {
            if (count == index) return &s_glasses_cache[i].det;
            count++;
        }
    }
    return NULL;
}
#endif /* CONFIG_FOF_GLASSES_DETECTION */

/* ── View state ─────────────────────────────────────────────────────────── */

static bool s_show_privacy = false;  /* false = drone view, true = privacy view */

/* ── Button polling ─────────────────────────────────────────────────────── */

static void poll_button(void)
{
    int level = gpio_get_level(BOOT_BUTTON_GPIO);
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (level == 0 && !s_btn_was_pressed) {
        /* Button just pressed (active low) */
        s_btn_was_pressed = true;
        s_btn_press_start = now_ms;
    } else if (level == 1 && s_btn_was_pressed) {
        /* Button just released */
        s_btn_was_pressed = false;
        int64_t duration = now_ms - s_btn_press_start;

        if (duration < LONG_PRESS_MS) {
            /* Short press = tap */
            s_tap_count++;
            s_last_tap_time = now_ms;
        }
    }

    /* Check for completed tap sequences */
    if (s_tap_count > 0 && !s_btn_was_pressed &&
        (now_ms - s_last_tap_time) > DOUBLE_TAP_WINDOW_MS) {
        if (s_tap_count >= 2) {
            s_button_double = true;
        } else {
            s_button_short = true;
        }
        s_tap_count = 0;
    }
}

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started (BOOT: tap=scroll, 2x=view toggle)");

    int page_index = 0;
    int cycle_counter = 0;

    /* Advance page every 10 ticks = 2.5s at 250ms refresh */
    #define PAGE_CYCLE_TICKS 10

    while (1) {
        /* Poll button every cycle */
        poll_button();

        /* Handle double-tap: toggle view */
        if (s_button_double) {
            s_button_double = false;
            s_show_privacy = !s_show_privacy;
            page_index = 0;
            cycle_counter = 0;
            ESP_LOGI(TAG, "View switched to %s", s_show_privacy ? "PRIVACY" : "DRONES");
        }

        /* Handle single tap: scroll page */
        if (s_button_short) {
            s_button_short = false;
            page_index++;
            cycle_counter = 0;
        }

        uint32_t uptime = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

#if CONFIG_FOF_GLASSES_DETECTION
        if (s_show_privacy) {
            /* ── Privacy view ──────────────────────────────────────────── */
            glasses_cache_update();

            if (s_glasses_count > 0) {
                if (page_index >= s_glasses_count) page_index = 0;

                glasses_detection_t *g = glasses_cache_get(page_index);
                if (g) {
                    oled_show_glasses_paged(
                        g->device_type, g->manufacturer,
                        g->device_name, g->confidence,
                        g->rssi, g->has_camera,
                        page_index + 1, s_glasses_count);
                }

                cycle_counter++;
                if (cycle_counter >= PAGE_CYCLE_TICKS) {
                    cycle_counter = 0;
                    page_index = (page_index + 1) % s_glasses_count;
                }
            } else {
                oled_show_privacy_status(0, uptime);
            }
        } else
#endif
        {
            /* ── Drone view (default) ──────────────────────────────────── */
            int total       = uart_tx_get_total_count();
            int active      = bayesian_fusion_get_active_count();
            uint8_t channel = uart_tx_get_current_channel();
            int ble         = uart_tx_get_ble_count();
            int wifi        = uart_tx_get_wifi_count();

            oled_update(total, active, channel, ble, wifi, uptime);

            scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
            int det_count = uart_tx_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

            if (det_count > 0) {
                if (page_index >= det_count) page_index = 0;

                oled_show_detection_paged(
                    det_list[page_index].drone_id,
                    det_list[page_index].manufacturer,
                    det_list[page_index].confidence,
                    det_list[page_index].rssi,
                    page_index + 1, det_count);

                cycle_counter++;
                if (cycle_counter >= PAGE_CYCLE_TICKS) {
                    cycle_counter = 0;
                    page_index = (page_index + 1) % det_count;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

/* ── UART command listener (lock-on from uplink) ──────────────────────── */

static void send_cal_mode_ack(bool ok_flag)
{
    char ack[192];
    snprintf(
        ack,
        sizeof(ack),
        "{\"type\":\"%s\",\"ok\":%s,\"session_id\":\"%s\",\"scan_mode\":\"%s\",\"calibration_uuid\":\"%s\"}",
        MSG_TYPE_CAL_MODE_ACK,
        ok_flag ? "true" : "false",
        scanner_calibration_mode_session_id(),
        scanner_calibration_mode_label(),
        scanner_calibration_mode_uuid()
    );
    uart_tx_send_raw_json(ack);
}

static void send_fw_check(const char *board, const char *caps, const char *reason)
{
    uart_tx_note_firmware_check();
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"%s\",\"board\":\"%s\",\"ver\":\"%s\","
             "\"caps\":\"%s\",\"fw_state\":\"%s\",\"fw_check_count\":%lu,"
             "\"last_fw_error\":\"%s\",\"reason\":\"%s\"}",
             MSG_TYPE_FW_CHECK,
             board ? board : "?",
             FOF_VERSION,
             caps ? caps : "?",
             uart_tx_firmware_update_state(),
             (unsigned long)uart_tx_firmware_check_count(),
             uart_tx_firmware_last_error(),
             reason ? reason : "periodic");
    uart_tx_send_raw_json(msg);
}

static void handle_fw_offer(cJSON *root, const char *board)
{
    const cJSON *update_j = cJSON_GetObjectItem(root, JSON_KEY_FW_UPDATE);
    bool update = (update_j && cJSON_IsTrue(update_j)) ||
                  (update_j && cJSON_IsNumber(update_j) && update_j->valueint != 0);
    const cJSON *target_j = cJSON_GetObjectItem(root, JSON_KEY_FW_TARGET_VERSION);
    const cJSON *name_j = cJSON_GetObjectItem(root, JSON_KEY_FW_NAME);
    const cJSON *size_j = cJSON_GetObjectItem(root, JSON_KEY_FW_SIZE);
    const cJSON *crc_j = cJSON_GetObjectItem(root, JSON_KEY_FW_CRC32);
    const char *target = (target_j && target_j->valuestring) ? target_j->valuestring : "";
    const char *fw_name = (name_j && name_j->valuestring) ? name_j->valuestring : "";
    uint32_t size = size_j ? (uint32_t)size_j->valuedouble : 0;
    uint32_t crc = crc_j ? (uint32_t)crc_j->valuedouble : 0;

    if (!update) {
        uart_tx_set_firmware_update_state(false, "", "current");
        s_fw_ready_target[0] = '\0';
        ESP_LOGI(TAG, "Firmware check: scanner is current");
        return;
    }

    if (fw_name[0] && board && board[0] && strcmp(fw_name, board) != 0) {
        uart_tx_set_firmware_update_state(false, "", "error");
        uart_tx_set_firmware_error("board_mismatch");
        ESP_LOGW(TAG, "Rejected firmware offer: board mismatch offer=%s scanner=%s",
                 fw_name, board);
        return;
    }
    if (!target[0] || strcmp(target, FOF_VERSION) == 0) {
        uart_tx_set_firmware_update_state(false, "", "current");
        ESP_LOGI(TAG, "Rejected firmware offer: target is current or missing");
        return;
    }
    if (size == 0 || crc == 0) {
        uart_tx_set_firmware_update_state(true, target, "error");
        uart_tx_set_firmware_error(size == 0 ? "missing_size" : "missing_crc");
        ESP_LOGW(TAG, "Rejected firmware offer: missing integrity metadata size=%lu crc=%08lX",
                 (unsigned long)size, (unsigned long)crc);
        return;
    }
    if (uart_tx_firmware_backoff_active()) {
        uart_tx_set_firmware_update_state(true, target, "deferred");
        ESP_LOGW(TAG, "Firmware offer deferred during error backoff: target=%s error=%s",
                 target, uart_tx_firmware_last_error());
        return;
    }

    uart_tx_set_firmware_update_state(true, target, "offered");
    ESP_LOGW(TAG, "Firmware update offered: current=%s target=%s size=%lu crc=%08lX",
             FOF_VERSION,
             target[0] ? target : "?",
             (unsigned long)size,
             (unsigned long)crc);

    if (target[0] && strcmp(s_fw_ready_target, target) == 0) {
        uart_tx_set_firmware_update_state(true, target, "deferred");
        ESP_LOGW(TAG, "Firmware target %s was already requested; leaving TX enabled",
                 target);
        return;
    }

    /* Scanner owns the quiet transition. This avoids forcing stop_ack through
     * a busy detection stream; once fw_ready is emitted, only the command
     * listener remains active to receive ota_begin and binary chunks. */
    uart_tx_flush_detection_queue();
    uart_tx_set_enabled(false);
    uart_tx_set_firmware_update_state(true, target, "ready");
    if (target[0]) {
        strncpy(s_fw_ready_target, target, sizeof(s_fw_ready_target) - 1);
        s_fw_ready_target[sizeof(s_fw_ready_target) - 1] = '\0';
    }

    char ready[224];
    snprintf(ready, sizeof(ready),
             "{\"type\":\"%s\",\"board\":\"%s\",\"ver\":\"%s\","
             "\"target_ver\":\"%s\",\"size\":%lu,\"crc\":%lu}",
             MSG_TYPE_FW_READY,
             board ? board : "?",
             FOF_VERSION,
             target[0] ? target : "?",
             (unsigned long)size,
             (unsigned long)crc);
    uart_tx_send_raw_json(ready);
}

static void uart_cmd_listener_task(void *arg)
{
    uint8_t buf[256];
    char line[256];
    int line_pos = 0;
    TickType_t line_started_tick = 0;

    ESP_LOGI(TAG, "UART cmd listener on UART1 (commands + OTA from uplink)");

    /* Determine board identity at compile time — matches firmware catalog names */
#if defined(SEED_SCANNER_PINS)
    static const char *s_board_name = "scanner-s3-combo-seed";
    static const char *s_chip_name = "esp32s3";
    static const char *s_caps = "ble,wifi";
#else
    static const char *s_board_name = "scanner-s3-combo";
    static const char *s_chip_name = "esp32s3";
    static const char *s_caps = "ble,wifi";
#endif

    /* Send scanner identity immediately on boot — uplink needs this to know
     * what's connected, even before sending "ready". This is a small JSON
     * message, not a data flood. */
    {
        /* Use FOF_VERSION (shared/version.h) rather than the ESP-IDF app
         * version — FOF_VERSION is the single source of truth we bump in
         * lockstep across uplink + scanner. */
        uart_tx_send_scanner_info(FOF_VERSION, s_board_name, s_chip_name, s_caps);
        ESP_LOGI(TAG, "Sent identity: %s v%s (%s) — waiting for uplink start command",
                 s_board_name, FOF_VERSION, s_caps);
    }

    TickType_t last_info_send = xTaskGetTickCount();
    int64_t next_fw_check_ms =
        (esp_timer_get_time() / 1000) + FW_CHECK_DAILY_INTERVAL_MS +
        (int64_t)(esp_random() % FW_CHECK_JITTER_MAX_MS);
    send_fw_check(s_board_name, s_caps, "boot");

    while (1) {
        int64_t loop_now_ms = esp_timer_get_time() / 1000;
        s_uart_cmd_last_loop_ms = loop_now_ms;
        maybe_expire_time_sync(loop_now_ms);
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(50));

        /* Resend scanner_info every 10s — always, even when TX is stopped.
         * This lets the uplink see us and know our version/capabilities.
         * Also sends TX state so uplink knows if we're waiting or active. */
        /* Suppress all UART TX during OTA — scanner must be silent so the
         * UART is fully dedicated to receiving firmware chunks. */
        if (!uart_ota_is_active() &&
            (xTaskGetTickCount() - last_info_send) >= pdMS_TO_TICKS(10000)) {
            last_info_send = xTaskGetTickCount();
            uart_tx_send_scanner_info(FOF_VERSION, s_board_name, s_chip_name, s_caps);
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms >= next_fw_check_ms && !uart_tx_firmware_backoff_active()) {
                next_fw_check_ms = now_ms + FW_CHECK_DAILY_INTERVAL_MS +
                                   (int64_t)(esp_random() % FW_CHECK_JITTER_MAX_MS);
                send_fw_check(s_board_name, s_caps, "daily");
            }
            /* Send status even when stopped — uplink can see we're alive */
            if (!uart_tx_is_enabled()) {
                ESP_LOGD(TAG, "Scanner waiting for start command (TX disabled)");
            }
        }

        if (len <= 0) {
            if (line_pos > 0 && line_started_tick != 0 &&
                (xTaskGetTickCount() - line_started_tick) >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "UART CMD stale partial line (%d bytes), resetting", line_pos);
                g_cmd_stale_count++;
                line_pos = 0;
                line_started_tick = 0;
            }
            continue;
        }

        /* During OTA: route raw bytes to OTA receiver */
        if (uart_ota_is_active()) {
            uart_ota_process_data(buf, len);
            continue;
        }

        ESP_LOGD(TAG, "UART CMD RX: %d bytes [%02X %02X %02X %02X...]",
                 len, buf[0], len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0);

        for (int i = 0; i < len; i++) {
            if (buf[i] == '\r') {
                continue;
            }
            if (buf[i] == '\n') {
                if (line_pos > 0) {
                    line[line_pos] = '\0';
                    /* Parse JSON command */
                    ESP_LOGD(TAG, "UART CMD LINE (%d chars): '%.*s'",
                             line_pos, line_pos > 48 ? 48 : line_pos, line);
                    cJSON *root = cJSON_Parse(line);
                    if (root) {
                        g_cmd_msg_count++;
                        g_last_cmd_local_ms = esp_timer_get_time() / 1000;
                        const char *type = NULL;
                        cJSON *t = cJSON_GetObjectItem(root, "type");
                        if (t && t->valuestring) type = t->valuestring;
                        ESP_LOGD(TAG, "UART CMD TYPE: '%s'", type ? type : "(null)");

                        if (type && (strcmp(type, "ready") == 0 || strcmp(type, "start") == 0)) {
                            /* Uplink tells scanner to start transmitting */
                            extern void uart_tx_set_enabled(bool enabled);
                            uart_tx_set_enabled(true);
                            if (uart_tx_firmware_update_needed()) {
                                uart_tx_set_firmware_update_state(
                                    true,
                                    uart_tx_firmware_target_version(),
                                    "deferred"
                                );
                            }
                            ESP_LOGI(TAG, "Uplink sent START — TX enabled");

                        } else if (type && strcmp(type, "stop") == 0) {
                            /* Uplink tells scanner to stop transmitting.
                             * Emit stop_ack so the uplink's relay handler
                             * knows our TX loop is halted before it starts
                             * the OTA begin handshake (v0.59+). */
                            extern void uart_tx_set_enabled(bool enabled);
                            extern void uart_tx_send_raw_json(const char *json_str);
                            uart_tx_set_enabled(false);
                            uart_tx_send_raw_json("{\"type\":\"stop_ack\"}");
                            ESP_LOGI(TAG, "Uplink sent STOP — TX disabled, ack sent");

                        } else if (type && strcmp(type, MSG_TYPE_FW_OFFER) == 0) {
                            handle_fw_offer(root, s_board_name);

                        } else if (type && strcmp(type, MSG_TYPE_FW_CHECK_NOW) == 0) {
                            uart_tx_clear_firmware_error();
                            next_fw_check_ms = (esp_timer_get_time() / 1000) +
                                               FW_CHECK_DAILY_INTERVAL_MS +
                                               (int64_t)(esp_random() % FW_CHECK_JITTER_MAX_MS);
                            send_fw_check(s_board_name, s_caps, "manual");

                        } else if (type && strcmp(type, "lockon") == 0) {
                            if (scanner_calibration_mode_is_active()) {
                                ESP_LOGW(TAG, "Rejecting WiFi lock-on while calibration mode is active");
                                cJSON_Delete(root);
                                line_pos = 0;
                                continue;
                            }
                            cJSON *ch = cJSON_GetObjectItem(root, "ch");
                            cJSON *dur = cJSON_GetObjectItem(root, "dur");
                            cJSON *bssid = cJSON_GetObjectItem(root, "bssid");
                            int channel = ch ? ch->valueint : 6;
                            int duration = dur ? dur->valueint : 60;
                            const char *bssid_str = (bssid && bssid->valuestring) ? bssid->valuestring : NULL;

                            ESP_LOGW(TAG, "WiFi LOCK-ON: ch=%d dur=%ds bssid=%s",
                                     channel, duration, bssid_str ? bssid_str : "*");
                            wifi_scanner_lockon((uint8_t)channel, bssid_str, duration);

                        } else if (type && strcmp(type, "lockon_cancel") == 0) {
                            if (scanner_calibration_mode_is_active()) {
                                ESP_LOGW(TAG, "Ignoring WiFi lock-on cancel while calibration mode is active");
                                cJSON_Delete(root);
                                line_pos = 0;
                                continue;
                            }
                            ESP_LOGI(TAG, "WiFi LOCK-ON cancel");
                            wifi_scanner_lockon_cancel();

                        } else if (type && strcmp(type, "ble_lockon") == 0) {
                            if (scanner_calibration_mode_is_active()) {
                                ESP_LOGW(TAG, "Rejecting BLE focus while calibration mode is active");
                                cJSON_Delete(root);
                                line_pos = 0;
                                continue;
                            }
                            cJSON *mac_j = cJSON_GetObjectItem(root, "mac");
                            cJSON *dur = cJSON_GetObjectItem(root, "dur");
                            int duration = dur ? dur->valueint : 45;
                            if (mac_j && mac_j->valuestring && strlen(mac_j->valuestring) >= 17) {
                                uint8_t mac[6];
                                unsigned int m[6];
                                if (sscanf(mac_j->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
                                           &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                                    for (int j = 0; j < 6; j++) mac[j] = (uint8_t)m[j];
                                    ESP_LOGW(TAG, "BLE FOCUS: %s dur=%ds", mac_j->valuestring, duration);
                                    ble_rid_lockon(mac, duration);
                                }
                            }
                        } else if (type && strcmp(type, "ble_lockon_cancel") == 0) {
                            if (scanner_calibration_mode_is_active()) {
                                ESP_LOGW(TAG, "Ignoring BLE focus cancel while calibration mode is active");
                                cJSON_Delete(root);
                                line_pos = 0;
                                continue;
                            }
                            ESP_LOGI(TAG, "BLE FOCUS cancel");
                            ble_rid_lockon_cancel();

                        } else if (type && strcmp(type, MSG_TYPE_CAL_MODE_START) == 0) {
                            cJSON *session_j = cJSON_GetObjectItem(root, JSON_KEY_SESSION_ID);
                            cJSON *uuid_j = cJSON_GetObjectItem(root, JSON_KEY_CALIBRATION_UUID);
                            bool ok = false;
                            if (session_j && session_j->valuestring &&
                                uuid_j && uuid_j->valuestring) {
                                ok = scanner_calibration_mode_start(
                                    session_j->valuestring,
                                    uuid_j->valuestring
                                );
                            }
                            send_cal_mode_ack(ok);
                            ESP_LOGW(TAG, "Calibration mode %s for session=%s uuid=%s",
                                     ok ? "armed" : "rejected",
                                     session_j && session_j->valuestring ? session_j->valuestring : "?",
                                     uuid_j && uuid_j->valuestring ? uuid_j->valuestring : "?");

                        } else if (type && strcmp(type, MSG_TYPE_CAL_MODE_STOP) == 0) {
                            scanner_calibration_mode_stop("uplink_stop");
                            send_cal_mode_ack(true);

                        } else if (type && strcmp(type, "scan_profile") == 0) {
                            cJSON *profile_j = cJSON_GetObjectItem(root, JSON_KEY_SCAN_PROFILE);
                            if (!profile_j) {
                                profile_j = cJSON_GetObjectItem(root, "profile");
                            }
                            const char *profile = (profile_j && profile_j->valuestring)
                                ? profile_j->valuestring
                                : "hybrid_failover";
                            scanner_scan_profile_set(profile);
                            char ack[96];
                            snprintf(ack, sizeof(ack),
                                     "{\"type\":\"scan_profile_ack\",\"scan_profile\":\"%s\"}",
                                     scanner_scan_profile_label());
                            uart_tx_send_raw_json(ack);

                        } else if (type && strcmp(type, MSG_TYPE_TIME) == 0) {
                            /* Uplink broadcasts its epoch-ms every 10s.
                             * Use a small flag so /api/status can show that we
                             * received SOMETHING even if the value was bad —
                             * helps diagnose UART vs HTTP-fetch failure. */
                            extern volatile int64_t g_epoch_offset_ms;
                            extern volatile uint32_t g_time_msg_count;
                            extern volatile uint32_t g_time_valid_count;
                            extern volatile int64_t g_last_valid_time_local_ms;
                            g_time_msg_count++;
                            cJSON *ms_j = cJSON_GetObjectItem(root, JSON_KEY_EPOCH_MS);
                            cJSON *ok_j = cJSON_GetObjectItem(root, JSON_KEY_TIME_OK);
                            bool has_ok = ok_j && (cJSON_IsBool(ok_j) || cJSON_IsNumber(ok_j));
                            bool ok = cJSON_IsTrue(ok_j) || (cJSON_IsNumber(ok_j) && ok_j->valueint != 0);
                            if (ms_j && cJSON_IsNumber(ms_j)) {
                                int64_t epoch_ms = (int64_t)ms_j->valuedouble;
                                if (fof_time_message_is_valid(has_ok, ok, epoch_ms)) {
                                    int64_t local_ms = esp_timer_get_time() / 1000;
                                    g_epoch_offset_ms = epoch_ms - local_ms;
                                    g_time_valid_count++;
                                    g_last_valid_time_local_ms = local_ms;
                                    ESP_LOGD(TAG, "TIME SYNC: epoch=%lld local=%lld offset=%lld",
                                             (long long)epoch_ms, (long long)local_ms,
                                             (long long)g_epoch_offset_ms);
                                } else {
                                    ESP_LOGW(TAG, "TIME SYNC rejected: epoch_ms=%lld ok=%s",
                                             (long long)epoch_ms,
                                             has_ok ? (ok ? "true" : "false") : "missing");
                                }
                            }

                        } else if (type && strcmp(type, MSG_TYPE_OTA_BEGIN) == 0) {
                            /* UART OTA: receive firmware from uplink */
                            uart_tx_set_firmware_update_state(
                                true,
                                uart_tx_firmware_target_version(),
                                "updating"
                            );
                            cJSON *sz = cJSON_GetObjectItem(root, "size");
                            cJSON *crc_j = cJSON_GetObjectItem(root, "crc");
                            uint32_t total = sz ? (uint32_t)sz->valueint : 0;
                            uint32_t expected_crc = crc_j ? (uint32_t)crc_j->valuedouble : 0;
                            bool has_crc = (crc_j != NULL);
                            if (total > 0) {
                                ESP_LOGW(TAG, "UART OTA begin: %lu bytes, crc=%s%08lX",
                                         (unsigned long)total,
                                         has_crc ? "" : "none/",
                                         (unsigned long)expected_crc);
                                if (!uart_ota_begin(total, expected_crc, has_crc, UART_NUM_1)) {
                                    uart_tx_set_firmware_update_state(
                                        true,
                                        uart_tx_firmware_target_version(),
                                        "error"
                                    );
                                    uart_tx_set_firmware_error("ota_begin_failed");
                                }
                            } else {
                                uart_tx_set_firmware_update_state(
                                    true,
                                    uart_tx_firmware_target_version(),
                                    "error"
                                );
                                uart_tx_set_firmware_error("bad_size");
                            }
                        } else if (type && strcmp(type, MSG_TYPE_OTA_END) == 0) {
                            ESP_LOGI(TAG, "UART OTA finalize");
                            if (!uart_ota_finalize()) {
                                uart_tx_set_firmware_update_state(
                                    true,
                                    uart_tx_firmware_target_version(),
                                    "error"
                                );
                                uart_tx_set_firmware_error("finalize_failed");
                            }
                        } else if (type && strcmp(type, MSG_TYPE_OTA_ABORT) == 0) {
                            ESP_LOGW(TAG, "UART OTA abort");
                            uart_ota_abort();
                            uart_tx_set_firmware_update_state(
                                true,
                                uart_tx_firmware_target_version(),
                                "error"
                            );
                            uart_tx_set_firmware_error("aborted");
                        }

                        cJSON_Delete(root);
                    } else {
                        g_cmd_parse_error_count++;
                        ESP_LOGW(TAG, "UART CMD parse error (%d chars)", line_pos);
                    }
                    line_pos = 0;
                    line_started_tick = 0;
                }
            } else if (line_pos < (int)sizeof(line) - 1) {
                if (line_pos == 0) {
                    line_started_tick = xTaskGetTickCount();
                }
                line[line_pos++] = (char)buf[i];
            } else {
                /* Buffer overflow — reset to prevent corruption */
                ESP_LOGW(TAG, "UART CMD line overflow at %d bytes, resetting (byte=0x%02X)", line_pos, buf[i]);
                g_cmd_overflow_count++;
                line_pos = 0;
                line_started_tick = 0;
            }
        }

    }
}

static void uart_cmd_watchdog_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t last_ms = s_uart_cmd_last_loop_ms;
        if (last_ms > 0 && (now_ms - last_ms) > 30000) {
            char reason[64];
            snprintf(reason, sizeof(reason),
                     "uart_cmd_stale_%lld_ms", (long long)(now_ms - last_ms));
            scanner_rollback_reboot_or_restart(reason);
        }
    }
}

static bool start_uart_command_tasks(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        uart_cmd_listener_task,
        "uart_cmd",
        UART_CMD_TASK_STACK_SIZE,
        NULL,
        UART_CMD_TASK_PRIORITY,
        NULL,
        UART_CMD_TASK_CORE
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create UART command listener (stack=%d, internal_heap=%u)",
                 UART_CMD_TASK_STACK_SIZE,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    ESP_LOGI(TAG, "UART command listener task created on core %d, priority %d",
             UART_CMD_TASK_CORE, UART_CMD_TASK_PRIORITY);

    ok = xTaskCreatePinnedToCore(
        uart_cmd_watchdog_task,
        "uart_cmd_wd",
        UART_CMD_WATCHDOG_TASK_STACK_SIZE,
        NULL,
        UART_CMD_WATCHDOG_TASK_PRIORITY,
        NULL,
        UART_CMD_WATCHDOG_TASK_CORE
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create UART command watchdog (stack=%d, internal_heap=%u)",
                 UART_CMD_WATCHDOG_TASK_STACK_SIZE,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    ESP_LOGI(TAG, "UART command watchdog task created on core %d, priority %d",
             UART_CMD_WATCHDOG_TASK_CORE, UART_CMD_WATCHDOG_TASK_PRIORITY);
    return true;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 0. Machine-readable firmware identification ──────────────────── */
    FOF_PRINT_IDENT(TAG, FIRMWARE_NAME);

    /* Log PSRAM state at boot so it's obvious in serial whether the board
     * actually initialized external memory (N16R8 scanners → 8 MiB). */
    {
        size_t psram_total  = psram_total_size();
        size_t psram_free   = psram_free_size();
        size_t heap_int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (psram_total > 0) {
            ESP_LOGW(TAG, "PSRAM: %u KB total, %u KB free  |  Internal: %u KB free",
                     (unsigned)(psram_total / 1024),
                     (unsigned)(psram_free  / 1024),
                     (unsigned)(heap_int_free / 1024));
        } else {
            ESP_LOGW(TAG, "PSRAM: none  |  Internal: %u KB free",
                     (unsigned)(heap_int_free / 1024));
        }
    }

    /* ── 1. Initialize NVS flash ──────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 1a. Rollback / crash-loop guard — must run before any task that can
     *        crash, so PENDING_VERIFY state and the crash counter are read
     *        from the *previous* boot, not whatever this one ends up doing. */
    scanner_rollback_init();

    /* ── 1b. Initialize TCP/IP network interface (required for radio subsystem) ── */
    ESP_ERROR_CHECK(esp_netif_init());

    /* ── 2. Initialize default event loop ─────────────────────────────── */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ── 3. Create shared detection queue ─────────────────────────────── */
    QueueHandle_t detection_queue = xQueueCreate(DETECTION_QUEUE_LEN,
                                                 sizeof(drone_detection_t));
    if (detection_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create detection queue -- halting");
        return;
    }
    ESP_LOGI(TAG, "Detection queue created (%d slots, %u bytes each)",
             DETECTION_QUEUE_LEN, (unsigned)sizeof(drone_detection_t));

    /* ── 4. Initialize Bayesian fusion engine ─────────────────────────── */
    bayesian_fusion_init();
    ESP_LOGI(TAG, "Bayesian fusion engine initialised");

    /* ── 5. Initialize status LED ────────────────────────────────────── */
    led_init();
    led_set_pattern(LED_BOOT);

    /* ── 5b. Initialize OLED display ─────────────────────────────────── */
    oled_init();
    oled_set_version(FOF_VERSION);
    oled_update(0, 0, 0, 0, 0, 0);

    /* ── 5c. Initialize BOOT button GPIO ─────────────────────────────── */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    ESP_LOGI(TAG, "BOOT button on GPIO%d (tap=scroll, 2x=privacy)", BOOT_BUTTON_GPIO);

    /* ── 6. Initialize UART TX (hardware setup, no task yet) ──────────── */
    uart_tx_init();

    /* ── 7. Set scanner identity before any UART status/check messages ── */
    {
#if defined(SEED_SCANNER_PINS)
        const char *bname = "scanner-s3-combo-seed", *cname = "esp32s3", *caps = "ble,wifi";
#else
        const char *bname = "scanner-s3-combo", *cname = "esp32s3", *caps = "ble,wifi";
#endif
        uart_tx_set_identity(bname, cname, caps);
    }

    /* ── 8. Start command listener early, before radio/display heap use ─ */
    if (!start_uart_command_tasks()) {
        ESP_LOGE(TAG, "UART command path is required for remote firmware recovery; rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        scanner_rollback_reboot_or_restart("uart_cmd_task_create_failed");
    }

    /* ── 8a. After uptime threshold, mark this image VALID and clear the
     *        crash counter. Spawned as a one-shot task so we don't block
     *        boot waiting for the threshold. */
    xTaskCreatePinnedToCore(scanner_rollback_mark_valid_task,
                            "rb_mark_valid",
                            2048, NULL, tskIDLE_PRIORITY + 1, NULL,
                            tskNO_AFFINITY);

    /* ── 9. Initialize WiFi scanner (sets up promiscuous mode) ────────── */
    wifi_scanner_init(detection_queue);
    ESP_LOGI(TAG, "WiFi scanner initialised");

    /* ── 9b. Initialize BLE scanner (NimBLE) ─────────────────────────── */
    ble_remote_id_init(detection_queue);
    ESP_LOGI(TAG, "BLE Remote ID scanner initialised");

#if CONFIG_FOF_GLASSES_DETECTION
    /* ── 9c. Create glasses detection queue and wire to BLE scanner ───── */
    s_glasses_queue = xQueueCreate(10, sizeof(glasses_detection_t));
    if (s_glasses_queue != NULL) {
        ble_remote_id_set_glasses_queue(s_glasses_queue);
        memset(s_glasses_cache, 0, sizeof(s_glasses_cache));
        ESP_LOGI(TAG, "Glasses detection queue created (10 slots)");
    }
#endif

    /* Start UART TX task on Core 1 (processing core).
     * The TX task has a 10s startup delay to let the uplink boot first. */
    uart_tx_start(detection_queue);

    /* ── 10. Start WiFi scanner task on Core 0 (radio core) ──────────── */
    wifi_scanner_start();
    ESP_LOGI(TAG, "WiFi scanner started on core %d, priority %d",
             WIFI_SCAN_TASK_CORE, WIFI_SCAN_TASK_PRIORITY);

    /* ── 10b. Start BLE scanner ──────────────────────────────────────── */
    ble_remote_id_start();
    ESP_LOGI(TAG, "BLE scanner started");

    /* ── 11. Start LED task ──────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_UPLINK_OK);   /* purple — UART active, connected to uplink */

    /* ── 12. Start display task ──────────────────────────────────────── */
    xTaskCreatePinnedToCore(
        display_task,
        "display",
        DISPLAY_TASK_STACK_SIZE,
        NULL,
        DISPLAY_TASK_PRIORITY,
        NULL,
        DISPLAY_TASK_CORE
    );
    ESP_LOGI(TAG, "Display task started on core %d, priority %d",
             DISPLAY_TASK_CORE, DISPLAY_TASK_PRIORITY);

    /* ── 13. Startup banner ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s", FIRMWARE_NAME, FOF_VERSION);
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi + BLE 5");
    ESP_LOGI(TAG, "  UART1 -> Uplink @ %d baud", UART_BAUD_RATE);
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "  BOOT button: tap=scroll, 2x=privacy view");
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
