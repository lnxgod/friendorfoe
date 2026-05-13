#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_RUNTIME_NETWORK_OFF = 0,
    BADGE_RUNTIME_NETWORK_LOCAL_AP,
    BADGE_RUNTIME_NETWORK_BACKEND,
} badge_runtime_network_mode_t;

typedef enum {
    BADGE_RUNTIME_RESET_CLEAN = 0,
    BADGE_RUNTIME_RESET_EXPECTED_SW,
    BADGE_RUNTIME_RESET_CRASH,
} badge_runtime_reset_class_t;

typedef struct {
    bool enter_safe_mode;
    bool force_ota_rollback;
    uint32_t new_crash_count;
} badge_runtime_boot_decision_t;

badge_runtime_network_mode_t badge_runtime_default_network_mode(bool badge_variant);
bool badge_runtime_parse_network_mode(const char *value,
                                      badge_runtime_network_mode_t *out);
const char *badge_runtime_network_mode_name(badge_runtime_network_mode_t mode);
bool badge_runtime_badge_allows_network_mode(badge_runtime_network_mode_t mode);
int badge_runtime_network_ttl_s(badge_runtime_network_mode_t mode,
                                int requested_ttl_s);
int badge_runtime_post_ota_hold_ttl_s(badge_runtime_network_mode_t mode,
                                      int requested_ttl_s);
badge_runtime_boot_decision_t badge_runtime_boot_decide(
    badge_runtime_reset_class_t reset_class,
    bool pending_verify,
    uint32_t prior_crash_count,
    uint32_t crash_loop_threshold
);
bool badge_runtime_can_mark_valid(bool safe_mode,
                                  bool display_alive,
                                  bool usb_control_alive,
                                  bool scanner_uart_alive,
                                  uint32_t free_heap_bytes,
                                  int64_t uptime_s,
                                  int64_t stable_after_s);
bool badge_runtime_usb_recovery_due(bool safe_mode,
                                    bool usb_control_alive,
                                    int64_t usb_control_age_s,
                                    int64_t uptime_s,
                                    int64_t stale_after_s,
                                    int64_t boot_grace_s);

#ifdef __cplusplus
}
#endif
