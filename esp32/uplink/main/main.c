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
#if defined(FOF_DC34_GAME_CANARY) && CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif

/* Core */
#include "config.h"
#include "nvs_config.h"
#include "serial_config.h"
#include "badge_usb_transport.h"
#include "badge_usb_health_policy.h"
#include "badge_usb_recovery.h"
#include "uart_startup_gate.h"
#include "badge_mode.h"
#include "badge_runtime.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_power_runtime.h"
#include "badge_easter_egg_runtime.h"
#include "badge_display_policy_runtime.h"
#include "badge_theme_runtime.h"
#if defined(FOF_DC34_GAME_CANARY)
#include "badge_con_protocol.h"
#include "badge_con_radio_runtime_policy.h"
#include "badge_con_runtime.h"
#include "badge_con_vhci.h"
#endif
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

#if CONFIG_BT_ENABLED && !(defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY))
#error "uplink firmware must keep Bluetooth disabled; only the explicit controller-only badge game canary may enable it"
#endif

#if !defined(UPLINK_ESP32S3)
#error "Supported FoF uplink firmware is ESP32-S3 only."
#endif

#ifdef FOF_BADGE_VARIANT
#define BADGE_DISPLAY_UPDATE_MS 250
#endif

static StaticTask_t s_display_task_tcb;
/* ESP-IDF uses byte-sized StackType_t on ESP32. Static storage keeps the
 * display worker available even when the heap is too fragmented for a late
 * task allocation. The task stays notification-gated during recovery-only
 * startup. */
_Static_assert(sizeof(StackType_t) == 1,
               "display static stack sizing requires byte StackType_t");
static StackType_t s_display_task_stack[CONFIG_DISPLAY_STACK]
    __attribute__((aligned(16)));
static TaskHandle_t s_display_task_handle = NULL;
static volatile bool s_ota_pending_verify = false;

/* Scanner firmware discovery is scanner-driven once after delayed boot.
 * Explicit force-checks remain available through the busy-safe control path. */

static const char *TAG = "main";
static scanner_info_t s_debug_scanner_snapshots[2] = {0};

#define UART_STARTUP_OPERATION_RETRIES 600U
#define UART_STARTUP_OPERATION_RETRY_MS 50U
#define UART_STARTUP_RELEASE_ATTEMPTS 3U
#define UART_STARTUP_RELEASE_RETRY_MS 10U

#if defined(FOF_DC34_GAME_CANARY)
static badge_con_radio_runtime_policy_t s_badge_con_radio_runtime;
/* Latched for the full boot. Clearing the RTC maintenance marker immediately
 * before a planned restart must not let the display task initialize game RF
 * during the final drain/restart window. */
static bool s_badge_update_maintenance_boot = false;

#define BADGE_UPDATE_SUPERVISOR_INTERVAL_MS 1000U
#define BADGE_UPDATE_HEALTH_TIMEOUT_MS 60000U
#define BADGE_UPDATE_RECEIPT_TIMEOUT_MS 1000U

static bool badge_con_campaign_allows_radio(
    const fw_store_campaign_snapshot_t *campaign)
{
    return campaign && !campaign->radio_inhibited &&
           (campaign->state == FW_CAMPAIGN_IDLE ||
            campaign->state == FW_CAMPAIGN_ALL_TERMINAL);
}

static void badge_con_radio_runtime_poll(uint32_t now_ms)
{
    fw_store_campaign_snapshot_t campaign = {0};
    bool campaign_sampled =
        fw_store_campaign_state_sample(&campaign);
    bool must_yield = !campaign_sampled ||
                      !badge_con_campaign_allows_radio(&campaign);
    if (!badge_con_vhci_apply_radio_policy(
            must_yield, campaign.operation_epoch)) {
        must_yield = true;
    }

    uint32_t peer = 0U;
    uint8_t session = 0U;
    bool identity_valid =
        badge_con_runtime_identity(&peer, &session);
    badge_con_snapshot_t game = {0};
    bool game_valid = badge_con_runtime_snapshot(&game);
    if (badge_con_radio_runtime_controller_init_allowed(
            !must_yield,
            identity_valid,
            game_valid,
            game.active,
            s_ota_pending_verify)) {
        (void)badge_con_vhci_init(peer, session);
    }

    bool scanner_rebooted = false;
    for (int lane = 0; lane < BADGE_CON_RADIO_SCANNER_LANES; lane++) {
        scanner_info_t scanner = {0};
        if (uart_rx_get_scanner_info_snapshot(lane, &scanner) &&
            badge_con_radio_runtime_observe_boot_id(
                &s_badge_con_radio_runtime, lane, scanner.boot_id)) {
            scanner_rebooted = true;
        }
    }
    if (scanner_rebooted) {
        badge_con_runtime_clear_self_ack();
        badge_con_vhci_set_self_ready(false);
    }

    bool exact_self_ack =
        badge_con_runtime_self_ack_matches(peer, session);
    if (badge_con_radio_runtime_retry_self_due(
            &s_badge_con_radio_runtime, exact_self_ack, now_ms)) {
        badge_con_runtime_clear_self_ack();
        exact_self_ack = false;
    }
    bool self_ready = identity_valid &&
        badge_con_radio_runtime_all_self_sent(
            &s_badge_con_radio_runtime) &&
        exact_self_ack;
    badge_con_vhci_set_identity_state(
        game_valid ? game.role : BADGE_CON_ROLE_NORMAL,
        game_valid && game.super);
    badge_con_vhci_set_game_active(game_valid && game.active);
    badge_con_vhci_set_self_ready(
        !must_yield && game_valid && game.active && self_ready);

    /* Poll even while inhibited. This is the only path that can consume the
     * controller's disable acknowledgment and publish exact radio quiescence
     * to a waiting firmware operation. */
    badge_con_vhci_poll(now_ms);
    if (must_yield || !game_valid || !game.active || !identity_valid ||
        self_ready) {
        return;
    }

    badge_con_vhci_snapshot_t radio = {0};
    badge_con_vhci_snapshot(&radio);
    if (!radio.controller_initialized ||
        radio.state == BADGE_CON_VHCI_FAILED ||
        radio.advertising || radio.command_in_flight ||
        radio.radio_state_uncertain) {
        return;
    }

    int lane = badge_con_radio_runtime_next_unsent_lane(
        &s_badge_con_radio_runtime);
    if (lane < 0) {
        return;
    }

    /* Checked UART writes may wait for the shared lease. Re-sample the
     * fail-busy gate immediately before each one so game setup never queues
     * behind or enters a firmware byte stream. */
    if (fw_store_game_radio_must_yield()) {
        return;
    }
    char command[96] = {0};
    if (badge_con_render_self_command(
            peer, session, command, sizeof(command)) &&
        uart_rx_send_command_to_scanner_checked(lane, command)) {
        badge_con_radio_runtime_note_self_sent(
            &s_badge_con_radio_runtime, lane, now_ms);
    }
}
#endif

