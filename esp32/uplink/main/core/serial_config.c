/**
 * Friend or Foe -- Serial Configuration Handler
 *
 * Reads config commands from the USB console (stdin) during a startup
 * window, allowing the web flasher to push NVS settings right after flash.
 *
 * Uses standard stdin/stdout via VFS — works with whatever console backend
 * is configured (USB-JTAG on C3, UART0 on other chips).
 */

#include "serial_config.h"
#include "config.h"
#include "nvs_config.h"
#include "badge_mode.h"
#include "uart_rx.h"
#include "wifi_sta.h"
#include "wifi_ap.h"
#include "fw_store.h"
#include "http_upload.h"
#include "oled_display.h"
#include "version.h"
#include "detection_policy.h"
#include "uart_protocol.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_runtime.h"
#include "badge_power_runtime.h"
#include "badge_display_policy_runtime.h"
#include "badge_theme_runtime.h"
#include "badge_ble_control.h"
#include "badge_ble_investigation.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

static const char *TAG = "serial_cfg";

#define LINE_BUF_SIZE   2048
#define CMD_PREFIX      "FOF_SET:"
#define CMD_CTL         "FOF_CTL:"
#define CMD_STATUS      "FOF_STATUS"
#define CMD_SAVE        "FOF_SAVE"
#define CMD_PING        "FOF_PING"
#define CMD_REBOOT      "FOF_REBOOT"
#define CMD_BOOTLOADER  "FOF_BOOTLOADER"
#define CMD_DOWNLOAD    "FOF_DOWNLOAD"
#define CMD_FLASH       "FOF_FLASH"
#define RESP_READY      "FOF_READY\n"
#define RESP_OK         "FOF_OK:"
#define RESP_SAVED      "FOF_SAVED\n"
#define RESP_ERROR      "FOF_ERROR:"
#define RESP_TIMEOUT    "FOF_TIMEOUT\n"
#define RESP_BOOTLOADER "FOF_BOOTLOADER:OK\n"
#define RESP_REBOOT     "FOF_REBOOT:OK\n"

#define CONTROL_STACK_BYTES 12288  /* Full badge status JSON includes scanner diagnostics. */
#define SERIAL_FW_BUF_SIZE 512
#define SERIAL_FW_IDLE_TIMEOUT_MS 30000

static bool s_control_task_started = false;
static bool s_serial_fw_rx_active = false;
static int64_t s_serial_fw_last_rx_ms = 0;
static SemaphoreHandle_t s_usb_output_lock = NULL;
static portMUX_TYPE s_usb_output_init_lock = portMUX_INITIALIZER_UNLOCKED;
/* The status response already has a deep formatting frame. Keep its complete
 * scanner copies out of the 12 KB USB control stack. Calls are serialized by
 * the USB command path (boot config completes before the runtime task starts). */
static scanner_info_t s_usb_status_scanner_snapshots[2] = {0};

static void handle_control_line(const char *line);
static void print_json_escaped_string(const char *value);
static void print_scanner_status_json(const char *name, uint8_t scanner_id,
                                      bool connected, bool peer_ready,
                                      const scanner_info_t *info,
                                      const scanner_uart_diag_t *uart_diag,
                                      bool first);

static bool serial_output_lock(void)
{
    if (!s_usb_output_lock) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (!created) return false;
        portENTER_CRITICAL(&s_usb_output_init_lock);
        if (!s_usb_output_lock) {
            s_usb_output_lock = created;
            created = NULL;
        }
        portEXIT_CRITICAL(&s_usb_output_init_lock);
        if (created) vSemaphoreDelete(created);
    }
    return xSemaphoreTake(s_usb_output_lock, portMAX_DELAY) == pdTRUE;
}

static void serial_output_unlock(void)
{
    if (s_usb_output_lock) xSemaphoreGive(s_usb_output_lock);
}

bool serial_config_emit_investigation_frame(const char *frame)
{
    if (!frame || strncmp(frame, "FOF_INV:", 8) != 0) return false;
    size_t len = strlen(frame);
    if (len == 0 || len >= (UART_JSON_MAX_SIZE + 10) ||
        frame[len - 1] != '\n' || strchr(frame, '\r') != NULL) {
        return false;
    }
    const char *newline = strchr(frame, '\n');
    if (!newline || newline != &frame[len - 1] || !serial_output_lock()) {
        return false;
    }
    size_t written = fwrite(frame, 1, len, stdout);
    fflush(stdout);
    serial_output_unlock();
    return written == len;
}

/* Allowed NVS keys — only accept known config keys */
static const char *ALLOWED_KEYS[] = {
    "wifi_ssid", "wifi_pass", "backend_url", "device_id",
    "ap_ssid", "ap_pass", "badge_mode", "badge_display_debug",
    "badge_display_policy_v1",
    NULL
};

