/**
 * Friend or Foe — BLE Scanner Firmware Entry Point
 *
 * Dedicated BLE Remote ID scanner. Runs NimBLE full-time to capture
 * OpenDroneID advertisements (UUID 0xFFFA), fuses detections through
 * a Bayesian engine, and outputs JSON to the serial console.
 *
 * Detection flow:
 *   BLE scanner ──▶ detection_queue ──▶ Console output task
 *                        (50 items)       │
 *                                         ├── Bayesian fusion
 *                                         └── JSON → printf (console)
 */

#include "ble_remote_id.h"
#include "bayesian_fusion.h"
#include "console_output.h"
#include "detection_types.h"
#include "core/task_priorities.h"
#include "led_status.h"
#include "oled_display.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#if CONFIG_FOF_SCAN_HYBRID || CONFIG_FOF_SCAN_WIFI_ONLY
#include "wifi_privacy_scanner.h"
#endif

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_ble_scanner";

#define FIRMWARE_VERSION    "0.27.0-beta"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   500

/* ── BOOT button (GPIO0) ─────────────────────────────────────────────── */

#define BOOT_BUTTON_GPIO    0
#define LONG_PRESS_MS       1500
#define TRIPLE_TAP_WINDOW_MS 800  /* 3 taps within this window = triple tap */

static volatile bool s_button_short = false;   /* single short press */
static volatile bool s_button_long = false;    /* long press */
static volatile bool s_button_triple = false;  /* triple tap */
static volatile int64_t s_button_down_ms = 0;

/* Triple-tap tracking */
static volatile int s_tap_count = 0;
static volatile int64_t s_first_tap_ms = 0;

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "BOOT: tap=scroll, hold=switch view, 3x tap=cycle mode");
}

/** Call from display loop to poll button state (non-blocking). */
static void button_poll(void)
{
    int level = gpio_get_level(BOOT_BUTTON_GPIO);
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (level == 0) {
        /* Button pressed (active low) */
        if (s_button_down_ms == 0) {
            s_button_down_ms = now_ms;
        } else if ((now_ms - s_button_down_ms) >= LONG_PRESS_MS && !s_button_long) {
            s_button_long = true;
            s_tap_count = 0; /* cancel any multi-tap in progress */
            ESP_LOGI(TAG, "BOOT: LONG press");
        }
    } else {
        /* Button released */
        if (s_button_down_ms > 0) {
            int64_t held_ms = now_ms - s_button_down_ms;
            if (held_ms < LONG_PRESS_MS && !s_button_long) {
                /* Short tap — accumulate for multi-tap detection */
                if (s_tap_count == 0 || (now_ms - s_first_tap_ms) > TRIPLE_TAP_WINDOW_MS) {
                    /* Start new tap sequence */
                    s_tap_count = 1;
                    s_first_tap_ms = now_ms;
                } else {
                    s_tap_count++;
                }

                if (s_tap_count >= 3) {
                    s_button_triple = true;
                    s_tap_count = 0;
                    ESP_LOGI(TAG, "BOOT: TRIPLE tap (cycle scan mode)");
                }
            }
            s_button_down_ms = 0;
            s_button_long = false;
        }
    }

    /* If tap sequence timed out with 1 tap, emit single press */
    if (s_tap_count > 0 && s_tap_count < 3 &&
        (now_ms - s_first_tap_ms) > TRIPLE_TAP_WINDOW_MS) {
        s_button_short = true;
        s_tap_count = 0;
    }
}

/* ── Scan mode (BLE / WiFi / Hybrid) ──────────────────────────────────── */

enum scan_mode { SMODE_HYBRID = 0, SMODE_BLE_ONLY, SMODE_WIFI_ONLY, SMODE_COUNT };
static volatile int s_scan_mode = SMODE_HYBRID;

static const char *scan_mode_label(int mode) {
    switch (mode) {
        case SMODE_HYBRID:   return "B+W";
        case SMODE_BLE_ONLY: return "BLE";
        case SMODE_WIFI_ONLY:return "WiFi";
        default:             return "???";
    }
}

