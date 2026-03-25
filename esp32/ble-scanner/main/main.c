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

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_ble_scanner";

#define FIRMWARE_VERSION    "0.19.0-beta"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   500

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    int page_index = 0;
    int cycle_counter = 0;

    #define PAGE_CYCLE_TICKS 6  /* 6 * 500ms = 3s per page */

    while (1) {
        /* Fetch cached detections */
        scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
        int det_count = console_output_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

        /* Build OLED drone entries from cache */
        oled_drone_entry_t oled_drones[DETECTION_CACHE_SIZE];
        for (int i = 0; i < det_count; i++) {
            oled_drones[i].id = det_list[i].drone_id;
            oled_drones[i].lat = det_list[i].latitude;
            oled_drones[i].lon = det_list[i].longitude;
            oled_drones[i].alt_m = det_list[i].altitude_m;
            oled_drones[i].speed_mps = det_list[i].speed_mps;
            oled_drones[i].rssi = det_list[i].rssi;
        }

        /* Check for glasses detections and show alert if any */
#if CONFIG_FOF_GLASSES_DETECTION
        glasses_detection_t glasses_list[GLASSES_CACHE_SIZE];
        int glasses_count = console_output_get_cached_glasses(glasses_list, GLASSES_CACHE_SIZE);

        if (glasses_count > 0) {
            /* Show glasses alert (highest RSSI first) */
            int best = 0;
            for (int i = 1; i < glasses_count; i++) {
                if (glasses_list[i].rssi > glasses_list[best].rssi) best = i;
            }
            oled_show_glasses_alert(
                glasses_list[best].device_type,
                glasses_list[best].manufacturer,
                glasses_list[best].device_name,
                glasses_list[best].rssi,
                glasses_list[best].has_camera
            );
        } else
#endif
        {
            /* Draw drone list (single clean screen, no separate stats overlay) */
            if (page_index >= det_count && det_count > 0) {
                page_index = 0;
            }
            oled_draw_drone_list(oled_drones, det_count, page_index);
        }

        /* Page through drones every 3 seconds */
        if (det_count > 1) {
            cycle_counter++;
            if (cycle_counter >= PAGE_CYCLE_TICKS) {
                cycle_counter = 0;
                page_index = (page_index + 1) % det_count;
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

    /* ── 4. Initialize status LED ────────────────────────────────────── */
    led_init();
    led_set_pattern(LED_BOOT);

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
