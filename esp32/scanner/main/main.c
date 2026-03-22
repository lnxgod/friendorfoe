/**
 * Friend or Foe -- Scanner Firmware Entry Point (ESP32-S3)
 *
 * The scanner board performs WiFi promiscuous scanning and BLE Remote ID
 * scanning, fuses detections through a Bayesian engine, and streams
 * results over UART to the uplink board for relay to the Android app.
 *
 * Architecture:
 *   Core 0 (radio):      WiFi scan task, BLE scan task
 *   Core 1 (processing): UART TX task (fusion runs inline)
 *
 * Detection flow:
 *   WiFi scanner ──┐
 *                   ├──▶ detection_queue ──▶ UART TX task
 *   BLE scanner  ──┘         (50 items)       │
 *                                              ├── Bayesian fusion
 *                                              └── JSON → UART1 → Uplink
 */

#include "wifi_scanner.h"
#include "ble_remote_id.h"
#include "bayesian_fusion.h"
#include "uart_tx.h"
#include "detection_types.h"
#include "uart_protocol.h"
#include "task_priorities.h"
#include "led_status.h"

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

#define FIRMWARE_VERSION    "0.10.0-beta"
#define DETECTION_QUEUE_LEN 50

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

    /* ── 6. Initialize UART TX (hardware setup, no task yet) ──────────── */
    uart_tx_init();

    /* ── 7. Initialize WiFi scanner (sets up promiscuous mode) ────────── */
    wifi_scanner_init(detection_queue);
    ESP_LOGI(TAG, "WiFi scanner initialised");

    /* ── 8. Initialize BLE Remote ID scanner ──────────────────────────── */
    ble_remote_id_init(detection_queue);
    ESP_LOGI(TAG, "BLE Remote ID scanner initialised");

    /* ── 9. Start UART TX task on Core 1 (processing core) ────────────── */
    uart_tx_start(detection_queue);

    /* ── 10. Start WiFi scanner task on Core 0 (radio core) ───────────── */
    wifi_scanner_start();
    ESP_LOGI(TAG, "WiFi scanner started on core %d, priority %d",
             WIFI_SCAN_TASK_CORE, WIFI_SCAN_TASK_PRIORITY);

    /* ── 11. Start BLE scanner task on Core 0 (radio core) ────────────── */
    ble_remote_id_start();
    ESP_LOGI(TAG, "BLE Remote ID scanner started on core %d, priority %d",
             BLE_SCAN_TASK_CORE, BLE_SCAN_TASK_PRIORITY);

    /* ── 12. Start LED task ───────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_SCANNING);

    /* ── 13. Startup banner ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe -- Scanner v%s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi promiscuous + BLE Remote ID");
    ESP_LOGI(TAG, "  UART1 -> Uplink @ %d baud", UART_BAUD_RATE);
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "  drone_detection_t: %u bytes", (unsigned)sizeof(drone_detection_t));
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