/* ── Last detection timestamp (for status bar) ────────────────────────── */

static volatile int64_t s_last_privacy_det_ms = 0;
static volatile int64_t s_last_drone_det_ms = 0;

/* ── ACK'd / whitelisted devices ──────────────────────────────────────── */
/* Devices the user has acknowledged as "known/friendly" via long press.
 * New devices that DON'T appear here get a "NEW" tag on the OLED. */

#define MAX_ACKED_MACS 20
static char s_acked_macs[MAX_ACKED_MACS][18]; /* "AA:BB:CC:DD:EE:FF" */
static int s_acked_count = 0;

static bool is_acked(const char *mac_str)
{
    for (int i = 0; i < s_acked_count; i++) {
        if (strcmp(s_acked_macs[i], mac_str) == 0) return true;
    }
    return false;
}

static void ack_device(const char *mac_str)
{
    if (is_acked(mac_str)) return;
    if (s_acked_count >= MAX_ACKED_MACS) {
        /* Evict oldest */
        memmove(&s_acked_macs[0], &s_acked_macs[1], (MAX_ACKED_MACS - 1) * 18);
        s_acked_count--;
    }
    strncpy(s_acked_macs[s_acked_count], mac_str, 17);
    s_acked_macs[s_acked_count][17] = '\0';
    s_acked_count++;
    ESP_LOGI(TAG, "Device ACK'd (friendly): %s (%d total)", mac_str, s_acked_count);
}

/* ── Hybrid WiFi scan task (time-sliced with BLE) ─────────────────────── */

