#include "backend_scanner_role.h"

#include <stddef.h>
#include <string.h>

static bool profile_is_valid(backend_scan_profile_t profile)
{
    return profile >= BACKEND_SCAN_PROFILE_QUIESCENT &&
           profile <= BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
}

void backend_scanner_role_init(
    backend_scanner_role_state_t *state,
    uint32_t boot_id)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->boot_id = boot_id;
    state->effective = BACKEND_SCAN_PROFILE_QUIESCENT;
}

backend_scan_profile_t backend_scanner_role_effective(
    const backend_scanner_role_state_t *state)
{
    if (state == NULL) {
        return BACKEND_SCAN_PROFILE_QUIESCENT;
    }
    return state->effective;
}

backend_scanner_role_result_t backend_scanner_role_apply(
    backend_scanner_role_state_t *state,
    uint32_t boot_id,
    uint32_t generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    uint32_t topology_generation,
#endif
    backend_scan_profile_t profile)
{
    if (state == NULL || boot_id != state->boot_id) {
        return BACKEND_ROLE_INVALID_BOOT;
    }
    if (!profile_is_valid(profile)) {
        return BACKEND_ROLE_INVALID_PROFILE;
    }
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    if (topology_generation == 0U) {
        return BACKEND_ROLE_INVALID_TOPOLOGY;
    }
    if (state->topology_generation != 0U) {
        if (topology_generation < state->topology_generation) {
            return BACKEND_ROLE_STALE;
        }
        if (topology_generation != state->topology_generation) {
            return BACKEND_ROLE_CONFLICT;
        }
    }
#endif
    if (generation < state->generation) {
        return BACKEND_ROLE_STALE;
    }
    if (generation == state->generation) {
        if (profile != state->effective) {
            return BACKEND_ROLE_CONFLICT;
        }
        state->ack_pending = true;
        return BACKEND_ROLE_REFRESHED;
    }

    if (profile != state->effective) {
        ++state->radio_transition_count;
    }
    state->generation = generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    state->topology_generation = topology_generation;
#endif
    state->effective = profile;
    state->ack_pending = true;
    return BACKEND_ROLE_APPLIED;
}

bool backend_scanner_role_take_ack(
    backend_scanner_role_state_t *state,
    bool ble_healthy,
    bool wifi_healthy,
    backend_scanner_role_ack_t *out)
{
    if (state == NULL || out == NULL || !state->ack_pending) {
        return false;
    }

    out->boot_id = state->boot_id;
    out->generation = state->generation;
    out->profile = state->effective;
    out->ble_healthy = ble_healthy;
    out->wifi_healthy = wifi_healthy;
    out->radio_healthy = backend_scanner_required_radio_healthy(
        state->effective, ble_healthy, wifi_healthy);
    state->ack_pending = false;
    return true;
}
