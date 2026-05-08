#pragma once

#include "badge_runtime_policy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*badge_runtime_apply_network_fn_t)(badge_runtime_network_mode_t mode);

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

void badge_runtime_note_display_alive(void);
void badge_runtime_note_usb_control_alive(void);
void badge_runtime_note_scanner_uart_alive(bool alive);
bool badge_runtime_health_can_mark_ota_valid(uint32_t free_heap_bytes,
                                             int64_t uptime_s);
void badge_runtime_mark_stable(void);

badge_runtime_network_mode_t badge_runtime_get_network_mode(void);
int badge_runtime_get_network_ttl_s(void);
bool badge_runtime_is_safe_mode(void);
const char *badge_runtime_safe_reason(void);
uint32_t badge_runtime_crash_count(void);
bool badge_runtime_pending_verify(void);
bool badge_runtime_display_alive(void);
bool badge_runtime_usb_control_alive(void);
bool badge_runtime_scanner_uart_alive(void);

#ifdef __cplusplus
}
#endif
