/**
 * Friend or Foe -- WiFi Scanner Firmware Entry Point
 *
 * Dedicated WiFi promiscuous scanner. Captures raw 802.11 frames,
 * fuses detections through a Bayesian engine, and streams results
 * over UART to the uplink board for relay to the Android app.
 *
 * Architecture:
 *   Core 0 (radio):      WiFi scan task
 *   Core 1 (processing): UART TX task (fusion runs inline)
 *
 * Detection flow:
 *   WiFi scanner ──▶ detection_queue ──▶ UART TX task
 *                         (50 items)       │
 *                                          ├── Bayesian fusion
 *                                          └── JSON → UART1 → Uplink
 */

#include "wifi_scanner.h"
#include "bayesian_fusion.h"
#include "uart_tx.h"
#include "detection_types.h"
#include "uart_protocol.h"
#include "task_priorities.h"
#include "led_status.h"
#include "oled_display.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_scanner";

#define FIRMWARE_VERSION    "0.15.0-beta"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   500

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    int page_index = 0;
    int cycle_counter = 0;

    /* Advance page every 5 ticks = 2.5s at 500ms refresh */
    #define PAGE_CYCLE_TICKS 5

    while (1) {
        int total       = uart_tx_get_total_count();
        int active      = bayesian_fusion_get_active_count();
        uint8_t channel = uart_tx_get_current_channel();
        int ble         = uart_tx_get_ble_count();
        int wifi        = uart_tx_get_wifi_count();
        uint32_t uptime = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

        /* Update stats (top section) */
        oled_update(total, active, channel, ble, wifi, uptime);

        /* Fetch all cached detections for scoreboard */
        scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
        int det_count = uart_tx_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

        if (det_count > 0) {
            /* Wrap page_index if list shrunk */
            if (page_index >= det_count) {
                page_index = 0;
            }

            oled_show_detection_paged(
                det_list[page_index].drone_id,
                det_list[page_index].manufacturer,
                det_list[page_index].confidence,
                det_list[page_index].rssi,
                page_index + 1, det_count);

            /* Advance page on cycle boundary */
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

    /* ── 1b. Initialize TCP/IP network interface (required before WiFi) ── */
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
    oled_set_version(FIRMWARE_VERSION);
    oled_update(0, 0, 0, 0, 0, 0);

    /* ── 6. Initialize UART TX (hardware setup, no task yet) ──────────── */
    uart_tx_init();

    /* ── 7. Initialize WiFi scanner (sets up promiscuous mode) ────────── */
    wifi_scanner_init(detection_queue);
    ESP_LOGI(TAG, "WiFi scanner initialised");

    /* ── 8. Start UART TX task on Core 1 (processing core) ──────────────── */
    uart_tx_start(detection_queue);

    /* ── 9. Start WiFi scanner task on Core 0 (radio core) ───────────── */
    wifi_scanner_start();
    ESP_LOGI(TAG, "WiFi scanner started on core %d, priority %d",
             WIFI_SCAN_TASK_CORE, WIFI_SCAN_TASK_PRIORITY);

    /* ── 10. Start LED task ───────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_SCANNING);

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
    ESP_LOGI(TAG, "  Friend or Foe — WiFi Scanner v%s", FIRMWARE_VERSION);
#if CONFIG_IDF_TARGET_ESP32C5
    ESP_LOGI(TAG, "  ESP32-C5 single-core RISC-V @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi 6 dual-band promiscuous");
#else
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi promiscuous");
#endif
    ESP_LOGI(TAG, "  UART1 -> Uplink @ %d baud", UART_BAUD_RATE);
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "  drone_detection_t: %u bytes", (unsigned)sizeof(drone_detection_t));
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
