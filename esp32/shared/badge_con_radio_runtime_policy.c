#include "badge_con_radio_runtime_policy.h"

#include <string.h>

static bool lane_is_valid(int scanner_lane)
{
    return scanner_lane >= 0 &&
           scanner_lane < BADGE_CON_RADIO_SCANNER_LANES;
}

bool badge_con_radio_runtime_controller_init_allowed(
    bool firmware_operation_allows_radio,
    bool identity_valid,
    bool game_snapshot_valid,
    bool game_active,
    bool ota_pending_verify)
{
    return firmware_operation_allows_radio &&
           identity_valid &&
           game_snapshot_valid &&
           game_active &&
           !ota_pending_verify;
}

badge_con_radio_memory_gate_t badge_con_radio_runtime_memory_gate(
    uint32_t internal_free,
    uint32_t internal_largest,
    bool psram_initialized,
    uint32_t psram_total,
    uint32_t psram_free)
{
    if (!psram_initialized ||
        psram_total < BADGE_CON_RADIO_PSRAM_TOTAL_MIN ||
        psram_free < BADGE_CON_RADIO_PSRAM_FREE_MIN) {
        return BADGE_CON_RADIO_MEMORY_PSRAM;
    }
    if (internal_free < BADGE_CON_RADIO_INTERNAL_HEAP_MIN ||
        internal_largest < BADGE_CON_RADIO_INTERNAL_BLOCK_MIN) {
        return BADGE_CON_RADIO_MEMORY_INTERNAL;
    }
    return BADGE_CON_RADIO_MEMORY_OK;
}

void badge_con_radio_runtime_policy_init(
    badge_con_radio_runtime_policy_t *policy)
{
    if (policy) {
        memset(policy, 0, sizeof(*policy));
    }
}

void badge_con_radio_runtime_clear_self_delivery(
    badge_con_radio_runtime_policy_t *policy)
{
    if (policy) {
        memset(policy->self_sent, 0, sizeof(policy->self_sent));
        policy->self_ack_deadline_ms = 0U;
        policy->self_ack_waiting = false;
    }
}

bool badge_con_radio_runtime_observe_boot_id(
    badge_con_radio_runtime_policy_t *policy,
    int scanner_lane,
    uint32_t boot_id)
{
    if (!policy || !lane_is_valid(scanner_lane) || boot_id == 0U ||
        policy->scanner_boot_id[scanner_lane] == boot_id) {
        return false;
    }
    policy->scanner_boot_id[scanner_lane] = boot_id;
    badge_con_radio_runtime_clear_self_delivery(policy);
    return true;
}

int badge_con_radio_runtime_next_unsent_lane(
    const badge_con_radio_runtime_policy_t *policy)
{
    if (!policy) {
        return -1;
    }
    for (int lane = 0; lane < BADGE_CON_RADIO_SCANNER_LANES; lane++) {
        if (!policy->self_sent[lane]) {
            return lane;
        }
    }
    return -1;
}

void badge_con_radio_runtime_note_self_sent(
    badge_con_radio_runtime_policy_t *policy,
    int scanner_lane,
    uint32_t now_ms)
{
    if (policy && lane_is_valid(scanner_lane)) {
        policy->self_sent[scanner_lane] = true;
        if (badge_con_radio_runtime_all_self_sent(policy) &&
            !policy->self_ack_waiting) {
            policy->self_ack_deadline_ms =
                now_ms + BADGE_CON_SELF_ACK_RETRY_MS;
            policy->self_ack_waiting = true;
        }
    }
}

bool badge_con_radio_runtime_all_self_sent(
    const badge_con_radio_runtime_policy_t *policy)
{
    return policy && policy->self_sent[0] && policy->self_sent[1];
}

bool badge_con_radio_runtime_retry_self_due(
    badge_con_radio_runtime_policy_t *policy,
    bool exact_self_ack_matches,
    uint32_t now_ms)
{
    if (!policy) {
        return false;
    }
    if (exact_self_ack_matches) {
        policy->self_ack_deadline_ms = 0U;
        policy->self_ack_waiting = false;
        return false;
    }
    if (!policy->self_ack_waiting ||
        !badge_con_radio_runtime_all_self_sent(policy) ||
        (int32_t)(now_ms - policy->self_ack_deadline_ms) < 0) {
        return false;
    }
    badge_con_radio_runtime_clear_self_delivery(policy);
    return true;
}
