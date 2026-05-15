/**
 * Friend or Foe -- Uplink Main Entry Point (ESP32-S3)
 *
 * The Uplink board receives drone detections from the Scanner over UART,
 * uploads them to the FastAPI backend via HTTP, and manages hardware
 * peripherals (OLED, GPS, LED, battery).
 *
 * Task layout:
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
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psram_alloc.h"

/* Core */
#include "config.h"
#include "nvs_config.h"
#include "serial_config.h"
#include "badge_mode.h"
#include "badge_runtime.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_display_policy_runtime.h"
#endif
#include "detection_types.h"
#include "detection_policy.h"
#include "uart_protocol.h"

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
#include "fw_auto_check.h"

#include "version.h"

#if CONFIG_BT_ENABLED && !defined(FOF_BADGE_VARIANT)
#error "uplink firmware must keep Bluetooth disabled; uplinks should never advertise or scan BLE"
#endif

#if !defined(UPLINK_ESP32S3)
#error "Supported FoF uplink firmware is ESP32-S3 only."
#endif

#define FIRMWARE_NAME "uplink-s3"
#ifdef FOF_BADGE_VARIANT
#define BADGE_DISPLAY_UPDATE_MS 250
#endif

static const char *TAG = "main";

#ifdef FOF_BADGE_VARIANT
static bool apply_badge_network_mode(badge_runtime_network_mode_t mode)
{
    if (mode == BADGE_RUNTIME_NETWORK_OFF) {
        wifi_sta_set_force_standalone(true);
        wifi_ap_stop();
        wifi_sta_stop_all();
        ESP_LOGW(TAG, "Badge network off; display/USB/scanners remain active");
        return true;
    }

    bool local_ap = mode == BADGE_RUNTIME_NETWORK_LOCAL_AP;
    wifi_sta_set_force_standalone(local_ap);
    wifi_sta_set_keep_ap_enabled(local_ap);
    wifi_sta_init();
    wifi_ap_init();

    if (local_ap) {
        wifi_ap_start();
        http_status_init();
        ESP_LOGW(TAG, "Badge temporary AP flash/control session active");
        return true;
    }

    wifi_sta_wait_connected(15000);
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "Badge backend/LAN network session could not connect");
        return false;
    }
    http_status_init();
    ESP_LOGW(TAG, "Badge temporary LAN/backend flash/control session active");
    return true;
}

static bool badge_backend_configured(void)
{
    char ssid[33] = {0};
    char backend_url[128] = {0};
    nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
    nvs_config_get_backend_url(backend_url, sizeof(backend_url));
    return ssid[0] != '\0' && backend_url[0] != '\0';
}

static badge_runtime_network_mode_t badge_boot_network_mode(badge_mode_t mode)
{
    if (mode == BADGE_MODE_BACKEND && badge_backend_configured()) {
        return BADGE_RUNTIME_NETWORK_BACKEND;
    }
    if (mode == BADGE_MODE_LOCAL_AP) {
        return BADGE_RUNTIME_NETWORK_LOCAL_AP;
    }
    return BADGE_RUNTIME_NETWORK_OFF;
}

static bool badge_scanner_control_healthy(const scanner_info_t *info)
{
    return info && info->received &&
           info->cmd_rx_count > 0 &&
           info->cmd_last_age_s >= 0 &&
           info->cmd_last_age_s <= 45;
}