static bool is_allowed_key(const char *key)
{
    for (int i = 0; ALLOWED_KEYS[i] != NULL; i++) {
        if (strcmp(key, ALLOWED_KEYS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void send_response(const char *msg)
{
    printf("%s", msg);
    fflush(stdout);
}

static void reboot_to_download_mode(void)
{
    ESP_LOGW(TAG, "USB serial requested ROM download mode");
    send_response(RESP_BOOTLOADER);
#ifdef FOF_BADGE_VARIANT
    badge_runtime_arm_expected_reboot("usb_bootloader");
#endif
    vTaskDelay(pdMS_TO_TICKS(120));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void reboot_app(void)
{
    ESP_LOGW(TAG, "USB serial requested app restart");
    send_response(RESP_REBOOT);
#ifdef FOF_BADGE_VARIANT
    badge_runtime_arm_expected_reboot("usb_reboot");
#endif
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_restart();
}

static void print_scanner_status_json(const char *name, uint8_t scanner_id,
                                      bool connected, bool peer_ready,
                                      const scanner_info_t *info,
                                      const scanner_uart_diag_t *uart_diag,
                                      bool first)
{
    bool calibration = info && strcmp(info->scan_mode, "calibration") == 0;
    const char *expected = fof_policy_scan_profile_for_topology(
        scanner_id, calibration, peer_ready,
        FOF_POLICY_FIXED_SLOT_TOPOLOGY);
    const char *actual = (info && info->scan_profile[0]) ? info->scan_profile : "";
    const char *expected_role = fof_policy_slot_role_for_slot(scanner_id);
    const char *actual_role = (info && info->slot_role[0])
        ? info->slot_role : "";
    bool role_acked = connected && actual[0] &&
        strcmp(actual, expected) == 0 && actual_role[0] &&
        strcmp(actual_role, expected_role) == 0;
    bool cmd_fresh = connected && info && info->cmd_rx_count > 0 &&
                     info->cmd_last_age_s >= 0 && info->cmd_last_age_s <= 45;
    const char *health = fof_policy_badge_scanner_health(
        expected,
        connected,
        role_acked,
        cmd_fresh,
        info && info->ble_initialized && info->ble_scanning &&
            info->ble_host_active && info->ble_host_synced,
        info && info->wifi_initialized && info->wifi_init_rc == 0,
        info && info->wifi_active,
        info && info->ble_quiesced,
        info && info->wifi_quiesced);

    printf("%s{\"slot\":%u,\"uart\":", first ? "" : ",", (unsigned)scanner_id);
    print_json_escaped_string(name ? name : "?");
    printf(",\"connected\":%s,\"slot_role\":",
           connected ? "true" : "false");
    print_json_escaped_string(actual_role);
    printf(",\"expected_slot_role\":");
    print_json_escaped_string(expected_role);
    printf(",\"expected_scan_profile\":");
    print_json_escaped_string(expected);
    printf(",\"scan_profile\":");
    print_json_escaped_string(actual);
    printf(",\"role_acked\":%s,\"health\":", role_acked ? "true" : "false");
    print_json_escaped_string(health);
    printf(",\"uart_raw_seen\":%s,\"uart_raw_age_s\":%lld,"
           "\"uart_raw_bytes\":%lu,\"uart_line_overflow\":%lu,"
           "\"uart_json_err\":%lu",
           uart_diag && uart_diag->raw_seen ? "true" : "false",
           (long long)(uart_diag ? uart_diag->raw_age_s : -1),
           (unsigned long)(uart_diag ? uart_diag->raw_bytes : 0),
           (unsigned long)(uart_diag ? uart_diag->line_overflow_count : 0),
           (unsigned long)(uart_diag ? uart_diag->json_parse_error_count : 0));
    if (info) {
        printf(",\"ver\":");
        print_json_escaped_string(info->version);
        printf(",\"board\":");
        print_json_escaped_string(info->board);
        printf(",\"firmware_name\":");
        print_json_escaped_string(info->firmware_name);
        printf(",\"app_project\":");
        print_json_escaped_string(info->app_project);
        printf(",\"hardware_type\":");
        print_json_escaped_string(info->hardware_type);
        printf(",\"hardware_id\":");
        print_json_escaped_string(info->hardware_id);
        printf(",\"boot_id\":%lu,\"identity_generation\":%lu,"
               "\"cmd_rx\":%lu,\"cmd_last_age_s\":%lld,"
               "\"cmd_parse_err\":%lu,\"cmd_overflow\":%lu,"
               "\"ble_initialized\":%s,\"ble_scanning\":%s,"
               "\"ble_host_active\":%s,\"ble_host_synced\":%s,"
               "\"ble_quiesced\":%s,"
               "\"wifi_initialized\":%s,\"wifi_active\":%s,"
               "\"wifi_quiesced\":%s,"
               "\"wifi_init_rc\":%d,\"wifi_paused\":%s,"
               "\"wifi_total_frames\":%lu,\"wifi_beacon_frames\":%lu,"
               "\"wifi_full_scan_count\":%lu,\"wifi_full_scan_ok\":%lu,"
               "\"wifi_full_scan_err\":%lu,\"wifi_full_scan_last_rc\":%d,"
               "\"wifi_last_ap_count\":%lu,\"wifi_last_scan_age_s\":%lld,"
               "\"wifi_drone_ssid_emit\":%lu,"
               "\"wifi_notable_ssid_emit\":%lu,"
               "\"wifi_last_drone_ssid_age_s\":%lld,"
               "\"wifi_last_notable_ssid_age_s\":%lld,"
               "\"wifi_last_drone_ssid\":",
               (unsigned long)info->boot_id,
               (unsigned long)info->identity_generation,
               (unsigned long)info->cmd_rx_count,
               (long long)info->cmd_last_age_s,
               (unsigned long)info->cmd_parse_error_count,
               (unsigned long)info->cmd_overflow_count,
               info->ble_initialized ? "true" : "false",
               info->ble_scanning ? "true" : "false",
               info->ble_host_active ? "true" : "false",
               info->ble_host_synced ? "true" : "false",
               info->ble_quiesced ? "true" : "false",
               info->wifi_initialized ? "true" : "false",
               info->wifi_active ? "true" : "false",
               info->wifi_quiesced ? "true" : "false",
               info->wifi_init_rc,
               info->wifi_paused ? "true" : "false",
               (unsigned long)info->wifi_total_frames,
               (unsigned long)info->wifi_beacon_frames,
               (unsigned long)info->wifi_full_scan_count,
               (unsigned long)info->wifi_full_scan_ok,
               (unsigned long)info->wifi_full_scan_err,
               info->wifi_full_scan_last_rc,
               (unsigned long)info->wifi_last_ap_count,
               (long long)info->wifi_last_scan_age_s,
               (unsigned long)info->wifi_drone_ssid_emit,
               (unsigned long)info->wifi_notable_ssid_emit,
               (long long)info->wifi_last_drone_ssid_age_s,
               (long long)info->wifi_last_notable_ssid_age_s);
        print_json_escaped_string(info->wifi_last_drone_ssid);
        printf(",\"wifi_last_notable_ssid\":");
        print_json_escaped_string(info->wifi_last_notable_ssid);
        printf(",\"wifi_oui_emit\":%lu,"
               "\"wifi_soft_ssid_emit\":%lu,\"wifi_hot_ch\":%lu,"
               "\"ble_adv_seen\":%lu,\"ble_any_seen\":%lu,"
               "\"ble_any_with_payload_seen\":%lu,"
               "\"ble_any_empty_seen\":%lu,"
               "\"uart_tx_dropped\":%lu,\"uart_tx_high_water\":%lu,"
               "\"tx_queue_depth\":%lu,\"tx_queue_capacity\":%lu,"
               "\"tx_queue_pressure_pct\":%lu,"
               "\"ble_any_last_rssi\":%d,\"ble_any_best_rssi\":%d,"
               "\"ble_any_last_len\":%u,\"ble_any_last_props\":%u,"
               "\"ble_any_last_addr_type\":%u,"
               "\"ble_fp_emit\":%lu,"
               "\"ble_meta_seen\":%lu,"
               "\"ble_meta_last_seen_age_s\":%lld,"
               "\"ble_meta_last_emit_age_s\":%lld,"
               "\"ble_meta_last_hash\":%lu,"
               "\"ble_meta_last_rssi\":%d,"
               "\"ble_meta_weak_age_s\":%lld,"
               "\"ble_meta_reacquire_count\":%lu,"
               "\"ble_tracker_seen\":%lu,"
               "\"ble_privacy_candidate_seen\":%lu,"
               "\"ble_near_unknown_seen\":%lu,"
               "\"ble_drop_profile\":%lu,\"ble_drop_rate\":%lu,"
               "\"ble_host_restart_count\":%lu,"
               "\"ble_scan_start_count\":%lu,\"ble_scan_start_ok\":%lu,"
               "\"ble_scan_last_rc\":%d,\"ble_sync_last_rc\":%d,"
               "\"ble_focus_active\":%s,\"ble_focus_age_s\":%lld,"
               "\"ble_focus_target_adv_count\":%lu,"
               "\"rid_service_seen\":%lu,\"rid_emit\":%lu,"
               "\"rid_queue_drop\":%lu,\"rid_queue_evict\":%lu,"
               "\"privacy_seen\":%lu",
               (unsigned long)info->wifi_oui_emit,
               (unsigned long)info->wifi_soft_ssid_emit,
               (unsigned long)info->wifi_hot_ch,
               (unsigned long)info->ble_adv_seen,
               (unsigned long)info->ble_any_seen,
               (unsigned long)info->ble_any_with_payload_seen,
               (unsigned long)info->ble_any_empty_seen,
               (unsigned long)info->uart_tx_dropped,
               (unsigned long)info->uart_tx_high_water,
               (unsigned long)info->tx_queue_depth,
               (unsigned long)info->tx_queue_capacity,
               (unsigned long)info->tx_queue_pressure_pct,
               (int)info->ble_any_last_rssi,
               (int)info->ble_any_best_rssi,
               (unsigned)info->ble_any_last_len,
               (unsigned)info->ble_any_last_props,
               (unsigned)info->ble_any_last_addr_type,
               (unsigned long)info->ble_fp_emit,
               (unsigned long)info->ble_meta_seen,
               (long long)info->ble_meta_last_seen_age_s,
               (long long)info->ble_meta_last_emit_age_s,
               (unsigned long)info->ble_meta_last_hash,
               (int)info->ble_meta_last_rssi,
               (long long)info->ble_meta_weak_age_s,
               (unsigned long)info->ble_meta_reacquire_count,
               (unsigned long)info->ble_tracker_seen,
               (unsigned long)info->ble_privacy_candidate_seen,
               (unsigned long)info->ble_near_unknown_seen,
               (unsigned long)info->ble_drop_profile,
               (unsigned long)info->ble_drop_rate,
               (unsigned long)info->ble_host_restart_count,
               (unsigned long)info->ble_scan_start_count,
               (unsigned long)info->ble_scan_start_ok,
               info->ble_scan_last_rc,
               info->ble_sync_last_rc,
               info->ble_focus_active ? "true" : "false",
               (long long)info->ble_focus_age_s,
               (unsigned long)info->ble_focus_target_adv_count,
               (unsigned long)info->rid_service_seen,
               (unsigned long)info->rid_emit,
               (unsigned long)info->rid_queue_drop,
               (unsigned long)info->rid_queue_evict,
               (unsigned long)info->privacy_seen);
        printf(",\"display_policy_hash\":%lu,"
               "\"display_policy_ack_hash\":%lu,"
               "\"filtered_counts\":{",
               (unsigned long)info->display_policy_hash,
               (unsigned long)info->display_policy_ack_hash);
        for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
            badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
            printf("%s", i == 0 ? "" : ",");
            print_json_escaped_string(badge_display_policy_class_key(cls));
            printf(":%lu", (unsigned long)info->display_policy_filtered[i]);
        }
        printf("}");
        printf(",\"ble_meta_last_reason\":");
        print_json_escaped_string(info->ble_meta_last_reason);
        printf(",\"ble_meta_identity\":");
        print_json_escaped_string(info->ble_meta_identity);
        printf(",\"ble_dbg_near_label\":");
        print_json_escaped_string(info->ble_dbg_near_label);
        printf(",\"ble_dbg_near_name\":");
        print_json_escaped_string(info->ble_dbg_near_name);
        printf(",\"ble_dbg_near_reason\":");
        print_json_escaped_string(info->ble_dbg_near_reason);
        printf(",\"ble_dbg_priv_label\":");
        print_json_escaped_string(info->ble_dbg_priv_label);
        printf(",\"ble_dbg_priv_name\":");
        print_json_escaped_string(info->ble_dbg_priv_name);
        printf(",\"ble_dbg_priv_reason\":");
        print_json_escaped_string(info->ble_dbg_priv_reason);
        printf(",\"ble_dbg_near_seen\":%lu,\"ble_dbg_near_rssi\":%d,"
               "\"ble_dbg_near_cid\":%u,\"ble_dbg_near_svc0\":%u,"
               "\"ble_dbg_near_svc_count\":%u,"
               "\"ble_dbg_near_payload_len\":%u,"
               "\"ble_dbg_priv_seen\":%lu,\"ble_dbg_priv_rssi\":%d,"
               "\"ble_dbg_priv_cid\":%u,\"ble_dbg_priv_svc0\":%u,"
               "\"ble_dbg_priv_svc_count\":%u,"
               "\"ble_dbg_priv_payload_len\":%u,\"fw_state\":",
               (unsigned long)info->ble_dbg_near_seen,
               (int)info->ble_dbg_near_rssi,
               (unsigned)info->ble_dbg_near_cid,
               (unsigned)info->ble_dbg_near_svc0,
               (unsigned)info->ble_dbg_near_svc_count,
               (unsigned)info->ble_dbg_near_payload_len,
               (unsigned long)info->ble_dbg_priv_seen,
               (int)info->ble_dbg_priv_rssi,
               (unsigned)info->ble_dbg_priv_cid,
               (unsigned)info->ble_dbg_priv_svc0,
               (unsigned)info->ble_dbg_priv_svc_count,
               (unsigned)info->ble_dbg_priv_payload_len);
        print_json_escaped_string(info->fw_update_state[0] ? info->fw_update_state : "idle");
        printf(",\"target_ver\":");
        print_json_escaped_string(info->fw_target_version);
        printf(",\"last_fw_error\":");
        print_json_escaped_string(info->last_fw_error);
        printf(",\"ota_state\":");
        print_json_escaped_string(info->ota_state[0] ? info->ota_state : "idle");
        printf(",\"ota_session_id\":");
        print_json_escaped_string(info->ota_session_id);
        printf(",\"ota_received\":%lu,\"ota_total\":%lu,"
               "\"recovery_mode\":",
               (unsigned long)info->ota_received,
               (unsigned long)info->ota_total);
        print_json_escaped_string(info->recovery_mode[0] ? info->recovery_mode : "normal");
        printf(",\"safe_reason\":");
        print_json_escaped_string(info->safe_reason);
        printf(",\"rollback_pending\":%s,\"crash_count\":%lu,"
               "\"radio_restart_count\":%lu",
               info->rollback_pending ? "true" : "false",
               (unsigned long)info->crash_count,
               (unsigned long)info->radio_restart_count);
    }
    printf("}");
}

#ifdef FOF_BADGE_VARIANT
static void print_display_policy_status_fields(
    const char *policy_json,
    uint32_t policy_hash,
    const uint32_t filtered_counts[BADGE_DISPLAY_POLICY_CLASS_COUNT])
{
    printf(",\"display_policy_hash\":%lu,\"display_policy\":%s,"
           "\"filtered_counts\":{",
           (unsigned long)policy_hash,
           policy_json && policy_json[0]
               ? policy_json
               : "{\"version\":1,\"classes\":{}}");
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
        printf("%s", i == 0 ? "" : ",");
        print_json_escaped_string(badge_display_policy_class_key(cls));
        printf(":%lu", (unsigned long)(filtered_counts
            ? filtered_counts[i]
            : 0));
    }
    printf("}");
}

static void print_badge_display_state_field(
    const oled_badge_display_state_t *captured_state,
    bool active)
{
    const oled_badge_display_state_t empty_state = {0};
    const oled_badge_display_state_t *state = captured_state
        ? captured_state
        : &empty_state;
    printf(",\"display_state\":{\"active\":%s,\"detail_mode\":%s,"
           "\"detail_page\":%d,\"investigation\":{\"active\":%s,"
           "\"request_id\":",
           active ? "true" : "false",
           state->detail_mode ? "true" : "false",
           state->detail_page,
           state->investigation_active ? "true" : "false");
    print_json_escaped_string(state->investigation_request_id);
    printf(",\"state\":");
    print_json_escaped_string(state->investigation_state);
    printf(",\"page\":%d},\"focus_index\":%d,\"focus_total\":%d,"
           "\"item_index\":%d,\"item_total\":%d,\"lane\":",
           state->investigation_page,
           state->focus_index,
           state->focus_total,
           state->item_index,
           state->item_total);
    print_json_escaped_string(state->lane);
    printf(",\"title\":");
    print_json_escaped_string(state->title);
    printf(",\"detail\":");
    print_json_escaped_string(state->detail);
    printf(",\"evidence\":");
    print_json_escaped_string(state->evidence);
    printf(",\"entity_key\":");
    print_json_escaped_string(state->entity_key);
    printf(",\"display_id\":");
    print_json_escaped_string(state->display_id);
    printf(",\"class\":");
    print_json_escaped_string(state->threat_class);
    printf(",\"category\":");
    print_json_escaped_string(state->category);
    printf(",\"code\":");
    print_json_escaped_string(state->code);
    printf(",\"source\":");
    print_json_escaped_string(state->source);
    if (state->ssid[0] != '\0') {
        printf(",\"ssid\":");
        print_json_escaped_string(state->ssid);
    }
    if (state->bssid[0] != '\0') {
        printf(",\"bssid\":");
        print_json_escaped_string(state->bssid);
    }
    printf(",\"auth_m\":%d,\"freq_mhz\":%d,"
           "\"score\":%d,\"confidence_pct\":%d,"
           "\"evidence_quality\":%d,\"display_rank\":%d,"
           "\"age_s\":%d,\"last_seen_s\":%d,\"rssi\":%d,\"best_rssi\":%d,"
           "\"events\":%lu,\"seen_count\":%lu,\"group_count\":%lu,"
           "\"proximity_level\":%d,\"stale\":%s",
           state->wifi_auth_mode,
           state->freq_mhz,
           state->score,
           state->confidence_pct,
           state->evidence_quality,
           state->display_rank,
           state->age_s,
           state->last_seen_s,
           state->rssi,
           state->best_rssi,
           (unsigned long)state->events,
           (unsigned long)state->seen_count,
           (unsigned long)state->group_count,
           state->proximity_level,
           state->stale ? "true" : "false");
    if (state->has_location) {
        printf(",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.1f",
               state->latitude, state->longitude, state->altitude_m);
    }
    if (state->has_operator_location) {
        printf(",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
               state->operator_lat, state->operator_lon);
    }
    if (state->operator_id[0] != '\0') {
        printf(",\"operator_id\":");
        print_json_escaped_string(state->operator_id);
    }
    printf("}");
}

static void print_badge_button_state_field(
    const oled_badge_button_state_t *captured_buttons,
    int64_t now_ms)
{
    const oled_badge_button_state_t empty_buttons = {0};
    const oled_badge_button_state_t *buttons = captured_buttons
        ? captured_buttons
        : &empty_buttons;
    int64_t b1_age = buttons->b1_last_event_ms > 0 && now_ms >= buttons->b1_last_event_ms
        ? (now_ms - buttons->b1_last_event_ms) / 1000
        : -1;
    int64_t b2_age = buttons->b2_last_event_ms > 0 && now_ms >= buttons->b2_last_event_ms
        ? (now_ms - buttons->b2_last_event_ms) / 1000
        : -1;
    printf(",\"buttons\":{\"b1_pin\":8,\"b1_active_high\":%s,"
           "\"b1_raw_level\":%d,\"b1_raw_pressed\":%s,"
           "\"b1_stable_pressed\":%s,\"b1_boot_ignored\":%s,"
           "\"b1_raw_edges\":%lu,\"b1_short_presses\":%lu,"
           "\"b1_long_presses\":%lu,\"b1_releases\":%lu,"
           "\"b1_last_event_age_s\":%lld,"
           "\"b2_pin\":43,\"b2_active_high\":%s,"
           "\"b2_raw_level\":%d,\"b2_raw_pressed\":%s,"
           "\"b2_stable_pressed\":%s,\"b2_boot_ignored\":%s,"
           "\"b2_raw_edges\":%lu,\"b2_short_presses\":%lu,"
           "\"b2_double_taps\":%lu,\"b2_long_presses\":%lu,"
           "\"b2_releases\":%lu,\"b2_last_event_age_s\":%lld,"
           "\"b2_pending_single\":%s,\"b2_last_gesture\":",
           buttons->b1_active_high ? "true" : "false",
           buttons->b1_raw_level,
           buttons->b1_raw_pressed ? "true" : "false",
           buttons->b1_stable_pressed ? "true" : "false",
           buttons->b1_boot_ignored ? "true" : "false",
           (unsigned long)buttons->b1_raw_edges,
           (unsigned long)buttons->b1_short_presses,
           (unsigned long)buttons->b1_long_presses,
           (unsigned long)buttons->b1_releases,
           (long long)b1_age,
           buttons->b2_active_high ? "true" : "false",
           buttons->b2_raw_level,
           buttons->b2_raw_pressed ? "true" : "false",
           buttons->b2_stable_pressed ? "true" : "false",
           buttons->b2_boot_ignored ? "true" : "false",
           (unsigned long)buttons->b2_raw_edges,
           (unsigned long)buttons->b2_short_presses,
           (unsigned long)buttons->b2_double_taps,
           (unsigned long)buttons->b2_long_presses,
           (unsigned long)buttons->b2_releases,
           (long long)b2_age,
           buttons->b2_pending_single ? "true" : "false");
    print_json_escaped_string(buttons->b2_last_gesture);
    printf("}");
}

static void forward_display_policy_to_scanners(bool *ble_sent,
                                               bool *wifi_sent)
{
    static char cmd[BADGE_DISPLAY_POLICY_JSON_MAX + 128];
    cmd[0] = '\0';
    badge_display_policy_runtime_command_json(cmd, sizeof(cmd));
    bool ble_ok = false;
    bool wifi_ok = false;
    if (cmd[0]) {
        ble_ok = uart_rx_send_command_to_scanner_checked(0, cmd);
#if CONFIG_DUAL_SCANNER
        wifi_ok = uart_rx_send_command_to_scanner_checked(1, cmd);
#else
        wifi_ok = true;
#endif
    }
    if (ble_sent) *ble_sent = ble_ok;
    if (wifi_sent) *wifi_sent = wifi_ok;
}
#endif

static void send_badge_status_response(void)
{
#ifdef FOF_BADGE_VARIANT
    badge_runtime_note_usb_stack_free(
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
#endif
    static badge_threat_snapshot_t snapshot;
    uart_rx_get_badge_threat_snapshot(&snapshot);

    badge_mode_t mode = badge_mode_get();
    const int64_t status_now_ms = esp_timer_get_time() / 1000;
    const int64_t uptime_s = status_now_ms / 1000;
    const bool wifi_sta_connected = wifi_sta_is_connected();
    const bool wifi_sta_standalone = wifi_sta_is_standalone();
    char ap_ssid[40] = {0};
    snprintf(ap_ssid, sizeof(ap_ssid), "%s", wifi_ap_get_ssid());

    const int64_t last_upload_ms = http_upload_get_last_success_ms();
    const int64_t upload_age_s = last_upload_ms > 0
        ? (status_now_ms >= last_upload_ms
            ? (status_now_ms - last_upload_ms) / 1000
            : 0)
        : -1;
    const bool http_task_alive = http_upload_task_alive();
    const int uploads_ok = http_upload_get_success_count();
    const int uploads_fail = http_upload_get_fail_count();

    const bool ble_connected = uart_rx_is_ble_scanner_connected();
    const bool wifi_connected = uart_rx_is_wifi_scanner_connected();
    const scanner_info_t *ble_info =
        uart_rx_get_scanner_info_snapshot(
            0, &s_usb_status_scanner_snapshots[0])
            ? &s_usb_status_scanner_snapshots[0] : NULL;
    const scanner_info_t *wifi_info =
        uart_rx_get_scanner_info_snapshot(
            1, &s_usb_status_scanner_snapshots[1])
            ? &s_usb_status_scanner_snapshots[1] : NULL;
    scanner_uart_diag_t ble_uart_diag = {0};
    scanner_uart_diag_t wifi_uart_diag = {0};
    uart_rx_get_scanner_uart_diag(0, &ble_uart_diag);
#if CONFIG_DUAL_SCANNER
    uart_rx_get_scanner_uart_diag(1, &wifi_uart_diag);
#endif
    static fw_store_info_t firmware_store;
    memset(&firmware_store, 0, sizeof(firmware_store));
    const bool firmware_stored = fw_store_get_info(&firmware_store) &&
        firmware_store.stored;
    static fw_auto_update_status_t auto_update;
    memset(&auto_update, 0, sizeof(auto_update));
    fw_store_get_auto_update_status(&auto_update);

#ifdef FOF_BADGE_VARIANT
    const badge_runtime_network_mode_t network_mode =
        badge_runtime_get_network_mode();
    const int network_ttl_s = badge_runtime_get_network_ttl_s();
    const bool runtime_safe_mode = badge_runtime_is_safe_mode();
    char runtime_safe_reason[48] = {0};
    snprintf(runtime_safe_reason, sizeof(runtime_safe_reason), "%s",
             badge_runtime_safe_reason());
    const uint32_t runtime_crash_count = badge_runtime_crash_count();
    const bool runtime_pending_verify = badge_runtime_pending_verify();
    const bool runtime_display_alive = badge_runtime_display_alive();
    const bool runtime_usb_control_alive = badge_runtime_usb_control_alive();
    const bool runtime_scanner_uart_alive = badge_runtime_scanner_uart_alive();
    const uint32_t runtime_reset_reason = badge_runtime_last_reset_reason();
    const char *runtime_reset_reason_name =
        badge_runtime_last_reset_reason_name();
    const bool runtime_reset_expected = badge_runtime_last_reset_expected();
    const int64_t runtime_usb_control_age_s =
        badge_runtime_usb_control_age_s();
    const char *runtime_recovery_mode = badge_runtime_recovery_mode();
    const uint32_t stack_main_free = badge_runtime_main_stack_free();
    const uint32_t stack_display_free = badge_runtime_display_stack_free();
    const uint32_t stack_usb_free = badge_runtime_usb_stack_free();
    const uint32_t stack_uart_ble_free = badge_runtime_uart_ble_stack_free();
    const uint32_t stack_uart_wifi_free = badge_runtime_uart_wifi_stack_free();
    const uint32_t heap_internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t heap_internal_min_free =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t heap_internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    static char policy_json[BADGE_DISPLAY_POLICY_JSON_MAX];
    policy_json[0] = '\0';
    badge_display_policy_runtime_json(policy_json, sizeof(policy_json));
    const uint32_t policy_hash = badge_display_policy_runtime_hash();
    uint32_t filtered_counts[BADGE_DISPLAY_POLICY_CLASS_COUNT] = {0};
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        filtered_counts[i] = badge_display_policy_runtime_filtered_count(
            (badge_display_policy_class_t)i);
    }
    static char theme_json[BADGE_THEME_JSON_MAX];
    theme_json[0] = '\0';
    badge_theme_runtime_json(theme_json, sizeof(theme_json));
    const uint32_t theme_hash = badge_theme_runtime_hash();
    oled_badge_display_state_t display_state = {0};
    const bool display_state_active =
        oled_badge_get_display_state(&display_state);
    static char ble_status[192];
    ble_status[0] = '\0';
    badge_ble_control_status_json(ble_status, sizeof(ble_status));
    static char investigation_status[BADGE_BLE_INVESTIGATION_STATUS_JSON_MAX];
    investigation_status[0] = '\0';
    badge_ble_investigation_status_json(investigation_status,
                                        sizeof(investigation_status));
    oled_badge_button_state_t button_state = {0};
    (void)oled_badge_get_button_state(&button_state);
    badge_power_state_t power_state = {0};
    badge_power_runtime_snapshot(&power_state);
    const bool scanner_power_converged =
        badge_power_state_converged(&power_state);
    const bool panel_power_converged =
        oled_is_powered() == !power_state.quiet;
    const bool power_converged =
        scanner_power_converged && panel_power_converged;
#endif

    /* IDF task logs share stdout. Hold its recursive FILE lock for the entire
     * machine frame, after every mutex-taking state capture above. */
    flockfile(stdout);
    /* Stable machine fields: "firmware_name", "app_project", "hardware_type". */
    printf("FOF_STATUS:{\"version\":");
    print_json_escaped_string(FOF_VERSION);
    printf(",\"firmware_name\":");
    print_json_escaped_string(FOF_FIRMWARE_TARGET);
    printf(",\"app_project\":");
    print_json_escaped_string(FOF_APP_PROJECT);
    printf(",\"hardware_type\":");
    print_json_escaped_string(FOF_HARDWARE_TYPE);
    printf(",\"uptime_s\":%lld", (long long)uptime_s);
    printf(",\"mode\":");
    print_json_escaped_string(badge_mode_to_string(mode));
    printf(",\"mode_label\":");
    print_json_escaped_string(badge_mode_display_name(mode));
    bool ap_enabled =
#ifdef FOF_BADGE_VARIANT
        network_mode != BADGE_RUNTIME_NETWORK_OFF;
#else
        badge_mode_ap_enabled(mode);
#endif
    printf(",\"wifi_sta\":%s,\"ap_enabled\":%s,\"ap_ssid\":",
           wifi_sta_connected ? "true" : "false",
           ap_enabled ? "true" : "false");
    print_json_escaped_string(ap_ssid);
    printf(",\"ap_url\":\"http://192.168.4.1\"");
    printf(",\"firmware_store\":{\"stored\":%s",
           firmware_stored ? "true" : "false");
    if (firmware_stored) {
        printf(",\"target\":");
        print_json_escaped_string(firmware_store.name);
        printf(",\"app_project\":");
        print_json_escaped_string(firmware_store.project);
        printf(",\"hardware_type\":");
        print_json_escaped_string(firmware_store.hardware);
        printf(",\"version\":");
        print_json_escaped_string(firmware_store.version);
        printf(",\"sha256\":");
        print_json_escaped_string(firmware_store.sha256);
        printf(",\"size\":%lu,\"crc32\":%lu,\"generation\":%lu",
               (unsigned long)firmware_store.size,
               (unsigned long)firmware_store.checksum,
               (unsigned long)firmware_store.generation);
    }
    printf(",\"auto_update\":{\"worker_running\":%s,"
           "\"generation\":%lu,\"target_slot_mask\":%u,"
           "\"pending_mask\":%u,\"readiness_probes\":[%u,%u],"
           "\"scanners\":[",
           auto_update.worker_running ? "true" : "false",
           (unsigned long)auto_update.generation,
           (unsigned)auto_update.target_slot_mask,
           (unsigned)auto_update.pending_mask,
           (unsigned)auto_update.readiness_probe_attempts[0],
           (unsigned)auto_update.readiness_probe_attempts[1]);
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        printf("%s{\"slot\":%d,\"attempts\":%u,\"state\":",
               scanner_id == 0 ? "" : ",",
               scanner_id,
               (unsigned)auto_update.attempts[scanner_id]);
        print_json_escaped_string(auto_update.state[scanner_id]);
        printf("}");
    }
    printf("]}}");
#ifdef FOF_BADGE_VARIANT
    printf(",\"safe_mode\":%s,\"safe_reason\":",
           runtime_safe_mode ? "true" : "false");
    print_json_escaped_string(runtime_safe_reason);
    printf(",\"crash_count\":%lu,\"pending_verify\":%s,"
           "\"network_mode\":",
           (unsigned long)runtime_crash_count,
           runtime_pending_verify ? "true" : "false");
    print_json_escaped_string(
        badge_runtime_network_mode_name(network_mode));
    printf(",\"network_ttl_s\":%d,\"display_alive\":%s,"
           "\"usb_control_alive\":%s,\"scanner_uart_alive\":%s,"
           "\"reset_reason\":",
           network_ttl_s,
           runtime_display_alive ? "true" : "false",
           runtime_usb_control_alive ? "true" : "false",
           runtime_scanner_uart_alive ? "true" : "false");
    print_json_escaped_string(runtime_reset_reason_name);
    printf(",\"reset_reason_code\":%lu,\"reset_expected\":%s,"
           "\"usb_control_age_s\":%lld,\"recovery_mode\":",
           (unsigned long)runtime_reset_reason,
           runtime_reset_expected ? "true" : "false",
           (long long)runtime_usb_control_age_s);
    print_json_escaped_string(runtime_recovery_mode);
    printf(",\"power_mode\":");
    print_json_escaped_string(power_state.quiet ? "quiet" : "active");
    printf(",\"power_generation\":%lu,\"power_converged\":%s,"
           "\"scanner_power_converged\":%s,"
           "\"panel_power_converged\":%s,"
           "\"panel_powered\":%s,\"power_scanners\":[",
           (unsigned long)power_state.generation,
           power_converged ? "true" : "false",
           scanner_power_converged ? "true" : "false",
           panel_power_converged ? "true" : "false",
           oled_is_powered() ? "true" : "false");
    for (int i = 0; i < BADGE_POWER_SCANNER_COUNT; i++) {
        const badge_power_scanner_state_t *scanner = &power_state.scanners[i];
        printf("%s{\"slot\":%d,\"connected\":%s,\"acked\":%s,"
               "\"transition_ok\":%s,\"generation\":%lu,\"quiet\":%s,"
               "\"tx_enabled\":%s,\"ble_scanning\":%s,"
               "\"wifi_paused\":%s,\"ble_quiesced\":%s,"
               "\"wifi_quiesced\":%s,\"ble_active\":%s,"
               "\"wifi_active\":%s,\"radios_ready\":%s,"
               "\"tx_restored\":%s,\"uart_commands\":%s}",
               i == 0 ? "" : ",", i,
               scanner->connected ? "true" : "false",
               scanner->acked ? "true" : "false",
               scanner->transition_ok ? "true" : "false",
               (unsigned long)scanner->ack_generation,
               scanner->quiet ? "true" : "false",
               scanner->tx_enabled ? "true" : "false",
               scanner->ble_scanning ? "true" : "false",
               scanner->wifi_paused ? "true" : "false",
               scanner->ble_quiesced ? "true" : "false",
               scanner->wifi_quiesced ? "true" : "false",
               scanner->ble_active ? "true" : "false",
               scanner->wifi_active ? "true" : "false",
               scanner->radios_ready ? "true" : "false",
               scanner->tx_restored ? "true" : "false",
               scanner->uart_commands ? "true" : "false");
    }
    printf("]");
    printf(",\"stack_main_free\":%lu,\"stack_display_free\":%lu,"
           "\"stack_usb_free\":%lu,\"stack_uart_ble_free\":%lu,"
           "\"stack_uart_wifi_free\":%lu,"
           "\"heap_internal_free\":%lu,\"heap_internal_min_free\":%lu,"
           "\"heap_internal_largest\":%lu,\"psram_total\":%lu,"
           "\"psram_free\":%lu,\"psram_largest\":%lu",
           (unsigned long)stack_main_free,
           (unsigned long)stack_display_free,
           (unsigned long)stack_usb_free,
           (unsigned long)stack_uart_ble_free,
           (unsigned long)stack_uart_wifi_free,
           (unsigned long)heap_internal_free,
           (unsigned long)heap_internal_min_free,
           (unsigned long)heap_internal_largest,
           (unsigned long)psram_total,
           (unsigned long)psram_free,
           (unsigned long)psram_largest);
#endif
    printf(",\"threat_score\":%.1f,\"color_rgb565\":%u",
           snapshot.threat_score, (unsigned)snapshot.color_rgb565);
    printf(",\"reporting\":{\"network_mode\":");
#ifdef FOF_BADGE_VARIANT
    print_json_escaped_string(
        badge_runtime_network_mode_name(network_mode));
    printf(",\"backend_enabled\":%s,\"network_ttl_s\":%d",
           network_mode == BADGE_RUNTIME_NETWORK_BACKEND ? "true" : "false",
           network_ttl_s);
#else
    print_json_escaped_string(badge_mode_to_string(mode));
    printf(",\"backend_enabled\":%s,\"network_ttl_s\":0",
           badge_mode_backend_enabled(mode) ? "true" : "false");
#endif
    printf(",\"http_task_alive\":%s,\"wifi_sta\":%s,\"standalone\":%s,"
           "\"uploads_ok\":%d,\"uploads_fail\":%d,"
           "\"last_upload_age_s\":%lld}",
           http_task_alive ? "true" : "false",
           wifi_sta_connected ? "true" : "false",
           wifi_sta_standalone ? "true" : "false",
           uploads_ok,
           uploads_fail,
           (long long)upload_age_s);
    printf(",\"dominant_class\":");
    print_json_escaped_string(badge_threat_class_name(snapshot.dominant_class));
    printf(",\"dominant_category\":");
    print_json_escaped_string(snapshot.entity_count > 0
        ? badge_threat_category_name(snapshot.entities[0].category)
        : badge_threat_category_name(BADGE_THREAT_CATEGORY_NONE));
    printf(",\"dominant_proximity\":%d", (int)snapshot.dominant_proximity);
    uint32_t active_remote_id = badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_DRONE,
        false
    );
    uint32_t active_drone_ssid = badge_threat_snapshot_count_active(
        &snapshot,
        BADGE_THREAT_DRONE,
        BADGE_THREAT_CATEGORY_SSID,
        false
    );
    printf(",\"counts\":{\"drone\":%lu,\"remote_id\":%lu,"
           "\"drone_ssid\":%lu,\"meta\":%lu,\"tracker\":%lu,"
           "\"wifi_anomaly\":%lu,\"ble\":%lu,\"other\":%lu}",
           (unsigned long)snapshot.active_counts[BADGE_THREAT_DRONE],
           (unsigned long)active_remote_id,
           (unsigned long)active_drone_ssid,
           (unsigned long)snapshot.active_counts[BADGE_THREAT_META],
           (unsigned long)snapshot.active_counts[BADGE_THREAT_TRACKER],
           (unsigned long)snapshot.active_counts[BADGE_THREAT_WIFI_ANOMALY],
           (unsigned long)snapshot.active_counts[BADGE_THREAT_BLE],
           (unsigned long)snapshot.active_counts[BADGE_THREAT_OTHER]);
#ifdef FOF_BADGE_VARIANT
    print_display_policy_status_fields(policy_json, policy_hash,
                                       filtered_counts);
    printf(",\"theme_hash\":%lu,\"theme\":",
           (unsigned long)theme_hash);
    printf("%s", theme_json[0] ? theme_json : "{\"version\":1}");
    print_badge_display_state_field(&display_state, display_state_active);
    printf(",\"ble_control\":%s", ble_status[0] ? ble_status : "{\"enabled\":false}");
    printf(",\"ble_investigation\":%s",
           investigation_status[0] ? investigation_status
                                   : "{\"request_id\":\"\",\"state\":\"idle\"}");
    print_badge_button_state_field(&button_state, status_now_ms);
#endif
    printf(",\"entities\":[");
    for (int i = 0; i < snapshot.entity_count; i++) {
        const badge_threat_snapshot_entity_t *entity = &snapshot.entities[i];
        printf("%s{\"label\":", i > 0 ? "," : "");
        print_json_escaped_string(entity->label);
        printf(",\"detail\":");
        print_json_escaped_string(entity->detail);
        printf(",\"evidence\":");
        print_json_escaped_string(entity->evidence);
        printf(",\"class\":");
        print_json_escaped_string(badge_threat_class_name(entity->cls));
        printf(",\"category\":");
        print_json_escaped_string(badge_threat_category_name(entity->category));
        printf(",\"code\":");
        print_json_escaped_string(badge_threat_category_code(entity->category));
        printf(",\"display_id\":");
        print_json_escaped_string(entity->display_id);
        printf(",\"source\":");
        print_json_escaped_string(badge_threat_source_code(entity->source));
        if (entity->ssid[0] != '\0') {
            printf(",\"ssid\":");
            print_json_escaped_string(entity->ssid);
        }
        if (entity->bssid[0] != '\0') {
            printf(",\"bssid\":");
            print_json_escaped_string(entity->bssid);
        }
        if (entity->wifi_auth_mode != 0xFF) {
            printf(",\"auth_m\":%d", (int)entity->wifi_auth_mode);
        }
        if (entity->freq_mhz > 0) {
            printf(",\"freq_mhz\":%d", (int)entity->freq_mhz);
        }
        printf(",\"source_id\":%u,"
               "\"score\":%d,\"confidence_pct\":%d,"
               "\"evidence_quality\":%u,"
               "\"display_rank\":%d,\"age_s\":%d,\"last_seen_s\":%d,"
               "\"rssi\":%d,\"best_rssi\":%d,\"events\":%lu,"
               "\"seen_count\":%lu,\"group_count\":%lu,"
               "\"proximity_level\":%d,\"stale\":%s",
               (unsigned)entity->source,
               entity->score,
               entity->confidence_pct,
               (unsigned)entity->evidence_quality,
               entity->display_rank,
               entity->age_s,
               entity->last_seen_s,
               entity->rssi,
               entity->best_rssi,
               (unsigned long)entity->event_count,
               (unsigned long)entity->seen_count,
               (unsigned long)entity->group_count,
               (int)entity->proximity_level,
               entity->stale ? "true" : "false");
        if (entity->has_location) {
            printf(",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.1f",
                   entity->latitude, entity->longitude, entity->altitude_m);
        }
        if (entity->has_operator_location) {
            printf(",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
                   entity->operator_lat, entity->operator_lon);
        }
        if (entity->operator_id[0] != '\0') {
            printf(",\"operator_id\":");
            print_json_escaped_string(entity->operator_id);
        }
        printf("}");
    }
    printf("],\"scanners\":[");
    print_scanner_status_json("ble", 0, ble_connected, wifi_connected,
                              ble_info, &ble_uart_diag, true);
#if CONFIG_DUAL_SCANNER
    print_scanner_status_json("wifi", 1, wifi_connected, ble_connected,
                              wifi_info, &wifi_uart_diag, false);
#endif
    printf("]}\n");
    fflush(stdout);
    funlockfile(stdout);
}

static void send_control_ok(const char *message, bool reboot_required)
{
    printf("FOF_CTL_OK:{\"message\":");
    print_json_escaped_string(message ? message : "ok");
    printf(",\"reboot_required\":%s}\n", reboot_required ? "true" : "false");
    fflush(stdout);
}

static void send_control_error(const char *message)
{
    printf("FOF_CTL_ERROR:{\"error\":");
    print_json_escaped_string(message ? message : "unknown");
    printf("}\n");
    fflush(stdout);
}

static bool ctl_bool_value(const cJSON *item, bool fallback)
{
    if (!item) {
        return fallback;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "1") == 0 ||
               strcmp(item->valuestring, "true") == 0 ||
               strcmp(item->valuestring, "on") == 0 ||
               strcmp(item->valuestring, "yes") == 0;
    }
    return fallback;
}

