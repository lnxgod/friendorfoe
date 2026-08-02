#include "backend_recovery_policy.h"

#include "time_sync_policy.h"

#define BACKEND_ALL_WORKER_MASK \
    (BACKEND_WORKER_UART_RX_CONTROL | BACKEND_WORKER_COORDINATOR | \
     BACKEND_WORKER_BLE_RADIO | BACKEND_WORKER_WIFI_RADIO | \
     BACKEND_WORKER_UPLOADER | BACKEND_WORKER_COMMAND_CLIENT | \
     BACKEND_WORKER_OTA)

static void reset_local_recovery(backend_recovery_policy_t *policy)
{
    policy->probes_sent = 0U;
    policy->last_probe_ms = 0;
    policy->uart_reinitialized = false;
    policy->unavailable = false;
    if (policy->pending_action != BACKEND_RECOVERY_SEND_RESTART_RADIOS) {
        policy->pending_action = BACKEND_RECOVERY_NONE;
        policy->pending_action_issued_ms = 0;
    }
}

void backend_recovery_policy_init(
    backend_recovery_policy_t *policy,
    int64_t now_ms)
{
    if (!policy) {
        return;
    }
    policy->monitoring_started_ms = now_ms >= 0 ? now_ms : 0;
    policy->last_valid_status_ms = 0;
    policy->boot_id = 0U;
    policy->remote_restart_generation = 0U;
    policy->have_status = false;
    policy->remote_restart_pending = false;
    policy->pending_action = BACKEND_RECOVERY_NONE;
    policy->pending_action_issued_ms = 0;
    reset_local_recovery(policy);
}

backend_status_boot_result_t backend_recovery_policy_note_status(
    backend_recovery_policy_t *policy,
    uint32_t boot_id,
    int64_t now_ms)
{
    if (!policy || boot_id == 0U ||
        now_ms < policy->monitoring_started_ms ||
        (policy->have_status && now_ms < policy->last_valid_status_ms)) {
        return BACKEND_STATUS_BOOT_INVALID;
    }

    backend_status_boot_result_t result = BACKEND_STATUS_BOOT_UNCHANGED;
    if (!policy->have_status) {
        result = BACKEND_STATUS_BOOT_FIRST;
    } else if (boot_id != policy->boot_id) {
        result = BACKEND_STATUS_BOOT_CHANGED;
    }

    if (!policy->have_status || boot_id != policy->boot_id) {
        policy->remote_restart_generation = 0U;
        policy->remote_restart_pending = false;
        policy->pending_action = BACKEND_RECOVERY_NONE;
        policy->pending_action_issued_ms = 0;
    }
    policy->boot_id = boot_id;
    policy->last_valid_status_ms = now_ms;
    policy->have_status = true;
    reset_local_recovery(policy);
    return result;
}

backend_remote_recovery_result_t backend_recovery_policy_request_restart(
    backend_recovery_policy_t *policy,
    uint32_t boot_id,
    uint32_t generation)
{
    if (!policy) {
        return BACKEND_REMOTE_RECOVERY_INVALID_ARGUMENT;
    }
    if (!policy->have_status || boot_id == 0U || boot_id != policy->boot_id) {
        return BACKEND_REMOTE_RECOVERY_INVALID_BOOT;
    }
    if (generation == 0U) {
        return BACKEND_REMOTE_RECOVERY_INVALID_GENERATION;
    }
    if (generation < policy->remote_restart_generation) {
        return BACKEND_REMOTE_RECOVERY_STALE;
    }
    if (generation == policy->remote_restart_generation) {
        return BACKEND_REMOTE_RECOVERY_REFRESHED;
    }

    policy->remote_restart_generation = generation;
    policy->remote_restart_pending = true;
    return BACKEND_REMOTE_RECOVERY_APPLIED;
}

backend_recovery_action_t backend_recovery_policy_tick(
    backend_recovery_policy_t *policy,
    int64_t now_ms,
    bool ota_active)
{
    if (!policy || now_ms < policy->monitoring_started_ms ||
        (policy->have_status && now_ms < policy->last_valid_status_ms)) {
        return BACKEND_RECOVERY_NONE;
    }

    const int64_t silence_started_ms = policy->have_status
        ? policy->last_valid_status_ms
        : policy->monitoring_started_ms;
    const int64_t silent_ms = now_ms - silence_started_ms;

    if (silent_ms >= BACKEND_STATUS_UNAVAILABLE_AFTER_MS) {
        if (!policy->unavailable) {
            if (policy->pending_action != BACKEND_RECOVERY_MARK_UNAVAILABLE) {
                policy->pending_action = BACKEND_RECOVERY_MARK_UNAVAILABLE;
                policy->pending_action_issued_ms = now_ms;
            }
            return policy->pending_action;
        }
        return BACKEND_RECOVERY_NONE;
    }

    if (policy->pending_action != BACKEND_RECOVERY_NONE) {
        if (policy->pending_action == BACKEND_RECOVERY_SEND_RESTART_RADIOS &&
            ota_active) {
            return BACKEND_RECOVERY_NONE;
        }
        return policy->pending_action;
    }

    if (policy->remote_restart_pending && !ota_active) {
        policy->pending_action = BACKEND_RECOVERY_SEND_RESTART_RADIOS;
        policy->pending_action_issued_ms = now_ms;
        return policy->pending_action;
    }

    if (policy->probes_sent < BACKEND_STATUS_PROBE_COUNT &&
        silent_ms >= BACKEND_STATUS_STALE_AFTER_MS &&
        (policy->probes_sent == 0U ||
         (now_ms >= policy->last_probe_ms &&
          now_ms - policy->last_probe_ms >=
              BACKEND_STATUS_PROBE_INTERVAL_MS))) {
        policy->pending_action = BACKEND_RECOVERY_SEND_PROBE;
        policy->pending_action_issued_ms = now_ms;
        return policy->pending_action;
    }

    if (policy->probes_sent == BACKEND_STATUS_PROBE_COUNT &&
        !policy->uart_reinitialized &&
        silent_ms >= BACKEND_STATUS_UART_REINIT_AFTER_MS) {
        policy->pending_action = BACKEND_RECOVERY_REINIT_LOCAL_UART;
        policy->pending_action_issued_ms = now_ms;
        return policy->pending_action;
    }
    return BACKEND_RECOVERY_NONE;
}