static bool uart_startup_try_claim(void *context,
                                   fw_operation_token_t *out)
{
    (void)context;
    return fw_store_operation_try_begin(
        FW_OPERATION_OWNER_RUNTIME_STARTUP, false, out);
}

static bool uart_startup_start(void *context)
{
    (void)context;
    return uart_rx_start();
}

static bool uart_startup_release(void *context,
                                 fw_operation_token_t token)
{
    (void)context;
    return fw_store_operation_end(token);
}

static void uart_startup_delay(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static uart_startup_gate_result_t start_uart_rx_with_operation_gate(void)
{
    const uart_startup_gate_hooks_t hooks = {
        .context = NULL,
        .try_claim = uart_startup_try_claim,
        .start = uart_startup_start,
        .release = uart_startup_release,
        .delay = uart_startup_delay,
    };
    uart_startup_gate_result_t result = uart_startup_gate_run(
        &hooks,
        UART_STARTUP_OPERATION_RETRIES,
        UART_STARTUP_OPERATION_RETRY_MS,
        UART_STARTUP_RELEASE_ATTEMPTS,
        UART_STARTUP_RELEASE_RETRY_MS);
    if (result == UART_STARTUP_GATE_CLAIM_TIMEOUT) {
        ESP_LOGE(TAG, "UART startup operation remained busy");
    } else if (result == UART_STARTUP_GATE_RELEASE_FAILED) {
        ESP_LOGE(TAG, "UART startup operation token release failed");
    }
    return result;
}

static void log_detection_queue_heap(const char *stage)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t required = sizeof(StaticQueue_t) +
        (CONFIG_DETECTION_QUEUE_SIZE * sizeof(drone_detection_t));
    ESP_LOGW(TAG,
             "Detection queue heap [%s]: free=%u largest=%u need=%u integrity=%s",
             stage,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)required,
             heap_caps_check_integrity(caps, true) ? "ok" : "BAD");
}

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
    const scanner_info_t *ble_info =
        uart_rx_get_scanner_info_snapshot(0, &s_debug_scanner_snapshots[0])
            ? &s_debug_scanner_snapshots[0] : NULL;
    const scanner_info_t *wifi_info =
        uart_rx_get_scanner_info_snapshot(1, &s_debug_scanner_snapshots[1])
            ? &s_debug_scanner_snapshots[1] : NULL;

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

/* Assign the two physical scanner slots as soon as their UART drivers exist.
 * This deliberately does not wait for an identity/connected frame: identical
 * scanner images only wait a bounded time for their role at cold boot, while
 * the uplink's USB configuration window may remain open longer than that. */
static void send_badge_boot_slot_roles(void)
{
    char cmd[128];
    const char *profile = fof_policy_scan_profile_for_slot(0, false);
    snprintf(cmd, sizeof(cmd),
             "{\"type\":\"scan_profile\",\"%s\":\"%s\","
             "\"%s\":\"%s\"}",
             JSON_KEY_SCAN_PROFILE, profile,
             JSON_KEY_SLOT_ROLE, fof_policy_slot_role_for_slot(0));
    bool ble_sent = uart_rx_send_command_to_scanner_checked(0, cmd);
    ESP_LOGI(TAG, "BADGE_BOOT_ROLE ble profile=%s sent=%d",
             profile, ble_sent ? 1 : 0);

#if CONFIG_DUAL_SCANNER
    profile = fof_policy_scan_profile_for_slot(1, false);
    snprintf(cmd, sizeof(cmd),
             "{\"type\":\"scan_profile\",\"%s\":\"%s\","
             "\"%s\":\"%s\"}",
             JSON_KEY_SCAN_PROFILE, profile,
             JSON_KEY_SLOT_ROLE, fof_policy_slot_role_for_slot(1));
    bool wifi_sent = uart_rx_send_command_to_scanner_checked(1, cmd);
    ESP_LOGI(TAG, "BADGE_BOOT_ROLE wifi profile=%s sent=%d",
             profile, wifi_sent ? 1 : 0);
#endif
}

