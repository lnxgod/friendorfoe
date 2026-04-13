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
#include "led_status.h"
#include "oled_display.h"
#include "ble_remote_id.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_scanner";

#include "version.h"

#ifdef WIFI_SCANNER_ONLY
#define FIRMWARE_NAME "wifi-scanner"
#else
#define FIRMWARE_NAME "scanner"
#endif
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   250

/* ── BOOT button ────────────────────────────────────────────────────────── */

#define BOOT_BUTTON_GPIO    GPIO_NUM_9   /* ESP32-C5 BOOT = GPIO9 */
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

#ifndef BLE_SCANNER_ONLY
#include "driver/uart.h"
#include "cJSON.h"
#include "comms/uart_ota.h"
#include "esp_ota_ops.h"

static void uart_cmd_listener_task(void *arg)
{
    uint8_t buf[256];
    char line[256];
    int line_pos = 0;

    ESP_LOGI(TAG, "UART cmd listener on UART1 (commands + OTA from uplink)");

    /* Determine board identity at compile time — matches firmware catalog names */
#if defined(BLE_SCANNER_ONLY)
    static const char *s_board_name = "scanner-s3-ble";
    static const char *s_chip_name = "esp32s3";
    static const char *s_caps = "ble";
#elif defined(WIFI_SCANNER_ONLY) && defined(CONFIG_IDF_TARGET_ESP32C5)
    static const char *s_board_name = "scanner-c5";
    static const char *s_chip_name = "esp32c5";
    static const char *s_caps = "wifi,5ghz";
#elif defined(WIFI_SCANNER_ONLY)
    static const char *s_board_name = "scanner-esp32";
    static const char *s_chip_name = "esp32";
    static const char *s_caps = "wifi";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    static const char *s_board_name = "scanner-s3-combo";
    static const char *s_chip_name = "esp32s3";
    static const char *s_caps = "ble,wifi";
#else
    static const char *s_board_name = "scanner-esp32";
    static const char *s_chip_name = "esp32";
    static const char *s_caps = "wifi";
#endif

    /* Send scanner identity immediately on boot — uplink needs this to know
     * what's connected, even before sending "ready". This is a small JSON
     * message, not a data flood. */
    {
        const esp_app_desc_t *app = esp_app_get_description();
        const char *ver = app ? app->version : "?";
        uart_tx_send_scanner_info(ver, s_board_name, s_chip_name, s_caps);
        ESP_LOGI(TAG, "Sent identity: %s v%s (%s) — waiting for uplink start command",
                 s_board_name, ver, s_caps);
    }

    TickType_t last_info_send = xTaskGetTickCount();

    while (1) {
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(500));

        /* Resend scanner_info every 10s — always, even when TX is stopped.
         * This lets the uplink see us and know our version/capabilities.
         * Also sends TX state so uplink knows if we're waiting or active. */
        if ((xTaskGetTickCount() - last_info_send) >= pdMS_TO_TICKS(10000)) {
            last_info_send = xTaskGetTickCount();
            const esp_app_desc_t *app2 = esp_app_get_description();
            uart_tx_send_scanner_info(app2 ? app2->version : "?",
                                      s_board_name, s_chip_name, s_caps);
            /* Send status even when stopped — uplink can see we're alive */
            if (!uart_tx_is_enabled()) {
                ESP_LOGI(TAG, "Scanner waiting for start command (TX disabled)");
            }
        }

        if (len <= 0) continue;

        /* Log any received data for debugging */
        ESP_LOGI(TAG, "UART CMD RX: %d bytes [%02X %02X %02X %02X...]",
                 len, buf[0], len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0);

        /* During OTA: route raw bytes to OTA receiver */
        if (uart_ota_is_active()) {
            uart_ota_process_data(buf, len);
            continue;
        }

        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                if (line_pos > 0) {
                    line[line_pos] = '\0';
                    /* Parse JSON command */
                    ESP_LOGW(TAG, "UART CMD LINE (%d chars): [%02X %02X %02X %02X] '%.*s'",
                             line_pos,
                             (uint8_t)line[0], line_pos > 1 ? (uint8_t)line[1] : 0,
                             line_pos > 2 ? (uint8_t)line[2] : 0, line_pos > 3 ? (uint8_t)line[3] : 0,
                             line_pos > 40 ? 40 : line_pos, line);
                    cJSON *root = cJSON_Parse(line);
                    if (root) {
                        const char *type = NULL;
                        cJSON *t = cJSON_GetObjectItem(root, "type");
                        if (t && t->valuestring) type = t->valuestring;
                        ESP_LOGW(TAG, "UART CMD TYPE: '%s'", type ? type : "(null)");

                        if (type && (strcmp(type, "ready") == 0 || strcmp(type, "start") == 0)) {
                            /* Uplink tells scanner to start transmitting */
                            extern void uart_tx_set_enabled(bool enabled);
                            uart_tx_set_enabled(true);
                            ESP_LOGI(TAG, "Uplink sent START — TX enabled");

                        } else if (type && strcmp(type, "stop") == 0) {
                            /* Uplink tells scanner to stop transmitting */
                            extern void uart_tx_set_enabled(bool enabled);
                            uart_tx_set_enabled(false);
                            ESP_LOGI(TAG, "Uplink sent STOP — TX disabled");

                        } else if (type && strcmp(type, "lockon") == 0) {
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
                            ESP_LOGI(TAG, "WiFi LOCK-ON cancel");
                            wifi_scanner_lockon_cancel();


#ifndef WIFI_SCANNER_ONLY
                        } else if (type && strcmp(type, "ble_lockon") == 0) {
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
                                    extern void ble_rid_lockon(const uint8_t *mac, int duration_s);
                                    ble_rid_lockon(mac, duration);
                                }
                            }
                        } else if (type && strcmp(type, "ble_lockon_cancel") == 0) {
                            ESP_LOGI(TAG, "BLE FOCUS cancel");
                            extern void ble_rid_lockon_cancel(void);
                            ble_rid_lockon_cancel();
#endif /* !WIFI_SCANNER_ONLY */

                        } else if (type && strcmp(type, MSG_TYPE_OTA_BEGIN) == 0) {
                            /* UART OTA: receive firmware from uplink */
                            cJSON *sz = cJSON_GetObjectItem(root, "size");
                            uint32_t total = sz ? (uint32_t)sz->valueint : 0;
                            if (total > 0) {
                                ESP_LOGW(TAG, "UART OTA begin: %lu bytes", (unsigned long)total);
                                uart_ota_begin(total, UART_NUM_1);
                            }
                        } else if (type && strcmp(type, MSG_TYPE_OTA_END) == 0) {
                            ESP_LOGI(TAG, "UART OTA finalize");
                            uart_ota_finalize();
                        } else if (type && strcmp(type, MSG_TYPE_OTA_ABORT) == 0) {
                            ESP_LOGW(TAG, "UART OTA abort");
                            uart_ota_abort();
                        }

                        cJSON_Delete(root);
                    }
                    line_pos = 0;
                }
            } else if (line_pos < (int)sizeof(line) - 1) {
                line[line_pos++] = (char)buf[i];
            } else {
                /* Buffer overflow — reset to prevent corruption */
                ESP_LOGW(TAG, "UART CMD line overflow at %d bytes, resetting (byte=0x%02X)", line_pos, buf[i]);
                line_pos = 0;
            }
        }

    }
}
#endif /* BLE_SCANNER_ONLY */

