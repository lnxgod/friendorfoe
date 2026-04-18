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
#include "esp_heap_caps.h"
#include "psram_alloc.h"

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
#include "fw_store.h"

#include "version.h"

#if defined(UPLINK_ESP32S3)
#define FIRMWARE_NAME "uplink-s3"
#elif defined(UPLINK_ESP32)
#define FIRMWARE_NAME "uplink-esp32"
#else
#define FIRMWARE_NAME "uplink"
#endif

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
        bool server_ok   = !standalone &&
                           http_upload_get_fail_count() <= http_upload_get_success_count();

#if !defined(UPLINK_ESP32) && !defined(UPLINK_ESP32S3)
        if (!gps_fix) {
            led_set_pattern(LED_NO_GPS);
        } else
#endif
        if (!standalone && !wifi_ok) {
            led_set_pattern(LED_WIFI_DOWN);         /* red/yellow flash */
        } else if (!scanner_ok) {
            led_set_pattern(LED_NO_SCANNER);        /* blue blink */
        } else if (!standalone && !server_ok) {
            led_set_pattern(LED_NO_SERVER);          /* yellow pulse */
        } else if (wifi_ok && scanner_ok && (standalone || server_ok)) {
            led_set_pattern(LED_ALL_GOOD);           /* solid green */
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
#if defined(UPLINK_ESP32S3)
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s (ESP32-S3)", FIRMWARE_NAME, FOF_VERSION);
#elif defined(UPLINK_ESP32)
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s (ESP32)", FIRMWARE_NAME, FOF_VERSION);
#else
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s (ESP32-C3)", FIRMWARE_NAME, FOF_VERSION);
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
    /* ── 0. Machine-readable firmware identification ──────────────────── */
    FOF_PRINT_IDENT(TAG, FIRMWARE_NAME);

    /* Report PSRAM presence up front so it's obvious in serial + /api/status
     * whether the board booted with external memory. On N16R8 hardware this
     * should log ~8 MiB; on non-PSRAM boards we log "none" and fall back to
     * internal SRAM throughout. */
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

    /* Send OTA abort to all scanners at boot — unsticks any scanner stuck in OTA mode.
     * Uses 4x 0xFF binary sequence because JSON commands are dropped during OTA mode.
     * Follow with \n to flush the scanner's line buffer so it doesn't pollute
     * future JSON commands (the 0xFF bytes are non-newline and would sit in
     * the line accumulator forever). */
    {
        const uint8_t abort_seq[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, '\n'};
        uart_write_bytes(CONFIG_BLE_SCANNER_UART, (const char *)abort_seq, sizeof(abort_seq));
#if CONFIG_DUAL_SCANNER
        uart_write_bytes(CONFIG_WIFI_SCANNER_UART, (const char *)abort_seq, sizeof(abort_seq));
#endif
        ESP_LOGI(TAG, "Sent OTA abort sequence to all scanners");
        vTaskDelay(pdMS_TO_TICKS(100));
    }

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

    /* ── 16b. Tell scanners to start transmitting ────────────────────── */
    /* Scanners boot silent (start/stop protocol) and wait for this signal. */
    {
        const char *ready_msg = "{\"type\":\"ready\"}\n";
        uart_write_bytes(CONFIG_BLE_SCANNER_UART, ready_msg, strlen(ready_msg));
#if CONFIG_DUAL_SCANNER
        uart_write_bytes(CONFIG_WIFI_SCANNER_UART, ready_msg, strlen(ready_msg));
#endif
        ESP_LOGW(TAG, "Sent ready signal to all scanners — detections enabled");
    }

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

            ESP_LOGI(TAG, "WATCHDOG: wifi=%s down=%llds upload_age=%llds heap=%lu uptime=%llds ok=%d fail=%d det=%d",
                     wifi_ok ? "OK" : "DOWN",
                     (long long)wifi_down_s,
                     (long long)upload_age_s,
                     (unsigned long)esp_get_free_heap_size(),
                     (long long)uptime_s,
                     http_upload_get_success_count(),
                     http_upload_get_fail_count(),
                     uart_rx_get_detection_count());

            /* Re-send ready signal every watchdog cycle (30s).
             * Ensures scanners get the start command even if they
             * missed it during boot or rebooted independently. */
            {
                const char *ready_msg = "{\"type\":\"ready\"}\n";
                uart_write_bytes(CONFIG_BLE_SCANNER_UART, ready_msg, strlen(ready_msg));
#if CONFIG_DUAL_SCANNER
                uart_write_bytes(CONFIG_WIFI_SCANNER_UART, ready_msg, strlen(ready_msg));
#endif
            }

            /* WiFi dead for >120s → hard reboot (was 60s, too aggressive for weak signal) */
            if (!wifi_ok && wifi_down_s > 120) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: WiFi disconnected for %llds — rebooting",
                         (long long)wifi_down_s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }

            /* No upload success for >300s (after first success) → hard reboot
             * Skip during firmware relay — uploads are intentionally paused. */
            if (had_first_upload && upload_age_s > 300 && !fw_store_is_relay_active()) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: No successful upload for %llds — rebooting",
                         (long long)upload_age_s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }

            /* No upload at all after 300s of uptime → something is very wrong */
            if (!had_first_upload && uptime_s > 300 && !fw_store_is_relay_active()) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: Never uploaded after %llds uptime — rebooting",
                         (long long)uptime_s);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }

            /* Heap critically low → reboot before stack overflow crash.
             * Legacy ESP32 heap fragments over time with HTTP traffic. */
            uint32_t free_heap = esp_get_free_heap_size();
            if (free_heap < 4000 && uptime_s > 20 && !fw_store_is_relay_active()) {
                ESP_LOGE(TAG, "WATCHDOG REBOOT: heap=%lu — rebooting to recover",
                         (unsigned long)free_heap);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
    }
}