static void log_badge_scanner_debug(const char *slot, bool connected,
                                    const scanner_info_t *info)
{
    if (!connected || !info || !info->received) {
        ESP_LOGW(TAG, "BADGE_DEBUG %s connected=0", slot);
        return;
    }

    ESP_LOGW(TAG,
             "BADGE_DEBUG %s connected=1 board=%s ver=%s cmd_rx=%lu cmd_age=%lld "
             "profile=%s role_ack=%d ble_scan=%d host=%d/%d ble_adv=%lu meta=%lu tracker=%lu "
             "meta_age=%lld meta_emit_age=%lld meta_id=%s meta_hash=%08lX "
             "meta_rssi=%d meta_reason=%s meta_weak_age=%lld reacq=%lu "
             "focus=%d/%lld focus_ads=%lu "
             "near=%lu rid=%lu wifi_paused=%d wifi_frames=%lu wifi_full=%lu/%lu rc=%d "
             "ssid_drone=%lu ssid_notable=%lu last_drone='%s' last_drone_age=%lld "
             "last_notable='%s' last_notable_age=%lld",
             slot,
             info->board[0] ? info->board : "?",
             info->version[0] ? info->version : "?",
             (unsigned long)info->cmd_rx_count,
             (long long)info->cmd_last_age_s,
             info->scan_profile[0] ? info->scan_profile : "?",
             info->calibration_mode_acked ? 1 : 0,
             info->ble_scanning ? 1 : 0,
             info->ble_host_active ? 1 : 0,
             info->ble_host_synced ? 1 : 0,
             (unsigned long)info->ble_adv_seen,
             (unsigned long)info->ble_meta_seen,
             (unsigned long)info->ble_tracker_seen,
             (long long)info->ble_meta_last_seen_age_s,
             (long long)info->ble_meta_last_emit_age_s,
             info->ble_meta_identity[0] ? info->ble_meta_identity : "?",
             (unsigned long)info->ble_meta_last_hash,
             (int)info->ble_meta_last_rssi,
             info->ble_meta_last_reason[0] ? info->ble_meta_last_reason : "?",
             (long long)info->ble_meta_weak_age_s,
             (unsigned long)info->ble_meta_reacquire_count,
             info->ble_focus_active ? 1 : 0,
             (long long)info->ble_focus_age_s,
             (unsigned long)info->ble_focus_target_adv_count,
             (unsigned long)info->ble_near_unknown_seen,
             (unsigned long)info->rid_emit,
             info->wifi_paused ? 1 : 0,
             (unsigned long)info->wifi_total_frames,
             (unsigned long)info->wifi_full_scan_ok,
             (unsigned long)info->wifi_full_scan_count,
             info->wifi_full_scan_last_rc,
             (unsigned long)info->wifi_drone_ssid_emit,
             (unsigned long)info->wifi_notable_ssid_emit,
             info->wifi_last_drone_ssid,
             (long long)info->wifi_last_drone_ssid_age_s,
             info->wifi_last_notable_ssid,
             (long long)info->wifi_last_notable_ssid_age_s);
}

static void log_badge_debug(uint32_t free_heap, int64_t uptime_s)
{
    static badge_threat_snapshot_t snap;
    uart_rx_get_badge_threat_snapshot(&snap);
    int visible_count = 0;
    for (int i = 0; i < snap.entity_count; i++) {
        if (snap.entities[i].cls != BADGE_THREAT_BLE) {
            visible_count++;
        }
    }
    bool ble_connected = uart_rx_is_ble_scanner_connected();
    bool wifi_connected = uart_rx_is_wifi_scanner_connected();
    const scanner_info_t *ble_info = uart_rx_get_ble_scanner_info();
    const scanner_info_t *wifi_info = uart_rx_get_wifi_scanner_info();

    ESP_LOGW(TAG,
             "BADGE_DEBUG uplink=%s uptime=%llds heap=%lu detections=%d "
             "entities=%d visible=%d score=%.2f ble_health=%d wifi_health=%d "
             "reset=%s reset_expected=%d crash_count=%lu stack_main=%lu "
             "stack_display=%lu stack_usb=%lu stack_uart_ble=%lu "
             "stack_uart_wifi=%lu usb_age=%lld recovery=%s",
             FOF_VERSION,
             (long long)uptime_s,
             (unsigned long)free_heap,
             uart_rx_get_detection_count(),
             snap.entity_count,
             visible_count,
             (double)snap.threat_score,
             badge_scanner_control_healthy(ble_info) ? 1 : 0,
             badge_scanner_control_healthy(wifi_info) ? 1 : 0,
             badge_runtime_last_reset_reason_name(),
             badge_runtime_last_reset_expected() ? 1 : 0,
             (unsigned long)badge_runtime_crash_count(),
             (unsigned long)badge_runtime_main_stack_free(),
             (unsigned long)badge_runtime_display_stack_free(),
             (unsigned long)badge_runtime_usb_stack_free(),
             (unsigned long)badge_runtime_uart_ble_stack_free(),
             (unsigned long)badge_runtime_uart_wifi_stack_free(),
             (long long)badge_runtime_usb_control_age_s(),
             badge_runtime_recovery_mode());
    log_badge_scanner_debug("ble", ble_connected, ble_info);
    log_badge_scanner_debug("wifi", wifi_connected, wifi_info);
}

