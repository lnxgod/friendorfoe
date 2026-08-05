#include "scanner_rollback.h"

#include <string.h>

#include "backend_recovery_policy.h"
#include "backend_scanner_runtime.h"

static bool valid_operational_profile(backend_scan_profile_t profile)
{
    return profile >= BACKEND_SCAN_PROFILE_BLE_PRIMARY &&
           profile <= BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
}

static bool current_boot_is_ready(
    const scanner_rollback_policy_t *policy,
    const scanner_rollback_readiness_t *readiness)
{
    const uint32_t required_watchdog = readiness == NULL
        ? 0U
        : backend_scanner_runtime_required_watchdog_mask(
            readiness->expected_profile);
    return policy != NULL && readiness != NULL &&
           readiness->boot_id == policy->boot_id &&
           readiness->command_ingress_boot_id == policy->boot_id &&
           readiness->role_boot_id == policy->boot_id &&
           readiness->uptime_ms >= SCANNER_ROLLBACK_MIN_UPTIME_MS &&
           readiness->command_ingress_healthy &&
           readiness->current_boot_role_acked &&
           valid_operational_profile(readiness->expected_profile) &&
           readiness->reported_profile == readiness->expected_profile &&
           readiness->required_radio_healthy &&
           required_watchdog != 0U &&
           (readiness->watchdog_ready_mask &
            required_watchdog) == required_watchdog;
}

bool scanner_rollback_policy_init(
    scanner_rollback_policy_t *policy,
    uint32_t boot_id,
    bool pending_verify)
{
    if (policy == NULL || boot_id == 0U) {
        return false;
    }
    memset(policy, 0, sizeof(*policy));
    policy->boot_id = boot_id;
    policy->pending_verify = pending_verify;
    return true;
}

bool scanner_rollback_policy_reset_boot(
    scanner_rollback_policy_t *policy,
    uint32_t boot_id,
    bool pending_verify)
{
    return scanner_rollback_policy_init(policy, boot_id, pending_verify);
}

scanner_rollback_action_t scanner_rollback_policy_evaluate(
    scanner_rollback_policy_t *policy,
    const scanner_rollback_readiness_t *readiness,
    bool reset_was_crash)
{
    if (policy == NULL) {
        return SCANNER_ROLLBACK_WAIT;
    }
    policy->readiness_latched = false;
    if (!policy->pending_verify) {
        return SCANNER_ROLLBACK_WAIT;
    }
    if (reset_was_crash) {
        return SCANNER_ROLLBACK_FORCE_ROLLBACK;
    }
    if (!current_boot_is_ready(policy, readiness)) {
        return SCANNER_ROLLBACK_WAIT;
    }
    policy->readiness_latched = true;
    return SCANNER_ROLLBACK_MARK_VALID;
}

bool scanner_rollback_policy_mark_valid_committed(
    scanner_rollback_policy_t *policy)
{
    if (policy == NULL || !policy->pending_verify ||
        !policy->readiness_latched) {
        return false;
    }
    policy->pending_verify = false;
    policy->readiness_latched = false;
    return true;
}