/* ── Entry point ────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 0. Machine-readable firmware identification ──────────────────── */
    FOF_PRINT_IDENT(TAG, FIRMWARE_NAME);

    /* ── 1. Initialize NVS flash ──────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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

#ifndef BLE_SCANNER_ONLY
    /* ── 7. Initialize WiFi scanner (sets up promiscuous mode) ────────── */
    wifi_scanner_init(detection_queue);
    ESP_LOGI(TAG, "WiFi scanner initialised");
#else
    ESP_LOGI(TAG, "WiFi scanner DISABLED (BLE-only mode)");
#endif

#ifndef WIFI_SCANNER_ONLY
    /* ── 7b. Initialize BLE scanner (NimBLE) ─────────────────────────── */
    ble_remote_id_init(detection_queue);
    ESP_LOGI(TAG, "BLE Remote ID scanner initialised");

#if CONFIG_FOF_GLASSES_DETECTION
    /* ── 7c. Create glasses detection queue and wire to BLE scanner ───── */
    s_glasses_queue = xQueueCreate(10, sizeof(glasses_detection_t));
    if (s_glasses_queue != NULL) {
        ble_remote_id_set_glasses_queue(s_glasses_queue);
        memset(s_glasses_cache, 0, sizeof(s_glasses_cache));
        ESP_LOGI(TAG, "Glasses detection queue created (10 slots)");
    }