static void send_badge_scan_profiles(void)
{
    if (badge_power_runtime_is_quiet() ||
        fw_store_is_relay_active() || http_upload_is_paused() ||
        uart_rx_is_node_calibration_mode()) {
        return;
    }

    bool ble_connected = uart_rx_is_ble_scanner_connected();
    bool wifi_connected = uart_rx_is_wifi_scanner_connected();
    char cmd[128];

    if (ble_connected) {
        /* Badge scanner slots are physically fixed.  Never promote a lone
         * peer to hybrid while the other MCU is rebooting: that transient
         * profile initializes both radios and can exhaust DMA-capable SRAM
         * before the second identity frame arrives. */
        const char *profile = fof_policy_scan_profile_for_slot(0, false);
        snprintf(cmd, sizeof(cmd),
                 "{\"type\":\"scan_profile\",\"%s\":\"%s\","
                 "\"%s\":\"%s\"}",
                 JSON_KEY_SCAN_PROFILE, profile,
                 JSON_KEY_SLOT_ROLE, fof_policy_slot_role_for_slot(0));
        bool ok = uart_rx_send_command_to_scanner_checked(0, cmd);
        ESP_LOGI(TAG, "BADGE_ROLE ble profile=%s sent=%d", profile, ok ? 1 : 0);
    }

#if CONFIG_DUAL_SCANNER
    if (wifi_connected) {
        const char *profile = fof_policy_scan_profile_for_slot(1, false);
        snprintf(cmd, sizeof(cmd),
                 "{\"type\":\"scan_profile\",\"%s\":\"%s\","
                 "\"%s\":\"%s\"}",
                 JSON_KEY_SCAN_PROFILE, profile,
                 JSON_KEY_SLOT_ROLE, fof_policy_slot_role_for_slot(1));
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
#ifdef FOF_BADGE_VARIANT
            badge_runtime_expected_reboot_lease_t rollback_lease = {0};
            badge_runtime_expected_reboot_arm_result_t arm_result =
                badge_runtime_arm_expected_reboot(
                    "boot_health_rollback",
                    BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK,
                    &rollback_lease);
            if (arm_result !=
                BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED) {
                ESP_LOGE(
                    TAG,
                    "OTA rollback blocked: expected-reboot ownership=%d",
                    (int)arm_result);
                return;
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_ota_mark_app_invalid_rollback_and_reboot();
#ifdef FOF_BADGE_VARIANT
            /* A successful rollback never returns. If no fallback slot can
             * boot, stop retrying the pending image and expose the minimal
             * USB/display recovery surface on the next expected app boot.
             * Reuse the rollback lease so no duplicate arm can erase it. */
            badge_runtime_arm_usb_recovery_once();
            if (!badge_runtime_expected_reboot_lease_is_owned(
                    &rollback_lease)) {
                ESP_LOGE(TAG, "OTA rollback fallback lost reboot ownership");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_restart();
#endif
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

#ifndef FOF_BADGE_VARIANT
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
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
#else
static _Noreturn void rollback_and_reboot_with_owned_lease(
    const char *reason,
    const badge_runtime_expected_reboot_lease_t *reboot_lease)
{
    /*
     * The firmware-operation restart reservation is deliberately
     * irreversible. Reaching this executor therefore requires a lease already
     * published by the caller, and every impossible ownership-loss branch
     * holds safely instead of returning to a permanently reserved runtime.
     */
    if (!badge_runtime_expected_reboot_lease_is_owned(reboot_lease)) {
        ESP_LOGE(TAG, "Watchdog executor entered without reboot ownership");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    if (s_ota_pending_verify) {
        ESP_LOGE(TAG,
                 "OTA ROLLBACK: %s while PENDING_VERIFY — reverting to "
                 "previous slot",
                 reason ? reason : "watchdog");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* Does not return on success. If it does fall through (e.g. no valid
         * fallback partition), reuse this exact legacy-compatible owner for
         * the safe app restart below. */
    }
    /*
     * A validated repeatable failure, or a pending rollback that unexpectedly
     * returned, gets one safe-USB boot. Ownership was proved before either
     * token is published, so a duplicate restart cannot alter retained bytes.
     */
    badge_runtime_arm_usb_recovery_once();
    ESP_LOGE(TAG, "WATCHDOG REBOOT: %s", reason);
    if (!badge_runtime_expected_reboot_lease_is_owned(reboot_lease)) {
        ESP_LOGE(TAG, "Watchdog restart lost expected-reboot ownership");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
#endif

/* ── Display update task ───────────────────────────────────────────────── */

#ifdef FOF_BADGE_VARIANT
/* USB dispatch is live before the late startup and watchdog checks below.
 * An automatic restart must therefore acquire the same atomic exclusion as
 * firmware staging/relay. If an operation already owns it, sleep between
 * retries so the upload can finish; the successful reservation also prevents
 * a new operation from starting before the non-returning restart. */
static bool badge_automatic_restart_when_firmware_idle(
    const char *reason)
{
    bool ownership_defer_logged = false;
    badge_runtime_expected_reboot_lease_t reboot_lease = {0};
    for (;;) {
        badge_runtime_expected_reboot_target_t target =
            s_ota_pending_verify
                ? BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK
                : BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT;
        badge_usb_firmware_restart_prepare_result_t prepare_result =
            badge_usb_recovery_prepare_firmware_restart(
                reason ? reason : "watchdog", target, &reboot_lease);
        if (prepare_result ==
            BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
            break;
        }
        if (prepare_result ==
            BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY) {
            /*
             * The shared preparation helper never waits while owning one
             * side of the reboot/firmware exclusion pair. It releases any
             * transient reboot lease before reporting firmware BUSY.
             */
            if (!ownership_defer_logged) {
                ESP_LOGW(
                    TAG,
                    "Automatic restart waiting for reboot/firmware owner: %s",
                    reason ? reason : "unknown");
                ownership_defer_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        ESP_LOGE(
            TAG,
            "Automatic restart cancelled: ownership preparation failed: %s",
            reason ? reason : "unknown");
        return false;
    }

    rollback_and_reboot_with_owned_lease(reason, &reboot_lease);
}
#endif

static void display_task(void *arg)
{
    /* Created before scanner/network work so its memory is guaranteed, but
     * do not let it overwrite the boot/recovery screen until normal startup
     * has completed all required worker creation. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Display task started");

    int prev_detection_count = 0;
#ifndef FOF_BADGE_VARIANT
    int overlay_ticks = 0;
    detection_summary_t last_det = {0};
#endif
#if defined(FOF_DC34_GAME_CANARY)
    bool update_mode_drawn = false;
#endif

    while (1) {
#if defined(FOF_DC34_GAME_CANARY)
        if (s_badge_update_maintenance_boot) {
            if (!update_mode_drawn) {
                char update_session[BADGE_UPDATE_SESSION_CAPACITY] = {0};
                (void)badge_runtime_update_session_copy(update_session);
                oled_show_boot_status("UPDATE MODE", "USB + UART",
                                      update_session);
                update_mode_drawn = true;
            }
            if (oled_is_powered()) {
                badge_runtime_note_display_alive();
            }
            badge_runtime_note_display_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
            vTaskDelay(pdMS_TO_TICKS(BADGE_UPDATE_SUPERVISOR_INTERVAL_MS));
            continue;
        }
#endif
        /* Gather current state */
        int detection_count = uart_rx_get_detection_count();
#if defined(FOF_DC34_GAME_CANARY)
        badge_con_radio_runtime_poll(
            (uint32_t)(esp_timer_get_time() / 1000));
#endif
#ifdef FOF_BADGE_VARIANT
        badge_power_runtime_poll();
        if (badge_power_runtime_is_quiet()) {
            prev_detection_count = detection_count;
            if (oled_is_powered()) {
                badge_runtime_note_display_alive();
            }
            badge_runtime_note_display_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
#if defined(FOF_DC34_GAME_CANARY)
            vTaskDelay(pdMS_TO_TICKS(BADGE_DISPLAY_UPDATE_MS));
#else
            vTaskDelay(pdMS_TO_TICKS(1000));
#endif
            continue;
        }
#endif
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
                                                   recent[i].badge_label,
                                                   recent[i].badge_class_name,
                                                   recent[i].badge_entity_key,
                                                   recent[i].source, recent[i].confidence,
                                                   recent[i].threat_score,
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
        bool ble_connected = uart_rx_is_ble_scanner_connected();
        bool wifi_connected = uart_rx_is_wifi_scanner_connected();
        bool scanner_ok  = ble_connected || wifi_connected;
        bool backend_ok  = (http_upload_get_success_count() > 0 &&
                           http_upload_get_fail_count() < http_upload_get_success_count());
        uint32_t uptime  = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
        static char device_id_buf[64] = {0};
        if (device_id_buf[0] == '\0') {
            nvs_config_get_device_id(device_id_buf, sizeof(device_id_buf));
        }

        /* OLED: BLE scanner, WiFi scanner, backend, uploads, WiFi network, battery, uptime, ID */
        oled_update(detection_count, ble_connected, wifi_connected, backend_ok,
                    upload_count, wifi_ok, battery_pct, uptime, device_id_buf);
#ifdef FOF_BADGE_VARIANT
        if (oled_is_powered()) {
            badge_runtime_note_display_alive();
        }
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
    ESP_LOGI(TAG, "  Friend or Foe — %s v%s (ESP32-S3)",
             FOF_FIRMWARE_TARGET, FOF_VERSION);
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

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
static void badge_send_scanner_ota_abort_sentinel(void)
{
    /* Scanners can reboot while their JSON parser is replaced by the OTA
     * binary parser. Clear that state before starting the RX workers, then
     * leave them silent until normal mode explicitly sends READY. */
    uint8_t abort_seq[OTA_ABORT_SENTINEL_COUNT + 1];
    memset(abort_seq, OTA_ABORT_SENTINEL_BYTE, OTA_ABORT_SENTINEL_COUNT);
    abort_seq[OTA_ABORT_SENTINEL_COUNT] = '\n';
    uart_write_bytes(CONFIG_BLE_SCANNER_UART, (const char *)abort_seq,
                     sizeof(abort_seq));
#if CONFIG_DUAL_SCANNER
    uart_write_bytes(CONFIG_WIFI_SCANNER_UART, (const char *)abort_seq,
                     sizeof(abort_seq));
#endif
    ESP_LOGI(TAG, "Sent OTA abort sequence to all scanners");
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void badge_update_emit_required_receipt(const char *receipt)
{
    if (!receipt) {
        return;
    }
    if (!badge_usb_transport_emit(
            receipt, strlen(receipt), BADGE_USB_FRAME_REQUIRED,
            pdMS_TO_TICKS(BADGE_UPDATE_RECEIPT_TIMEOUT_MS))) {
        ESP_LOGE(TAG, "Update-maintenance required response was not delivered");
    }
    if (!badge_usb_transport_drain(
            pdMS_TO_TICKS(BADGE_UPDATE_RECEIPT_TIMEOUT_MS))) {
        ESP_LOGE(TAG, "Update-maintenance response drain timed out");
    }
}

static bool badge_update_health_rollback(
    const char session[BADGE_UPDATE_SESSION_CAPACITY])
{
    badge_runtime_expected_reboot_lease_t rollback_lease = {0};
    badge_usb_firmware_restart_prepare_result_t prepare_result =
        badge_usb_recovery_prepare_firmware_restart(
            "update_health_rollback",
            BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK,
            &rollback_lease);
    if (prepare_result !=
        BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
        ESP_LOGE(
            TAG,
            "Update-health rollback ownership preparation=%d",
            (int)prepare_result);
        return false;
    }

    char receipt[256] = {0};
    snprintf(
        receipt, sizeof(receipt),
        "FOF_UPDATE_MODE:{\"ok\":false,\"phase\":\"error\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":true,"
        "\"error\":\"maintenance_health_failed\"}\n",
        session ? session : "");
    oled_show_boot_status("UPDATE FAILED", "rolling back",
                          "health check");
    badge_update_emit_required_receipt(receipt);
    (void)badge_runtime_clear_update_maintenance(
        "maintenance_health_failed");
    if (!badge_runtime_expected_reboot_lease_is_owned(&rollback_lease)) {
        ESP_LOGE(TAG, "Update-health rollback lost reboot ownership");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    esp_ota_mark_app_invalid_rollback_and_reboot();

    /* No rollback slot was usable. Keep USB/display recovery reachable
     * instead of retrying a degraded pending image indefinitely. Reuse the
     * already-owned rollback token so +4 remains visible to v0.78. */
    badge_runtime_arm_usb_recovery_once();
    badge_usb_recovery_restart_with_owned_lease(
        BADGE_USB_RESET_APP,
        "update_health_rollback",
        &rollback_lease);
}

static bool badge_update_inactivity_restart(
    const char session[BADGE_UPDATE_SESSION_CAPACITY])
{
    badge_runtime_expected_reboot_lease_t reboot_lease = {0};
    badge_usb_firmware_restart_prepare_result_t prepare_result =
        badge_usb_recovery_prepare_firmware_restart(
            "update_inactivity",
            BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
            &reboot_lease);
    if (prepare_result !=
        BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
        return false;
    }

    char receipt[224] = {0};
    snprintf(
        receipt, sizeof(receipt),
        "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"aborting\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":true}\n",
        session ? session : "");
    oled_show_boot_status("UPDATE MODE", "session idle", "restarting");
    badge_update_emit_required_receipt(receipt);
    (void)badge_runtime_clear_update_maintenance("inactivity_timeout");
    badge_usb_recovery_restart_with_owned_lease(
        BADGE_USB_RESET_APP, "update_inactivity", &reboot_lease);
}

static bool badge_update_terminal_failure_restart(
    const char session[BADGE_UPDATE_SESSION_CAPACITY])
{
    badge_runtime_expected_reboot_lease_t reboot_lease = {0};
    badge_usb_firmware_restart_prepare_result_t prepare_result =
        badge_usb_recovery_prepare_firmware_restart(
            "update_terminal_failure",
            BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
            &reboot_lease);
    if (prepare_result !=
        BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
        return false;
    }

    char receipt[256] = {0};
    snprintf(
        receipt, sizeof(receipt),
        "FOF_UPDATE_MODE:{\"ok\":false,\"phase\":\"error\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":true,"
        "\"error\":\"scanner_campaign_failed\"}\n",
        session ? session : "");
    oled_show_boot_status("UPDATE FAILED", "scanner lane",
                          "restarting");
    badge_update_emit_required_receipt(receipt);
    (void)badge_runtime_clear_update_maintenance(
        "scanner_campaign_failed");
    badge_usb_recovery_restart_with_owned_lease(
        BADGE_USB_RESET_APP,
        "update_terminal_failure",
        &reboot_lease);
}
#endif

/* ── app_main ──────────────────────────────────────────────────────────── */

void app_main(void)
{
    bool required_tasks_started = true;
    bool usb_transport_started = badge_usb_transport_start(3000);
#ifdef FOF_BADGE_VARIANT
    bool badge_startup_recovery_only = false;
    const char *badge_startup_safe_reason = NULL;
#endif

    /* ── 0. Machine-readable firmware identification ──────────────────── */
    FOF_PRINT_IDENT(TAG, FOF_FIRMWARE_TARGET);

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
    if (!fw_store_init_auto_update_coordinator()) {
        ESP_LOGE(TAG, "Scanner-update coordinator initialization failed");
#ifdef FOF_BADGE_VARIANT
        badge_startup_recovery_only = true;
        badge_startup_safe_reason = "coordinator_init";
#else
        return;
#endif
    }
    log_detection_queue_heap("after_nvs");

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    QueueHandle_t detection_queue = NULL;
    ESP_LOGI(TAG,
             "CON CRUD canary: reclaimed unused %u-byte detection queue",
             (unsigned)(CONFIG_DETECTION_QUEUE_SIZE *
                        sizeof(drone_detection_t)));
#else
    /* Reserve the live detection queue before BLE, display DMA, USB control,
     * and the event loop fragment internal RAM. xQueueCreate() requires one
     * contiguous internal allocation for all 48 drone_detection_t records. */
    QueueHandle_t detection_queue = xQueueCreate(
        CONFIG_DETECTION_QUEUE_SIZE, sizeof(drone_detection_t));
    if (!detection_queue) {
        ESP_LOGE(TAG, "Failed to create detection queue!");
#ifdef FOF_BADGE_VARIANT
        badge_startup_recovery_only = true;
        if (!badge_startup_safe_reason) {
            badge_startup_safe_reason = "detection_queue";
        }
#else
        return;
#endif
    } else {
        ESP_LOGI(TAG, "Detection queue created (%d slots, %d bytes each)",
                 CONFIG_DETECTION_QUEUE_SIZE, (int)sizeof(drone_detection_t));
    }
#endif
    log_detection_queue_heap("after_queue");

#ifdef FOF_BADGE_VARIANT
#if defined(FOF_DC34_GAME_CANARY)
    badge_runtime_init(s_ota_pending_verify);
    bool badge_update_maintenance =
        badge_runtime_update_maintenance_active();
    s_badge_update_maintenance_boot = badge_update_maintenance;
    /* This state-only runtime owns the expected-reboot hook used to preserve
     * the exact game identity across every planned maintenance/OTA reboot. */
    badge_con_runtime_init();
    if (s_badge_update_maintenance_boot) {
#if CONFIG_BT_ENABLED
        esp_err_t bt_release =
            esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        if (bt_release == ESP_OK) {
            ESP_LOGW(TAG,
                     "Update maintenance: BLE controller memory released");
        } else {
            ESP_LOGE(TAG,
                     "Update maintenance: BLE memory release failed: %s",
                     esp_err_to_name(bt_release));
        }
#endif
    } else {
        badge_easter_egg_runtime_init();
        badge_con_radio_runtime_policy_init(&s_badge_con_radio_runtime);
        if (!badge_con_vhci_prepare()) {
            ESP_LOGE(TAG,
                     "CON CRUD radio mutex unavailable; game radio held off");
        }
    }
#else
    badge_easter_egg_runtime_init();
    badge_runtime_init(s_ota_pending_verify);
#endif
    badge_power_runtime_init();
    badge_display_policy_runtime_init();
    badge_theme_runtime_init();
    if (badge_runtime_usb_recovery_once_consumed()) {
        badge_startup_recovery_only = true;
        if (!badge_startup_safe_reason) {
            badge_startup_safe_reason = "usb_safe_once";
        }
    }
    if (badge_startup_safe_reason) {
        badge_runtime_force_safe_mode(true, badge_startup_safe_reason);
    }
#endif
    log_detection_queue_heap("after_badge_runtime");

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
    if (!oled_badge_buttons_start()) {
        badge_startup_recovery_only = true;
        if (!badge_startup_safe_reason) {
            badge_startup_safe_reason = "button_task";
        }
        badge_runtime_force_safe_mode(true, badge_startup_safe_reason);
        badge_safe_usb = true;
    }
    oled_show_boot_status("Starting", badge_mode_display_name(badge_mode), "");
    if (!usb_transport_started) {
        oled_show_boot_status("USB RECOVERY", badge_mode_display_name(badge_mode),
                              "transport init failed");
        /* The first initialization failure receives one expected restart.
         * RTC state prevents a panic/restart loop; the second boot remains
         * alive with the display/button recovery surface available. */
        if (!badge_runtime_usb_recovery_once_consumed()) {
            if (!badge_usb_recovery_restart(
                    BADGE_USB_RESET_APP, "usb_safe_once")) {
                ESP_LOGE(
                    TAG,
                    "USB transport recovery restart blocked without owner");
            }
        }
        badge_startup_recovery_only = true;
        badge_startup_safe_reason = "usb_transport_init";
        badge_runtime_force_safe_mode(true, "usb_transport_init");
        badge_safe_usb = true;
    }
#endif

    s_display_task_handle = xTaskCreateStatic(
        display_task, "display", CONFIG_DISPLAY_STACK,
        NULL, CONFIG_DISPLAY_PRIORITY, s_display_task_stack,
        &s_display_task_tcb);
    if (!s_display_task_handle) {
        ESP_LOGE(TAG, "Failed to create static display task (stack=%d)",
                 CONFIG_DISPLAY_STACK);
#ifdef FOF_BADGE_VARIANT
        badge_startup_recovery_only = true;
        if (!badge_startup_safe_reason) {
            badge_startup_safe_reason = "display_task";
        }
        badge_runtime_force_safe_mode(true, badge_startup_safe_reason);
        badge_safe_usb = true;
#else
        required_tasks_started = false;
#endif
    }
    log_detection_queue_heap("after_display");

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
#ifdef FOF_BADGE_VARIANT
    if (!badge_startup_recovery_only) {
#endif
    if (!uart_rx_scanner_tx_lease_init()) {
        ESP_LOGE(TAG, "Scanner UART lease unavailable before USB firmware staging");
#ifdef FOF_BADGE_VARIANT
        badge_startup_recovery_only = true;
        badge_startup_safe_reason = "scanner_uart_lease";
        badge_runtime_force_safe_mode(true, "scanner_uart_lease");
        badge_safe_usb = true;
#else
        required_tasks_started = false;
#endif
    }
#ifdef FOF_BADGE_VARIANT
    }
#endif
#ifdef FOF_BADGE_VARIANT
#if defined(FOF_DC34_GAME_CANARY)
    if (badge_update_maintenance) {
        /* Maintenance initializes the same UART hardware without emitting
         * normal slot-role/game/profile chatter. */
        uart_rx_init(detection_queue);
    } else if (!badge_safe_usb) {
        uart_rx_init(detection_queue);
        send_badge_boot_slot_roles();
    } else {
        ESP_LOGW(TAG, "Badge safe USB mode: scanner UART driver init held off");
    }
#else
    if (!badge_safe_usb) {
        uart_rx_init(detection_queue);
        send_badge_boot_slot_roles();
    } else {
        ESP_LOGW(TAG, "Badge safe USB mode: scanner UART driver init held off");
    }
#endif
#endif
#ifdef FOF_BADGE_VARIANT
    badge_usb_transport_set_recovery_only(badge_startup_recovery_only);
    bool usb_dispatch_ready = usb_transport_started;
#else
    bool usb_dispatch_ready = usb_transport_started && required_tasks_started;
#endif
    if (usb_dispatch_ready) {
        badge_usb_transport_set_dispatch_ready();
        if (!badge_usb_transport_wait_boot_window(pdMS_TO_TICKS(3500))) {
            ESP_LOGW(TAG, "USB boot window wait timed out; transport remains active");
        }
    } else if (usb_transport_started) {
        ESP_LOGE(TAG,
                 "USB dispatch remains gated: startup dependency unavailable");
    }
#ifdef FOF_BADGE_VARIANT
    /* Repeat once at the far side of the bounded USB window. This covers a
     * simultaneous power-up where the first role frame reaches a scanner
     * just before its command listener comes online, while remaining inside
     * the scanner's six-second cold-boot allocation budget. */
#if defined(FOF_DC34_GAME_CANARY)
    if (!badge_safe_usb && !badge_update_maintenance) {
#else
    if (!badge_safe_usb) {
#endif
        send_badge_boot_slot_roles();
    }
#endif
    log_detection_queue_heap("after_usb_control");
#ifdef FOF_BADGE_VARIANT
    badge_mode = badge_mode_get();
    badge_backend_enabled = badge_mode_backend_enabled(badge_mode);
    badge_ap_enabled = badge_mode_ap_enabled(badge_mode);
    badge_safe_usb = badge_runtime_is_safe_mode();
    oled_show_boot_status(badge_safe_usb ? "USB RECOVERY" : "USB Ready",
                          badge_mode_display_name(badge_mode),
                          badge_safe_usb ? badge_runtime_safe_reason() : "network off");
    if (badge_startup_recovery_only) {
        ESP_LOGE(TAG,
                 "Badge startup dependency unavailable; holding the minimal "
                 "USB/display recovery surface");
        return;
    }
#endif

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    /* Update maintenance is a distinct, radio-free boot. USB was started
     * first, the LCD/buttons are live, and both scanner UART drivers own their
     * fixed board pins. Do not create the event loop or initialize BLE,
     * Wi-Fi, GPS, normal scanning, or game effects anywhere on this path. */
    badge_update_maintenance =
        badge_update_maintenance && !badge_runtime_is_safe_mode();
    if (badge_update_maintenance) {
        char update_session[BADGE_UPDATE_SESSION_CAPACITY] = {0};
        if (!badge_runtime_update_session_copy(update_session)) {
            ESP_LOGE(TAG,
                     "Update-maintenance session disappeared after admission");
            badge_runtime_force_safe_mode(true, "update_session_missing");
            oled_show_boot_status("USB RECOVERY", "update failed",
                                  "session missing");
            return;
        }

        oled_show_boot_status("UPDATE MODE", "USB + UART", update_session);
        badge_send_scanner_ota_abort_sentinel();

        uart_startup_gate_result_t uart_startup =
            start_uart_rx_with_operation_gate();
        if (uart_startup != UART_STARTUP_GATE_OK) {
            ESP_LOGE(TAG,
                     "Update-maintenance scanner UART workers unavailable");
            if (!badge_automatic_restart_when_firmware_idle(
                    "update_uart_rx_start")) {
                badge_runtime_force_safe_mode(true, "reboot_arm_failed");
            }
        }
        if (!badge_power_runtime_start()) {
            ESP_LOGE(TAG,
                     "Update-maintenance button/power runtime unavailable");
            if (!badge_automatic_restart_when_firmware_idle(
                    "update_power_runtime")) {
                badge_runtime_force_safe_mode(true, "reboot_arm_failed");
            }
        }
        if (!fw_store_start_auto_update_coordinator()) {
            ESP_LOGE(TAG,
                     "Update-maintenance coordinator worker unavailable");
            if (!badge_automatic_restart_when_firmware_idle(
                    "update_coordinator_start")) {
                badge_runtime_force_safe_mode(true, "reboot_arm_failed");
            }
        }
        if (!fw_store_restore_auto_update_coordinator()) {
            ESP_LOGE(TAG,
                     "Update-maintenance durable coordinator restore failed");
            if (!badge_automatic_restart_when_firmware_idle(
                    "update_coordinator_restore")) {
                badge_runtime_force_safe_mode(true, "reboot_arm_failed");
            }
        }
        fw_store_scanner_stage_status_t maintenance_entry_stage = {0};
        bool maintenance_entry_stage_certain =
            fw_store_scanner_stage_status_snapshot(
                &maintenance_entry_stage);
        uint32_t maintenance_entry_scanner_generation =
            maintenance_entry_stage_certain &&
            maintenance_entry_stage.phase == FW_SCANNER_STAGE_COMMITTED
                ? maintenance_entry_stage.generation
                : 0U;

        /* The display task stays on its maintenance fast path for this entire
         * boot and only publishes LCD liveness; it never polls the game radio
         * or renders the normal detection dashboard. */
        xTaskNotifyGive(s_display_task_handle);
        ESP_LOGW(TAG,
                 "UPDATE MODE active session=%s; BLE/WiFi/GPS/scanning held off",
                 update_session);

        const uint32_t maintenance_boot_ms =
            (uint32_t)(esp_timer_get_time() / 1000);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(
                BADGE_UPDATE_SUPERVISOR_INTERVAL_MS));
            uint32_t now_ms =
                (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t uptime_ms =
                (uint32_t)(now_ms - maintenance_boot_ms);
            uint32_t internal_free = (uint32_t)heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL);
            uint32_t internal_largest =
                (uint32_t)heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL);

            badge_runtime_poll();
            badge_runtime_note_main_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));

            if (s_ota_pending_verify) {
                if (badge_runtime_update_health_can_mark_ota_valid(
                        internal_free, internal_largest, uptime_ms)) {
                    rollback_mark_valid();
                }
                if (s_ota_pending_verify &&
                    uptime_ms >= BADGE_UPDATE_HEALTH_TIMEOUT_MS) {
                    ESP_LOGE(
                        TAG,
                        "Update-maintenance health timeout: free=%lu "
                        "largest=%lu display=%d uart=%d usb=%d",
                        (unsigned long)internal_free,
                        (unsigned long)internal_largest,
                        badge_runtime_display_alive() ? 1 : 0,
                        badge_runtime_scanner_uart_alive() ? 1 : 0,
                        badge_runtime_usb_control_alive() ? 1 : 0);
                    if (!badge_update_health_rollback(update_session)) {
                        ESP_LOGE(
                            TAG,
                            "Update-health rollback remains blocked; "
                            "keeping maintenance recovery alive");
                    }
                }
            }

            fw_store_campaign_completion_t completion =
                fw_store_campaign_completion_sample();
            fw_store_scanner_stage_status_t current_stage = {0};
            bool current_stage_valid =
                fw_store_scanner_stage_status_snapshot(&current_stage);
            if (!s_ota_pending_verify &&
                fw_store_campaign_terminal_exit_allowed(
                    maintenance_entry_stage_certain,
                    maintenance_entry_scanner_generation,
                    current_stage_valid,
                    &current_stage,
                    completion)) {
                ESP_LOGE(
                    TAG,
                    "Update-maintenance scanner campaign reached a "
                    "durable terminal failure; returning to normal boot");
                if (!badge_update_terminal_failure_restart(update_session)) {
                    ESP_LOGE(
                        TAG,
                        "Terminal-failure restart blocked without "
                        "expected-reboot ownership");
                }
            }

            if (badge_runtime_update_inactivity_due(now_ms)) {
                fw_store_campaign_snapshot_t campaign = {0};
                bool campaign_sampled =
                    fw_store_campaign_state_sample(&campaign);
                bool campaign_idle =
                    campaign_sampled &&
                    (campaign.state == FW_CAMPAIGN_IDLE ||
                     campaign.state == FW_CAMPAIGN_ALL_TERMINAL);
                if (campaign_idle) {
                    ESP_LOGW(TAG,
                             "Update-maintenance session inactive; "
                             "returning to normal boot");
                    if (!badge_update_inactivity_restart(update_session)) {
                        ESP_LOGE(
                            TAG,
                            "Inactivity restart blocked without "
                            "expected-reboot ownership");
                    }
                }
                ESP_LOGW(TAG,
                         "Update-maintenance inactivity exit deferred: "
                         "campaign=%d owner=%d sampled=%d",
                         (int)campaign.state, (int)campaign.owner,
                         campaign_sampled ? 1 : 0);
            }
        }
    }
#endif

    /* ── 3. Create default event loop ─────────────────────────────────── */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    log_detection_queue_heap("after_event_loop");

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
    /* Badge UART drivers were initialized before the USB configuration
     * window so cold-boot role assignment could not be missed. */
#else
    uart_rx_init(detection_queue);
#endif

    /* ── 13. Initialize outbound detection upload ────────────────────── */
#ifndef FOF_BADGE_VARIANT
    http_upload_init(detection_queue);
#else
    /* Badge detections feed the local threat/display state directly in
     * uart_rx.c and are never queued for backend upload. Keep the badge on
     * its actual crunch-mode transports: USB control and UART scanner relay. */
    ESP_LOGI(TAG, "Badge HTTP detection upload disabled; USB control and UART firmware relay remain active");
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
        uint8_t abort_seq[OTA_ABORT_SENTINEL_COUNT + 1];
        memset(abort_seq, OTA_ABORT_SENTINEL_BYTE, OTA_ABORT_SENTINEL_COUNT);
        abort_seq[OTA_ABORT_SENTINEL_COUNT] = '\n';
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
        uart_startup_gate_result_t uart_startup =
            start_uart_rx_with_operation_gate();
        if (uart_startup != UART_STARTUP_GATE_OK) {
            badge_runtime_force_safe_mode(true, "uart_rx_start");
            badge_usb_transport_set_recovery_only(true);
            oled_show_boot_status("USB RECOVERY",
                                  badge_mode_display_name(badge_mode),
                                  "uart_rx_start");
            if (uart_startup == UART_STARTUP_GATE_RELEASE_FAILED) {
                (void)badge_usb_transport_drain(pdMS_TO_TICKS(250));
                if (!badge_usb_recovery_restart(
                        BADGE_USB_RESET_APP,
                        "uart_start_token_release")) {
                    ESP_LOGE(
                        TAG,
                        "UART startup recovery restart blocked without owner");
                }
            }
            if (!badge_automatic_restart_when_firmware_idle(
                    "uart_rx_start")) {
                ESP_LOGE(TAG, "UART startup automatic restart cancelled");
            }
        }
    } else {
        ESP_LOGW(TAG, "Badge safe USB mode: scanner RX tasks held off");
    }
#else
    if (start_uart_rx_with_operation_gate() != UART_STARTUP_GATE_OK) {
        required_tasks_started = false;
    }
    if (!http_upload_start()) {
        required_tasks_started = false;
    }
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

#ifdef FOF_BADGE_VARIANT
    if (!badge_power_runtime_start()) {
        ESP_LOGE(TAG, "Failed to start badge power runtime");
        required_tasks_started = false;
    }
#endif

    if (!required_tasks_started) {
#ifdef FOF_BADGE_VARIANT
        if (!badge_automatic_restart_when_firmware_idle(
                "required worker task creation failed")) {
            badge_runtime_force_safe_mode(true, "reboot_arm_failed");
        }
#else
        rollback_and_reboot_or_restart("required worker task creation failed");
#endif
    }

    /* The permanent coordinator is created only after the shared scanner TX
     * lease and both scanner RX tasks exist. It remains notification-blocked
     * until durable restore runs after the generic READY commands below. */
#ifdef FOF_BADGE_VARIANT
    if (!badge_safe_usb) {
        if (!fw_store_start_auto_update_coordinator()) {
            if (!badge_automatic_restart_when_firmware_idle(
                    "scanner-update coordinator dependencies unavailable")) {
                badge_runtime_force_safe_mode(true, "reboot_arm_failed");
            }
        }
    } else {
        ESP_LOGW(TAG,
                 "Badge safe USB mode: coordinator creation deferred with "
                 "scanner RX tasks");
    }
#else
    if (!fw_store_start_auto_update_coordinator()) {
        rollback_and_reboot_or_restart(
            "scanner-update coordinator dependencies unavailable");
    }
#endif

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

    /* ── 15a. Non-badge network updater. The badge build never starts this
     *         task: badge images arrive over USB and scanners receive them
     *         over UART after their single delayed boot check. */
#ifndef FOF_BADGE_VARIANT
    if (badge_backend_enabled && wifi_sta_is_connected()) {
        fw_auto_check_init();
    } else {
        ESP_LOGI(TAG, "Firmware auto-check disabled until backend WiFi is up");
    }
#else
    ESP_LOGI(TAG,
             "Badge firmware network auto-check disabled; "
             "updates arrive over USB and relay over scanner UARTs");
#endif

    ESP_LOGI(TAG, "All tasks started. Uplink is operational.");

    /* ── 16b. Tell scanners to start transmitting ────────────────────── */
    /* Scanners boot silent (start/stop protocol) and wait for this signal. */
    {
#ifdef FOF_BADGE_VARIANT
        if (!badge_safe_usb && !badge_power_runtime_is_quiet()) {
            uart_rx_send_command("{\"type\":\"ready\"}");
            send_badge_scan_profiles();
            ESP_LOGW(TAG, "Sent ready signal to all scanners — detections enabled");
            /* The display task is already running; let it own the LCD from
             * here so boot status does not linger over the live threat view. */
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

    /* A scanner's generic READY command clears its volatile firmware-ready
     * latch. Restore and re-prompt the durable coordinator only after normal
     * startup commands have completed, or an interrupted update can be lost. */
#ifdef FOF_BADGE_VARIANT
    if (!badge_safe_usb && !fw_store_restore_auto_update_coordinator()) {
        if (!badge_automatic_restart_when_firmware_idle(
                "scanner-update coordinator restore failed")) {
            badge_runtime_force_safe_mode(true, "reboot_arm_failed");
        }
#else
    if (!fw_store_restore_auto_update_coordinator()) {
        rollback_and_reboot_or_restart(
            "scanner-update coordinator restore failed");
#endif
    }

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    /* Durable campaign restore is authoritative before controller admission.
     * A pending/unknown campaign leaves the controller uninitialized; the
     * display loop retries only after a later exact allowed epoch. */
    badge_con_radio_runtime_poll(
        (uint32_t)(esp_timer_get_time() / 1000));
#endif

    /* Normal startup and durable campaign restore are complete.
     * Recovery-only returns above and intentionally leaves this task blocked
     * on its static boot screen. */
    xTaskNotifyGive(s_display_task_handle);

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
            badge_usb_health_t usb_health = {0};
            badge_usb_transport_snapshot(&usb_health);
            fw_store_activity_t firmware_activity =
                fw_store_activity_sample();
            int64_t relay_progress_ms = fw_store_last_relay_progress_ms();
            int64_t usb_now_ms = esp_timer_get_time() / 1000;
            bool relay_active =
                firmware_activity == FW_STORE_ACTIVITY_ACTIVE;
            int64_t transaction_progress_ms = usb_health.last_upload_progress_ms;
            if (relay_active && relay_progress_ms > transaction_progress_ms) {
                transaction_progress_ms = relay_progress_ms;
            }
            int64_t oldest_unanswered_ms =
                usb_health.oldest_hard_unanswered_response_ms;
            if (usb_health.oldest_enqueued_response_ms >= 0 &&
                (oldest_unanswered_ms < 0 ||
                 usb_health.oldest_enqueued_response_ms <
                    oldest_unanswered_ms)) {
                oldest_unanswered_ms =
                    usb_health.oldest_enqueued_response_ms;
            }
            badge_usb_health_action_t usb_action = BADGE_USB_HEALTH_WAITING;
            if (firmware_activity != FW_STORE_ACTIVITY_UNKNOWN) {
                badge_usb_health_inputs_t usb_inputs = {
                    .safe_usb = badge_runtime_is_safe_mode(),
                    .one_boot_recovery_consumed =
                        badge_runtime_usb_recovery_once_consumed(),
                    .task_started = usb_health.task_started,
                    .host_connected = usb_health.host_connected,
                    .transaction_active =
                        usb_health.parser_target != BADGE_USB_BINARY_NONE ||
                        relay_active,
                    .now_ms = usb_now_ms,
                    .task_heartbeat_ms = usb_health.task_heartbeat_ms,
                    .last_rx_ms = usb_health.last_rx_ms,
                    .last_command_ms = usb_health.last_command_ms,
                    .last_response_ms = usb_health.last_response_ms,
                    .oldest_unanswered_command_ms = oldest_unanswered_ms,
                    .last_transaction_progress_ms = transaction_progress_ms,
                    .boot_grace_ms = 120000,
                    .stale_after_ms = 90000,
                };
                usb_action = badge_usb_health_decide(&usb_inputs);
            } else {
                ESP_LOGW(TAG,
                         "Badge USB health waiting: firmware activity unknown");
            }
            if (usb_action == BADGE_USB_HEALTH_RESTART_SAFE_USB) {
                badge_runtime_expected_reboot_lease_t reboot_lease = {0};
                badge_usb_firmware_restart_prepare_result_t prepare_result =
                    badge_usb_recovery_prepare_firmware_restart(
                        "usb_safe_once",
                        BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
                        &reboot_lease);
                if (prepare_result ==
                    BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
                    ESP_LOGE(TAG,
                             "Badge USB health policy requested one safe restart");
                    oled_show_boot_status("USB RECOVERY", "restarting",
                                          "usb_safe_once");
                    badge_usb_recovery_restart_with_owned_lease(
                        BADGE_USB_RESET_APP,
                        "usb_safe_once",
                        &reboot_lease);
                } else {
                    ESP_LOGW(TAG,
                             "Badge USB health restart deferred: ownership "
                             "preparation=%d",
                             (int)prepare_result);
                }
            }
            badge_runtime_note_main_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
            if (badge_runtime_health_can_mark_stable(free_heap, uptime_s)) {
                badge_runtime_mark_stable();
            }
            if (badge_runtime_health_can_mark_ota_valid(free_heap, uptime_s)) {
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
                !badge_power_runtime_is_quiet() &&
#endif
                !fw_store_is_relay_active() && !http_upload_is_paused()) {
                uart_rx_send_command("{\"type\":\"ready\"}");
#ifdef FOF_BADGE_VARIANT
                send_badge_scan_profiles();
#endif
            }

            if (!badge_backend_enabled) {
                if (free_heap < 4000 && uptime_s > 20
#ifndef FOF_BADGE_VARIANT
                    && !fw_store_is_relay_active()
#endif
                ) {
                    snprintf(reason, sizeof(reason), "heap=%lu critically low",
                             (unsigned long)free_heap);
#ifdef FOF_BADGE_VARIANT
                    if (!badge_automatic_restart_when_firmware_idle(reason)) {
                        badge_runtime_force_safe_mode(
                            true, "reboot_arm_failed");
                    }
#else
                    rollback_and_reboot_or_restart(reason);
#endif
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
            if (free_heap < 4000 && uptime_s > 20
#ifndef FOF_BADGE_VARIANT
                && !fw_store_is_relay_active()
#endif
            ) {
                snprintf(reason, sizeof(reason), "heap=%lu critically low",
                         (unsigned long)free_heap);
#ifdef FOF_BADGE_VARIANT
                if (!badge_automatic_restart_when_firmware_idle(reason)) {
                    badge_runtime_force_safe_mode(
                        true, "reboot_arm_failed");
                }
#else
                rollback_and_reboot_or_restart(reason);
#endif
            }
        }
    }
}
