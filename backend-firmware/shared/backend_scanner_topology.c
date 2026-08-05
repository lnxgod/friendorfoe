#include "backend_scanner_topology.h"

#include <stddef.h>
#include <string.h>

#define BACKEND_SCANNER_SLOT_COUNT 2U

static bool elapsed_at_least(
    int64_t started_ms,
    int64_t now_ms,
    uint64_t duration_ms)
{
    if (now_ms < started_ms) {
        return false;
    }
    return ((uint64_t)now_ms - (uint64_t)started_ms) >= duration_ms;
}

static bool transport_is_eligible(const backend_scanner_health_t *health)
{
    return health != NULL &&
           health->connected &&
           health->identity_valid &&
           health->command_healthy &&
           health->boot_id != 0;
}

static void assign_profiles(uint8_t mask, backend_scan_profile_t desired[2])
{
    desired[0] = BACKEND_SCAN_PROFILE_QUIESCENT;
    desired[1] = BACKEND_SCAN_PROFILE_QUIESCENT;

    if ((mask & UINT8_C(0x03)) == UINT8_C(0x03)) {
        desired[0] = BACKEND_SCAN_PROFILE_BLE_PRIMARY;
        desired[1] = BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
    } else if ((mask & UINT8_C(0x01)) != 0) {
        desired[0] = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    } else if ((mask & UINT8_C(0x02)) != 0) {
        desired[1] = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    }
}

static bool scanner_has_converged(
    const backend_scanner_health_t *health,
    backend_scan_profile_t desired)
{
    return health->role_acked &&
           health->commanded_generation != 0U &&
           health->acknowledged_generation == health->commanded_generation &&
           health->commanded_profile == desired &&
           health->reported_profile == desired &&
           health->radio_healthy;
}

static bool scanner_is_within_convergence_grace(
    const backend_scanner_health_t *health,
    int64_t now_ms)
{
    if (!health->convergence_started) {
        return true;
    }
    return !elapsed_at_least(
        health->convergence_started_ms,
        now_ms,
        (uint64_t)BACKEND_SCANNER_CONVERGENCE_GRACE_MS);
}

static bool scanner_is_converged_transition_candidate(
    const backend_scanner_health_t *health,
    backend_scan_profile_t desired)
{
    return health->commanded_profile != desired &&
           scanner_has_converged(health, health->commanded_profile);
}

bool backend_scanner_required_radio_healthy(
    backend_scan_profile_t profile,
    bool ble_healthy,
    bool wifi_healthy)
{
    switch (profile) {
        case BACKEND_SCAN_PROFILE_BLE_PRIMARY:
            return ble_healthy;
        case BACKEND_SCAN_PROFILE_WIFI_PRIMARY:
            return wifi_healthy;
        case BACKEND_SCAN_PROFILE_HYBRID_FAILOVER:
            return ble_healthy && wifi_healthy;
        case BACKEND_SCAN_PROFILE_QUIESCENT:
        default:
            return false;
    }
}

void backend_scanner_plan_compute(
    const backend_scanner_health_t health[2],
    int64_t system_boot_ms,
    int64_t now_ms,
    backend_scanner_plan_t *out)
{
    backend_scan_profile_t initial_desired[2];
    uint8_t transport_mask = 0;
    uint8_t candidate_mask = 0;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    assign_profiles(0, out->desired);

    if (health != NULL) {
        for (size_t slot = 0; slot < BACKEND_SCANNER_SLOT_COUNT; ++slot) {
            if (transport_is_eligible(&health[slot])) {
                transport_mask |= (uint8_t)(UINT8_C(1) << slot);
            }
        }
    }

    assign_profiles(transport_mask, initial_desired);
    for (size_t slot = 0; slot < BACKEND_SCANNER_SLOT_COUNT; ++slot) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << slot);
        if ((transport_mask & bit) == 0) {
            continue;
        }
        if (scanner_has_converged(&health[slot], initial_desired[slot]) ||
            scanner_is_converged_transition_candidate(
                &health[slot], initial_desired[slot]) ||
            scanner_is_within_convergence_grace(&health[slot], now_ms)) {
            candidate_mask |= bit;
        }
    }

    out->eligible_mask = candidate_mask;
    assign_profiles(candidate_mask, out->desired);

    for (size_t slot = 0; slot < BACKEND_SCANNER_SLOT_COUNT; ++slot) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << slot);
        if ((candidate_mask & bit) != 0 &&
            scanner_has_converged(&health[slot], out->desired[slot])) {
            out->converged_mask |= bit;
        }
    }

    if (candidate_mask == 0) {
        const bool boot_grace_expired = elapsed_at_least(
            system_boot_ms,
            now_ms,
            (uint64_t)BACKEND_SCANNER_CONVERGENCE_GRACE_MS);
        out->converging = !boot_grace_expired;
        out->fatal = boot_grace_expired;
        return;
    }

    out->converging = out->converged_mask != candidate_mask;
    out->degraded = candidate_mask == UINT8_C(0x01) ||
                    candidate_mask == UINT8_C(0x02);
}

int backend_scanner_ble_owner(const backend_scanner_plan_t *plan)
{
    if (plan == NULL) {
        return -1;
    }

    const uint8_t ready_mask = plan->eligible_mask & plan->converged_mask;
    for (size_t slot = 0; slot < BACKEND_SCANNER_SLOT_COUNT; ++slot) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << slot);
        const backend_scan_profile_t profile = plan->desired[slot];
        if ((ready_mask & bit) != 0 &&
            (profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY ||
             profile == BACKEND_SCAN_PROFILE_HYBRID_FAILOVER)) {
            return (int)slot;
        }
    }
    return -1;
}