static void send_badge_scan_profiles(void)
{
    if (fw_store_is_relay_active() || http_upload_is_paused() ||
        uart_rx_is_node_calibration_mode()) {
        return;
    }

    bool ble_connected = uart_rx_is_ble_scanner_connected();
    bool wifi_connected = uart_rx_is_wifi_scanner_connected();
    bool both_connected = ble_connected && wifi_connected;
    char cmd[80];

    if (ble_connected) {
        const char *profile = both_connected
            ? fof_policy_scan_profile_for_slot(0, false)
            : "hybrid_failover";
        snprintf(cmd, sizeof(cmd),
                 "{\"type\":\"scan_profile\",\"%s\":\"%s\"}",
                 JSON_KEY_SCAN_PROFILE, profile);
        bool ok = uart_rx_send_command_to_scanner_checked(0, cmd);
        ESP_LOGI(TAG, "BADGE_ROLE ble profile=%s sent=%d", profile, ok ? 1 : 0);
    }

#if CONFIG_DUAL_SCANNER
    if (wifi_connected) {
        const char *profile = both_connected
            ? fof_policy_scan_profile_for_slot(1, false)
            : "hybrid_failover";
        snprintf(cmd, sizeof(cmd),
                 "{\"type\":\"scan_profile\",\"%s\":\"%s\"}",
                 JSON_KEY_SCAN_PROFILE, profile);
        bool ok = uart_rx_send_command_to_scanner_checked(1, cmd);
        ESP_LOGI(TAG, "BADGE_ROLE wifi profile=%s sent=%d", profile, ok ? 1 : 0);
    }
#endif
}
#endif

/* ── Self-OTA rollback state ─────────────────────────────────────────────
 * When true, the running image is an OTA that hasn't been verified yet:
     *   - As soon as the node proves WiFi association after boot we call
     *     esp_ota_mark_app_valid_cancel_rollback() and clear the flag.
 *   - If the connectivity watchdog would otherwise esp_restart() while
 *     the flag is still set, we call esp_ota_mark_app_invalid_rollback_
 *     and_reboot() instead, which boots the previous slot.
 * Rollback is required for fleet OTA safety.
 */
static volatile bool s_ota_pending_verify = false;

static bool reset_reason_is_unhealthy_for_rollback(esp_reset_reason_t reason)
{
    bool crash = reason == ESP_RST_PANIC ||
                 reason == ESP_RST_INT_WDT ||
                 reason == ESP_RST_TASK_WDT ||
                 reason == ESP_RST_WDT;
#ifdef FOF_BADGE_VARIANT
    crash = crash ||
            (reason == ESP_RST_SW &&
             !badge_runtime_reset_reason_was_expected_software((uint32_t)reason));
#endif
    return crash;
}

static void rollback_check_at_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_ota_pending_verify = true;
        ESP_LOGW(TAG, "OTA: running from PENDING_VERIFY partition '%s' — "
                      "will mark valid after runtime health checks",
                 running->label);
        if (reset_reason_is_unhealthy_for_rollback(esp_reset_reason())) {
            ESP_LOGE(TAG, "OTA ROLLBACK: pending image crashed before validation");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    } else {
        ESP_LOGI(TAG, "OTA: running partition '%s' state=%d (already verified)",
                 running->label, state);
    }
}

static void rollback_mark_valid(void)
{
    if (!s_ota_pending_verify) return;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_ota_pending_verify = false;
#ifdef FOF_BADGE_VARIANT
        badge_runtime_set_pending_verify(false);
#endif
        ESP_LOGW(TAG, "OTA: image marked VALID (rollback cancelled)");
    } else {
        ESP_LOGE(TAG, "OTA: mark_valid failed: %s — will retry", esp_err_to_name(err));
    }
}

