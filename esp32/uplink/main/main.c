/**
 * Friend or Foe -- Uplink Main Entry Point (ESP32-C3)
 *
 * The Uplink board receives drone detections from the Scanner over UART,
 * uploads them to the FastAPI backend via HTTP, and manages hardware
 * peripherals (OLED, GPS, LED, battery).
 *
 * Task layout (single-core ESP32-C3):
 *   uart_rx_task     - priority 5, stack 4096   - UART line parsing + JSON decode
 *   http_upload_task - priority 4, stack 8192   - Batch upload to backend
 *   gps_task         - priority 3, stack 4096   - NMEA parsing
 *   display_task     - priority 2, stack 4096   - OLED status screen refresh
 *   led_task         - priority 1, stack 2048   - Status LED blink patterns
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

/* Core */
#include "config.h"
#include "nvs_config.h"
#include "serial_config.h"
#include "detection_types.h"

/* Comms */
#include "uart_rx.h"
#include "wifi_sta.h"
#include "http_upload.h"

/* Hardware */
#include "gps.h"
#include "oled_display.h"
#include "battery.h"
#include "led_status.h"

/* Network */
#include "time_sync.h"
#include "wifi_ap.h"
#include "http_status.h"

static const char *TAG = "main";

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    int prev_detection_count = 0;
    int overlay_ticks = 0;
    detection_summary_t last_det = {0};

    while (1) {
        /* Gather current state */
        int drone_count    = uart_rx_get_detection_count();
        bool gps_fix       = gps_has_fix();
        bool wifi_ok       = wifi_sta_is_connected();
        float battery_pct  = battery_get_percentage();
        int upload_count   = http_upload_get_success_count();

        /* Check for new detections */
        if (drone_count > prev_detection_count) {
            detection_summary_t recent[1];
            if (uart_rx_get_recent_detections(recent, 1) > 0) {
                last_det = recent[0];
                overlay_ticks = 6;  /* ~3s at 500ms interval */
            }
        }
        prev_detection_count = drone_count;

        /* Gather additional state for the new OLED layout */
        bool ble_ok      = uart_rx_is_ble_scanner_connected();
        bool wifi_scan_ok = uart_rx_is_wifi_scanner_connected();
        bool scanner_ok  = ble_ok || wifi_scan_ok;
        bool backend_ok  = (http_upload_get_success_count() > 0 &&
                           http_upload_get_fail_count() < http_upload_get_success_count());
        uint32_t uptime  = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
        static char device_id_buf[64] = {0};
        if (device_id_buf[0] == '\0') {
            nvs_config_get_device_id(device_id_buf, sizeof(device_id_buf));
        }

        /* OLED: BLE scanner, WiFi scanner, backend, uploads, WiFi network, battery, uptime, ID */
        oled_update(drone_count, ble_ok, wifi_scan_ok, backend_ok,
                    upload_count, wifi_ok, battery_pct, uptime, device_id_buf);

        if (overlay_ticks > 0) {
            oled_show_detection(last_det.drone_id, NULL,
                                last_det.confidence, last_det.rssi);
            overlay_ticks--;
        }

        /* Update LED pattern based on system state */
        bool standalone  = wifi_sta_is_standalone();

#ifndef UPLINK_ESP32
        if (!gps_fix) {
            led_set_pattern(LED_NO_GPS);
        } else
#endif
        if (!standalone && !wifi_ok) {
            led_set_pattern(LED_ERROR);
        } else if (!scanner_ok) {
            led_set_pattern(LED_NO_SCANNER);
        } else if (!standalone &&
                   http_upload_get_fail_count() > http_upload_get_success_count()) {
            led_set_pattern(LED_ERROR);
        } else if (drone_count > 0) {
            led_set_pattern(LED_SCANNING);
        } else {
            led_set_pattern(LED_IDLE);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_DISPLAY_UPDATE_MS));
    }
}

/* ── Startup banner ────────────────────────────────────────────────────── */

