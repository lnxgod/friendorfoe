#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "backend_scanner_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCANNER_ROLLBACK_MIN_UPTIME_MS INT64_C(60000)

typedef enum {
    SCANNER_ROLLBACK_WAIT = 0,
    SCANNER_ROLLBACK_MARK_VALID,
    SCANNER_ROLLBACK_FORCE_ROLLBACK,
} scanner_rollback_action_t;

typedef struct {
    uint32_t boot_id;
    uint32_t command_ingress_boot_id;
    uint32_t role_boot_id;
    int64_t uptime_ms;
    backend_scan_profile_t expected_profile;
    backend_scan_profile_t reported_profile;
    uint32_t watchdog_ready_mask;
    bool command_ingress_healthy;
    bool current_boot_role_acked;
    bool required_radio_healthy;
} scanner_rollback_readiness_t;

typedef struct {
    uint32_t boot_id;
    bool pending_verify;
    bool readiness_latched;
} scanner_rollback_policy_t;

bool scanner_rollback_policy_init(
    scanner_rollback_policy_t *policy,
    uint32_t boot_id,
    bool pending_verify);

bool scanner_rollback_policy_reset_boot(
    scanner_rollback_policy_t *policy,
    uint32_t boot_id,
    bool pending_verify);

scanner_rollback_action_t scanner_rollback_policy_evaluate(
    scanner_rollback_policy_t *policy,
    const scanner_rollback_readiness_t *readiness,
    bool reset_was_crash);

bool scanner_rollback_policy_mark_valid_committed(
    scanner_rollback_policy_t *policy);

#ifdef __cplusplus
}
#endif