static int ctl_int_value(const cJSON *item, int fallback)
{
    if (!item) {
        return fallback;
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return atoi(item->valuestring);
    }
    return fallback;
}

static bool ctl_add_bool_if_present(cJSON *out,
                                    const cJSON *root,
                                    const char *out_key,
                                    const char *in_key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, in_key);
    if (!item) {
        return false;
    }
    cJSON_AddBoolToObject(out, out_key, ctl_bool_value(item, false));
    return true;
}

static bool serial_json_uint32_exact(const cJSON *item, uint32_t *out)
{
    if (!out || !cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX) {
        return false;
    }
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

static void handle_fw_upload_begin(cJSON *root)
{
#ifdef FOF_BADGE_VARIANT
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *target_item = cJSON_GetObjectItemCaseSensitive(root, "target");
    const cJSON *project_item = cJSON_GetObjectItemCaseSensitive(root, "project");
    const cJSON *hardware_item = cJSON_GetObjectItemCaseSensitive(root, "hardware_type");
    const cJSON *version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *crc_item = cJSON_GetObjectItemCaseSensitive(root, "crc32");
    const cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *slot_mask_item = cJSON_GetObjectItemCaseSensitive(
        root, "slot_mask");
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : "";
    const char *target = cJSON_IsString(target_item) ? target_item->valuestring : "";
    const char *project = cJSON_IsString(project_item) ? project_item->valuestring : "";
    const char *hardware = cJSON_IsString(hardware_item) ? hardware_item->valuestring : "";
    const char *version = cJSON_IsString(version_item) ? version_item->valuestring : "";
    const char *sha256 = cJSON_IsString(sha_item) ? sha_item->valuestring : "";
    uint32_t size = 0;
    uint32_t crc32 = 0;
    uint32_t slot_mask = 0;
    if (!serial_json_uint32_exact(size_item, &size) ||
        !serial_json_uint32_exact(crc_item, &crc32) ||
        !serial_json_uint32_exact(slot_mask_item, &slot_mask) ||
        slot_mask == 0 || slot_mask > 0x3 ||
        strcmp(name, "scanner-s3-combo-fof_badge") != 0 ||
        strcmp(target, "scanner-s3-combo-fof_badge") != 0 ||
        strcmp(project, "fof_badge_scanner") != 0 ||
        strcmp(hardware, "seeed_xiao_esp32s3") != 0 ||
        !version[0] || !fof_firmware_sha256_hex_is_valid(sha256)) {
        printf("FOF_FW_UPLOAD:{\"ok\":false,\"error\":\"invalid_manifest\"}\n");
        fflush(stdout);
        return;
    }
    char resp[640];
    bool ok = fw_store_serial_upload_begin(
        name,
        version,
        size,
        crc32,
        sha256,
        (uint8_t)slot_mask,
        resp,
        sizeof(resp)
    );
    printf("FOF_FW_UPLOAD:%s\n", resp);
    fflush(stdout);
    if (ok) {
        s_serial_fw_rx_active = true;
        s_serial_fw_last_rx_ms = esp_timer_get_time() / 1000;
    }
#else
    printf("FOF_FW_UPLOAD:{\"ok\":false,\"error\":\"badge_only\"}\n");
    fflush(stdout);
#endif
}

static void handle_scanner_display_control(cJSON *root, const char *cmd_name)
{
#ifdef FOF_BADGE_VARIANT
    if (badge_runtime_is_safe_mode()) {
        send_control_error("safe mode blocks scanner display control");
        return;
    }
#endif
    cJSON *scanner_cmd = cJSON_CreateObject();
    if (!scanner_cmd) {
        send_control_error("no memory");
        return;
    }
    cJSON_AddStringToObject(scanner_cmd, "type", "display_control");

    const cJSON *button_item = cJSON_GetObjectItemCaseSensitive(root, "button_enabled");
    if (!button_item) {
        button_item = cJSON_GetObjectItemCaseSensitive(root, "trigger_enabled");
    }
    if (!button_item) {
        button_item = cJSON_GetObjectItemCaseSensitive(root, "boot_enabled");
    }
    if (strcmp(cmd_name, "scanner_trigger") == 0 || strcmp(cmd_name, "trigger") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (enabled) {
            button_item = enabled;
        } else if (!button_item) {
            cJSON_AddBoolToObject(scanner_cmd, "button_enabled", false);
        }
    }
    if (button_item) {
        cJSON_AddBoolToObject(scanner_cmd, "button_enabled",
                              ctl_bool_value(button_item, false));
    }

    const cJSON *view = cJSON_GetObjectItemCaseSensitive(root, "view");
    if (cJSON_IsString(view) && view->valuestring && view->valuestring[0]) {
        cJSON_AddStringToObject(scanner_cmd, "view", view->valuestring);
    }
    const cJSON *page = cJSON_GetObjectItemCaseSensitive(root, "page");
    if (cJSON_IsNumber(page)) {
        cJSON_AddNumberToObject(scanner_cmd, "page", page->valueint);
    }
    ctl_add_bool_if_present(scanner_cmd, root, "page_lock", "page_lock");
    ctl_add_bool_if_present(scanner_cmd, root, "auto_page", "auto_page");

    char *payload = cJSON_PrintUnformatted(scanner_cmd);
    cJSON_Delete(scanner_cmd);
    if (!payload) {
        send_control_error("no memory");
        return;
    }

    const cJSON *uart_item = cJSON_GetObjectItemCaseSensitive(root, "uart");
    const char *uart = cJSON_IsString(uart_item) ? uart_item->valuestring : "all";
    bool ble_sent = false;
    bool wifi_sent = false;
    if (strcmp(uart, "ble") == 0 || strcmp(uart, "0") == 0) {
        ble_sent = uart_rx_send_command_to_scanner_checked(0, payload);
    } else if (strcmp(uart, "wifi") == 0 || strcmp(uart, "1") == 0) {
        wifi_sent = uart_rx_send_command_to_scanner_checked(1, payload);
    } else if (strcmp(uart, "all") == 0 || strcmp(uart, "*") == 0) {
        ble_sent = uart_rx_send_command_to_scanner_checked(0, payload);
#if CONFIG_DUAL_SCANNER
        wifi_sent = uart_rx_send_command_to_scanner_checked(1, payload);
#endif
    } else {
        cJSON_free(payload);
        send_control_error("uart must be ble, wifi, or all");
        return;
    }
    cJSON_free(payload);

    printf("FOF_CTL_OK:{\"message\":\"scanner display command sent\","
           "\"ble_sent\":%s,\"wifi_sent\":%s,\"reboot_required\":false}\n",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false");
    fflush(stdout);
}

static void handle_scanner_safe_mode_control(cJSON *root)
{
#ifdef FOF_BADGE_VARIANT
    cJSON *scanner_cmd = cJSON_CreateObject();
    if (!scanner_cmd) {
        send_control_error("no memory");
        return;
    }

    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    bool enabled = ctl_bool_value(enabled_item, true);
    cJSON_AddStringToObject(scanner_cmd, "type", "safe_mode");
    cJSON_AddBoolToObject(scanner_cmd, "enabled", enabled);

    char *payload = cJSON_PrintUnformatted(scanner_cmd);
    cJSON_Delete(scanner_cmd);
    if (!payload) {
        send_control_error("no memory");
        return;
    }

    const cJSON *uart_item = cJSON_GetObjectItemCaseSensitive(root, "uart");
    const char *uart = cJSON_IsString(uart_item) ? uart_item->valuestring : "all";
    bool ble_sent = false;
    bool wifi_sent = false;
    if (strcmp(uart, "ble") == 0 || strcmp(uart, "0") == 0) {
        ble_sent = uart_rx_send_command_to_scanner_checked(0, payload);
    } else if (strcmp(uart, "wifi") == 0 || strcmp(uart, "1") == 0) {
        wifi_sent = uart_rx_send_command_to_scanner_checked(1, payload);
    } else if (strcmp(uart, "all") == 0 || strcmp(uart, "*") == 0) {
        ble_sent = uart_rx_send_command_to_scanner_checked(0, payload);
#if CONFIG_DUAL_SCANNER
        wifi_sent = uart_rx_send_command_to_scanner_checked(1, payload);
#endif
    } else {
        cJSON_free(payload);
        send_control_error("uart must be ble, wifi, or all");
        return;
    }
    cJSON_free(payload);

    printf("FOF_CTL_OK:{\"message\":\"scanner safe mode command sent\","
           "\"ble_sent\":%s,\"wifi_sent\":%s,\"enabled\":%s,"
           "\"reboot_required\":true}\n",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false",
           enabled ? "true" : "false");
    fflush(stdout);
#else
    (void)root;
    send_control_error("scanner safe mode is badge-only");
#endif
}

#ifdef FOF_BADGE_VARIANT
static void send_network_ok(const char *message, bool applied)
{
    printf("FOF_CTL_OK:{\"message\":");
    print_json_escaped_string(message ? message : "network");
    printf(",\"network_mode\":");
    print_json_escaped_string(
        badge_runtime_network_mode_name(badge_runtime_get_network_mode()));
    printf(",\"network_ttl_s\":%d,\"applied\":%s,\"reboot_required\":false}\n",
           badge_runtime_get_network_ttl_s(),
           applied ? "true" : "false");
    fflush(stdout);
}

static void handle_network_command(cJSON *root)
{
    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *ttl_item = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
    badge_runtime_network_mode_t mode;
    if (!cJSON_IsString(mode_item) ||
        !badge_runtime_parse_network_mode(mode_item->valuestring, &mode)) {
        send_control_error("invalid network mode");
        return;
    }

    if (!badge_runtime_badge_allows_network_mode(mode)) {
        (void)badge_runtime_request_network(BADGE_RUNTIME_NETWORK_OFF, 0,
                                            "network_disabled");
        send_control_error("badge network disabled");
        return;
    }

    bool applied = badge_runtime_request_network(mode,
                                                 ctl_int_value(ttl_item, 0),
                                                 "usb");
    send_network_ok("network session updated", applied);
}

static void handle_safe_mode_command(cJSON *root)
{
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    bool on = ctl_bool_value(enabled, true);
    badge_runtime_force_safe_mode(
        on,
        cJSON_IsString(reason) ? reason->valuestring : "usb"
    );
    send_control_ok(on ? "safe mode enabled" : "safe mode disabled", false);
}

static void handle_display_policy_command(cJSON *root)
{
    const cJSON *policy_item = cJSON_GetObjectItemCaseSensitive(root, "policy");
    if (!policy_item) {
        send_control_error("missing policy");
        return;
    }
    char *policy_json = cJSON_PrintUnformatted(policy_item);
    if (!policy_json) {
        send_control_error("no memory");
        return;
    }

    badge_display_policy_t policy;
    char err[64] = {0};
    bool parsed = badge_display_policy_parse_json(policy_json, &policy,
                                                  err, sizeof(err));
    cJSON_free(policy_json);
    if (!parsed) {
        send_control_error(err[0] ? err : "invalid display policy");
        return;
    }

    bool persist = ctl_bool_value(
        cJSON_GetObjectItemCaseSensitive(root, "persist"),
        false);
    if (!badge_display_policy_runtime_set(&policy, persist)) {
        send_control_error("display policy save failed");
        return;
    }

    bool ble_sent = false;
    bool wifi_sent = false;
    forward_display_policy_to_scanners(&ble_sent, &wifi_sent);
    printf("FOF_CTL_OK:{\"message\":\"display policy updated\","
           "\"display_policy_hash\":%lu,\"persisted\":%s,"
           "\"ble_sent\":%s,\"wifi_sent\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_display_policy_runtime_hash(),
           persist ? "true" : "false",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false");
    fflush(stdout);
}

static void handle_display_policy_reset_command(cJSON *root)
{
    bool persist = ctl_bool_value(
        cJSON_GetObjectItemCaseSensitive(root, "persist"),
        false);
    badge_display_policy_runtime_reset(persist);
    bool ble_sent = false;
    bool wifi_sent = false;
    forward_display_policy_to_scanners(&ble_sent, &wifi_sent);
    printf("FOF_CTL_OK:{\"message\":\"display policy reset\","
           "\"display_policy_hash\":%lu,\"persisted\":%s,"
           "\"ble_sent\":%s,\"wifi_sent\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_display_policy_runtime_hash(),
           persist ? "true" : "false",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false");
    fflush(stdout);
}

static void handle_badge_theme_command(cJSON *root)
{
    const cJSON *theme_item = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (!theme_item) {
        send_control_error("missing theme");
        return;
    }
    char *theme_json = cJSON_PrintUnformatted(theme_item);
    if (!theme_json) {
        send_control_error("no memory");
        return;
    }

    badge_theme_t theme;
    char err[64] = {0};
    bool parsed = badge_theme_parse_json(theme_json, &theme, err, sizeof(err));
    cJSON_free(theme_json);
    if (!parsed) {
        send_control_error(err[0] ? err : "invalid badge theme");
        return;
    }

    bool persist = ctl_bool_value(
        cJSON_GetObjectItemCaseSensitive(root, "persist"),
        false);
    if (!badge_theme_runtime_set(&theme, persist)) {
        send_control_error("badge theme save failed");
        return;
    }

    printf("FOF_CTL_OK:{\"message\":\"badge theme updated\","
           "\"theme_hash\":%lu,\"persisted\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_theme_runtime_hash(),
           persist ? "true" : "false");
    fflush(stdout);
}

static void handle_badge_theme_reset_command(cJSON *root)
{
    bool persist = ctl_bool_value(
        cJSON_GetObjectItemCaseSensitive(root, "persist"),
        false);
    if (!badge_theme_runtime_reset(persist)) {
        send_control_error("badge theme reset failed");
        return;
    }
    printf("FOF_CTL_OK:{\"message\":\"badge theme reset\","
           "\"theme_hash\":%lu,\"persisted\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_theme_runtime_hash(),
           persist ? "true" : "false");
    fflush(stdout);
}
#endif

static void handle_fw_check_now_command(cJSON *root)
{
    const cJSON *uart_item = cJSON_GetObjectItemCaseSensitive(root, "uart");
    const char *uart = cJSON_IsString(uart_item)
        ? uart_item->valuestring : "both";
    uint8_t target_mask = 0;
    if (strcmp(uart, "ble") == 0 || strcmp(uart, "0") == 0) {
        target_mask = FW_AUTO_UPDATE_SLOT_BLE;
    } else if (strcmp(uart, "wifi") == 0 || strcmp(uart, "1") == 0) {
        target_mask = FW_AUTO_UPDATE_SLOT_WIFI;
    } else if (strcmp(uart, "both") == 0 || strcmp(uart, "all") == 0 ||
               strcmp(uart, "*") == 0) {
        target_mask = FW_AUTO_UPDATE_SLOT_ALL;
    } else {
        send_control_error("uart must be ble, wifi, or both");
        return;
    }

    bool deferred = fw_store_is_relay_active() || http_upload_is_paused();
    uint8_t sent_mask = deferred ? 0 :
        fw_store_request_scanner_checks(target_mask);
    bool requested_ble = (target_mask & FW_AUTO_UPDATE_SLOT_BLE) != 0;
    bool requested_wifi = (target_mask & FW_AUTO_UPDATE_SLOT_WIFI) != 0;
    bool ble_sent = (sent_mask & FW_AUTO_UPDATE_SLOT_BLE) != 0;
    bool wifi_sent = (sent_mask & FW_AUTO_UPDATE_SLOT_WIFI) != 0;
    bool ok = (!requested_ble || ble_sent) &&
              (!requested_wifi || wifi_sent);
    printf("FOF_CTL_%s:{\"message\":\"firmware check requested\","
           "\"uart\":\"%s\",\"ble_sent\":%s,\"wifi_sent\":%s,"
           "\"deferred\":%s,\"error\":\"%s\"}\n",
           ok ? "OK" : "ERROR",
           uart,
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false",
           deferred ? "true" : "false",
           deferred ? "firmware_operation_active" :
               (ok ? "" : "scanner_command_ingress_unreachable"));
    fflush(stdout);
}

static void handle_ctl_command(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        send_control_error("invalid json");
        return;
    }

    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";

    if (strcmp(cmd, "status") == 0) {
        send_badge_status_response();
    } else if (strcmp(cmd, "power_mode") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (!cJSON_IsString(mode_item)) {
            send_control_error("power mode must be active or quiet");
        } else {
            bool quiet = strcmp(mode_item->valuestring, "quiet") == 0 ||
                         strcmp(mode_item->valuestring, "off") == 0;
            bool active = strcmp(mode_item->valuestring, "active") == 0 ||
                          strcmp(mode_item->valuestring, "on") == 0;
            if (!quiet && !active) {
                send_control_error("power mode must be active or quiet");
            } else {
                (void)badge_power_runtime_request(quiet, "usb");
                badge_power_state_t state = {0};
                badge_power_runtime_snapshot(&state);
                printf("FOF_CTL_OK:{\"message\":\"power mode updated\","
                       "\"power_mode\":\"%s\",\"power_generation\":%lu,"
                       "\"reboot_required\":false}\n",
                       state.quiet ? "quiet" : "active",
                       (unsigned long)state.generation);
                fflush(stdout);
            }
        }
#else
        send_control_error("power mode is badge-only");
#endif
    } else if (strcmp(cmd, "set_mode") == 0) {
        const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
#ifdef FOF_BADGE_VARIANT
        badge_runtime_network_mode_t runtime_mode;
        const cJSON *ttl_item = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
        const bool persist_mode = ctl_bool_value(
            cJSON_GetObjectItemCaseSensitive(root, "persist"),
            false);
        if (!cJSON_IsString(mode_item) ||
            !badge_runtime_parse_network_mode(mode_item->valuestring, &runtime_mode)) {
            send_control_error("invalid mode");
        } else if (!badge_runtime_badge_allows_network_mode(runtime_mode)) {
            (void)badge_runtime_request_network(BADGE_RUNTIME_NETWORK_OFF, 0,
                                                "set_mode_disabled");
            send_control_error("badge network disabled");
        } else {
            if (persist_mode) {
                badge_mode_t persisted_mode;
                if (badge_mode_parse(mode_item->valuestring, &persisted_mode)) {
                    badge_mode_set(persisted_mode);
                }
            }
            bool applied = badge_runtime_request_network(
                runtime_mode,
                persist_mode ? -1 : ctl_int_value(ttl_item, 0),
                "set_mode"
            );
            send_network_ok("session mode updated", applied);
        }
#else
        badge_mode_t mode;
        if (!cJSON_IsString(mode_item) ||
            !badge_mode_parse(mode_item->valuestring, &mode)) {
            send_control_error("invalid mode");
        } else if (badge_mode_set(mode)) {
            send_control_ok("mode saved", true);
        } else {
            send_control_error("mode save failed");
        }
#endif
    } else if (strcmp(cmd, "set_backend") == 0) {
        const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
        const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "wifi_pass");
        const cJSON *enable_item = cJSON_GetObjectItemCaseSensitive(root, "enable");
        if (cJSON_IsString(url_item) && url_item->valuestring[0]) {
            nvs_config_set_string("backend_url", url_item->valuestring);
        }
        if (cJSON_IsString(ssid_item) && ssid_item->valuestring[0]) {
            nvs_config_set_string("wifi_ssid", ssid_item->valuestring);
        }
        if (cJSON_IsString(pass_item) && pass_item->valuestring[0]) {
            nvs_config_set_string("wifi_pass", pass_item->valuestring);
        }
        if ((cJSON_IsBool(enable_item) && cJSON_IsTrue(enable_item)) ||
            (cJSON_IsNumber(enable_item) && enable_item->valueint != 0)) {
#ifdef FOF_BADGE_VARIANT
            badge_mode_set(BADGE_MODE_BACKEND);
            const cJSON *ttl_item = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
            bool applied = badge_runtime_request_network(
                BADGE_RUNTIME_NETWORK_BACKEND,
                ctl_int_value(ttl_item, -1),
                "set_backend"
            );
            send_network_ok("backend config saved", applied);
            cJSON_Delete(root);
            return;
#else
            badge_mode_set(BADGE_MODE_BACKEND);
#endif
        }
#ifdef FOF_BADGE_VARIANT
        send_network_ok("backend config saved", true);
#else
        send_control_ok("backend config saved", true);
#endif
    } else if (strcmp(cmd, "set_display_debug") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        nvs_config_set_string("badge_display_debug",
                              (cJSON_IsBool(enabled) && cJSON_IsTrue(enabled)) ? "1" : "0");
        send_control_ok("display debug saved", false);
    } else if (strcmp(cmd, "network") == 0) {
#ifdef FOF_BADGE_VARIANT
        handle_network_command(root);
#else
        send_control_error("network sessions are badge-only");
#endif
    } else if (strcmp(cmd, "safe_mode") == 0) {
#ifdef FOF_BADGE_VARIANT
        handle_safe_mode_command(root);
#else
        send_control_error("safe mode is badge-only");
#endif
    } else if (strcmp(cmd, "ble_investigate") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
        char err[64] = {0};
        if (!cJSON_IsString(request_id) || !cJSON_IsString(mode) ||
            (target && !cJSON_IsNull(target) && !cJSON_IsString(target)) ||
            !badge_ble_investigation_start(
                request_id->valuestring,
                mode->valuestring,
                cJSON_IsString(target) ? target->valuestring : "",
                "usb",
                err,
                sizeof(err))) {
            send_control_error(err[0] ? err : "invalid investigation request");
        } else {
            send_control_ok("BLE investigation started", false);
        }
#else
        send_control_error("BLE investigation is badge-only");
#endif
    } else if (strcmp(cmd, "ble_investigation_chunk") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        int chunk_seq = -1;
        char chunk_json[UART_JSON_MAX_SIZE];
        char frame[BADGE_BLE_INVESTIGATION_USB_FRAME_MAX];
        if (!cJSON_IsString(request_id) || !cJSON_IsNumber(seq) ||
            !badge_ble_investigation_index_from_number(seq->valuedouble,
                                                        &chunk_seq) ||
            badge_ble_investigation_chunk_json(request_id->valuestring,
                                               chunk_seq,
                                               chunk_json,
                                               sizeof(chunk_json)) == 0 ||
            badge_ble_investigation_usb_frame(chunk_json, frame,
                                              sizeof(frame)) == 0) {
            send_control_error("invalid investigation chunk cursor");
        } else {
            printf("%s", frame);
            fflush(stdout);
        }
#else
        send_control_error("BLE investigation is badge-only");
#endif
    } else if (strcmp(cmd, "badge_display_policy") == 0) {
#ifdef FOF_BADGE_VARIANT
        handle_display_policy_command(root);
#else
        send_control_error("badge display policy is badge-only");
#endif
    } else if (strcmp(cmd, "badge_display_policy_reset") == 0) {
#ifdef FOF_BADGE_VARIANT
        handle_display_policy_reset_command(root);
#else
        send_control_error("badge display policy is badge-only");
#endif
    } else if (strcmp(cmd, "badge_theme") == 0) {
#ifdef FOF_BADGE_VARIANT
        handle_badge_theme_command(root);
#else
        send_control_error("badge theme is badge-only");
#endif
    } else if (strcmp(cmd, "badge_theme_reset") == 0) {
#ifdef FOF_BADGE_VARIANT
        handle_badge_theme_reset_command(root);
#else
        send_control_error("badge theme is badge-only");
#endif
    } else if (strcmp(cmd, "display_nav") == 0) {
#ifdef FOF_BADGE_VARIANT
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsString(action) ||
            !oled_badge_handle_nav_command(action->valuestring)) {
            send_control_error("invalid display nav action");
        } else {
            send_control_ok("display nav updated", false);
        }
#else
        send_control_error("display nav is badge-only");
#endif
    } else if (strcmp(cmd, "scanner_display") == 0 ||
               strcmp(cmd, "scanner_trigger") == 0 ||
               strcmp(cmd, "trigger") == 0) {
        handle_scanner_display_control(root, cmd);
    } else if (strcmp(cmd, "scanner_safe_mode") == 0 ||
               strcmp(cmd, "scanner_recovery") == 0) {
        handle_scanner_safe_mode_control(root);
    } else if (strcmp(cmd, "fw_check_now") == 0 ||
               strcmp(cmd, "fw_check") == 0) {
        handle_fw_check_now_command(root);
    } else if (strcmp(cmd, "fw_stage_metadata") == 0) {
        /* Metadata is derived from and cryptographically bound to uploaded
         * bytes. Never allow a control command to manufacture a valid image. */
        send_control_error("fw_stage_metadata disabled; upload verified bytes");
    } else if (strcmp(cmd, "fw_upload_begin") == 0) {
        handle_fw_upload_begin(root);
    } else if (strcmp(cmd, "fw_relay") == 0) {
#ifdef FOF_BADGE_VARIANT
        if (badge_runtime_is_safe_mode()) {
            send_control_error("safe mode blocks firmware relay");
            cJSON_Delete(root);
            return;
        }
#endif
        const cJSON *uart_item = cJSON_GetObjectItemCaseSensitive(root, "uart");
        const char *uart = cJSON_IsString(uart_item) ? uart_item->valuestring : "ble";
        int scanner_id = (strcmp(uart, "wifi") == 0) ? 1 : 0;
        if (strcmp(uart, "ble") != 0 && strcmp(uart, "wifi") != 0) {
            send_control_error("uart must be ble or wifi");
        } else {
            char resp[1024];
            const cJSON *force_item = cJSON_GetObjectItemCaseSensitive(root, "force");
            if (!force_item) {
                force_item = cJSON_GetObjectItemCaseSensitive(root, "skip_command_probe");
            }
            bool force = (force_item && cJSON_IsTrue(force_item)) ||
                         (force_item && cJSON_IsNumber(force_item) &&
                          force_item->valueint != 0);
            const cJSON *allow_same_item =
                cJSON_GetObjectItemCaseSensitive(root, "allow_same_version");
            bool allow_same = (allow_same_item && cJSON_IsTrue(allow_same_item)) ||
                              (allow_same_item && cJSON_IsNumber(allow_same_item) &&
                               allow_same_item->valueint != 0);
            fw_store_relay_staged_to_scanner_ex(scanner_id, force, allow_same,
                                                resp, sizeof(resp));
            printf("FOF_FW_RELAY:%s\n", resp);
            fflush(stdout);
        }
    } else if (strcmp(cmd, "reboot") == 0) {
        cJSON_Delete(root);
        reboot_app();
        return;
    } else if (strcmp(cmd, "rollback") == 0) {
#ifdef FOF_BADGE_VARIANT
        if (!badge_runtime_pending_verify()) {
            send_control_error("no pending OTA image");
        } else {
            send_response("FOF_CTL_OK:{\"message\":\"rollback\",\"reboot_required\":true}\n");
#ifdef FOF_BADGE_VARIANT
            badge_runtime_arm_expected_reboot("usb_rollback");
#endif
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
#else
        send_control_error("rollback control is badge-only");
#endif
    } else if (strcmp(cmd, "bootloader") == 0 || strcmp(cmd, "ota") == 0) {
        cJSON_Delete(root);
        reboot_to_download_mode();
        return;
    } else {
        send_control_error("unknown command");
    }

    cJSON_Delete(root);
}

/**
 * Check if data is available on stdin within timeout_ms.
 * Returns true if data is ready to read.
 */
static bool stdin_has_data(int timeout_ms)
{
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    return (ret > 0 && FD_ISSET(STDIN_FILENO, &fds));
}

/**
 * Read one line from stdin with a timeout.
 * Returns the number of characters read, or 0 on timeout.
 */
static int read_line(char *buf, int buf_size, int timeout_ms)
{
    int pos = 0;
    int elapsed = 0;
    const int poll_interval = 50;

    memset(buf, 0, buf_size);

    while (elapsed < timeout_ms && pos < buf_size - 1) {
        if (stdin_has_data(poll_interval)) {
            int ch = fgetc(stdin);
            if (ch == EOF) {
                elapsed += poll_interval;
                continue;
            }

            if (ch == '\n' || ch == '\r') {
                buf[pos] = '\0';
                if (pos > 0) {
                    return pos;
                }
                /* Skip empty lines / lone CR/LF */
                continue;
            }

            buf[pos++] = (char)ch;
            /* Reset timeout on each received character */
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }

    buf[pos] = '\0';
    return pos;
}

static bool handle_set_command(const char *line)
{
    /* Expected format: FOF_SET:key=value */
    const char *payload = line + strlen(CMD_PREFIX);

    /* Find the '=' separator */
    const char *eq = strchr(payload, '=');
    if (!eq || eq == payload) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%smalformed command\n", RESP_ERROR);
        send_response(msg);
        return false;
    }

    /* Extract key */
    int key_len = eq - payload;
    char key[32] = {0};
    if (key_len >= (int)sizeof(key)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%skey too long\n", RESP_ERROR);
        send_response(msg);
        return false;
    }
    memcpy(key, payload, key_len);
    key[key_len] = '\0';

    /* Validate key */
    if (!is_allowed_key(key)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%sunknown key '%s'\n", RESP_ERROR, key);
        send_response(msg);
        return false;
    }

    /* Extract value */
    const char *value = eq + 1;

    /* Write to NVS */
    if (nvs_config_set_string(key, value)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s%s\n", RESP_OK, key);
        send_response(msg);
        ESP_LOGI(TAG, "Set %s = %s", key,
                 (strcmp(key, "wifi_pass") == 0 || strcmp(key, "ap_pass") == 0)
                 ? "****" : value);
        return true;
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "%sNVS write failed for '%s'\n",
                 RESP_ERROR, key);
        send_response(msg);
        return false;
    }
}

bool serial_config_listen(int timeout_ms)
{
    char line[LINE_BUF_SIZE];
    bool any_saved = false;

    /* Make stdin non-blocking friendly via VFS select */
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    ESP_LOGI(TAG, "Config mode: waiting %dms for serial commands...", timeout_ms);
    send_response(RESP_READY);

    /* Wait for first command */
    int n = read_line(line, sizeof(line), timeout_ms);
    if (n == 0) {
        ESP_LOGI(TAG, "No serial config received, continuing boot");
        send_response(RESP_TIMEOUT);
        goto cleanup;
    }

    /* Process commands until SAVE or timeout */
    while (1) {
        if (strncmp(line, CMD_PREFIX, strlen(CMD_PREFIX)) == 0) {
            if (handle_set_command(line)) {
                any_saved = true;
            }
        } else if (strcmp(line, CMD_SAVE) == 0) {
            send_response(RESP_SAVED);
            ESP_LOGI(TAG, "Configuration saved to NVS");
            break;
        } else if (strcmp(line, CMD_PING) == 0 ||
                   strcmp(line, CMD_STATUS) == 0 ||
                   strcmp(line, CMD_REBOOT) == 0 ||
                   strcmp(line, CMD_BOOTLOADER) == 0 ||
                   strcmp(line, CMD_DOWNLOAD) == 0 ||
                   strcmp(line, CMD_FLASH) == 0 ||
                   strncmp(line, CMD_CTL, strlen(CMD_CTL)) == 0) {
            handle_control_line(line);
        } else if (strlen(line) > 0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%sunknown command\n", RESP_ERROR);
            send_response(msg);
        }

        /* Read next line with 5-second inter-command timeout */
        n = read_line(line, sizeof(line), 5000);
        if (n == 0) {
            ESP_LOGI(TAG, "Config timeout (inter-command), continuing boot");
            send_response(RESP_TIMEOUT);
            break;
        }
    }

cleanup:
    /* Restore blocking mode */
    fcntl(STDIN_FILENO, F_SETFL, 0);
    return any_saved;
}

static void handle_control_line(const char *line)
{
    if (!line || line[0] == '\0') {
        return;
    }
#ifdef FOF_BADGE_VARIANT
    badge_runtime_note_usb_control_alive();
#endif
    if (!serial_output_lock()) {
        return;
    }

    if (strcmp(line, CMD_PING) == 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "FOF_PONG:%s\n", FOF_VERSION);
        send_response(msg);
    } else if (strcmp(line, CMD_STATUS) == 0) {
        send_badge_status_response();
    } else if (strncmp(line, CMD_CTL, strlen(CMD_CTL)) == 0) {
        handle_ctl_command(line + strlen(CMD_CTL));
    } else if (strcmp(line, CMD_REBOOT) == 0) {
        reboot_app();
    } else if (strcmp(line, CMD_BOOTLOADER) == 0 ||
               strcmp(line, CMD_DOWNLOAD) == 0 ||
               strcmp(line, CMD_FLASH) == 0) {
        reboot_to_download_mode();
    } else if (strncmp(line, CMD_PREFIX, strlen(CMD_PREFIX)) == 0) {
        (void)handle_set_command(line);
    } else if (strcmp(line, CMD_SAVE) == 0) {
        send_response(RESP_SAVED);
    }
    serial_output_unlock();
}