#endif
#else
    ESP_LOGI(TAG, "BLE scanner DISABLED (WiFi-only mode)");
#endif

    /* Start UART TX task on Core 1 (processing core).
     * The TX task has a 10s startup delay to let the uplink boot first. */
    uart_tx_start(detection_queue);

    /* ── 8. Set scanner identity — sent by TX task after startup delay ── */
    {
#if defined(BLE_SCANNER_ONLY)
        const char *bname = "scanner-s3-ble", *cname = "esp32s3", *caps = "ble";
#elif defined(WIFI_SCANNER_ONLY) && defined(CONFIG_IDF_TARGET_ESP32C5)
        const char *bname = "scanner-c5", *cname = "esp32c5", *caps = "wifi,5ghz";
#elif defined(WIFI_SCANNER_ONLY)
        const char *bname = "scanner-esp32", *cname = "esp32", *caps = "wifi";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
        const char *bname = "scanner-s3-combo", *cname = "esp32s3", *caps = "ble,wifi";
#else
        const char *bname = "scanner-esp32", *cname = "esp32", *caps = "wifi";
#endif
        /* Store identity — TX task will send it after its startup delay */
        uart_tx_set_identity(bname, cname, caps);
    }

#ifndef BLE_SCANNER_ONLY
    /* ── 9. Start WiFi scanner task on Core 0 (radio core) ───────────── */
    wifi_scanner_start();
    ESP_LOGI(TAG, "WiFi scanner started on core %d, priority %d",
             WIFI_SCAN_TASK_CORE, WIFI_SCAN_TASK_PRIORITY);
#endif

#ifndef WIFI_SCANNER_ONLY
    /* ── 9b. Start BLE scanner ───────────────────────────────────────── */
    ble_remote_id_start();
    ESP_LOGI(TAG, "BLE scanner started");
#endif

    /* ── 10. Start LED task ───────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_UPLINK_OK);   /* purple — UART active, connected to uplink */

    /* ── 11. Start display task ──────────────────────────────────────── */
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

    /* ── 12. Startup banner ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s", FIRMWARE_NAME, FOF_VERSION);
#if CONFIG_IDF_TARGET_ESP32C5
    ESP_LOGI(TAG, "  ESP32-C5 single-core RISC-V @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi 6 dual-band + BLE 5");
#elif CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi + BLE 5");
#elif CONFIG_IDF_TARGET_ESP32
    ESP_LOGI(TAG, "  ESP32 dual-core Xtensa @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi promiscuous scanner");
#endif
    ESP_LOGI(TAG, "  UART1 -> Uplink @ %d baud", UART_BAUD_RATE);
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "  BOOT button: tap=scroll, 2x=privacy view");
    ESP_LOGI(TAG, "============================================");

    /* ── 13. UART command listener (receives lock-on from uplink) ────── */
#ifndef BLE_SCANNER_ONLY
    xTaskCreatePinnedToCore(
        uart_cmd_listener_task,
        "uart_cmd",
        8192,   /* Increased from 4096: esp_ota_write needs ~2-3KB stack */
        NULL,
        3,      /* Raised from 1: must compete with WiFi/BLE scan tasks during OTA */
        NULL,
        DISPLAY_TASK_CORE
    );
    ESP_LOGI(TAG, "UART command listener started");
#endif

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