bool backend_recovery_policy_complete_action(
    backend_recovery_policy_t *policy,
    backend_recovery_action_t action,
    bool succeeded,
    int64_t now_ms)
{
    if (!policy || action == BACKEND_RECOVERY_NONE ||
        action != policy->pending_action ||
        now_ms < policy->pending_action_issued_ms ||
        now_ms < policy->monitoring_started_ms ||
        (policy->have_status && now_ms < policy->last_valid_status_ms)) {
        return false;
    }
    if (!succeeded) {
        return true;
    }

    switch (action) {
    case BACKEND_RECOVERY_SEND_PROBE:
        if (policy->probes_sent >= BACKEND_STATUS_PROBE_COUNT) {
            return false;
        }
        policy->probes_sent++;
        policy->last_probe_ms = now_ms;
        break;
    case BACKEND_RECOVERY_REINIT_LOCAL_UART:
        policy->uart_reinitialized = true;
        break;
    case BACKEND_RECOVERY_MARK_UNAVAILABLE:
        policy->unavailable = true;
        break;
    case BACKEND_RECOVERY_SEND_RESTART_RADIOS:
        policy->remote_restart_pending = false;
        break;
    case BACKEND_RECOVERY_NONE:
    default:
        return false;
    }
    policy->pending_action = BACKEND_RECOVERY_NONE;
    policy->pending_action_issued_ms = 0;
    return true;
}

void backend_status_cadence_init(backend_status_cadence_t *cadence)
{
    if (!cadence) {
        return;
    }
    cadence->last_emit_ms = 0;
    cadence->emitted = false;
    cadence->change_pending = false;
}

bool backend_status_cadence_due(
    backend_status_cadence_t *cadence,
    int64_t now_ms,
    bool state_changed)
{
    if (!cadence || now_ms < 0) {
        return false;
    }
    if (state_changed) {
        cadence->change_pending = true;
    }
    return !cadence->emitted || cadence->change_pending ||
        (now_ms >= cadence->last_emit_ms &&
         now_ms - cadence->last_emit_ms >= BACKEND_STATUS_EMIT_INTERVAL_MS);
}

bool backend_status_cadence_mark_sent(
    backend_status_cadence_t *cadence,
    int64_t now_ms)
{
    if (!cadence || now_ms < 0 ||
        (cadence->emitted && now_ms < cadence->last_emit_ms)) {
        return false;
    }
    cadence->last_emit_ms = now_ms;
    cadence->emitted = true;
    cadence->change_pending = false;
    return true;
}

void backend_command_cadence_init(backend_command_cadence_t *cadence)
{
    if (!cadence) {
        return;
    }
    cadence->last_emit_ms = 0;
    cadence->emitted = false;
    cadence->immediate_pending = false;
}

bool backend_command_cadence_due(
    backend_command_cadence_t *cadence,
    int64_t now_ms,
    bool boot_or_link_changed)
{
    if (!cadence || now_ms < 0) {
        return false;
    }
    if (boot_or_link_changed) {
        cadence->immediate_pending = true;
    }
    return !cadence->emitted || cadence->immediate_pending ||
        (now_ms >= cadence->last_emit_ms &&
         now_ms - cadence->last_emit_ms >=
             BACKEND_COORDINATOR_COMMAND_INTERVAL_MS);
}

bool backend_command_cadence_mark_sent(
    backend_command_cadence_t *cadence,
    int64_t now_ms)
{
    if (!cadence || now_ms < 0 ||
        (cadence->emitted && now_ms < cadence->last_emit_ms)) {
        return false;
    }
    cadence->last_emit_ms = now_ms;
    cadence->emitted = true;
    cadence->immediate_pending = false;
    return true;
}

bool backend_watchdog_mark_iteration(
    uint32_t *completed_mask,
    uint32_t worker)
{
    if (!completed_mask || worker == 0U ||
        (worker & (worker - 1U)) != 0U ||
        (worker & BACKEND_ALL_WORKER_MASK) == 0U) {
        return false;
    }
    *completed_mask |= worker;
    return true;
}

bool backend_watchdog_ready(uint32_t required_mask, uint32_t completed_mask)
{
    return required_mask != 0U &&
           (required_mask & ~BACKEND_ALL_WORKER_MASK) == 0U &&
           (completed_mask & required_mask) == required_mask;
}

bool backend_coordinator_epoch_valid(int64_t epoch_ms)
{
    return fof_time_epoch_is_valid(epoch_ms);
}