static int read_control_char(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    uint8_t c = 0;
    int n = usb_serial_jtag_ll_read_rxfifo(&c, 1);
    return n > 0 ? (int)c : EOF;
#else
    return fgetc(stdin);
#endif
}

static int read_control_bytes(uint8_t *buf, int max_len)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_ll_read_rxfifo(buf, max_len);
#else
    int n = 0;
    while (n < max_len && stdin_has_data(0)) {
        int ch = fgetc(stdin);
        if (ch == EOF) break;
        buf[n++] = (uint8_t)ch;
    }
    return n;
#endif
}

static void serial_control_task(void *arg)
{
    (void)arg;
    char line[LINE_BUF_SIZE];
    uint8_t fw_buf[SERIAL_FW_BUF_SIZE];
    int pos = 0;
#ifdef FOF_BADGE_VARIANT
    int64_t last_heartbeat_ms = 0;
#endif

    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "Runtime USB serial control listener ready");

    while (1) {
#ifdef FOF_BADGE_VARIANT
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (last_heartbeat_ms == 0 || (now_ms - last_heartbeat_ms) >= 1000) {
            badge_runtime_note_usb_control_alive();
            badge_runtime_note_usb_stack_free(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = now_ms;
        }
#endif
        if (s_serial_fw_rx_active) {
            uint32_t remaining = fw_store_serial_upload_remaining();
            if (remaining == 0) {
                char resp[640];
                fw_store_serial_upload_end(resp, sizeof(resp));
                printf("FOF_FW_UPLOAD:%s\n", resp);
                fflush(stdout);
                s_serial_fw_rx_active = false;
                pos = 0;
                continue;
            }
            int want = remaining > sizeof(fw_buf) ? sizeof(fw_buf) : (int)remaining;
            int n = read_control_bytes(fw_buf, want);
            if (n > 0) {
                char resp[160];
                s_serial_fw_last_rx_ms = esp_timer_get_time() / 1000;
                if (!fw_store_serial_upload_write(fw_buf, (size_t)n,
                                                  resp, sizeof(resp))) {
                    printf("FOF_FW_UPLOAD:%s\n", resp);
                    fflush(stdout);
                    s_serial_fw_rx_active = false;
                    pos = 0;
                }
                continue;
            }
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (s_serial_fw_last_rx_ms > 0 &&
                (now_ms - s_serial_fw_last_rx_ms) > SERIAL_FW_IDLE_TIMEOUT_MS) {
                fw_store_serial_upload_abort("usb_idle_timeout");
                printf("FOF_FW_UPLOAD:{\"ok\":false,\"error\":\"usb_idle_timeout\"}\n");
                fflush(stdout);
                s_serial_fw_rx_active = false;
                pos = 0;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        int ch = read_control_char();
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                handle_control_line(line);
                pos = 0;
            }
            continue;
        }

        if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        } else {
            pos = 0;
            send_response(RESP_ERROR "line too long\n");
        }
    }
}