static void rollback_and_reboot_or_restart(const char *reason)
{
    if (s_ota_pending_verify) {
        ESP_LOGE(TAG, "OTA ROLLBACK: %s while PENDING_VERIFY — reverting to previous slot", reason);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* Does not return on success. If it does fall through (e.g. no valid
         * fallback partition), fall back to a normal restart. */
    }
    ESP_LOGE(TAG, "WATCHDOG REBOOT: %s", reason);
#ifdef FOF_BADGE_VARIANT
    badge_runtime_arm_expected_reboot(reason ? reason : "watchdog");
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    int prev_detection_count = 0;
#ifndef FOF_BADGE_VARIANT
    int overlay_ticks = 0;
    detection_summary_t last_det = {0};
#endif

    while (1) {
        /* Gather current state */
        int detection_count = uart_rx_get_detection_count();
        bool wifi_ok       = wifi_sta_is_connected();
        float battery_pct  = battery_get_percentage();
        int upload_count   = http_upload_get_success_count();

        /* Check for new detections */
        if (detection_count > prev_detection_count) {
#ifdef FOF_BADGE_VARIANT
            int delta = detection_count - prev_detection_count;
            detection_summary_t recent[8];
            int recent_count = uart_rx_get_recent_detections(
                recent,
                delta < (int)(sizeof(recent) / sizeof(recent[0]))
                    ? delta
                    : (int)(sizeof(recent) / sizeof(recent[0]))
            );
            for (int i = recent_count - 1; i >= 0; i--) {
                oled_show_detection(recent[i].drone_id, recent[i].manufacturer,
                                    recent[i].source, recent[i].confidence, recent[i].rssi);
                serial_config_emit_badge_detection(recent[i].drone_id, recent[i].manufacturer,
                                                   recent[i].source, recent[i].confidence,
                                                   recent[i].rssi);
            }
#else
            detection_summary_t recent[1];
            if (uart_rx_get_recent_detections(recent, 1) > 0) {
                last_det = recent[0];
                overlay_ticks = 6;  /* ~3s at 500ms interval */
            }
#endif
        }
        prev_detection_count = detection_count;

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
        oled_update(detection_count, ble_ok, wifi_scan_ok, backend_ok,
                    upload_count, wifi_ok, battery_pct, uptime, device_id_buf);
#ifdef FOF_BADGE_VARIANT
        badge_runtime_note_display_alive();
        badge_runtime_note_scanner_uart_alive(scanner_ok);
        badge_runtime_note_display_stack_free(
            (uint32_t)uxTaskGetStackHighWaterMark(NULL));
#endif

#ifndef FOF_BADGE_VARIANT
        if (overlay_ticks > 0) {
            oled_show_detection(last_det.drone_id, last_det.manufacturer,
                                last_det.source, last_det.confidence, last_det.rssi);
            overlay_ticks--;
        }
#endif

        /* Update LED pattern based on system state */
        bool standalone  = wifi_sta_is_standalone();
        bool server_ok   = !standalone &&
                           http_upload_get_fail_count() <= http_upload_get_success_count();

        if (!standalone && !wifi_ok) {
            led_set_pattern(LED_WIFI_DOWN);         /* red/yellow flash */
        } else if (!scanner_ok) {
            led_set_pattern(LED_NO_SCANNER);        /* blue blink */
        } else if (!standalone && !server_ok) {
            led_set_pattern(LED_NO_SERVER);          /* yellow pulse */
        } else if (wifi_ok && scanner_ok && (standalone || server_ok)) {
            led_set_pattern(LED_ALL_GOOD);           /* solid green */
        } else if (detection_count > 0) {
            led_set_pattern(LED_SCANNING);
        } else {
            led_set_pattern(LED_IDLE);
        }

#ifdef FOF_BADGE_VARIANT
        vTaskDelay(pdMS_TO_TICKS(BADGE_DISPLAY_UPDATE_MS));
#else
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DISPLAY_UPDATE_MS));
#endif
    }
}

/* ── Startup banner ────────────────────────────────────────────────────── */

static void print_banner(void)
{
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s (ESP32-S3)", FIRMWARE_NAME, FOF_VERSION);
    ESP_LOGI(TAG, "  Drone Detection Backend Relay");
    ESP_LOGI(TAG, "=============================================");

    char device_id[32] = {0};
    nvs_config_get_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

#ifdef FOF_BADGE_VARIANT
    ESP_LOGI(TAG, "Mode:      BADGE USB ONLY (display + USB control)");
    ESP_LOGI(TAG, "Network:   disabled (no AP/STA/backend/SNTP)");
#else
    if (wifi_sta_is_standalone()) {
        ESP_LOGI(TAG, "Mode:      STANDALONE (AP-only, no backend)");
    } else {
        char backend_url[128] = {0};
        nvs_config_get_backend_url(backend_url, sizeof(backend_url));
        ESP_LOGI(TAG, "Backend:   %s%s", backend_url, CONFIG_UPLOAD_ENDPOINT);
    }
#endif

    ESP_LOGI(TAG, "Batch:     %d detections / %dms",
             CONFIG_MAX_BATCH_SIZE, CONFIG_BATCH_INTERVAL_MS);
    ESP_LOGI(TAG, "Queue:     %d slots", CONFIG_DETECTION_QUEUE_SIZE);
#ifndef FOF_BADGE_VARIANT
    ESP_LOGI(TAG, "AP SSID:   %s", wifi_ap_get_ssid());
    ESP_LOGI(TAG, "AP URL:    http://192.168.4.1");
#endif
    ESP_LOGI(TAG, "=============================================");
}

