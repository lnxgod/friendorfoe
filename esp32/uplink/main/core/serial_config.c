/**
 * Friend or Foe -- Serial Configuration Handler
 *
 * Dispatches complete command lines framed by badge_usb_transport and renders
 * each machine response into one bounded transport transaction.
 */

#include "serial_config.h"
#include "serial_config_ingress.h"
#include "badge_usb_transport.h"
#include "badge_usb_recovery.h"
#include "badge_usb_uplink_ota.h"
#include "uplink_usb_ota.h"
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
#include "scanner_command_producer_policy.h"
#include "badge_update_maintenance_policy.h"
#ifdef FOF_BADGE_VARIANT
#include "badge_runtime.h"
#include "badge_power_runtime.h"
#include "badge_display_policy_runtime.h"
#include "badge_theme_runtime.h"
#include "badge_ble_control.h"
#include "badge_ble_investigation.h"
#if defined(FOF_DC34_GAME_CANARY)
#include "badge_con_runtime.h"
#endif
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "serial_cfg";

#define CMD_PREFIX      "FOF_SET:"
#define CMD_CTL         "FOF_CTL:"
#define CMD_STATUS      "FOF_STATUS"
#define CMD_SAVE        "FOF_SAVE"
#define CMD_PING        "FOF_PING"
#define CMD_REBOOT      "FOF_REBOOT"
#define CMD_BOOTLOADER  "FOF_BOOTLOADER"
#define CMD_DOWNLOAD    "FOF_DOWNLOAD"
#define CMD_FLASH       "FOF_FLASH"
#define RESP_OK         "FOF_OK:"
#define RESP_SAVED      "FOF_SAVED\n"
#define RESP_ERROR      "FOF_ERROR:"
#define RESP_BOOTLOADER "FOF_BOOTLOADER:OK\n"
#define RESP_REBOOT     "FOF_REBOOT:OK\n"

#define BADGE_USB_STATUS_MAX_BYTES 65535
#define COMMAND_FRAME_BYTES 8192
#define LINE_PROJECTION_BYTES 2048
#define UPDATE_PREPARE_ORPHAN_POLL_MS 1500U
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
#define UPDATE_MAINTENANCE_STATUS_MAX_BYTES 4096
#endif

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} serial_render_t;

typedef struct {
    const char *seed;
    const char *state;
    bool active;
    uint8_t shield;
    uint8_t maximum;
    uint8_t scar;
    bool cured;
    bool dead;
    bool super;
} serial_game_status_t;

static serial_render_t s_render;
static char s_command_frame[COMMAND_FRAME_BYTES];
static char s_line_projection[LINE_PROJECTION_BYTES];
/* The status response already has a deep formatting frame. Keep its complete
 * scanner copies out of the 12 KB USB control stack. Calls are serialized by
 * the USB command path (boot config completes before the runtime task starts). */
static scanner_info_t s_usb_status_scanner_snapshots[2] = {0};

static void handle_control_line(
    const uint8_t *line,
    size_t line_byte_len,
    const serial_config_ingress_result_t *ingress);
static bool s_direct_emit_result;
static bool s_direct_emitted;
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
static const char *s_update_restart_reason;
static bool s_update_restart_owned;
static badge_runtime_expected_reboot_lease_t s_update_restart_lease;
static bool s_update_orphan_poll_started;
static uint32_t s_update_last_orphan_poll_ms;
static bool s_update_abort_pending;
static char s_update_abort_session[BADGE_UPDATE_SESSION_CAPACITY];
#endif
static void print_json_escaped_string(const char *value);
static void print_scanner_status_json(const char *name, uint8_t scanner_id,
                                      bool connected, bool peer_ready,
                                      const scanner_info_t *info,
                                      const scanner_uart_diag_t *uart_diag,
                                      bool first);

static void render_begin(char *data, size_t capacity)
{
    s_render.data = data;
    s_render.capacity = capacity;
    s_render.length = 0;
    s_render.overflow = !data || capacity == 0;
    if (data && capacity > 0) {
        data[0] = '\0';
    }
}

