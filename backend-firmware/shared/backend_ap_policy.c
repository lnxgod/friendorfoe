#include "backend_ap_policy.h"

#include <string.h>

static bool elapsed_at_least(
    int64_t now_ms, int64_t started_ms, int64_t duration_ms)
{
    return started_ms >= 0 && now_ms >= started_ms &&
           now_ms - started_ms >= duration_ms;
}

void backend_ap_policy_init(backend_ap_policy_t *policy, int64_t boot_ms)
{
    if (!policy) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
    policy->boot_ms = boot_ms;
    policy->ap_started_ms = -1;
    policy->last_success_ms = -1;
    policy->outage_started_ms = boot_ms;
}

void backend_ap_policy_note_config_commit(
    backend_ap_policy_t *policy,
    uint32_t new_generation,
    int64_t now_ms)
{
    if (!policy) {
        return;
    }
    if (policy->running) {
        policy->ap_started_config_generation = new_generation;
    }
    policy->last_success_generation = 0;
    policy->last_success_ms = -1;
    policy->outage_started_ms = now_ms;
}

void backend_ap_policy_note_backend_success(
    backend_ap_policy_t *policy,
    uint32_t config_generation,
    int64_t now_ms)
{
    if (!policy) {
        return;
    }
    policy->last_success_generation = config_generation;
    policy->last_success_ms = now_ms;
    policy->outage_started_ms = now_ms;
}

static backend_ap_action_t start_ap(
    backend_ap_policy_t *policy,
    uint32_t config_generation,
    int64_t now_ms)
{
    policy->running = true;
    policy->ap_started_ms = now_ms;
    policy->ap_started_config_generation = config_generation;
    return BACKEND_AP_START;
}

backend_ap_action_t backend_ap_policy_tick(
    backend_ap_policy_t *policy,
    backend_ap_input_t input,
    int64_t now_ms)
{
    if (!policy) {
        return BACKEND_AP_NO_CHANGE;
    }

    if (input.backend_connected) {
        policy->outage_started_ms = now_ms;
    }

    if (!policy->running) {
        if (!input.config_valid || input.usb_start_requested) {
            return start_ap(policy, input.config_generation, now_ms);
        }
        if (!input.backend_connected &&
            elapsed_at_least(
                now_ms,
                policy->outage_started_ms,
                BACKEND_AP_OUTAGE_START_MS)) {
            return start_ap(policy, input.config_generation, now_ms);
        }
        return BACKEND_AP_NO_CHANGE;
    }

    const bool current_generation_success =
        input.config_valid &&
        input.config_generation == policy->ap_started_config_generation &&
        policy->last_success_generation == input.config_generation;
    const bool success_after_start =
        policy->last_success_ms >= policy->ap_started_ms;
    if (current_generation_success && success_after_start &&
        elapsed_at_least(
            now_ms,
            policy->last_success_ms,
            BACKEND_AP_SUCCESS_GRACE_MS)) {
        policy->running = false;
        policy->ap_started_ms = -1;
        policy->ap_started_config_generation = 0;
        return BACKEND_AP_STOP;
    }
    return BACKEND_AP_NO_CHANGE;
}