static void print_banner(void)
{
    ESP_LOGI(TAG, "=============================================");
#ifdef UPLINK_ESP32
    ESP_LOGI(TAG, "  Friend or Foe -- Uplink (ESP32)");
#else
    ESP_LOGI(TAG, "  Friend or Foe -- Uplink (ESP32-C3)");
#endif
    ESP_LOGI(TAG, "  Drone Detection Backend Relay");
    ESP_LOGI(TAG, "=============================================");

    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    if (wifi_sta_is_standalone()) {
        ESP_LOGI(TAG, "Mode:      STANDALONE (AP-only, no backend)");
    } else {
        char backend_url[128] = {0};
        nvs_config_get_backend_url(backend_url, sizeof(backend_url));
        ESP_LOGI(TAG, "Backend:   %s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);
    }

    ESP_LOGI(TAG, "Batch:     %d detections / %dms",
             CONFIG_MAX_BATCH_SIZE, CONFIG_BATCH_INTERVAL_MS);
    ESP_LOGI(TAG, "Queue:     %d slots", CONFIG_DETECTION_QUEUE_SIZE);
    ESP_LOGI(TAG, "AP SSID:   %s", wifi_ap_get_ssid());
    ESP_LOGI(TAG, "AP URL:    http://192.168.4.1");
    ESP_LOGI(TAG, "=============================================");
}

/* ── app_main ──────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Friend or Foe Uplink...");

    /* ── 1. Initialize NVS flash ──────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash and retrying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. Initialize NVS configuration ──────────────────────────────── */
    nvs_config_init();

    /* ── 2b. Serial config window (web flasher sends config here) ──── */
    serial_config_listen(3000);

    /* ── 3. Create default event loop ─────────────────────────────────── */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ── 4. Create detection queue ────────────────────────────────────── */
    QueueHandle_t detection_queue = xQueueCreate(
        CONFIG_DETECTION_QUEUE_SIZE, sizeof(drone_detection_t));
    if (!detection_queue) {
        ESP_LOGE(TAG, "Failed to create detection queue!");
        return;
    }
    ESP_LOGI(TAG, "Detection queue created (%d slots, %d bytes each)",
             CONFIG_DETECTION_QUEUE_SIZE, (int)sizeof(drone_detection_t));

    /* ── 5. Initialize WiFi STA + connect ─────────────────────────────── */
    wifi_sta_init();

    /* ── 5b. Initialize WiFi AP (runs concurrently with STA) ──────── */
    wifi_ap_init();

    /* ── 6. Wait for WiFi connection (30s timeout) ────────────────────── */
    wifi_sta_wait_connected(30000);

    /* ── 7. Initialize SNTP time sync ─────────────────────────────────── */
    if (wifi_sta_is_connected()) {
        time_sync_init();
    } else {
        ESP_LOGW(TAG, "WiFi not connected, deferring SNTP init");
    }

    /* ── 8. Initialize GPS ────────────────────────────────────────────── */
    gps_init();

    /* ── 9. Initialize OLED display ───────────────────────────────────── */
    oled_init();
    oled_update(0, false, false, false, 0, wifi_sta_is_connected(), 0.0f, 0, NULL);

    /* ── 10. Initialize battery monitor ───────────────────────────────── */
    battery_init();

    /* ── 11. Initialize status LED ────────────────────────────────────── */
    led_init();
    led_set_pattern(LED_IDLE);

    /* ── 12. Initialize UART RX ───────────────────────────────────────── */
    uart_rx_init(detection_queue);

    /* ── 13. Initialize HTTP upload ───────────────────────────────────── */
    http_upload_init(detection_queue);

    /* ── 14. Start all tasks ──────────────────────────────────────────── */
    ESP_LOGI(TAG, "Starting tasks...");

    uart_rx_start();
    http_upload_start();
    gps_start();
    led_start();

    /* Start display task (inline since it's simple) */
    xTaskCreate(display_task, "display", CONFIG_DISPLAY_STACK,
                NULL, CONFIG_DISPLAY_PRIORITY, NULL);

    /* ── 15. Start HTTP status server ────────────────────────────────── */
    ESP_LOGW(TAG, "HEAP before HTTP server: %lu bytes", (unsigned long)esp_get_free_heap_size());
    http_status_init();
    ESP_LOGW(TAG, "HEAP after HTTP server: %lu bytes", (unsigned long)esp_get_free_heap_size());

    /* ── 16. Print startup banner ─────────────────────────────────────── */
    print_banner();

    ESP_LOGI(TAG, "All tasks started. Uplink is operational.");

    /* ── 17. Connectivity watchdog ───────────────────────────────────── */
    /*
     * Hard reboot if:
     *   - WiFi disconnected for > 60 seconds
     *   - No successful HTTP upload for > 120 seconds (after first success)
     *
     * This catches all edge cases: WiFi driver stuck, HTTP client frozen,
     * DHCP lease expired, router rebooted, etc.
     */
    {
        int64_t last_wifi_connected_ms = esp_timer_get_time() / 1000;
        int64_t boot_ms = last_wifi_connected_ms;
        bool had_first_upload = false;

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(30000));  /* Check every 30s */

            int64_t now_ms = esp_timer_get_time() / 1000;
            bool wifi_ok = wifi_sta_is_connected();

            if (wifi_ok) {
                last_wifi_connected_ms = now_ms;
            }

            /* Check HTTP upload health */
            int64_t last_upload_ms = http_upload_get_last_success_ms();
            if (last_upload_ms > 0) {
                had_first_upload = true;
            }

            int64_t wifi_down_s = (now_ms - last_wifi_connected_ms) / 1000;
            int64_t upload_age_s = had_first_upload ? (now_ms - last_upload_ms) / 1000 : 0;
            int64_t uptime_s = (now_ms - boot_ms) / 1000;

            ESP_LOGI(TAG, "WATCHDOG: wifi=%s down=%llds upload_age=%llds heap=%lu uptime=%llds ok=%d fail=%d",
                     wifi_ok ? "OK" : "DOWN",
                     (long long)wifi_down_s,
                     (long long)upload_age_s,
                     (unsigned long)esp_get_free_heap_size(),
                     (long long)uptime_s,
                     http_upload_get_success_count(),
                     http_upload_get_fail_count());

            /* WiFi dead for >120s → hard reboot (was 60s, too aggressive for weak signal) */
            if (!wifi_ok && wifi_down_s > 120) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: WiFi disconnected for %llds — rebooting",
                         (long long)wifi_down_s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }

            /* No upload success for >300s (after first success) → hard reboot */
            if (had_first_upload && upload_age_s > 300) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: No successful upload for %llds — rebooting",
                         (long long)upload_age_s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }

            /* No upload at all after 300s of uptime → something is very wrong */
            if (!had_first_upload && uptime_s > 300) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: Never uploaded after %llds uptime — rebooting",
                         (long long)uptime_s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }
    }
}