/* ── app_main ──────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 0. Machine-readable firmware identification ──────────────────── */
    FOF_PRINT_IDENT(TAG, FIRMWARE_NAME);

    /* ── 0b. OTA rollback state detection ─────────────────────────────── */
    /* Must run before any subsystem that might crash/panic — a rollback
     * from this boot only happens if we haven't marked the image valid,
     * which only happens after a successful HTTP upload below. */
    rollback_check_at_boot();

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

#ifdef FOF_BADGE_VARIANT
    badge_runtime_init(s_ota_pending_verify);
    badge_display_policy_runtime_init();
#endif

    badge_mode_t badge_mode = badge_mode_get();
    bool badge_backend_enabled = badge_mode_backend_enabled(badge_mode);
    bool badge_ap_enabled = badge_mode_ap_enabled(badge_mode);
#ifdef FOF_BADGE_VARIANT
    bool badge_safe_usb = badge_runtime_is_safe_mode();
#endif
#ifndef FOF_BADGE_VARIANT
    badge_mode = BADGE_MODE_BACKEND;
    badge_backend_enabled = true;
    badge_ap_enabled = true;
#else
    /* The badge LCD must show signs of life before Wi-Fi/AP work can stall
     * boot. Keep this before serial config, Wi-Fi init, and HTTP startup. */
    oled_init();
    oled_show_boot_status("Starting", badge_mode_display_name(badge_mode), "");
#endif

    /* ── 2a. Cached scanner firmware status — visible proof that the
     *        uplink retains the latest scanner image across reboots so
     *        a crash-looping scanner can fw_check us and re-flash. */
    {
        fw_store_info_t fw_info = {0};
        if (fw_store_get_info(&fw_info) && fw_info.stored) {
            ESP_LOGW(TAG, "fw_store: cached %s v%s (%lu bytes, partition=%s) "
                          "available for scanner recovery",
                     fw_info.name[0] ? fw_info.name : "scanner",
                     fw_info.version[0] ? fw_info.version : "?",
                     (unsigned long)fw_info.size,
                     fw_info.partition[0] ? fw_info.partition : "?");
        } else {
            ESP_LOGW(TAG, "fw_store: no cached scanner firmware — first stage "
                          "from backend is needed before scanner self-recovery works");
        }
    }

    /* ── 2b. Serial config window (web flasher sends config here) ──── */
    serial_config_listen(3000);
    serial_config_start_control_task();
#ifdef FOF_BADGE_VARIANT
    badge_mode = badge_mode_get();
    badge_backend_enabled = badge_mode_backend_enabled(badge_mode);
    badge_ap_enabled = badge_mode_ap_enabled(badge_mode);
    badge_safe_usb = badge_runtime_is_safe_mode();
    oled_show_boot_status(badge_safe_usb ? "USB RECOVERY" : "USB Ready",
                          badge_mode_display_name(badge_mode),
                          badge_safe_usb ? badge_runtime_safe_reason() : "network off");
#endif

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

    /* ── 5. Initialize WiFi STA/AP according to badge mode ────────────── */
#ifdef FOF_BADGE_VARIANT
    ESP_LOGW(TAG, "Badge boot network policy: persisted_mode=%s configured_backend=%d safe=%d",
             badge_mode_to_string(badge_mode),
             badge_backend_configured() ? 1 : 0,
             badge_runtime_is_safe_mode() ? 1 : 0);
    wifi_sta_set_force_standalone(true);
    badge_safe_usb = badge_runtime_is_safe_mode();
    badge_runtime_network_mode_t boot_network = badge_safe_usb
        ? BADGE_RUNTIME_NETWORK_OFF
        : badge_boot_network_mode(badge_mode);
    badge_runtime_request_network(
        boot_network,
        boot_network == BADGE_RUNTIME_NETWORK_BACKEND ? -1 : 0,
        "boot_persisted_mode");
    badge_backend_enabled = boot_network == BADGE_RUNTIME_NETWORK_BACKEND;
    badge_ap_enabled = boot_network == BADGE_RUNTIME_NETWORK_LOCAL_AP;
    oled_show_boot_status(badge_safe_usb ? "USB RECOVERY" : "USB Ready",
                          badge_mode_display_name(badge_mode),
                          badge_safe_usb ? "safe_usb" :
                          badge_runtime_network_mode_name(
                              badge_runtime_get_network_mode()));
