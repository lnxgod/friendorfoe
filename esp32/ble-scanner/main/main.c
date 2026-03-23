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

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_ble_scanner";

#define FIRMWARE_VERSION    "0.11.0-beta"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   500

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    int page_index = 0;
    int cycle_counter = 0;

    #define PAGE_CYCLE_TICKS 5

    while (1) {
        int total       = console_output_get_total_count();
        int active      = bayesian_fusion_get_active_count();
        int ble         = console_output_get_ble_count();
        uint32_t uptime = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

        /* Update stats (top section) — channel=0, wifi=0 for BLE-only scanner */
        oled_update(total, active, 0, ble, 0, uptime);

        /* Fetch all cached detections for scoreboard */
        scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
        int det_count = console_output_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

        if (det_count > 0) {
            if (page_index >= det_count) {
                page_index = 0;
            }

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
#endif
    ESP_LOGI(TAG, "  BLE Remote ID (OpenDroneID 0xFFFA)");
    ESP_LOGI(TAG, "  Console JSON output");
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