static int status_printf(const char *format, ...)
{
    if (s_render.overflow || !s_render.data ||
        s_render.length >= s_render.capacity) {
        s_render.overflow = true;
        return -1;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(s_render.data + s_render.length,
                            s_render.capacity - s_render.length,
                            format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= s_render.capacity - s_render.length) {
        s_render.overflow = true;
        s_render.data[s_render.capacity - 1] = '\0';
        return -1;
    }
    s_render.length += (size_t)written;
    return written;
}

static int status_putchar(int ch)
{
    char byte = (char)ch;
    return status_printf("%c", byte) < 0 ? EOF : ch;
}

static void serial_game_status_snapshot(serial_game_status_t *out)
{
    if (!out) {
        return;
    }
    *out = (serial_game_status_t) {
        .seed = "normal",
        .state = "normal",
        .active = false,
        .shield = 0U,
        .maximum = 100U,
        .scar = 0U,
        .cured = false,
        .dead = false,
        .super = false,
    };
#if defined(FOF_DC34_GAME_CANARY)
    badge_con_snapshot_t snapshot = {0};
    (void)badge_con_runtime_snapshot(&snapshot);
    const char *seed = badge_con_role_name(snapshot.seed);
    const char *state = badge_con_role_name(snapshot.role);
    if (seed && state) {
        out->seed = seed;
        out->state = state;
        out->active = snapshot.active;
        out->shield = snapshot.shield;
        out->maximum = snapshot.maximum;
        out->scar = snapshot.scar_level;
        out->cured = snapshot.cured;
        out->dead = snapshot.dead;
        out->super = snapshot.super;
    }
#endif
}

static void print_game_status_json(const serial_game_status_t *game)
{
#if defined(FOF_DC34_GAME_CANARY)
    if (!game) {
        return;
    }
    status_printf(",\"game_seed\":\"%s\",\"game_state\":\"%s\","
                  "\"game_active\":%s,\"game_shield\":%u,"
                  "\"game_max\":%u,\"game_scar\":%u,"
                  "\"game_cured\":%s,\"game_dead\":%s,"
                  "\"game_super\":%s",
                  game->seed, game->state,
                  game->active ? "true" : "false",
                  (unsigned)game->shield,
                  (unsigned)game->maximum,
                  (unsigned)game->scar,
                  game->cured ? "true" : "false",
                  game->dead ? "true" : "false",
                  game->super ? "true" : "false");
#else
    (void)game;
#endif
}

static bool render_emit(badge_usb_frame_priority_t priority,
                        TickType_t timeout)
{
    if (s_render.overflow) {
        /* Record a required response that could not be represented without
         * ever placing a partial machine frame on the wire. */
        if (priority == BADGE_USB_FRAME_REQUIRED) {
            (void)badge_usb_transport_emit(NULL, 0, priority, timeout);
        }
        return false;
    }
    if (s_render.length == 0) {
        return false;
    }
    bool emitted = badge_usb_transport_emit(s_render.data, s_render.length,
                                             priority, timeout);
    s_render.length = 0;
    if (s_render.data && s_render.capacity > 0) {
        s_render.data[0] = '\0';
    }
    return emitted;
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
    if (!newline || newline != &frame[len - 1]) {
        return false;
    }
    return badge_usb_transport_emit(frame, len, BADGE_USB_FRAME_OPTIONAL, 0);
}

static bool project_span(const uint8_t *bytes,
                         size_t byte_len,
                         const char **projected)
{
    if (projected) {
        *projected = NULL;
    }
    if (!bytes || !projected || byte_len >= sizeof(s_line_projection)) {
        return false;
    }
    memcpy(s_line_projection, bytes, byte_len);
    s_line_projection[byte_len] = '\0';
    *projected = s_line_projection;
    return true;
}

static void send_response(const char *msg)
{
    status_printf("%s", msg);
}

static void reboot_to_download_mode(void)
{
    ESP_LOGW(TAG, "USB serial requested ROM download mode");
    send_response(RESP_BOOTLOADER);
    if (!render_emit(BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(1000))) {
        return;
    }
    if (!badge_usb_recovery_restart(
            BADGE_USB_RESET_ROM, "usb_bootloader")) {
        ESP_LOGE(TAG,
                 "USB bootloader restart blocked without reboot ownership");
    }
}

static void reboot_app(void)
{
    ESP_LOGW(TAG, "USB serial requested app restart");
    send_response(RESP_REBOOT);
    if (!render_emit(BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(1000))) {
        return;
    }
    if (!badge_usb_recovery_restart(
            BADGE_USB_RESET_APP, "usb_reboot")) {
        ESP_LOGE(TAG, "USB app restart blocked without reboot ownership");
    }
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

    status_printf("%s{\"slot\":%u,\"uart\":", first ? "" : ",", (unsigned)scanner_id);
    print_json_escaped_string(name ? name : "?");
    status_printf(",\"connected\":%s,\"slot_role\":",
           connected ? "true" : "false");
    print_json_escaped_string(actual_role);
    status_printf(",\"expected_slot_role\":");
    print_json_escaped_string(expected_role);
    status_printf(",\"expected_scan_profile\":");
    print_json_escaped_string(expected);
    status_printf(",\"scan_profile\":");
    print_json_escaped_string(actual);
    status_printf(",\"role_acked\":%s,\"health\":", role_acked ? "true" : "false");
    print_json_escaped_string(health);
    status_printf(",\"uart_raw_seen\":%s,\"uart_raw_age_s\":%lld,"
           "\"uart_raw_bytes\":%lu,\"uart_line_overflow\":%lu,"
           "\"uart_json_err\":%lu",
           uart_diag && uart_diag->raw_seen ? "true" : "false",
           (long long)(uart_diag ? uart_diag->raw_age_s : -1),
           (unsigned long)(uart_diag ? uart_diag->raw_bytes : 0),
           (unsigned long)(uart_diag ? uart_diag->line_overflow_count : 0),
           (unsigned long)(uart_diag ? uart_diag->json_parse_error_count : 0));
    if (info) {
        status_printf(",\"ver\":");
        print_json_escaped_string(info->version);
        status_printf(",\"board\":");
        print_json_escaped_string(info->board);
        status_printf(",\"firmware_name\":");
        print_json_escaped_string(info->firmware_name);
        status_printf(",\"app_project\":");
        print_json_escaped_string(info->app_project);
        status_printf(",\"hardware_type\":");
        print_json_escaped_string(info->hardware_type);
        status_printf(",\"hardware_id\":");
        print_json_escaped_string(info->hardware_id);
        status_printf(",\"boot_id\":%lu,\"identity_generation\":%lu,"
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
        status_printf(",\"wifi_last_notable_ssid\":");
        print_json_escaped_string(info->wifi_last_notable_ssid);
        status_printf(",\"wifi_oui_emit\":%lu,"
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
        status_printf(",\"display_policy_hash\":%lu,"
               "\"display_policy_ack_hash\":%lu,"
               "\"filtered_counts\":{",
               (unsigned long)info->display_policy_hash,
               (unsigned long)info->display_policy_ack_hash);
        for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
            badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
            status_printf("%s", i == 0 ? "" : ",");
            print_json_escaped_string(badge_display_policy_class_key(cls));
            status_printf(":%lu", (unsigned long)info->display_policy_filtered[i]);
        }
        status_printf("}");
        status_printf(",\"ble_meta_last_reason\":");
        print_json_escaped_string(info->ble_meta_last_reason);
        status_printf(",\"ble_meta_identity\":");
        print_json_escaped_string(info->ble_meta_identity);
        status_printf(",\"ble_dbg_near_label\":");
        print_json_escaped_string(info->ble_dbg_near_label);
        status_printf(",\"ble_dbg_near_name\":");
        print_json_escaped_string(info->ble_dbg_near_name);
        status_printf(",\"ble_dbg_near_reason\":");
        print_json_escaped_string(info->ble_dbg_near_reason);
        status_printf(",\"ble_dbg_priv_label\":");
        print_json_escaped_string(info->ble_dbg_priv_label);
        status_printf(",\"ble_dbg_priv_name\":");
        print_json_escaped_string(info->ble_dbg_priv_name);
        status_printf(",\"ble_dbg_priv_reason\":");
        print_json_escaped_string(info->ble_dbg_priv_reason);
        status_printf(",\"ble_dbg_near_seen\":%lu,\"ble_dbg_near_rssi\":%d,"
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
        status_printf(",\"target_ver\":");
        print_json_escaped_string(info->fw_target_version);
        status_printf(",\"last_fw_error\":");
        print_json_escaped_string(info->last_fw_error);
        status_printf(",\"ota_state\":");
        print_json_escaped_string(info->ota_state[0] ? info->ota_state : "idle");
        status_printf(",\"ota_session_id\":");
        print_json_escaped_string(info->ota_session_id);
        status_printf(",\"ota_received\":%lu,\"ota_total\":%lu,"
               "\"recovery_mode\":",
               (unsigned long)info->ota_received,
               (unsigned long)info->ota_total);
        print_json_escaped_string(info->recovery_mode[0] ? info->recovery_mode : "normal");
        status_printf(",\"safe_reason\":");
        print_json_escaped_string(info->safe_reason);
        status_printf(",\"rollback_pending\":%s,\"crash_count\":%lu,"
               "\"radio_restart_count\":%lu",
               info->rollback_pending ? "true" : "false",
               (unsigned long)info->crash_count,
               (unsigned long)info->radio_restart_count);
    }
    status_printf("}");
}

#ifdef FOF_BADGE_VARIANT
static void print_display_policy_status_fields(
    const char *policy_json,
    uint32_t policy_hash,
    const uint32_t filtered_counts[BADGE_DISPLAY_POLICY_CLASS_COUNT])
{
    status_printf(",\"display_policy_hash\":%lu,\"display_policy\":%s,"
           "\"filtered_counts\":{",
           (unsigned long)policy_hash,
           policy_json && policy_json[0]
               ? policy_json
               : "{\"version\":1,\"classes\":{}}");
    for (int i = 0; i < BADGE_DISPLAY_POLICY_CLASS_COUNT; i++) {
        badge_display_policy_class_t cls = (badge_display_policy_class_t)i;
        status_printf("%s", i == 0 ? "" : ",");
        print_json_escaped_string(badge_display_policy_class_key(cls));
        status_printf(":%lu", (unsigned long)(filtered_counts
            ? filtered_counts[i]
            : 0));
    }
    status_printf("}");
}

static void print_badge_display_state_field(
    const oled_badge_display_state_t *captured_state,
    bool active)
{
    const oled_badge_display_state_t empty_state = {0};
    const oled_badge_display_state_t *state = captured_state
        ? captured_state
        : &empty_state;
    status_printf(",\"display_state\":{\"active\":%s,\"detail_mode\":%s,"
           "\"detail_page\":%d,\"investigation\":{\"active\":%s,"
           "\"request_id\":",
           active ? "true" : "false",
           state->detail_mode ? "true" : "false",
           state->detail_page,
           state->investigation_active ? "true" : "false");
    print_json_escaped_string(state->investigation_request_id);
    status_printf(",\"state\":");
    print_json_escaped_string(state->investigation_state);
    status_printf(",\"page\":%d},\"focus_index\":%d,\"focus_total\":%d,"
           "\"item_index\":%d,\"item_total\":%d,\"lane\":",
           state->investigation_page,
           state->focus_index,
           state->focus_total,
           state->item_index,
           state->item_total);
    print_json_escaped_string(state->lane);
    status_printf(",\"title\":");
    print_json_escaped_string(state->title);
    status_printf(",\"detail\":");
    print_json_escaped_string(state->detail);
    status_printf(",\"evidence\":");
    print_json_escaped_string(state->evidence);
    status_printf(",\"entity_key\":");
    print_json_escaped_string(state->entity_key);
    status_printf(",\"display_id\":");
    print_json_escaped_string(state->display_id);
    status_printf(",\"class\":");
    print_json_escaped_string(state->threat_class);
    status_printf(",\"category\":");
    print_json_escaped_string(state->category);
    status_printf(",\"code\":");
    print_json_escaped_string(state->code);
    status_printf(",\"source\":");
    print_json_escaped_string(state->source);
    if (state->ssid[0] != '\0') {
        status_printf(",\"ssid\":");
        print_json_escaped_string(state->ssid);
    }
    if (state->bssid[0] != '\0') {
        status_printf(",\"bssid\":");
        print_json_escaped_string(state->bssid);
    }
    status_printf(",\"auth_m\":%d,\"freq_mhz\":%d,"
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
        status_printf(",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.1f",
               state->latitude, state->longitude, state->altitude_m);
    }
    if (state->has_operator_location) {
        status_printf(",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
               state->operator_lat, state->operator_lon);
    }
    if (state->operator_id[0] != '\0') {
        status_printf(",\"operator_id\":");
        print_json_escaped_string(state->operator_id);
    }
    status_printf("}");
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
    status_printf(",\"buttons\":{\"b1_pin\":8,\"b1_active_high\":%s,"
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
    status_printf("}");
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

static const char *usb_parser_state_name(badge_usb_binary_target_t target)
{
    switch (target) {
        case BADGE_USB_BINARY_SCANNER: return "scanner_upload";
        case BADGE_USB_BINARY_UPLINK:  return "uplink_upload";
        case BADGE_USB_BINARY_NONE:
        default:                       return "command";
    }
}

static void status_nullable_age(const char *key, int64_t timestamp_ms,
                                int64_t status_now_ms)
{
    status_printf(",\"%s\":", key);
    if (timestamp_ms < 0) {
        status_printf("null");
    } else {
        int64_t age_s = status_now_ms >= timestamp_ms
            ? (status_now_ms - timestamp_ms) / 1000 : 0;
        status_printf("%lld", (long long)age_s);
    }
}

static void print_uplink_ota_status_json(
    const uplink_usb_ota_status_t *status)
{
    status_printf(",\"uplink_ota\":{\"state\":");
    print_json_escaped_string(uplink_usb_ota_state_name(status->state));
    status_printf(",\"partition\":");
    print_json_escaped_string(status->partition);
    status_printf(",\"received\":%lu,\"total\":%lu,\"target_version\":",
                  (unsigned long)status->received,
                  (unsigned long)status->total);
    print_json_escaped_string(status->target_version);
    status_printf(",\"last_error\":");
    print_json_escaped_string(status->last_error);
    status_printf("}");
}

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
static void print_update_maintenance_status_json(
    const uplink_usb_ota_status_t *live_status,
    bool include_update_campaign)
{
    char session[BADGE_UPDATE_SESSION_CAPACITY] = {0};
    badge_update_maintenance_marker_t marker = {0};
    fw_store_scanner_stage_status_t scanner_stage = {0};
    fw_auto_update_status_t campaign = {0};
    bool marker_valid =
        badge_runtime_update_marker_snapshot(&marker);
    bool scanner_stage_valid =
        fw_store_scanner_stage_status_snapshot(&scanner_stage);
    (void)badge_runtime_update_session_copy(session);

    const char *phase = "idle";
    const char *version = "";
    const char *sha256 = "";
    const char *partition = "";
    uint32_t size = 0U;
    uint32_t received = 0U;
    if (live_status &&
        (live_status->state == UPLINK_USB_OTA_RECEIVING ||
         live_status->state == UPLINK_USB_OTA_VERIFYING ||
         live_status->state == UPLINK_USB_OTA_COMMITTED) &&
        live_status->total > 0U) {
        phase = live_status->state == UPLINK_USB_OTA_COMMITTED
            ? "committed" : "receiving";
        version = live_status->target_version;
        sha256 = live_status->target_sha256;
        partition = live_status->partition;
        size = live_status->total;
        received = live_status->received;
    } else if (marker_valid && marker.uplink_committed) {
        phase = "committed";
        version = marker.uplink_version;
        sha256 = marker.uplink_sha256;
        partition = marker.uplink_partition;
        size = marker.uplink_size;
        received = marker.uplink_received;
    }

    status_printf(",\"update_session\":");
    print_json_escaped_string(session);
    status_printf(",\"ble_initialized\":false,\"update_uplink\":{\"phase\":");
    print_json_escaped_string(phase);
    status_printf(",\"session\":");
    print_json_escaped_string(session);
    status_printf(",\"version\":");
    print_json_escaped_string(version);
    status_printf(",\"sha256\":");
    print_json_escaped_string(sha256);
    status_printf(",\"size\":%lu,\"partition\":",
                  (unsigned long)size);
    print_json_escaped_string(partition);
    status_printf(",\"received\":%lu}", (unsigned long)received);

    const char *scanner_phase = "unknown";
    if (scanner_stage_valid) {
        switch (scanner_stage.phase) {
            case FW_SCANNER_STAGE_IDLE:
                scanner_phase = "idle";
                break;
            case FW_SCANNER_STAGE_RECEIVING:
                scanner_phase = "receiving";
                break;
            case FW_SCANNER_STAGE_COMMITTED:
                scanner_phase = "committed";
                break;
            default:
                scanner_stage_valid = false;
                break;
        }
    }
    if (!scanner_stage_valid) {
        memset(&scanner_stage, 0, sizeof(scanner_stage));
    }
    status_printf(",\"update_scanner\":{\"phase\":");
    print_json_escaped_string(scanner_phase);
    status_printf(",\"session\":");
    print_json_escaped_string(session);
    status_printf(",\"target\":");
    print_json_escaped_string(scanner_stage.target);
    status_printf(",\"sha256\":");
    print_json_escaped_string(scanner_stage.sha256);
    status_printf(
        ",\"size\":%lu,\"slot_mask\":%u,\"received\":%lu,"
        "\"generation\":%lu}",
        (unsigned long)scanner_stage.size,
        (unsigned)scanner_stage.slot_mask,
        (unsigned long)scanner_stage.received,
        (unsigned long)scanner_stage.generation);

    if (!include_update_campaign) {
        return;
    }
    fw_store_get_auto_update_status(&campaign);
    status_printf(
        ",\"update_campaign\":{\"generation\":%lu,"
        "\"target_slot_mask\":%u,\"pending_mask\":%u,"
        "\"worker_running\":%s,\"readiness_probes\":[%u,%u],"
        "\"scanners\":[",
        (unsigned long)campaign.generation,
        (unsigned)campaign.target_slot_mask,
        (unsigned)campaign.pending_mask,
        campaign.worker_running ? "true" : "false",
        (unsigned)campaign.readiness_probe_attempts[0],
        (unsigned)campaign.readiness_probe_attempts[1]);
    for (int scanner_id = 0; scanner_id < FW_AUTO_UPDATE_SCANNER_COUNT;
         ++scanner_id) {
        status_printf(
            "%s{\"slot\":%d,\"attempts\":%u,\"state\":",
            scanner_id == 0 ? "" : ",",
            scanner_id,
            (unsigned)campaign.attempts[scanner_id]);
        print_json_escaped_string(
            campaign.state[scanner_id][0] ? campaign.state[scanner_id] : "idle");
        status_printf("}");
    }
    status_printf("]}");
}

static void print_update_preparing_status_json(void)
{
    char session[BADGE_UPDATE_SESSION_CAPACITY] = {0};
    (void)badge_runtime_update_session_copy(session);
    status_printf(",\"update_session\":");
    print_json_escaped_string(session);
}
#endif

typedef struct {
    uint32_t stack_main_free;
    uint32_t stack_display_free;
    uint32_t stack_usb_free;
    uint32_t stack_uart_ble_free;
    uint32_t stack_uart_wifi_free;
    uint32_t heap_internal_free;
    uint32_t heap_internal_min_free;
    uint32_t heap_internal_largest;
    uint32_t detection_queue_capacity;
} serial_live_metrics_t;

static serial_live_metrics_t serial_live_metrics_snapshot(void)
{
    serial_live_metrics_t live_metrics = {
        .detection_queue_capacity = uart_rx_detection_queue_capacity(),
    };
#ifdef FOF_BADGE_VARIANT
    live_metrics.stack_main_free = badge_runtime_main_stack_free();
    live_metrics.stack_display_free = badge_runtime_display_stack_free();
    live_metrics.stack_usb_free = badge_runtime_usb_stack_free();
    live_metrics.stack_uart_ble_free = badge_runtime_uart_ble_stack_free();
    live_metrics.stack_uart_wifi_free = badge_runtime_uart_wifi_stack_free();
    live_metrics.heap_internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    live_metrics.heap_internal_min_free =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    live_metrics.heap_internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
#endif
    return live_metrics;
}

static void print_live_metrics_status(
    const serial_live_metrics_t *live_metrics)
{
    status_printf(",\"stack_main_free\":%lu,\"stack_display_free\":%lu,"
                  "\"stack_usb_free\":%lu,\"stack_uart_ble_free\":%lu,"
                  "\"stack_uart_wifi_free\":%lu,"
                  "\"heap_internal_free\":%lu,"
                  "\"heap_internal_min_free\":%lu,"
                  "\"heap_internal_largest\":%lu,"
                  "\"detection_queue_capacity\":%lu",
                  (unsigned long)live_metrics->stack_main_free,
                  (unsigned long)live_metrics->stack_display_free,
                  (unsigned long)live_metrics->stack_usb_free,
                  (unsigned long)live_metrics->stack_uart_ble_free,
                  (unsigned long)live_metrics->stack_uart_wifi_free,
                  (unsigned long)live_metrics->heap_internal_free,
                  (unsigned long)live_metrics->heap_internal_min_free,
                  (unsigned long)live_metrics->heap_internal_largest,
                  (unsigned long)live_metrics->detection_queue_capacity);
}

static bool emit_minimal_status(const char *hardware_id,
                                const char *running_partition,
                                const char *rollback_state,
                                const char *recovery_mode,
                                const badge_usb_health_t *health,
                                const uplink_usb_ota_status_t *uplink_ota,
                                int64_t status_now_ms,
                                const serial_game_status_t *game_status,
                                const serial_live_metrics_t *live_metrics)
{
#ifdef FOF_BADGE_VARIANT
    const char *last_expected_reboot_reason =
        badge_runtime_last_expected_reboot_reason();
#if defined(FOF_DC34_GAME_CANARY)
    const uint32_t last_expected_reboot_generation =
        badge_runtime_last_expected_reboot_generation();
#endif
#else
    const char *last_expected_reboot_reason = "";
#endif
    char minimal_status[2048];
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    char *status_storage = minimal_status;
    size_t status_capacity = sizeof(minimal_status);
    bool include_update_campaign = false;
    char *maintenance_status = NULL;
    if (badge_runtime_update_maintenance_active()) {
        maintenance_status = heap_caps_malloc(
            UPDATE_MAINTENANCE_STATUS_MAX_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (maintenance_status) {
            status_storage = maintenance_status;
            status_capacity = UPDATE_MAINTENANCE_STATUS_MAX_BYTES;
            include_update_campaign = true;
        }
    }
    render_begin(status_storage, status_capacity);
#else
    render_begin(minimal_status, sizeof(minimal_status));
#endif
    status_printf("FOF_STATUS:{\"version\":\"%s\",\"target\":\"%s\","
                  "\"firmware_name\":\"%s\",\"project\":\"%s\","
                  "\"app_project\":\"%s\",\"hardware_type\":\"%s\","
                  "\"hardware_id\":\"%s\",\"running_partition\":\"%s\","
                  "\"pending_verify\":%s,\"rollback_state\":\"%s\","
                  "\"recovery_mode\":\"%s\","
                  "\"detection_queue_reclaimed_bytes\":%lu,"
                  "\"last_expected_reboot_reason\":\"%s\",\"usb_health\":{"
                  "\"schema\":1,\"task_started\":%s,\"host_connected\":%s,"
                  "\"parser_state\":\"%s\",\"rx_bytes\":%llu,"
                  "\"valid_commands\":%lu,\"responses_completed\":%lu,"
                  "\"required_response_failures\":%lu,\"malformed_lines\":%lu,"
                  "\"dropped_progress_frames\":%lu,\"dropped_optional_frames\":%lu,"
                  "\"upload_received\":%lu,\"upload_size\":%lu",
                  FOF_VERSION, FOF_FIRMWARE_TARGET, FOF_FIRMWARE_TARGET,
                  FOF_APP_PROJECT, FOF_APP_PROJECT, FOF_HARDWARE_TYPE,
                  hardware_id, running_partition,
                  strcmp(rollback_state, "pending_verify") == 0 ? "true" : "false",
                  rollback_state, recovery_mode,
                  (unsigned long)uart_rx_detection_queue_reclaimed_bytes(),
                  last_expected_reboot_reason,
                  health->task_started ? "true" : "false",
                  health->host_connected ? "true" : "false",
                  usb_parser_state_name(health->parser_target),
                  (unsigned long long)health->rx_bytes,
                  (unsigned long)health->valid_commands,
                  (unsigned long)health->responses_completed,
                  (unsigned long)health->required_response_failures,
                  (unsigned long)health->malformed_lines,
                  (unsigned long)health->dropped_progress_frames,
                  (unsigned long)health->dropped_optional_frames,
                  (unsigned long)health->upload_received,
                  (unsigned long)health->upload_size);
    status_nullable_age("task_heartbeat_age_s", health->task_heartbeat_ms,
                        status_now_ms);
    status_nullable_age("last_rx_age_s", health->last_rx_ms, status_now_ms);
    status_nullable_age("last_command_age_s", health->last_command_ms,
                        status_now_ms);
    status_nullable_age("last_response_age_s", health->last_response_ms,
                        status_now_ms);
    status_nullable_age("last_upload_progress_age_s",
                        health->last_upload_progress_ms, status_now_ms);
    status_printf("}");
    print_live_metrics_status(live_metrics);
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    status_printf(",\"last_expected_reboot_generation\":%lu",
                  (unsigned long)last_expected_reboot_generation);
#endif
    print_uplink_ota_status_json(uplink_ota);
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (badge_runtime_update_maintenance_active()) {
        print_update_maintenance_status_json(
            uplink_ota, include_update_campaign);
    } else if (badge_runtime_update_preparing()) {
        print_update_preparing_status_json();
    }
#endif
    print_game_status_json(game_status);
    status_printf("}\n");
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    bool emitted = render_emit(
        BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(1000));
    if (maintenance_status) {
        heap_caps_free(maintenance_status);
    }
    return emitted;
#else
    return render_emit(BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(1000));
#endif
}

static void send_startup_recovery_status_response(void)
{
#ifdef FOF_BADGE_VARIANT
    badge_runtime_note_usb_stack_free(
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
#endif
    const int64_t status_now_ms = esp_timer_get_time() / 1000;
    uint8_t base_mac[6] = {0};
    char hardware_id[18] = "00:00:00:00:00:00";
    if (esp_efuse_mac_get_default(base_mac) == ESP_OK) {
        snprintf(hardware_id, sizeof(hardware_id),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 base_mac[0], base_mac[1], base_mac[2],
                 base_mac[3], base_mac[4], base_mac[5]);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *running_partition = running ? running->label : "unknown";
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    bool pending_verify = running &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    const char *rollback_state = pending_verify ? "pending_verify" : "clear";
    badge_usb_health_t usb_health = {0};
    badge_usb_transport_snapshot(&usb_health);
    uplink_usb_ota_status_t uplink_ota = {0};
    (void)uplink_usb_ota_get_status(&uplink_ota);
    serial_game_status_t game_status;
    serial_game_status_snapshot(&game_status);
    const serial_live_metrics_t live_metrics =
        serial_live_metrics_snapshot();

    serial_render_t previous_render = s_render;
    s_direct_emitted = true;
    s_direct_emit_result = emit_minimal_status(
        hardware_id, running_partition, rollback_state,
        "startup_dependency", &usb_health, &uplink_ota, status_now_ms,
        &game_status, &live_metrics);
    s_render = previous_render;
}

static void send_badge_status_response(void)
{
#ifdef FOF_BADGE_VARIANT
    badge_runtime_note_usb_stack_free(
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
#endif
    const int64_t status_now_ms = esp_timer_get_time() / 1000;
    uint8_t base_mac[6] = {0};
    char hardware_id[18] = "00:00:00:00:00:00";
    if (esp_efuse_mac_get_default(base_mac) == ESP_OK) {
        snprintf(hardware_id, sizeof(hardware_id),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 base_mac[0], base_mac[1], base_mac[2],
                 base_mac[3], base_mac[4], base_mac[5]);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *running_partition = running ? running->label : "unknown";
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    bool pending_verify = running &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    const char *rollback_state = pending_verify ? "pending_verify" : "clear";
#ifdef FOF_BADGE_VARIANT
    const char *status_recovery_mode = badge_runtime_recovery_mode();
    const char *status_expected_reboot_reason =
        badge_runtime_last_expected_reboot_reason();
#if defined(FOF_DC34_GAME_CANARY)
    const uint32_t status_expected_reboot_generation =
        badge_runtime_last_expected_reboot_generation();
#endif
#else
    const char *status_recovery_mode = "normal";
    const char *status_expected_reboot_reason = "";
#endif
    badge_usb_health_t usb_health = {0};
    badge_usb_transport_snapshot(&usb_health);
    uplink_usb_ota_status_t uplink_ota = {0};
    (void)uplink_usb_ota_get_status(&uplink_ota);
    serial_game_status_t game_status;
    serial_game_status_snapshot(&game_status);
    const serial_live_metrics_t live_metrics =
        serial_live_metrics_snapshot();
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (badge_runtime_update_maintenance_active()) {
        serial_render_t previous_render = s_render;
        s_direct_emitted = true;
        s_direct_emit_result = emit_minimal_status(
            hardware_id, running_partition, rollback_state,
            "update_maintenance", &usb_health, &uplink_ota,
            status_now_ms, &game_status, &live_metrics);
        s_render = previous_render;
        return;
    }
#endif
    serial_render_t previous_render = s_render;
    char *status_buffer = heap_caps_malloc(BADGE_USB_STATUS_MAX_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!status_buffer) {
        s_direct_emitted = true;
        s_direct_emit_result = emit_minimal_status(
            hardware_id, running_partition, rollback_state,
            status_recovery_mode, &usb_health, &uplink_ota, status_now_ms,
            &game_status, &live_metrics);
        s_render = previous_render;
        return;
    }
    render_begin(status_buffer, BADGE_USB_STATUS_MAX_BYTES);

    static badge_threat_snapshot_t snapshot;
    uart_rx_get_badge_threat_snapshot(&snapshot);

    badge_mode_t mode = badge_mode_get();
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
    const bool runtime_display_alive = badge_runtime_display_alive();
    const bool runtime_usb_control_alive = badge_runtime_usb_control_alive();
    const bool runtime_scanner_uart_alive = badge_runtime_scanner_uart_alive();
    const uint32_t runtime_reset_reason = badge_runtime_last_reset_reason();
    const char *runtime_reset_reason_name =
        badge_runtime_last_reset_reason_name();
    const bool runtime_reset_expected = badge_runtime_last_reset_expected();
    const int64_t runtime_usb_control_age_s =
        badge_runtime_usb_control_age_s();
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

    /* Stable machine fields: target/firmware_name, project/app_project,
     * hardware_type, immutable hardware_id, partition and rollback state. */
    status_printf("FOF_STATUS:{\"version\":");
    print_json_escaped_string(FOF_VERSION);
    status_printf(",\"target\":");
    print_json_escaped_string(FOF_FIRMWARE_TARGET);
    status_printf(",\"firmware_name\":");
    print_json_escaped_string(FOF_FIRMWARE_TARGET);
    status_printf(",\"project\":");
    print_json_escaped_string(FOF_APP_PROJECT);
    status_printf(",\"app_project\":");
    print_json_escaped_string(FOF_APP_PROJECT);
    status_printf(",\"hardware_type\":");
    print_json_escaped_string(FOF_HARDWARE_TYPE);
    status_printf(",\"hardware_id\":");
    print_json_escaped_string(hardware_id);
    status_printf(",\"running_partition\":");
    print_json_escaped_string(running_partition);
    status_printf(",\"pending_verify\":%s,\"rollback_state\":",
                  pending_verify ? "true" : "false");
    print_json_escaped_string(rollback_state);
    status_printf(",\"recovery_mode\":");
    print_json_escaped_string(status_recovery_mode);
    status_printf(",\"last_expected_reboot_reason\":");
    print_json_escaped_string(status_expected_reboot_reason);
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    status_printf(",\"last_expected_reboot_generation\":%lu",
                  (unsigned long)status_expected_reboot_generation);
#endif
    status_printf(",\"detection_queue_reclaimed_bytes\":%lu",
                  (unsigned long)uart_rx_detection_queue_reclaimed_bytes());
    status_printf(",\"usb_health\":{\"schema\":1,\"task_started\":%s,"
                  "\"host_connected\":%s,\"parser_state\":",
                  usb_health.task_started ? "true" : "false",
                  usb_health.host_connected ? "true" : "false");
    print_json_escaped_string(usb_parser_state_name(usb_health.parser_target));
    status_printf(",\"rx_bytes\":%llu,\"valid_commands\":%lu,"
                  "\"responses_completed\":%lu,"
                  "\"required_response_failures\":%lu,"
                  "\"malformed_lines\":%lu,"
                  "\"dropped_progress_frames\":%lu,"
                  "\"dropped_optional_frames\":%lu,"
                  "\"upload_received\":%lu,\"upload_size\":%lu",
                  (unsigned long long)usb_health.rx_bytes,
                  (unsigned long)usb_health.valid_commands,
                  (unsigned long)usb_health.responses_completed,
                  (unsigned long)usb_health.required_response_failures,
                  (unsigned long)usb_health.malformed_lines,
                  (unsigned long)usb_health.dropped_progress_frames,
                  (unsigned long)usb_health.dropped_optional_frames,
                  (unsigned long)usb_health.upload_received,
                  (unsigned long)usb_health.upload_size);
    status_nullable_age("task_heartbeat_age_s", usb_health.task_heartbeat_ms,
                        status_now_ms);
    status_nullable_age("last_rx_age_s", usb_health.last_rx_ms, status_now_ms);
    status_nullable_age("last_command_age_s", usb_health.last_command_ms,
                        status_now_ms);
    status_nullable_age("last_response_age_s", usb_health.last_response_ms,
                        status_now_ms);
    status_nullable_age("last_upload_progress_age_s",
                        usb_health.last_upload_progress_ms, status_now_ms);
    status_printf("}");
    print_uplink_ota_status_json(&uplink_ota);
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (badge_runtime_update_preparing()) {
        print_update_preparing_status_json();
    }
#endif
    status_printf(",\"uptime_s\":%lld", (long long)uptime_s);
    print_game_status_json(&game_status);
    status_printf(",\"mode\":");
    print_json_escaped_string(badge_mode_to_string(mode));
    status_printf(",\"mode_label\":");
    print_json_escaped_string(badge_mode_display_name(mode));
    bool ap_enabled =
#ifdef FOF_BADGE_VARIANT
        network_mode != BADGE_RUNTIME_NETWORK_OFF;
#else
        badge_mode_ap_enabled(mode);
#endif
    status_printf(",\"wifi_sta\":%s,\"ap_enabled\":%s,\"ap_ssid\":",
           wifi_sta_connected ? "true" : "false",
           ap_enabled ? "true" : "false");
    print_json_escaped_string(ap_ssid);
    status_printf(",\"ap_url\":\"http://192.168.4.1\"");
    status_printf(",\"firmware_store\":{\"stored\":%s",
           firmware_stored ? "true" : "false");
    if (firmware_stored) {
        status_printf(",\"target\":");
        print_json_escaped_string(firmware_store.name);
        status_printf(",\"app_project\":");
        print_json_escaped_string(firmware_store.project);
        status_printf(",\"hardware_type\":");
        print_json_escaped_string(firmware_store.hardware);
        status_printf(",\"version\":");
        print_json_escaped_string(firmware_store.version);
        status_printf(",\"sha256\":");
        print_json_escaped_string(firmware_store.sha256);
        status_printf(",\"size\":%lu,\"crc32\":%lu,\"generation\":%lu",
               (unsigned long)firmware_store.size,
               (unsigned long)firmware_store.checksum,
               (unsigned long)firmware_store.generation);
    }
    status_printf(",\"auto_update\":{\"worker_running\":%s,"
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
        status_printf("%s{\"slot\":%d,\"attempts\":%u,\"state\":",
               scanner_id == 0 ? "" : ",",
               scanner_id,
               (unsigned)auto_update.attempts[scanner_id]);
        print_json_escaped_string(auto_update.state[scanner_id]);
        status_printf("}");
    }
    status_printf("]}}");
#ifdef FOF_BADGE_VARIANT
    status_printf(",\"safe_mode\":%s,\"safe_reason\":",
           runtime_safe_mode ? "true" : "false");
    print_json_escaped_string(runtime_safe_reason);
    status_printf(",\"crash_count\":%lu,\"network_mode\":",
           (unsigned long)runtime_crash_count);
    print_json_escaped_string(
        badge_runtime_network_mode_name(network_mode));
    status_printf(",\"network_ttl_s\":%d,\"display_alive\":%s,"
           "\"usb_control_alive\":%s,\"scanner_uart_alive\":%s,"
           "\"reset_reason\":",
           network_ttl_s,
           runtime_display_alive ? "true" : "false",
           runtime_usb_control_alive ? "true" : "false",
           runtime_scanner_uart_alive ? "true" : "false");
    print_json_escaped_string(runtime_reset_reason_name);
    status_printf(",\"reset_reason_code\":%lu,\"reset_expected\":%s,"
           "\"usb_control_age_s\":%lld",
           (unsigned long)runtime_reset_reason,
           runtime_reset_expected ? "true" : "false",
           (long long)runtime_usb_control_age_s);
    status_printf(",\"power_mode\":");
    print_json_escaped_string(power_state.quiet ? "quiet" : "active");
    status_printf(",\"power_generation\":%lu,\"power_converged\":%s,"
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
        status_printf("%s{\"slot\":%d,\"connected\":%s,\"acked\":%s,"
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
    status_printf("]");
    print_live_metrics_status(&live_metrics);
    status_printf(",\"psram_total\":%lu,"
           "\"psram_free\":%lu,\"psram_largest\":%lu",
           (unsigned long)psram_total,
           (unsigned long)psram_free,
           (unsigned long)psram_largest);
#endif
    status_printf(",\"threat_score\":%.1f,\"color_rgb565\":%u",
           snapshot.threat_score, (unsigned)snapshot.color_rgb565);
    status_printf(",\"reporting\":{\"network_mode\":");
#ifdef FOF_BADGE_VARIANT
    print_json_escaped_string(
        badge_runtime_network_mode_name(network_mode));
    status_printf(",\"backend_enabled\":%s,\"network_ttl_s\":%d",
           network_mode == BADGE_RUNTIME_NETWORK_BACKEND ? "true" : "false",
           network_ttl_s);
#else
    print_json_escaped_string(badge_mode_to_string(mode));
    status_printf(",\"backend_enabled\":%s,\"network_ttl_s\":0",
           badge_mode_backend_enabled(mode) ? "true" : "false");
#endif
    status_printf(",\"http_task_alive\":%s,\"wifi_sta\":%s,\"standalone\":%s,"
           "\"uploads_ok\":%d,\"uploads_fail\":%d,"
           "\"last_upload_age_s\":%lld}",
           http_task_alive ? "true" : "false",
           wifi_sta_connected ? "true" : "false",
           wifi_sta_standalone ? "true" : "false",
           uploads_ok,
           uploads_fail,
           (long long)upload_age_s);
    status_printf(",\"dominant_class\":");
    print_json_escaped_string(badge_threat_class_name(snapshot.dominant_class));
    status_printf(",\"dominant_category\":");
    print_json_escaped_string(snapshot.entity_count > 0
        ? badge_threat_category_name(snapshot.entities[0].category)
        : badge_threat_category_name(BADGE_THREAT_CATEGORY_NONE));
    status_printf(",\"dominant_proximity\":%d", (int)snapshot.dominant_proximity);
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
    status_printf(",\"counts\":{\"drone\":%lu,\"remote_id\":%lu,"
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
    status_printf(",\"theme_hash\":%lu,\"theme\":",
           (unsigned long)theme_hash);
    status_printf("%s", theme_json[0] ? theme_json : "{\"version\":1}");
    print_badge_display_state_field(&display_state, display_state_active);
    status_printf(",\"ble_control\":%s", ble_status[0] ? ble_status : "{\"enabled\":false}");
    status_printf(",\"ble_investigation\":%s",
           investigation_status[0] ? investigation_status
                                   : "{\"request_id\":\"\",\"state\":\"idle\"}");
    print_badge_button_state_field(&button_state, status_now_ms);
#endif
    status_printf(",\"entities\":[");
    for (int i = 0; i < snapshot.entity_count; i++) {
        const badge_threat_snapshot_entity_t *entity = &snapshot.entities[i];
        status_printf("%s{\"label\":", i > 0 ? "," : "");
        print_json_escaped_string(entity->label);
        status_printf(",\"detail\":");
        print_json_escaped_string(entity->detail);
        status_printf(",\"evidence\":");
        print_json_escaped_string(entity->evidence);
        status_printf(",\"class\":");
        print_json_escaped_string(badge_threat_class_name(entity->cls));
        status_printf(",\"category\":");
        print_json_escaped_string(badge_threat_category_name(entity->category));
        status_printf(",\"code\":");
        print_json_escaped_string(badge_threat_category_code(entity->category));
        status_printf(",\"display_id\":");
        print_json_escaped_string(entity->display_id);
        status_printf(",\"source\":");
        print_json_escaped_string(badge_threat_source_code(entity->source));
        if (entity->ssid[0] != '\0') {
            status_printf(",\"ssid\":");
            print_json_escaped_string(entity->ssid);
        }
        if (entity->bssid[0] != '\0') {
            status_printf(",\"bssid\":");
            print_json_escaped_string(entity->bssid);
        }
        if (entity->wifi_auth_mode != 0xFF) {
            status_printf(",\"auth_m\":%d", (int)entity->wifi_auth_mode);
        }
        if (entity->freq_mhz > 0) {
            status_printf(",\"freq_mhz\":%d", (int)entity->freq_mhz);
        }
        status_printf(",\"source_id\":%u,"
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
            status_printf(",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.1f",
                   entity->latitude, entity->longitude, entity->altitude_m);
        }
        if (entity->has_operator_location) {
            status_printf(",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
                   entity->operator_lat, entity->operator_lon);
        }
        if (entity->operator_id[0] != '\0') {
            status_printf(",\"operator_id\":");
            print_json_escaped_string(entity->operator_id);
        }
        status_printf("}");
    }
    status_printf("],\"scanners\":[");
    print_scanner_status_json("ble", 0, ble_connected, wifi_connected,
                              ble_info, &ble_uart_diag, true);
#if CONFIG_DUAL_SCANNER
    print_scanner_status_json("wifi", 1, wifi_connected, ble_connected,
                              wifi_info, &wifi_uart_diag, false);
#endif
    status_printf("]}\n");
    bool overflow = s_render.overflow;
    s_direct_emitted = true;
    s_direct_emit_result = !overflow && render_emit(
        BADGE_USB_FRAME_REQUIRED, pdMS_TO_TICKS(2000));
    heap_caps_free(status_buffer);
    s_render = previous_render;
    if (overflow) {
        s_direct_emit_result = emit_minimal_status(
            hardware_id, running_partition, rollback_state,
            status_recovery_mode, &usb_health, &uplink_ota, status_now_ms,
            &game_status, &live_metrics);
        s_render = previous_render;
    }
}

static void send_control_ok(const char *message, bool reboot_required)
{
    status_printf("FOF_CTL_OK:{\"message\":");
    print_json_escaped_string(message ? message : "ok");
    status_printf(",\"reboot_required\":%s}\n", reboot_required ? "true" : "false");
}

static void send_control_error(const char *message)
{
    status_printf("FOF_CTL_ERROR:{\"error\":");
    print_json_escaped_string(message ? message : "unknown");
    status_printf("}\n");
}

#ifdef FOF_BADGE_VARIANT
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
#endif

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
#if defined(FOF_DC34_GAME_CANARY)
    if (!badge_runtime_update_maintenance_active()) {
        status_printf("FOF_FW_UPLOAD:{\"ok\":false,"
                      "\"error\":\"update_maintenance_required\"}\n");
        return;
    }
#endif
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
    const cJSON *flow_item = cJSON_GetObjectItemCaseSensitive(
        root, "flow_control");
    const cJSON *session_item = cJSON_GetObjectItemCaseSensitive(
        root, "session");
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : "";
    const char *target = cJSON_IsString(target_item) ? target_item->valuestring : "";
    const char *project = cJSON_IsString(project_item) ? project_item->valuestring : "";
    const char *hardware = cJSON_IsString(hardware_item) ? hardware_item->valuestring : "";
    const char *version = cJSON_IsString(version_item) ? version_item->valuestring : "";
    const char *sha256 = cJSON_IsString(sha_item) ? sha_item->valuestring : "";
    bool credit_v1 = flow_item != NULL;
    uint32_t size = 0;
    uint32_t crc32 = 0;
    uint32_t slot_mask = 0;
    if (!serial_json_uint32_exact(size_item, &size) ||
        !serial_json_uint32_exact(crc_item, &crc32) ||
        !serial_json_uint32_exact(slot_mask_item, &slot_mask) ||
        (flow_item &&
         (!cJSON_IsString(flow_item) ||
          strcmp(flow_item->valuestring, "credit-v1") != 0)) ||
        slot_mask == 0 || slot_mask > 0x3 ||
        strcmp(name, "scanner-s3-combo-fof_badge") != 0 ||
        strcmp(target, "scanner-s3-combo-fof_badge") != 0 ||
        strcmp(project, "fof_badge_scanner") != 0 ||
        strcmp(hardware, "seeed_xiao_esp32s3") != 0 ||
        !version[0] || !fof_firmware_sha256_hex_is_valid(sha256)) {
        status_printf("FOF_FW_UPLOAD:{\"ok\":false,\"error\":\"invalid_manifest\"}\n");
        return;
    }
    bool session_is_string = cJSON_IsString(session_item);
    bool session_matches = false;
#if defined(FOF_DC34_GAME_CANARY)
    const bool canary_build = true;
    const bool maintenance_active =
        badge_runtime_update_maintenance_active();
    if (maintenance_active && session_is_string) {
        session_matches = badge_runtime_update_session_matches(
            session_item->valuestring);
    }
#else
    const bool canary_build = false;
    const bool maintenance_active = false;
#endif
    badge_update_ota_begin_admission_t admission =
        badge_update_scanner_stage_begin_admission_decide(
            canary_build,
            maintenance_active,
            (size_t)cJSON_GetArraySize(root),
            session_item != NULL,
            session_is_string,
            session_matches);
    if (admission != BADGE_UPDATE_OTA_BEGIN_ADMIT) {
        const char *error = admission ==
                BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH
            ? "update_session_mismatch"
            : "unexpected_update_session";
        status_printf(
            "FOF_FW_UPLOAD:{\"ok\":false,\"error\":\"%s\"}\n",
            error);
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
        credit_v1,
        resp,
        sizeof(resp)
    );
    if (ok) {
        if (!badge_usb_transport_begin_scanner_binary(size, credit_v1)) {
            fw_store_serial_upload_abort("usb_parser_busy");
            status_printf("FOF_FW_UPLOAD:{\"ok\":false,"
                          "\"error\":\"usb_parser_busy\"}\n");
            return;
        }
    }
    status_printf("FOF_FW_UPLOAD:%s\n", resp);
#else
    status_printf("FOF_FW_UPLOAD:{\"ok\":false,\"error\":\"badge_only\"}\n");
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
    char payload[FOF_SCANNER_PRODUCER_JSON_CAPACITY] = {0};
    bool payload_ok = false;
    if (strcmp(cmd_name, "scanner_trigger") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (cJSON_IsBool(enabled)) {
            payload_ok = fof_scanner_display_button_command_json(
                cJSON_IsTrue(enabled), payload, sizeof(payload));
        }
    } else if (strcmp(cmd_name, "scanner_display") == 0) {
        const cJSON *button_item =
            cJSON_GetObjectItemCaseSensitive(root, "button_enabled");
        if (!button_item) {
            button_item =
                cJSON_GetObjectItemCaseSensitive(root, "trigger_enabled");
        }
        if (!button_item) {
            button_item =
                cJSON_GetObjectItemCaseSensitive(root, "boot_enabled");
        }
        const cJSON *view =
            cJSON_GetObjectItemCaseSensitive(root, "view");
        const cJSON *page =
            cJSON_GetObjectItemCaseSensitive(root, "page");
        const cJSON *page_lock =
            cJSON_GetObjectItemCaseSensitive(root, "page_lock");
        const cJSON *auto_page =
            cJSON_GetObjectItemCaseSensitive(root, "auto_page");
        if (cJSON_IsBool(button_item) &&
            cJSON_IsString(view) &&
            view->valuestring &&
            cJSON_IsNumber(page) &&
            page->valuedouble == (double)page->valueint &&
            cJSON_IsBool(page_lock) &&
            cJSON_IsBool(auto_page)) {
            payload_ok = fof_scanner_display_full_command_json(
                cJSON_IsTrue(button_item),
                view->valuestring,
                page->valueint,
                cJSON_IsTrue(page_lock),
                cJSON_IsTrue(auto_page),
                payload,
                sizeof(payload));
        }
    }
    if (!payload_ok) {
        send_control_error("invalid scanner display control");
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
        send_control_error("uart must be ble, wifi, or all");
        return;
    }

    status_printf("FOF_CTL_OK:{\"message\":\"scanner display command sent\","
           "\"ble_sent\":%s,\"wifi_sent\":%s,\"reboot_required\":false}\n",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false");
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

    status_printf("FOF_CTL_OK:{\"message\":\"scanner safe mode command sent\","
           "\"ble_sent\":%s,\"wifi_sent\":%s,\"enabled\":%s,"
           "\"reboot_required\":true}\n",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false",
           enabled ? "true" : "false");
#else
    (void)root;
    send_control_error("scanner safe mode is badge-only");
#endif
}

#ifdef FOF_BADGE_VARIANT
static void send_network_ok(const char *message, bool applied)
{
    status_printf("FOF_CTL_OK:{\"message\":");
    print_json_escaped_string(message ? message : "network");
    status_printf(",\"network_mode\":");
    print_json_escaped_string(
        badge_runtime_network_mode_name(badge_runtime_get_network_mode()));
    status_printf(",\"network_ttl_s\":%d,\"applied\":%s,\"reboot_required\":false}\n",
           badge_runtime_get_network_ttl_s(),
           applied ? "true" : "false");
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
    status_printf("FOF_CTL_OK:{\"message\":\"display policy updated\","
           "\"display_policy_hash\":%lu,\"persisted\":%s,"
           "\"ble_sent\":%s,\"wifi_sent\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_display_policy_runtime_hash(),
           persist ? "true" : "false",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false");
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
    status_printf("FOF_CTL_OK:{\"message\":\"display policy reset\","
           "\"display_policy_hash\":%lu,\"persisted\":%s,"
           "\"ble_sent\":%s,\"wifi_sent\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_display_policy_runtime_hash(),
           persist ? "true" : "false",
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false");
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

    status_printf("FOF_CTL_OK:{\"message\":\"badge theme updated\","
           "\"theme_hash\":%lu,\"persisted\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_theme_runtime_hash(),
           persist ? "true" : "false");
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
    status_printf("FOF_CTL_OK:{\"message\":\"badge theme reset\","
           "\"theme_hash\":%lu,\"persisted\":%s,"
           "\"reboot_required\":false}\n",
           (unsigned long)badge_theme_runtime_hash(),
           persist ? "true" : "false");
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
    status_printf("FOF_CTL_%s:{\"message\":\"firmware check requested\","
           "\"uart\":\"%s\",\"ble_sent\":%s,\"wifi_sent\":%s,"
           "\"deferred\":%s,\"error\":\"%s\"}\n",
           ok ? "OK" : "ERROR",
           uart,
           ble_sent ? "true" : "false",
           wifi_sent ? "true" : "false",
           deferred ? "true" : "false",
           deferred ? "firmware_operation_active" :
               (ok ? "" : "scanner_command_ingress_unreachable"));
}

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
static void render_update_mode_conflict(const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":false,\"phase\":\"busy\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":false,\"error\":\"session_conflict\"}\n",
        session);
}

static void render_update_mode_active(const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"active\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":false}\n",
        session);
}

static void render_update_mode_campaign_busy(const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":false,\"phase\":\"busy\","
        "\"session\":\"%s\",\"retryable\":true,"
        "\"reboot_required\":false,"
        "\"error\":\"campaign_state_busy\"}\n",
        session);
}

static void render_update_mode_success_gates_pending(
    const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":false,\"phase\":\"busy\","
        "\"session\":\"%s\",\"retryable\":true,"
        "\"reboot_required\":false,"
        "\"error\":\"success_gates_pending\"}\n",
        session);
}

static void render_update_mode_operation_busy(const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":false,\"phase\":\"busy\","
        "\"session\":\"%s\",\"retryable\":true,"
        "\"reboot_required\":false,"
        "\"error\":\"firmware_operation_active\"}\n",
        session);
}

static void render_update_mode_finishing(const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"finishing\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":true}\n",
        session);
}

static void render_update_mode_aborting(const char *session)
{
    status_printf(
        "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"aborting\","
        "\"session\":\"%s\",\"retryable\":false,"
        "\"reboot_required\":true}\n",
        session);
}

static void render_update_mode_preemption(
    const char *session, fw_update_preempt_result_t result)
{
    if (result == FW_UPDATE_PREEMPT_QUIESCED ||
        result == FW_UPDATE_PREEMPT_REBOOT_SAFE) {
        status_printf(
            "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"rebooting\","
            "\"session\":\"%s\",\"retryable\":true,"
            "\"reboot_required\":true}\n",
            session);
        s_update_restart_owned = false;
        memset(&s_update_restart_lease, 0, sizeof(s_update_restart_lease));
        s_update_restart_reason = "update_maintenance";
    } else if (result == FW_UPDATE_PREEMPT_WAITING_FOR_OWNER) {
        status_printf(
            "FOF_UPDATE_MODE:{\"ok\":false,"
            "\"phase\":\"waiting_for_owner\",\"session\":\"%s\","
            "\"retryable\":true,\"reboot_required\":false,"
            "\"error\":\"firmware_operation_active\"}\n",
            session);
    } else {
        render_update_mode_campaign_busy(session);
    }
}

static void handle_update_mode_command(
    cJSON *root, badge_usb_control_schema_id_t control_schema_id)
{
    const cJSON *session_item =
        cJSON_GetObjectItemCaseSensitive(root, "session");
    if (!cJSON_IsString(session_item) || !session_item->valuestring ||
        strlen(session_item->valuestring) != BADGE_UPDATE_SESSION_LENGTH) {
        send_control_error("invalid update session");
        return;
    }
    const char *session = session_item->valuestring;

    char owned_session[BADGE_UPDATE_SESSION_CAPACITY] = {0};
    bool marker_present =
        badge_runtime_update_session_copy(owned_session);
    if (marker_present && strcmp(owned_session, session) != 0) {
        render_update_mode_conflict(session);
        return;
    }

    if (control_schema_id == BADGE_USB_CONTROL_SCHEMA_PREPARE_UPDATE) {
        if (s_update_abort_pending) {
            render_update_mode_operation_busy(session);
            return;
        }
        if (badge_runtime_update_maintenance_active()) {
            badge_runtime_update_keepalive(
                (uint32_t)(esp_timer_get_time() / 1000));
            render_update_mode_active(session);
            return;
        }

        /* badge_runtime_prepare_update() creates/persists PREPARING before
         * the first call below can latch update preemption or touch radio
         * policy. */
        if (!badge_runtime_prepare_update(session)) {
            send_control_error("update maintenance prepare failed");
            return;
        }
        s_update_orphan_poll_started = false;
        s_update_last_orphan_poll_ms = 0U;
        render_update_mode_preemption(
            session, fw_store_request_update_preemption());
        return;
    }

    if (control_schema_id == BADGE_USB_CONTROL_SCHEMA_ABORT_UPDATE &&
        marker_present &&
        badge_runtime_update_preparing()) {
        if (!badge_runtime_update_session_matches(session)) {
            render_update_mode_conflict(session);
            return;
        }
        s_update_abort_pending = true;
        memcpy(
            s_update_abort_session,
            session,
            BADGE_UPDATE_SESSION_CAPACITY);
        s_update_orphan_poll_started = false;
        s_update_last_orphan_poll_ms = 0U;
        /*
         * PREPARING owns a durable preemption latch. Keep polling that exact
         * safe point even if the host leaves; the terminal abort receipt is
         * emitted only after reboot ownership is secured.
         */
        render_update_mode_operation_busy(session);
        return;
    }

    if (!marker_present ||
        !badge_runtime_update_maintenance_active()) {
        render_update_mode_conflict(session);
        return;
    }

    badge_runtime_update_keepalive(
        (uint32_t)(esp_timer_get_time() / 1000));

    if (control_schema_id == BADGE_USB_CONTROL_SCHEMA_FINISH_UPDATE) {
        fw_store_campaign_completion_t completion =
            fw_store_campaign_completion_sample();
        if (badge_runtime_pending_verify() ||
            completion != FW_CAMPAIGN_COMPLETION_SUCCESS) {
            render_update_mode_success_gates_pending(session);
            return;
        }
        badge_usb_firmware_restart_prepare_result_t prepare_result =
            badge_usb_recovery_prepare_firmware_restart(
                "update_finish",
                BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
                &s_update_restart_lease);
        if (prepare_result ==
            BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY) {
            render_update_mode_operation_busy(session);
            return;
        }
        if (prepare_result !=
            BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
            send_control_error("update restart preparation failed");
            return;
        }
        s_update_restart_owned = true;
        (void)badge_runtime_clear_update_maintenance("host_finish");
        s_update_restart_reason = "update_finish";
        render_update_mode_finishing(session);
        return;
    }

    if (control_schema_id == BADGE_USB_CONTROL_SCHEMA_ABORT_UPDATE) {
        badge_usb_firmware_restart_prepare_result_t prepare_result =
            badge_usb_recovery_prepare_firmware_restart(
                "update_abort",
                BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
                &s_update_restart_lease);
        if (prepare_result ==
            BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY) {
            render_update_mode_operation_busy(session);
            return;
        }
        if (prepare_result !=
            BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED) {
            send_control_error("update restart preparation failed");
            return;
        }
        s_update_restart_owned = true;
        if (!badge_runtime_abort_update_session(
                session, "host_abort")) {
            /*
             * Firmware restart ownership is irreversible at this point.
             * Keep the owned restart path intact; the next boot rejects and
             * clears any non-armed maintenance marker.
             */
            ESP_LOGE(
                TAG,
                "ACTIVE abort lost exact session after restart ownership");
        }
        s_update_restart_reason = "update_abort";
        render_update_mode_aborting(session);
        return;
    }

    send_control_error("unauthorized update lifecycle command");
}

void serial_config_poll_update_preparation(uint32_t now_ms)
{
    if (!badge_runtime_update_preparing()) {
        s_update_orphan_poll_started = false;
        s_update_last_orphan_poll_ms = 0U;
        s_update_abort_pending = false;
        memset(
            s_update_abort_session,
            0,
            sizeof(s_update_abort_session));
        return;
    }
    if (!badge_runtime_update_prepare_orphan_due(now_ms) ||
        (s_update_orphan_poll_started &&
         (uint32_t)(now_ms - s_update_last_orphan_poll_ms) <
             UPDATE_PREPARE_ORPHAN_POLL_MS)) {
        return;
    }
    s_update_orphan_poll_started = true;
    s_update_last_orphan_poll_ms = now_ms;

    fw_update_preempt_result_t result =
        fw_store_request_update_preemption();
    bool preemption_safe =
        result == FW_UPDATE_PREEMPT_QUIESCED ||
        result == FW_UPDATE_PREEMPT_REBOOT_SAFE;

    if (s_update_abort_pending) {
        badge_update_maintenance_marker_t marker = {0};
        if (!badge_runtime_update_marker_snapshot(&marker)) {
            s_update_abort_pending = false;
            memset(
                s_update_abort_session,
                0,
                sizeof(s_update_abort_session));
            return;
        }
        badge_update_abort_action_t action =
            badge_update_preparing_abort_decide(
                &marker,
                s_update_abort_session,
                strnlen(
                    s_update_abort_session,
                    sizeof(s_update_abort_session)),
                preemption_safe,
                false);
        if (action == BADGE_UPDATE_ABORT_CANCEL) {
            s_update_abort_pending = false;
            memset(
                s_update_abort_session,
                0,
                sizeof(s_update_abort_session));
            return;
        }
        if (!preemption_safe) {
            return;
        }

        badge_runtime_expected_reboot_lease_t reboot_lease = {0};
        badge_runtime_expected_reboot_arm_result_t arm_result =
            badge_runtime_arm_expected_reboot(
                "update_abort",
                BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
                &reboot_lease);
        action = badge_update_preparing_abort_decide(
            &marker,
            s_update_abort_session,
            strnlen(
                s_update_abort_session,
                sizeof(s_update_abort_session)),
            true,
            arm_result ==
                BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED);
        if (action != BADGE_UPDATE_ABORT_CLEAR_AND_REBOOT) {
            return;
        }
        if (!badge_runtime_abort_update_session(
                s_update_abort_session, "host_abort_preparing")) {
            ESP_LOGE(
                TAG,
                "PREPARING abort lost exact session after reboot ownership");
            bool released =
                badge_runtime_release_expected_reboot(&reboot_lease);
            if (!released) {
                if (badge_runtime_expected_reboot_lease_is_owned(
                        &reboot_lease)) {
                    badge_usb_recovery_restart_with_owned_lease(
                        BADGE_USB_RESET_APP,
                        "update_abort",
                        &reboot_lease);
                }
                ESP_LOGE(
                    TAG,
                    "PREPARING abort could not prove reboot lease release; "
                    "fail-stopping");
                for (;;) {
                    vTaskDelay(portMAX_DELAY);
                }
            }
            return;
        }

        char frame[256];
        int frame_length = snprintf(
            frame, sizeof(frame),
            "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"aborting\","
            "\"session\":\"%s\",\"retryable\":false,"
            "\"reboot_required\":true}\n",
            s_update_abort_session);
        if (frame_length > 0 && (size_t)frame_length < sizeof(frame)) {
            (void)badge_usb_transport_emit(
                frame, (size_t)frame_length, BADGE_USB_FRAME_REQUIRED,
                pdMS_TO_TICKS(1000));
        } else {
            (void)badge_usb_transport_emit(
                NULL, 0U, BADGE_USB_FRAME_REQUIRED,
                pdMS_TO_TICKS(1000));
        }
        (void)badge_usb_transport_drain(pdMS_TO_TICKS(1000));
        badge_usb_recovery_restart_with_owned_lease(
            BADGE_USB_RESET_APP,
            "update_abort",
            &reboot_lease);
    }

    if (!preemption_safe) {
        return;
    }

    char session[BADGE_UPDATE_SESSION_CAPACITY] = {0};
    if (!badge_runtime_update_session_copy(session)) {
        return;
    }
    char frame[256];
    int frame_length = snprintf(
        frame, sizeof(frame),
        "FOF_UPDATE_MODE:{\"ok\":true,\"phase\":\"rebooting\","
        "\"session\":\"%s\",\"retryable\":true,"
        "\"reboot_required\":true}\n",
        session);
    if (frame_length > 0 && (size_t)frame_length < sizeof(frame)) {
        (void)badge_usb_transport_emit(
            frame, (size_t)frame_length, BADGE_USB_FRAME_REQUIRED,
            pdMS_TO_TICKS(1000));
    } else {
        (void)badge_usb_transport_emit(
            NULL, 0U, BADGE_USB_FRAME_REQUIRED,
            pdMS_TO_TICKS(1000));
    }
    (void)badge_usb_transport_drain(pdMS_TO_TICKS(1000));
    if (!badge_usb_recovery_restart(
            BADGE_USB_RESET_APP, "update_maintenance")) {
        ESP_LOGE(
            TAG,
            "Update-maintenance restart blocked without reboot ownership");
    }
}
#endif

static void handle_ctl_command(
    const uint8_t *json,
    size_t json_byte_len,
    badge_usb_control_schema_id_t control_schema_id,
    badge_usb_control_handler_kind_t control_handler_kind)
{
    if (control_schema_id != BADGE_USB_CONTROL_SCHEMA_NONE) {
        const badge_usb_control_schema_descriptor_t *descriptor =
            badge_usb_control_schema_descriptor(control_schema_id);
        if (!descriptor ||
            descriptor->handler_kind != control_handler_kind) {
            send_control_error("unauthorized control route");
            return;
        }
    } else if (control_handler_kind != BADGE_USB_CONTROL_HANDLER_NONE) {
        send_control_error("unauthorized control route");
        return;
    }

    const char *projected = NULL;
    if (!project_span(json, json_byte_len, &projected)) {
        send_control_error("invalid json");
        return;
    }
    cJSON *root = cJSON_ParseWithLengthOpts(
        projected, json_byte_len + 1U, NULL, true);
    if (!root) {
        send_control_error("invalid json");
        return;
    }

    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";

    if (control_handler_kind == BADGE_USB_CONTROL_HANDLER_STATUS) {
        send_badge_status_response();
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_POWER_MODE) {
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
                status_printf("FOF_CTL_OK:{\"message\":\"power mode updated\","
                       "\"power_mode\":\"%s\",\"power_generation\":%lu,"
                       "\"reboot_required\":false}\n",
                       state.quiet ? "quiet" : "active",
                       (unsigned long)state.generation);
            }
        }
#else
        send_control_error("power mode is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_SET_MODE) {
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
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_SET_BACKEND) {
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
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_DISPLAY_DEBUG) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        nvs_config_set_string("badge_display_debug",
                              (cJSON_IsBool(enabled) && cJSON_IsTrue(enabled)) ? "1" : "0");
        send_control_ok("display debug saved", false);
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_NETWORK) {
#ifdef FOF_BADGE_VARIANT
        handle_network_command(root);
#else
        send_control_error("network sessions are badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_SAFE_MODE) {
#ifdef FOF_BADGE_VARIANT
        handle_safe_mode_command(root);
#else
        send_control_error("safe mode is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_BLE_INVESTIGATE) {
#ifdef FOF_BADGE_VARIANT
        ble_investigation_request_t request = {0};
        char err[64] = {0};
        const char *mode = NULL;
        if (!badge_usb_control_decode_ble_investigate(
                json, json_byte_len, &request) ||
            !(mode = ble_investigation_mode_name(request.mode)) ||
            !badge_ble_investigation_start_with_timeout(
                request.request_id,
                mode,
                request.target_mac,
                request.timeout_ms,
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
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_BLE_CHUNK) {
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
            status_printf("%s", frame);
        }
#else
        send_control_error("BLE investigation is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY) {
#ifdef FOF_BADGE_VARIANT
        handle_display_policy_command(root);
#else
        send_control_error("badge display policy is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY_RESET) {
#ifdef FOF_BADGE_VARIANT
        handle_display_policy_reset_command(root);
#else
        send_control_error("badge display policy is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_THEME) {
#ifdef FOF_BADGE_VARIANT
        handle_badge_theme_command(root);
#else
        send_control_error("badge theme is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_THEME_RESET) {
#ifdef FOF_BADGE_VARIANT
        handle_badge_theme_reset_command(root);
#else
        send_control_error("badge theme is badge-only");
#endif
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_DISPLAY_NAV) {
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
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_SCANNER_DISPLAY) {
        handle_scanner_display_control(root, "scanner_display");
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_SCANNER_TRIGGER) {
        handle_scanner_display_control(root, "scanner_trigger");
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_SCANNER_SAFE_MODE) {
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
            const cJSON *generation_item =
                cJSON_GetObjectItemCaseSensitive(
                    root, "expected_generation");
            const cJSON *hardware_id_item =
                cJSON_GetObjectItemCaseSensitive(
                    root, "expected_hardware_id");
            uint32_t expected_generation = 0;
            if (!serial_json_uint32_exact(
                    generation_item, &expected_generation) ||
                expected_generation == 0U ||
                !cJSON_IsString(hardware_id_item) ||
                !hardware_id_item->valuestring ||
                hardware_id_item->valuestring[0] == '\0') {
                status_printf(
                    "FOF_FW_RELAY:{\"ok\":false,"
                    "\"error\":\"bound_request_required\"}\n");
                cJSON_Delete(root);
                return;
            }
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
            fw_store_relay_staged_to_scanner_bound(
                scanner_id, expected_generation,
                hardware_id_item->valuestring,
                force, allow_same, resp, sizeof(resp));
            status_printf("FOF_FW_RELAY:%s\n", resp);
        }
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_REBOOT) {
        cJSON_Delete(root);
        reboot_app();
        return;
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    } else if (control_handler_kind ==
               BADGE_USB_CONTROL_HANDLER_UPDATE_MODE) {
        handle_update_mode_command(root, control_schema_id);
#endif
    } else if (strcmp(cmd, "rollback") == 0) {
#ifdef FOF_BADGE_VARIANT
        if (!badge_runtime_pending_verify()) {
            send_control_error("no pending OTA image");
        } else {
            send_response("FOF_CTL_OK:{\"message\":\"rollback\",\"reboot_required\":true}\n");
            if (!render_emit(BADGE_USB_FRAME_REQUIRED,
                             pdMS_TO_TICKS(1000))) {
                cJSON_Delete(root);
                return;
            }
#ifdef FOF_BADGE_VARIANT
            badge_runtime_expected_reboot_lease_t rollback_lease = {0};
            badge_runtime_expected_reboot_arm_result_t arm_result =
                badge_runtime_arm_expected_reboot(
                    "usb_rollback",
                    BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK,
                    &rollback_lease);
            if (arm_result !=
                BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED) {
                send_control_error(
                    arm_result ==
                            BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY
                        ? "rollback_busy"
                        : "rollback_arm_failed");
                cJSON_Delete(root);
                return;
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_ota_mark_app_invalid_rollback_and_reboot();
            /* This API should not return. If it does, only the exact owner
             * may clear the token before the failure is reported. */
            if (!badge_runtime_release_expected_reboot(&rollback_lease)) {
                ESP_LOGE(TAG, "USB rollback lost expected-reboot ownership");
            }
            send_control_error("rollback_failed");
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

static bool handle_set_command(const uint8_t *line, size_t line_byte_len)
{
    serial_config_set_parts_t parts;
    if (!serial_config_ingress_parse_set(line, line_byte_len, &parts)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%smalformed command\n", RESP_ERROR);
        send_response(msg);
        return false;
    }

    char key[32] = {0};
    memcpy(key, parts.key, parts.key_len);
    key[parts.key_len] = '\0';

    const char *value = NULL;
    if (!project_span(parts.value, parts.value_len, &value)) {
        send_response(RESP_ERROR "malformed command\n");
        return false;
    }

#if defined(FOF_DC34_GAME_CANARY)
    if (strcmp(key, "game_seed") == 0) {
        badge_con_role_t seed;
        if (!badge_con_role_parse_exact(value, &seed) ||
            !badge_con_runtime_set_factory_seed(seed)) {
            send_response(RESP_ERROR "game_seed update failed\n");
            return false;
        }
        send_response("FOF_OK:game_seed\n");
        return true;
    }
#endif

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

static void handle_control_line(
    const uint8_t *line,
    size_t line_byte_len,
    const serial_config_ingress_result_t *ingress)
{
    if (!ingress) {
        return;
    }
#ifdef FOF_BADGE_VARIANT
    badge_runtime_note_usb_control_alive();
#endif
    switch (ingress->kind) {
        case SERIAL_CONFIG_INGRESS_PING: {
            char msg[48];
            snprintf(msg, sizeof(msg), "FOF_PONG:%s\n", FOF_VERSION);
            send_response(msg);
            break;
        }
        case SERIAL_CONFIG_INGRESS_STATUS:
            send_badge_status_response();
            break;
        case SERIAL_CONFIG_INGRESS_SAVE:
            send_response(RESP_SAVED);
            break;
        case SERIAL_CONFIG_INGRESS_REBOOT:
            reboot_app();
            break;
        case SERIAL_CONFIG_INGRESS_SET:
            (void)handle_set_command(line, line_byte_len);
            break;
        case SERIAL_CONFIG_INGRESS_CTL_COMPAT:
            handle_ctl_command(
                line + (sizeof(CMD_CTL) - 1U),
                line_byte_len - (sizeof(CMD_CTL) - 1U),
                ingress->control_schema_id,
                ingress->control_handler_kind);
            break;
        case SERIAL_CONFIG_INGRESS_CTL_FIRMWARE:
            handle_ctl_command(
                line + (sizeof(CMD_CTL) - 1U),
                line_byte_len - (sizeof(CMD_CTL) - 1U),
                BADGE_USB_CONTROL_SCHEMA_NONE,
                BADGE_USB_CONTROL_HANDLER_NONE);
            break;
        case SERIAL_CONFIG_INGRESS_REJECTED:
        default:
            break;
    }
}

bool serial_config_dispatch_uplink_ota_begin(
    const uint8_t *line, size_t line_byte_len)
{
    if (!serial_config_ingress_is_uplink_ota_begin(
            line, line_byte_len)) {
        return badge_usb_transport_reject_uplink_ota_begin("invalid_command");
    }
    const size_t prefix_len = sizeof(CMD_CTL) - 1U;
    const size_t json_byte_len = line_byte_len - prefix_len;
    const char *projected = NULL;
    if (!project_span(
            line + prefix_len, json_byte_len, &projected)) {
        return badge_usb_transport_reject_uplink_ota_begin("invalid_command");
    }
    cJSON *root = cJSON_ParseWithLengthOpts(
        projected, json_byte_len + 1U, NULL, true);
    int member_count = cJSON_IsObject(root)
        ? cJSON_GetArraySize(root)
        : 0;
    if (!cJSON_IsObject(root) ||
        (member_count != 10 && member_count != 11)) {
        cJSON_Delete(root);
        return badge_usb_transport_reject_uplink_ota_begin("invalid_manifest");
    }

    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *target_item = cJSON_GetObjectItemCaseSensitive(root, "target");
    const cJSON *project_item = cJSON_GetObjectItemCaseSensitive(root, "project");
    const cJSON *hardware_item = cJSON_GetObjectItemCaseSensitive(
        root, "hardware_type");
    const cJSON *version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *crc_item = cJSON_GetObjectItemCaseSensitive(root, "crc32");
    const cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *flow_item = cJSON_GetObjectItemCaseSensitive(
        root, "flow_control");
    const cJSON *recovery_item = cJSON_GetObjectItemCaseSensitive(
        root, "recovery_rewrite_same_version");
    const cJSON *session_item = cJSON_GetObjectItemCaseSensitive(
        root, "session");

    badge_usb_uplink_manifest_fields_t fields = {
        .target = cJSON_IsString(target_item) ? target_item->valuestring : NULL,
        .project = cJSON_IsString(project_item) ? project_item->valuestring : NULL,
        .hardware = cJSON_IsString(hardware_item) ? hardware_item->valuestring : NULL,
        .version = cJSON_IsString(version_item) ? version_item->valuestring : NULL,
        .sha256 = cJSON_IsString(sha_item) ? sha_item->valuestring : NULL,
        .flow_control = cJSON_IsString(flow_item) ? flow_item->valuestring : NULL,
    };
    const char *error = "invalid_manifest";
    bool valid = cJSON_IsString(cmd_item) &&
        strcmp(cmd_item->valuestring, "uplink_ota_begin") == 0 &&
        serial_json_uint32_exact(size_item, &fields.size) &&
        serial_json_uint32_exact(crc_item, &fields.crc32) &&
        cJSON_IsBool(recovery_item);
    if (valid) {
        fields.recovery_rewrite_same_version = cJSON_IsTrue(recovery_item);
    }
    bool session_is_string = cJSON_IsString(session_item);
    bool session_matches = false;
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    const bool canary_build = true;
    const bool maintenance_active =
        badge_runtime_update_maintenance_active();
    if (valid && maintenance_active && session_is_string) {
        session_matches = badge_runtime_update_session_matches(
            session_item->valuestring);
    }
#else
    const bool canary_build = false;
    const bool maintenance_active = false;
#endif
    if (valid) {
        badge_update_ota_begin_admission_t admission =
            badge_update_uplink_ota_begin_admission_decide(
                canary_build,
                maintenance_active,
                (size_t)member_count,
                session_item != NULL,
                session_is_string,
                session_matches);
        if (admission != BADGE_UPDATE_OTA_BEGIN_ADMIT) {
            valid = false;
            error = admission ==
                    BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH
                ? "update_session_mismatch"
                : "unexpected_update_session";
        }
    }
    uplink_ota_manifest_t manifest = {0};
    valid = valid && badge_usb_uplink_ota_manifest_from_fields(
        &fields, &manifest, &error);
    cJSON_Delete(root);
    if (!valid) {
        return badge_usb_transport_reject_uplink_ota_begin(error);
    }
    return badge_usb_transport_handle_uplink_ota_begin(&manifest);
}

static bool execute_recovery_command(
    serial_config_recovery_command_t command)
{
    if (command == SERIAL_CONFIG_RECOVERY_DENIED) {
        return false;
    }

    render_begin(s_command_frame, sizeof(s_command_frame));
    s_direct_emitted = false;
    s_direct_emit_result = false;
    switch (command) {
        case SERIAL_CONFIG_RECOVERY_PING: {
            char msg[48];
            snprintf(msg, sizeof(msg), "FOF_PONG:%s\n", FOF_VERSION);
            send_response(msg);
            break;
        }
        case SERIAL_CONFIG_RECOVERY_STATUS:
            send_startup_recovery_status_response();
            break;
        case SERIAL_CONFIG_RECOVERY_APP_REBOOT:
            reboot_app();
            break;
        case SERIAL_CONFIG_RECOVERY_ROM_BOOT:
            reboot_to_download_mode();
            break;
        case SERIAL_CONFIG_RECOVERY_UPLINK_OTA_BEGIN:
            /* Payload-bearing recovery commands dispatch through the dedicated
             * line handler before this dependency-free enum switch. */
            return false;
        case SERIAL_CONFIG_RECOVERY_DENIED:
        default:
            return false;
    }

    bool completed;
    if (s_render.length > 0 || s_render.overflow) {
        completed = render_emit(BADGE_USB_FRAME_REQUIRED,
                                pdMS_TO_TICKS(1000));
    } else {
        completed = s_direct_emitted && s_direct_emit_result;
    }
#ifdef FOF_BADGE_VARIANT
    if (completed && (command == SERIAL_CONFIG_RECOVERY_PING ||
                      command == SERIAL_CONFIG_RECOVERY_STATUS)) {
        badge_runtime_note_usb_response_completed();
    }
#endif
    return completed;
}

bool serial_config_dispatch_recovery_command(
    const uint8_t *line, size_t line_byte_len)
{
    serial_config_recovery_command_t command =
        serial_config_recovery_command_classify(line, line_byte_len);
    if (command == SERIAL_CONFIG_RECOVERY_DENIED) {
        return false;
    }
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (command == SERIAL_CONFIG_RECOVERY_PING ||
        command == SERIAL_CONFIG_RECOVERY_STATUS) {
        badge_runtime_update_keepalive(
            (uint32_t)(esp_timer_get_time() / 1000));
    }
#endif
    if (command == SERIAL_CONFIG_RECOVERY_UPLINK_OTA_BEGIN) {
        return serial_config_dispatch_uplink_ota_begin(
            line, line_byte_len);
    }
    return execute_recovery_command(command);
}

bool serial_config_dispatch_line(
    const uint8_t *line, size_t line_byte_len)
{
    serial_config_ingress_result_t ingress;
    if (!serial_config_ingress_authorize(
            line, line_byte_len, &ingress)) {
        return false;
    }
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (ingress.kind != SERIAL_CONFIG_INGRESS_CTL_COMPAT ||
        ingress.control_handler_kind !=
            BADGE_USB_CONTROL_HANDLER_UPDATE_MODE) {
        badge_runtime_update_keepalive(
            (uint32_t)(esp_timer_get_time() / 1000));
    }
#endif
    if (ingress.kind == SERIAL_CONFIG_INGRESS_CTL_FIRMWARE &&
        (ingress.firmware_schema_id ==
             FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN ||
         ingress.firmware_schema_id ==
             FOF_FW_JSON_SCHEMA_USB_UPLINK_OTA_BEGIN_SESSION)) {
        return serial_config_dispatch_uplink_ota_begin(
            line, line_byte_len);
    }

    render_begin(s_command_frame, sizeof(s_command_frame));
    s_direct_emitted = false;
    s_direct_emit_result = false;
    handle_control_line(line, line_byte_len, &ingress);
    bool completed;
    if (s_render.length > 0 || s_render.overflow) {
        completed = render_emit(BADGE_USB_FRAME_REQUIRED,
                                pdMS_TO_TICKS(1000));
    } else {
        completed = s_direct_emitted && s_direct_emit_result;
    }
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
    if (s_update_restart_reason) {
        const char *restart_reason = s_update_restart_reason;
        (void)badge_usb_transport_drain(pdMS_TO_TICKS(1000));
        if (s_update_restart_owned) {
            badge_usb_recovery_restart_with_owned_lease(
                BADGE_USB_RESET_APP,
                restart_reason,
                &s_update_restart_lease);
        }
        if (!badge_usb_recovery_restart(
                BADGE_USB_RESET_APP, restart_reason)) {
            /*
             * Keep the durable lifecycle's restart request pending. A BUSY
             * owner should reset imminently; any later command retries if it
             * did not. Clearing this latch before ownership would strand a
             * committed abort/recovery transition without its reboot.
             */
            ESP_LOGE(
                TAG,
                "Update lifecycle restart blocked without reboot ownership");
        }
    }
#endif
#ifdef FOF_BADGE_VARIANT
    if (completed &&
        (ingress.kind == SERIAL_CONFIG_INGRESS_PING ||
         ingress.kind == SERIAL_CONFIG_INGRESS_STATUS)) {
        /* This is after the Task-2 required emit/drain returned success. */
        badge_runtime_note_usb_response_completed();
    }
#endif
    return completed;
}

static void print_json_escaped_string(const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    status_putchar('"');
    while (*p) {
        unsigned char c = *p++;
        if (c == '"' || c == '\\') {
            status_putchar('\\');
            status_putchar((int)c);
        } else if (c == '\b') {
            status_printf("\\b");
        } else if (c == '\f') {
            status_printf("\\f");
        } else if (c == '\n') {
            status_printf("\\n");
        } else if (c == '\r') {
            status_printf("\\r");
        } else if (c == '\t') {
            status_printf("\\t");
        } else if (c < 0x20) {
            status_printf("\\u%04x", c);
        } else {
            status_putchar((int)c);
        }
    }
    status_putchar('"');
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
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "id", detection_id ? detection_id : "");
    cJSON_AddStringToObject(root, "manufacturer",
                           manufacturer ? manufacturer : "");
    cJSON_AddStringToObject(root, "badge_label",
                           badge_label ? badge_label : "");
    cJSON_AddStringToObject(root, "badge_class",
                           badge_class ? badge_class : "");
    cJSON_AddStringToObject(root, "badge_entity_key",
                           badge_entity_key ? badge_entity_key : "");
    cJSON_AddNumberToObject(root, "source", source);
    cJSON_AddNumberToObject(root, "confidence", confidence);
    cJSON_AddNumberToObject(root, "threat_score", threat_score);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    char json[1536];
    char frame[1550];
    if (cJSON_PrintPreallocated(root, json, sizeof(json), false)) {
        int len = snprintf(frame, sizeof(frame), "FOF_DET:%s\n", json);
        if (len > 0 && (size_t)len < sizeof(frame)) {
            (void)badge_usb_transport_emit(frame, (size_t)len,
                                           BADGE_USB_FRAME_OPTIONAL, 0);
        }
    }
    cJSON_Delete(root);
}