#else
    wifi_sta_init();
    wifi_ap_init();
    wifi_sta_wait_connected(30000);
    if (wifi_sta_is_connected()) {
        rollback_mark_valid();
    }
#endif

    /* ── 7. Initialize SNTP time sync ─────────────────────────────────── */
    if (badge_backend_enabled && wifi_sta_is_connected()) {
        time_sync_init();
    } else {
        ESP_LOGW(TAG, "WiFi not connected, deferring SNTP init");
    }

    /* ── 8. Initialize GPS ────────────────────────────────────────────── */
    gps_init();

    /* ── 9. Initialize OLED display ───────────────────────────────────── */
#ifndef FOF_BADGE_VARIANT
    oled_init();
    oled_update(0, false, false, false, 0, wifi_sta_is_connected(), 0.0f, 0, NULL);
#else
    oled_show_boot_status(badge_safe_usb ? "USB RECOVERY" : "Starting Scanners",
                          badge_mode_display_name(badge_mode),
                          badge_safe_usb ? "safe_usb" : "USB control");
#endif

    /* ── 10. Initialize battery monitor ───────────────────────────────── */
    battery_init();

    /* ── 11. Initialize status LED ────────────────────────────────────── */
#ifndef FOF_BADGE_VARIANT
    led_init();
    led_set_pattern(LED_IDLE);
#else
    ESP_LOGI(TAG, "Badge build: RGB LED task disabled; LCD is primary status");
#endif

    /* ── 12. Initialize UART RX ───────────────────────────────────────── */
#ifdef FOF_BADGE_VARIANT
    if (!badge_safe_usb) {
        uart_rx_init(detection_queue);
    } else {
        ESP_LOGW(TAG, "Badge safe USB mode: scanner UART driver init held off");
    }
#else
    uart_rx_init(detection_queue);
#endif

    /* ── 13. Initialize HTTP upload ───────────────────────────────────── */
    http_upload_init(detection_queue);
#ifdef FOF_BADGE_VARIANT
    badge_runtime_set_network_apply_callback(apply_badge_network_mode);
    if (badge_runtime_get_network_mode() == BADGE_RUNTIME_NETWORK_BACKEND &&
        wifi_sta_is_connected()) {
        time_sync_init();
    }
#endif

    /* ── 14. Start all tasks ──────────────────────────────────────────── */
    ESP_LOGI(TAG, "Starting tasks...");

    /* Send OTA abort to all scanners at boot — unsticks any scanner stuck in OTA mode.
     * Uses 4x 0xFF binary sequence because JSON commands are dropped during OTA mode.
     * Follow with \n to flush the scanner's line buffer so it doesn't pollute
     * future JSON commands (the 0xFF bytes are non-newline and would sit in
     * the line accumulator forever). */
    if (
#ifdef FOF_BADGE_VARIANT
        !badge_safe_usb
#else
        true
#endif
    ) {
        const uint8_t abort_seq[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, '\n'};
        uart_write_bytes(CONFIG_BLE_SCANNER_UART, (const char *)abort_seq, sizeof(abort_seq));
#if CONFIG_DUAL_SCANNER
        uart_write_bytes(CONFIG_WIFI_SCANNER_UART, (const char *)abort_seq, sizeof(abort_seq));
#endif
        ESP_LOGI(TAG, "Sent OTA abort sequence to all scanners");
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
#ifdef FOF_BADGE_VARIANT
        ESP_LOGW(TAG, "Badge safe USB mode: skipping scanner OTA abort chatter");
#endif
    }

#ifdef FOF_BADGE_VARIANT
    if (!badge_safe_usb) {
        uart_rx_start();
        badge_runtime_note_scanner_uart_alive(true);
        http_upload_start();
        ESP_LOGI(TAG, "Badge build: HTTP upload task active; standalone mode controls upload");
    } else {
        ESP_LOGW(TAG, "Badge safe USB mode: scanner RX and HTTP upload tasks held off");
    }
#else
    uart_rx_start();
    http_upload_start();
#endif
#ifdef FOF_BADGE_VARIANT
    if (!badge_safe_usb) {
        gps_start();
    } else {
        ESP_LOGW(TAG, "Badge safe USB mode: GPS task held off");
    }
#else
    gps_start();
#endif
#ifndef FOF_BADGE_VARIANT
    led_start();
