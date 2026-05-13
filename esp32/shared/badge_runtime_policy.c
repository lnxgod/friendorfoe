#include "badge_runtime_policy.h"

#include <string.h>

badge_runtime_network_mode_t badge_runtime_default_network_mode(bool badge_variant)
{
    return badge_variant ? BADGE_RUNTIME_NETWORK_OFF
                         : BADGE_RUNTIME_NETWORK_BACKEND;
}

bool badge_runtime_parse_network_mode(const char *value,
                                      badge_runtime_network_mode_t *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "off") == 0 ||
        strcmp(value, "usb") == 0 ||
        strcmp(value, "usb_only") == 0) {
        *out = BADGE_RUNTIME_NETWORK_OFF;
        return true;
    }
    if (strcmp(value, "local_ap") == 0 ||
        strcmp(value, "ap") == 0) {
        *out = BADGE_RUNTIME_NETWORK_LOCAL_AP;
        return true;
    }
    if (strcmp(value, "backend") == 0) {
        *out = BADGE_RUNTIME_NETWORK_BACKEND;
        return true;
    }
    return false;
}

const char *badge_runtime_network_mode_name(badge_runtime_network_mode_t mode)
{
    switch (mode) {
        case BADGE_RUNTIME_NETWORK_LOCAL_AP: return "local_ap";
        case BADGE_RUNTIME_NETWORK_BACKEND:  return "backend";
        case BADGE_RUNTIME_NETWORK_OFF:
        default:                             return "off";
    }
}

bool badge_runtime_badge_allows_network_mode(badge_runtime_network_mode_t mode)
{
    return mode == BADGE_RUNTIME_NETWORK_OFF ||
           mode == BADGE_RUNTIME_NETWORK_LOCAL_AP ||
           mode == BADGE_RUNTIME_NETWORK_BACKEND;
}

int badge_runtime_network_ttl_s(badge_runtime_network_mode_t mode,
                                int requested_ttl_s)
{
    if (mode == BADGE_RUNTIME_NETWORK_OFF) {
        return 0;
    }
    if (requested_ttl_s < 0) {
        return 0; /* persisted mode: no expiry */
    }
    if (requested_ttl_s > 0) {
        return requested_ttl_s;
    }
    return (mode == BADGE_RUNTIME_NETWORK_BACKEND) ? 900 : 600;
}

int badge_runtime_post_ota_hold_ttl_s(badge_runtime_network_mode_t mode,
                                      int requested_ttl_s)
{
    if (mode == BADGE_RUNTIME_NETWORK_OFF) {
        return 0;
    }
    if (!badge_runtime_badge_allows_network_mode(mode)) {
        return 0;
    }
    return requested_ttl_s > 0 ? requested_ttl_s : 300;
}

badge_runtime_boot_decision_t badge_runtime_boot_decide(
    badge_runtime_reset_class_t reset_class,
    bool pending_verify,
    uint32_t prior_crash_count,
    uint32_t crash_loop_threshold
) {
    badge_runtime_boot_decision_t decision = {
        .enter_safe_mode = false,
        .force_ota_rollback = false,
        .new_crash_count = prior_crash_count,
    };

    if (reset_class != BADGE_RUNTIME_RESET_CRASH) {
        return decision;
    }

    decision.new_crash_count = prior_crash_count + 1;
    if (pending_verify) {
        decision.force_ota_rollback = true;
        return decision;
    }
    if (crash_loop_threshold > 0 &&
        decision.new_crash_count >= crash_loop_threshold) {
        decision.enter_safe_mode = true;
    }
    return decision;
}

bool badge_runtime_can_mark_valid(bool safe_mode,
                                  bool display_alive,
                                  bool usb_control_alive,
                                  bool scanner_uart_alive,
                                  uint32_t free_heap_bytes,
                                  int64_t uptime_s,
                                  int64_t stable_after_s)
{
    if (safe_mode) {
        return false;
    }
    if (uptime_s < stable_after_s) {
        return false;
    }
    if (!display_alive || !usb_control_alive || !scanner_uart_alive) {
        return false;
    }
    return free_heap_bytes >= 12000;
}

bool badge_runtime_usb_recovery_due(bool safe_mode,
                                    bool usb_control_alive,
                                    int64_t usb_control_age_s,
                                    int64_t uptime_s,
                                    int64_t stale_after_s,
                                    int64_t boot_grace_s)
{
    if (safe_mode) {
        return false;
    }
    if (uptime_s < boot_grace_s) {
        return false;
    }
    if (!usb_control_alive) {
        return true;
    }
    return usb_control_age_s >= stale_after_s;
}
