#pragma once

#include "badge_runtime_policy.h"
#include "badge_runtime_rtc_policy.h"
#if defined(FOF_DC34_GAME_CANARY)
#include "badge_update_maintenance_policy.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*badge_runtime_apply_network_fn_t)(badge_runtime_network_mode_t mode);
typedef bool (*badge_runtime_expected_reboot_hook_t)(
    uint32_t expected_reboot_generation);

typedef enum {
    BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_FAILED = 0,
    BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY,
    BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED,
} badge_runtime_expected_reboot_arm_result_t;

void badge_runtime_init(bool pending_verify);
void badge_runtime_set_pending_verify(bool pending_verify);
void badge_runtime_set_network_apply_callback(badge_runtime_apply_network_fn_t cb);
bool badge_runtime_request_network(badge_runtime_network_mode_t mode,
                                   int ttl_s,
                                   const char *reason);
bool badge_runtime_arm_reboot_network_hold(badge_runtime_network_mode_t mode,
                                           int ttl_s);
void badge_runtime_poll(void);
void badge_runtime_force_safe_mode(bool enabled, const char *reason);
badge_runtime_expected_reboot_arm_result_t
badge_runtime_arm_expected_reboot(
    const char *reason,
    badge_runtime_expected_reboot_target_t target,
    badge_runtime_expected_reboot_lease_t *out_lease);
void badge_runtime_set_expected_reboot_hook(
    badge_runtime_expected_reboot_hook_t hook);
bool badge_runtime_expected_reboot_lease_is_owned(
    const badge_runtime_expected_reboot_lease_t *lease);
bool badge_runtime_release_expected_reboot(
    const badge_runtime_expected_reboot_lease_t *lease);
void badge_runtime_arm_usb_recovery_once(void);
bool badge_runtime_reset_reason_was_expected_software(uint32_t reset_reason);
bool badge_runtime_usb_recovery_once_consumed(void);
bool badge_runtime_usb_control_recovery_due(int64_t uptime_s);

void badge_runtime_note_display_alive(void);
void badge_runtime_note_usb_control_alive(void);
void badge_runtime_note_usb_response_completed(void);
void badge_runtime_note_scanner_uart_worker_alive(uint8_t scanner_id);
void badge_runtime_note_display_stack_free(uint32_t words);
void badge_runtime_note_main_stack_free(uint32_t words);
void badge_runtime_note_usb_stack_free(uint32_t words);
void badge_runtime_note_uart_stack_free(uint8_t scanner_id, uint32_t words);
bool badge_runtime_health_can_mark_stable(uint32_t free_heap_bytes,
                                          int64_t uptime_s);
bool badge_runtime_health_can_mark_ota_valid(uint32_t free_heap_bytes,
                                             int64_t uptime_s);
void badge_runtime_mark_stable(void);

badge_runtime_network_mode_t badge_runtime_get_network_mode(void);
int badge_runtime_get_network_ttl_s(void);
bool badge_runtime_is_safe_mode(void);
const char *badge_runtime_safe_reason(void);
uint32_t badge_runtime_crash_count(void);
bool badge_runtime_pending_verify(void);
uint32_t badge_runtime_last_reset_reason(void);
const char *badge_runtime_last_reset_reason_name(void);
bool badge_runtime_last_reset_expected(void);
const char *badge_runtime_last_expected_reboot_reason(void);
uint32_t badge_runtime_last_expected_reboot_generation(void);
int64_t badge_runtime_usb_control_age_s(void);
const char *badge_runtime_recovery_mode(void);
bool badge_runtime_display_alive(void);
bool badge_runtime_usb_control_alive(void);
bool badge_runtime_scanner_uart_alive(void);
uint32_t badge_runtime_display_stack_free(void);
uint32_t badge_runtime_main_stack_free(void);
uint32_t badge_runtime_usb_stack_free(void);
uint32_t badge_runtime_uart_ble_stack_free(void);
uint32_t badge_runtime_uart_wifi_stack_free(void);

#if defined(FOF_DC34_GAME_CANARY)
bool badge_runtime_game_rtc_read(void *out, size_t record_size);
bool badge_runtime_game_rtc_write(
    const void *record, size_t record_size);
void badge_runtime_game_rtc_clear(void);
bool badge_runtime_prepare_update(const char session[17]);
bool badge_runtime_update_maintenance_active(void);
bool badge_runtime_update_preparing(void);
bool badge_runtime_update_session_matches(const char session[17]);
bool badge_runtime_update_session_copy(char session_out[17]);
void badge_runtime_update_keepalive(uint32_t now_ms);
bool badge_runtime_update_inactivity_due(uint32_t now_ms);
bool badge_runtime_update_prepare_orphan_due(uint32_t now_ms);
bool badge_runtime_update_health_can_mark_ota_valid(
    uint32_t free_internal_heap,
    uint32_t largest_internal_block,
    uint32_t uptime_ms);
bool badge_runtime_clear_update_maintenance(const char *reason);
bool badge_runtime_update_marker_snapshot(
    badge_update_maintenance_marker_t *out);
bool badge_runtime_abort_update_session(
    const char session[17], const char *reason);
bool badge_runtime_update_commit_uplink(
    const char *version,
    const char *sha256,
    uint32_t size,
    const char *partition);
bool badge_runtime_update_clear_uplink_commit(void);
#endif

#ifdef __cplusplus
}
#endif