#endif

    /* Start display task (inline since it's simple) */
    xTaskCreate(display_task, "display", CONFIG_DISPLAY_STACK,
                NULL, CONFIG_DISPLAY_PRIORITY, NULL);

    /* ── 15. Start HTTP status server ────────────────────────────────── */
    if (badge_ap_enabled) {
        ESP_LOGW(TAG, "HEAP before HTTP server: %lu bytes", (unsigned long)esp_get_free_heap_size());
        http_status_init();
        ESP_LOGW(TAG, "HEAP after HTTP server: %lu bytes", (unsigned long)esp_get_free_heap_size());
#ifdef FOF_BADGE_VARIANT
        oled_show_boot_status("AP Ready", badge_mode_display_name(badge_mode),
                              "192.168.4.1");
#endif
    } else {
        ESP_LOGW(TAG, "HTTP status server disabled by badge mode");
    }

    /* ── 16. Print startup banner ─────────────────────────────────────── */
    print_banner();

    /* ── 15a. Spawn the periodic firmware auto-check task. Polls the
     *         backend on a 30-min cadence: self-OTAs the uplink and
     *         refreshes the cached scanner firmware so a connected
     *         scanner can self-recover via the existing fw_check flow. */
    if (badge_backend_enabled && wifi_sta_is_connected()) {
        fw_auto_check_init();
    } else {
        ESP_LOGI(TAG, "Firmware auto-check disabled until backend WiFi is up");
    }

    ESP_LOGI(TAG, "All tasks started. Uplink is operational.");

    /* ── 16b. Tell scanners to start transmitting ────────────────────── */
    /* Scanners boot silent (start/stop protocol) and wait for this signal. */
    {
#ifdef FOF_BADGE_VARIANT
        if (!badge_safe_usb) {
            uart_rx_send_command("{\"type\":\"ready\"}");
            send_badge_scan_profiles();
            ESP_LOGW(TAG, "Sent ready signal to all scanners — detections enabled");
            oled_show_boot_status("Scanner Ready", badge_mode_display_name(badge_mode),
                                  "USB control");
        } else {
            ESP_LOGW(TAG, "Badge safe USB mode: scanner ready signal held off");
            oled_show_boot_status("USB RECOVERY", badge_mode_display_name(badge_mode),
                                  badge_runtime_safe_reason());
        }
#else
        uart_rx_send_command("{\"type\":\"ready\"}");
        ESP_LOGW(TAG, "Sent ready signal to all scanners — detections enabled");
#endif
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

#ifndef FOF_BADGE_VARIANT
            /* Mark the OTA image valid as soon as WiFi associates — do NOT
             * wait on a successful backend upload. A long scanner-flash
             * sequence pauses the upload task for 3–5 min, which was
             * enough to trip the watchdog and revert a legitimately good
             * firmware. WiFi association is a weaker but more stable
             * liveness signal and still catches the bad-OTA case (bricked
             * WiFi stack → never associates → rollback fires). */
            if (wifi_ok) {
                rollback_mark_valid();
            }
#endif

            /* Check HTTP upload health */
            int64_t last_upload_ms = http_upload_get_last_success_ms();
            if (last_upload_ms > 0) {
                had_first_upload = true;
            }

            int64_t wifi_down_s = (now_ms - last_wifi_connected_ms) / 1000;
            int64_t upload_age_s = had_first_upload ? (now_ms - last_upload_ms) / 1000 : 0;
            int64_t uptime_s = (now_ms - boot_ms) / 1000;
            uint32_t free_heap = esp_get_free_heap_size();
            char reason[96];

#ifdef FOF_BADGE_VARIANT
            badge_runtime_poll();
            if (badge_runtime_usb_control_recovery_due(uptime_s)) {
                snprintf(reason, sizeof(reason), "usb_control_age=%llds",
                         (long long)badge_runtime_usb_control_age_s());
                ESP_LOGE(TAG, "Badge USB control watchdog entering safe USB mode: %s",
                         reason);
                oled_show_boot_status("USB RECOVERY", "restarting", reason);
                badge_runtime_force_safe_mode(true, "usb_control_stale");
                badge_runtime_arm_expected_reboot("usb_recovery");
                vTaskDelay(pdMS_TO_TICKS(120));
                esp_restart();
            }
            badge_runtime_note_scanner_uart_alive(uart_rx_is_scanner_connected());
            badge_runtime_note_main_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
            if (badge_runtime_health_can_mark_ota_valid(free_heap, uptime_s)) {
                badge_runtime_mark_stable();
                rollback_mark_valid();
            }
#endif

            ESP_LOGI(TAG, "WATCHDOG: wifi=%s down=%llds upload_age=%llds heap=%lu uptime=%llds ok=%d fail=%d det=%d",
                     wifi_ok ? "OK" : "DOWN",
                     (long long)wifi_down_s,
                     (long long)upload_age_s,
                     (unsigned long)free_heap,
                     (long long)uptime_s,
                     http_upload_get_success_count(),
                     http_upload_get_fail_count(),
                     uart_rx_get_detection_count());

#ifdef FOF_BADGE_VARIANT
            log_badge_debug(free_heap, uptime_s);
#endif

            /* Re-send ready signal every watchdog cycle (30s).
             * Ensures scanners get the start command even if they
             * missed it during boot or rebooted independently. Keep it off
             * the UART while a scanner relay owns the link; even one JSON
             * line in the binary OTA stream can corrupt a chunk. */
            if (
#ifdef FOF_BADGE_VARIANT
                !badge_runtime_is_safe_mode() &&
#endif
                !fw_store_is_relay_active() && !http_upload_is_paused()) {
                uart_rx_send_command("{\"type\":\"ready\"}");
#ifdef FOF_BADGE_VARIANT
                send_badge_scan_profiles();
#endif
            }

            if (!badge_backend_enabled) {
                if (free_heap < 4000 && uptime_s > 20 && !fw_store_is_relay_active()) {
                    snprintf(reason, sizeof(reason), "heap=%lu critically low",
                             (unsigned long)free_heap);
                    rollback_and_reboot_or_restart(reason);
                }
                continue;
            }

#ifdef FOF_BADGE_VARIANT
            if (!wifi_ok) {
                ESP_LOGW(TAG, "Badge backend WiFi is down for %llds; keeping LCD/USB/scanners alive",
                         (long long)wifi_down_s);
            }
            if (had_first_upload && upload_age_s > 900 && !fw_store_is_relay_active()) {
                ESP_LOGW(TAG, "Badge upload stale for %llds; reporting diagnostics without reboot",
                         (long long)upload_age_s);
            }
            if (!had_first_upload && uptime_s > 300 && !fw_store_is_relay_active()) {
                ESP_LOGW(TAG, "Badge backend has not uploaded after %llds; staying in USB/display fallback",
                         (long long)uptime_s);
            }
#else
            /* WiFi dead for >120s → hard reboot (was 60s, too aggressive for weak signal).
             * If the running image is still PENDING_VERIFY, rollback_and_reboot
             * reverts to the previous slot instead of restarting in place. */
            if (!wifi_ok && wifi_down_s > 120) {
                snprintf(reason, sizeof(reason), "WiFi disconnected for %llds",
                         (long long)wifi_down_s);
                rollback_and_reboot_or_restart(reason);
            }

            /* No upload success for >900s (15 min after first success) → hard
             * reboot. Wider than the old 300s because scanner-flash sequences
             * legitimately pause the upload task for 3–5 min per slot.
             * Skip during firmware relay — uploads are intentionally paused. */
            if (had_first_upload && upload_age_s > 900 && !fw_store_is_relay_active()) {
                snprintf(reason, sizeof(reason), "no successful upload for %llds",
                         (long long)upload_age_s);
                rollback_and_reboot_or_restart(reason);
            }

            /* Primary rollback trigger: first-boot firmware that never reaches
             * the backend in its first 5 min. Kept at 300 s since a truly
             * bricked image never even associates with WiFi (which is why
             * mark_valid fires on WiFi up, not on upload success). */
            if (!had_first_upload && uptime_s > 300 && !fw_store_is_relay_active()) {
                snprintf(reason, sizeof(reason), "never uploaded after %llds uptime",
                         (long long)uptime_s);
                rollback_and_reboot_or_restart(reason);
            }
#endif

            /* Heap critically low → reboot before stack overflow crash. A leak
             * this severe in the first 20s is a strong bad-OTA signal, so a
             * PENDING_VERIFY image rolls back rather than restarts-in-place. */
            if (free_heap < 4000 && uptime_s > 20 && !fw_store_is_relay_active()) {
                snprintf(reason, sizeof(reason), "heap=%lu critically low",
                         (unsigned long)free_heap);
                rollback_and_reboot_or_restart(reason);
            }
        }
    }
}