#if CONFIG_FOF_SCAN_HYBRID || CONFIG_FOF_SCAN_WIFI_ONLY
static wifi_privacy_result_t s_wifi_results[MAX_WIFI_PRIVACY_RESULTS];
static int s_wifi_result_count = 0;
static bool s_meta_wifi_active = false;

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi privacy scan task started (every 30s)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(25000)); /* Wait 25s (BLE scanning) */

        /* Only scan WiFi in hybrid or wifi-only mode */
        if (s_scan_mode == SMODE_BLE_ONLY) {
            continue;
        }

        ESP_LOGI(TAG, "WiFi scan starting...");
        s_wifi_result_count = wifi_privacy_scan(s_wifi_results, MAX_WIFI_PRIVACY_RESULTS);
        s_meta_wifi_active = wifi_privacy_meta_transfer_detected();

        if (s_wifi_result_count > 0) {
            ESP_LOGI(TAG, "WiFi scan: %d privacy matches", s_wifi_result_count);
        }
        if (s_meta_wifi_active) {
            ESP_LOGW(TAG, "!! META WIFI TRANSFER DETECTED !!");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif

/* ── Display update task ───────────────────────────────────────────────── */

/*
 * Display:
 *   Tap:        next page / switch between privacy & drone views
 *   Long press: ACK current device as "known/friendly"
 *   Triple tap: cycle scan mode (Hybrid → BLE → WiFi)
 *   Bottom row: [mode] time-since-last-detection
 */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    bool show_privacy = true;
    int page_index = 0;
    int cycle_counter = 0;

    #define PAGE_CYCLE_TICKS  6

    while (1) {
        button_poll();
        int64_t now_ms = esp_timer_get_time() / 1000;

        /* ── Triple tap: cycle scan mode ──────────────────────── */
        if (s_button_triple) {
            s_button_triple = false;
            s_scan_mode = (s_scan_mode + 1) % SMODE_COUNT;
            ESP_LOGI(TAG, "Scan mode: %s", scan_mode_label(s_scan_mode));
        }

        /* Fetch data */
        scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
        int det_count = console_output_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

        int glasses_count = 0;
#if CONFIG_FOF_GLASSES_DETECTION
        glasses_detection_t glasses_list[GLASSES_CACHE_SIZE];
        glasses_count = console_output_get_cached_glasses(glasses_list, GLASSES_CACHE_SIZE);
        if (glasses_count > 0) s_last_privacy_det_ms = now_ms;
#endif
        if (det_count > 0) s_last_drone_det_ms = now_ms;

        int current_max = show_privacy ? glasses_count : det_count;

        /* ── Short press: next page, wrap to other mode ─────────── */
        if (s_button_short) {
            s_button_short = false;
            page_index++;
            if (page_index >= current_max || current_max == 0) {
                show_privacy = !show_privacy;
                page_index = 0;
            }
            cycle_counter = 0;
        }

        /* ── Long press: ACK current device as friendly ─────────── */
        if (s_button_long) {
            s_button_long = false;
#if CONFIG_FOF_GLASSES_DETECTION
            if (show_privacy && glasses_count > 0 && page_index < glasses_count) {
                /* Build MAC string from the glasses detection */
                char mac_str[18];
                snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                    glasses_list[page_index].mac[0], glasses_list[page_index].mac[1],
                    glasses_list[page_index].mac[2], glasses_list[page_index].mac[3],
                    glasses_list[page_index].mac[4], glasses_list[page_index].mac[5]);
                ack_device(mac_str);
            } else
#endif
            {
                /* In drone mode, long press switches view */
                show_privacy = !show_privacy;
                page_index = 0;
            }
            cycle_counter = 0;
        }

        /* ── Render current mode ────────────────────────────────── */
#if CONFIG_FOF_GLASSES_DETECTION
        if (show_privacy && glasses_count > 0) {
            if (page_index >= glasses_count) page_index = 0;

            /* Check if this device is ACK'd */
            char cur_mac[18];
            snprintf(cur_mac, sizeof(cur_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                glasses_list[page_index].mac[0], glasses_list[page_index].mac[1],
                glasses_list[page_index].mac[2], glasses_list[page_index].mac[3],
                glasses_list[page_index].mac[4], glasses_list[page_index].mac[5]);
            bool acked = is_acked(cur_mac);

            /* Show alert — prefix device type with "NEW" or "OK" */
            char type_buf[32];
            snprintf(type_buf, sizeof(type_buf), "%s%s",
                acked ? "" : "NEW ",
                glasses_list[page_index].device_type);

            oled_show_glasses_alert(
                type_buf,
                glasses_list[page_index].manufacturer,
                glasses_list[page_index].device_name,
                glasses_list[page_index].rssi,
                glasses_list[page_index].has_camera
            );
        } else if (show_privacy && glasses_count == 0) {
            oled_draw_drone_list(NULL, 0, 0);
            show_privacy = false;
            page_index = 0;
        } else
#endif
        {
            /* Drone mode */
            oled_drone_entry_t oled_drones[DETECTION_CACHE_SIZE];
            for (int i = 0; i < det_count; i++) {
                oled_drones[i].id   = det_list[i].drone_id;
                oled_drones[i].lat  = det_list[i].latitude;
                oled_drones[i].lon  = det_list[i].longitude;
                oled_drones[i].alt_m = det_list[i].altitude_m;
                oled_drones[i].speed_mps = det_list[i].speed_mps;
                oled_drones[i].rssi = det_list[i].rssi;
            }
            if (page_index >= det_count && det_count > 0) page_index = 0;
            oled_draw_drone_list(oled_drones, det_count, page_index);
        }

        /* ── Status bar: [mode] time since last detection ─────────── */
        {
            int64_t last_det = show_privacy ? s_last_privacy_det_ms : s_last_drone_det_ms;
            uint32_t ago_s = (last_det > 0) ? (uint32_t)((now_ms - last_det) / 1000) : 0;
            char status[22];
            if (last_det == 0) {
                snprintf(status, sizeof(status), "[%s] no detections", scan_mode_label(s_scan_mode));
            } else if (ago_s < 60) {
                snprintf(status, sizeof(status), "[%s] %lus ago", scan_mode_label(s_scan_mode), (unsigned long)ago_s);
            } else {
                snprintf(status, sizeof(status), "[%s] %lum%lus ago",
                    scan_mode_label(s_scan_mode),
                    (unsigned long)(ago_s / 60), (unsigned long)(ago_s % 60));
            }
            oled_draw_status_bar(status, 0); /* pass 0 for uptime — not used anymore */
        }

        /* ── Auto-advance page every 3s ─────────────────────────── */
        cycle_counter++;
        if (cycle_counter >= PAGE_CYCLE_TICKS) {
            cycle_counter = 0;
            int max_pages = show_privacy ? glasses_count : det_count;
            if (max_pages > 1) {
                page_index = (page_index + 1) % max_pages;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 1. Initialize NVS flash ──────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. Create shared detection queue ─────────────────────────────── */
    QueueHandle_t detection_queue = xQueueCreate(DETECTION_QUEUE_LEN,
                                                 sizeof(drone_detection_t));
    if (detection_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create detection queue -- halting");
        return;
    }
    ESP_LOGI(TAG, "Detection queue created (%d slots, %u bytes each)",
             DETECTION_QUEUE_LEN, (unsigned)sizeof(drone_detection_t));

    /* ── 3. Initialize Bayesian fusion engine ─────────────────────────── */
    bayesian_fusion_init();
    ESP_LOGI(TAG, "Bayesian fusion engine initialised");

    /* ── 4. Initialize status LED + BOOT button ──────────────────────── */
    led_init();
    led_set_pattern(LED_BOOT);
    button_init();

    /* ── 5. Initialize OLED display ─────────────────────────────────── */
    oled_init();
    oled_set_version(FIRMWARE_VERSION);
    oled_update(0, 0, 0, 0, 0, 0);

    /* ── 6. Initialize BLE Remote ID scanner ──────────────────────────── */
    ble_remote_id_init(detection_queue);
    ESP_LOGI(TAG, "BLE Remote ID scanner initialised");

#if CONFIG_FOF_GLASSES_DETECTION
    /* ── 6b. Create glasses detection queue and attach ──────────────── */
    QueueHandle_t glasses_queue = xQueueCreate(20, sizeof(glasses_detection_t));
    if (glasses_queue != NULL) {
        ble_remote_id_set_glasses_queue(glasses_queue);
        console_output_set_glasses_queue(glasses_queue);
        bool glasses_on = glasses_detection_is_enabled();
        ESP_LOGI(TAG, "Smart glasses detection: %s", glasses_on ? "ENABLED" : "DISABLED");
    }
#endif

    /* ── 7. Start console output task ─────────────────────────────────── */
    console_output_start(detection_queue);

    /* ── 8. Start BLE scanner ─────────────────────────────────────────── */
    ble_remote_id_start();
    ESP_LOGI(TAG, "BLE Remote ID scanner started on core %d, priority %d",
             BLE_SCAN_TASK_CORE, BLE_SCAN_TASK_PRIORITY);

    /* ── 8b. Start WiFi privacy scanner (hybrid mode) ─────────────────── */
#if CONFIG_FOF_SCAN_HYBRID || CONFIG_FOF_SCAN_WIFI_ONLY
    wifi_privacy_init();
    xTaskCreatePinnedToCore(
        wifi_scan_task, "wifi_priv", 4096, NULL,
        2, NULL, CONSOLE_TX_TASK_CORE
    );
    ESP_LOGI(TAG, "WiFi privacy scan task started (hybrid mode)");
#endif

    /* ── 9. Start LED task ───────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_SCANNING);

    /* ── 10. Start display task ─────────────────────────────────────── */
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

    /* ── 11. Startup banner ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe — BLE Scanner v%s", FIRMWARE_VERSION);
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
#elif CONFIG_IDF_TARGET_ESP32C5
    ESP_LOGI(TAG, "  ESP32-C5 single-core RISC-V @ 240 MHz");
#elif CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "  ESP32-C3 single-core RISC-V @ 160 MHz");
#elif CONFIG_IDF_TARGET_ESP32
    ESP_LOGI(TAG, "  ESP32 dual-core Xtensa @ 240 MHz");
#endif
    ESP_LOGI(TAG, "  BLE Remote ID (OpenDroneID 0xFFFA)");
#if CONFIG_FOF_GLASSES_DETECTION
    ESP_LOGI(TAG, "  Smart glasses / privacy device detection: ON");
#endif
    ESP_LOGI(TAG, "  Console JSON output");
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