bool serial_config_start_control_task(void)
{
    if (s_control_task_started) {
        return true;
    }
    BaseType_t ok = xTaskCreate(serial_control_task,
                                "serial_ctrl",
                                CONTROL_STACK_BYTES,
                                NULL,
                                tskIDLE_PRIORITY + 1,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start USB serial control listener");
        return false;
    }
    s_control_task_started = true;
    return true;
}

static void print_json_escaped_string(const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    putchar('"');
    while (*p) {
        unsigned char c = *p++;
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar((int)c);
        } else if (c == '\b') {
            printf("\\b");
        } else if (c == '\f') {
            printf("\\f");
        } else if (c == '\n') {
            printf("\\n");
        } else if (c == '\r') {
            printf("\\r");
        } else if (c == '\t') {
            printf("\\t");
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar((int)c);
        }
    }
    putchar('"');
}

void serial_config_emit_badge_detection(const char *detection_id,
                                        const char *manufacturer,
                                        const char *badge_label,
                                        const char *badge_class,
                                        const char *badge_entity_key,
                                        uint8_t source,
                                        float confidence,
                                        float threat_score,
                                        int rssi)
{
    if (!serial_output_lock()) return;
    printf("FOF_DET:{\"id\":");
    print_json_escaped_string(detection_id);
    printf(",\"manufacturer\":");
    print_json_escaped_string(manufacturer);
    printf(",\"badge_label\":");
    print_json_escaped_string(badge_label);
    printf(",\"badge_class\":");
    print_json_escaped_string(badge_class);
    printf(",\"badge_entity_key\":");
    print_json_escaped_string(badge_entity_key);
    printf(",\"source\":%u,\"confidence\":%.3f,"
           "\"threat_score\":%.1f,\"rssi\":%d}\n",
           (unsigned)source, confidence, threat_score, rssi);
    fflush(stdout);
    serial_output_unlock();
}
