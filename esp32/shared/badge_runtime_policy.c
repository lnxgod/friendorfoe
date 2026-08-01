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

uint32_t badge_runtime_expected_reboot_next_generation(
    uint32_t prior_generation)
{
    uint32_t next = prior_generation + 1U;
    if (next == 0U) {
        next = 1U;
    }
    if (next == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC) {
        next++;
    }
    return next;
}

uint32_t badge_runtime_expected_reboot_generation_for_target(
    badge_runtime_expected_reboot_target_t target,
    uint32_t prior_generation)
{
    switch (target) {
        case BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT:
            return badge_runtime_expected_reboot_next_generation(
                prior_generation);
        case BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK:
            /*
             * Original v0.64.78 authenticates its expected software-reset
             * marker at retained offset +4. In the stable ABI that word is
             * the generation, so this reserved generation is deliberately
             * visible to both v0.78 (+4) and current firmware (+4 plus +8).
             */
            return BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC;
        default:
            return 0U;
    }
}

void badge_runtime_expected_reboot_arm_state_init(
    badge_runtime_expected_reboot_arm_state_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

static bool expected_reboot_lease_matches(
    const badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    return state && lease &&
           lease->owner_id != 0U &&
           lease->generation != 0U &&
           state->active_owner_id == lease->owner_id &&
           state->active_generation == lease->generation;
}

static void expected_reboot_arm_clear_active(
    badge_runtime_expected_reboot_arm_state_t *state)
{
    state->phase = BADGE_RUNTIME_EXPECTED_REBOOT_ARM_IDLE;
    state->active_owner_id = 0U;
    state->active_generation = 0U;
}

bool badge_runtime_expected_reboot_arm_reserve(
    badge_runtime_expected_reboot_arm_state_t *state,
    uint32_t generation,
    badge_runtime_expected_reboot_lease_t *out_lease)
{
    if (out_lease) {
        memset(out_lease, 0, sizeof(*out_lease));
    }
    if (!state || !out_lease || generation == 0U ||
        state->phase != BADGE_RUNTIME_EXPECTED_REBOOT_ARM_IDLE ||
        state->last_owner_id == UINT32_MAX) {
        return false;
    }

    state->last_owner_id++;
    if (state->last_owner_id == 0U) {
        return false;
    }
    state->phase = BADGE_RUNTIME_EXPECTED_REBOOT_ARM_PREPARING;
    state->active_owner_id = state->last_owner_id;
    state->active_generation = generation;
    out_lease->owner_id = state->active_owner_id;
    out_lease->generation = generation;
    return true;
}

bool badge_runtime_expected_reboot_arm_is_preparing(
    const badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    return state &&
           state->phase == BADGE_RUNTIME_EXPECTED_REBOOT_ARM_PREPARING &&
           expected_reboot_lease_matches(state, lease);
}

bool badge_runtime_expected_reboot_arm_publish(
    badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    if (!badge_runtime_expected_reboot_arm_is_preparing(state, lease)) {
        return false;
    }
    state->phase = BADGE_RUNTIME_EXPECTED_REBOOT_ARM_OWNED;
    return true;
}

bool badge_runtime_expected_reboot_arm_is_owned(
    const badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    return state &&
           state->phase == BADGE_RUNTIME_EXPECTED_REBOOT_ARM_OWNED &&
           expected_reboot_lease_matches(state, lease);
}

bool badge_runtime_expected_reboot_arm_cancel(
    badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    if (!badge_runtime_expected_reboot_arm_is_preparing(state, lease)) {
        return false;
    }
    expected_reboot_arm_clear_active(state);
    return true;
}

bool badge_runtime_expected_reboot_arm_release(
    badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    if (!badge_runtime_expected_reboot_arm_is_owned(state, lease)) {
        return false;
    }
    expected_reboot_arm_clear_active(state);
    return true;
}

badge_runtime_expected_reboot_decision_t
badge_runtime_expected_reboot_decide(
    bool software_reset,
    uint32_t generation_word,
    uint32_t magic_word)
{
    badge_runtime_expected_reboot_decision_t decision = {
        .expected_software_reset = false,
        .legacy_v078 = false,
        .clear_generation = true,
        .consumed_generation = 0U,
    };
    if (!software_reset) {
        return decision;
    }
    if (magic_word == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC &&
        generation_word != 0U) {
        decision.expected_software_reset = true;
        decision.clear_generation =
            generation_word == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC;
        decision.consumed_generation = generation_word;
        return decision;
    }
    if (generation_word == BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC &&
        magic_word != BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC) {
        decision.expected_software_reset = true;
        decision.legacy_v078 = true;
        decision.consumed_generation = 1U;
    }
    return decision;
}

bool badge_runtime_normal_stability_satisfied(
    bool safe_mode,
    bool display_alive,
    bool ble_uart_worker_heartbeat,
    bool wifi_uart_worker_heartbeat,
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
    if (!display_alive ||
        !ble_uart_worker_heartbeat ||
        !wifi_uart_worker_heartbeat) {
        return false;
    }
    return free_heap_bytes >= 12000;
}

bool badge_runtime_uart_heartbeat_fresh(int64_t heartbeat_ms,
                                        int64_t now_ms,
                                        int64_t stale_after_ms)
{
    return heartbeat_ms > 0 &&
           now_ms >= heartbeat_ms &&
           now_ms - heartbeat_ms < stale_after_ms;
}

badge_runtime_recovery_token_action_t badge_runtime_recovery_token_decide(
    bool token_armed,
    badge_runtime_reset_class_t reset_class)
{
    return token_armed && reset_class == BADGE_RUNTIME_RESET_EXPECTED_SW
        ? BADGE_RUNTIME_RECOVERY_TOKEN_CONSUME_SAFE_USB
        : BADGE_RUNTIME_RECOVERY_TOKEN_CLEAR;
}

bool badge_runtime_rollback_health_satisfied(
    bool safe_usb,
    bool display_alive,
    bool completed_usb_response,
    bool scanner_uart_worker_heartbeat,
    uint32_t free_heap_bytes,
    int64_t uptime_s,
    int64_t stable_after_s)
{
    if (uptime_s < stable_after_s || free_heap_bytes < 12000) {
        return false;
    }
    if (!display_alive || !completed_usb_response) {
        return false;
    }
    return safe_usb || scanner_uart_worker_heartbeat;
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
