#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "backend_scanner_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_ROLE_APPLIED = 0,
    BACKEND_ROLE_REFRESHED,
    BACKEND_ROLE_STALE,
    BACKEND_ROLE_CONFLICT,
    BACKEND_ROLE_INVALID_BOOT,
    BACKEND_ROLE_INVALID_PROFILE,
    BACKEND_ROLE_INVALID_TOPOLOGY,
} backend_scanner_role_result_t;

typedef struct {
    uint32_t boot_id;
    uint32_t generation;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    uint32_t topology_generation;
#endif
    backend_scan_profile_t effective;
    uint32_t radio_transition_count;
    bool ack_pending;
} backend_scanner_role_state_t;

typedef struct {
    uint32_t boot_id;
    uint32_t generation;
    backend_scan_profile_t profile;
    bool ble_healthy;
    bool wifi_healthy;
    /* Locally derived for the effective profile; never trusted from wire. */
    bool radio_healthy;
} backend_scanner_role_ack_t;

void backend_scanner_role_init(
    backend_scanner_role_state_t *state,
    uint32_t boot_id);

backend_scan_profile_t backend_scanner_role_effective(
    const backend_scanner_role_state_t *state);

backend_scanner_role_result_t backend_scanner_role_apply(
    backend_scanner_role_state_t *state,
    uint32_t boot_id,
    uint32_t generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    uint32_t topology_generation,
#endif
    backend_scan_profile_t profile);

bool backend_scanner_role_take_ack(
    backend_scanner_role_state_t *state,
    bool ble_healthy,
    bool wifi_healthy,
    backend_scanner_role_ack_t *out);

#ifdef __cplusplus
}
#endif
