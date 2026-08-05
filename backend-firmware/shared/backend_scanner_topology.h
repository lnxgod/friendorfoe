#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_SCANNER_CONVERGENCE_GRACE_MS INT64_C(15000)

typedef enum {
    BACKEND_SCAN_PROFILE_QUIESCENT = 0,
    BACKEND_SCAN_PROFILE_BLE_PRIMARY,
    BACKEND_SCAN_PROFILE_WIFI_PRIMARY,
    BACKEND_SCAN_PROFILE_HYBRID_FAILOVER,
} backend_scan_profile_t;

typedef struct {
    bool connected;
    bool identity_valid;
    bool command_healthy;
    bool radio_healthy;
    bool role_acked;
    uint32_t boot_id;
    uint32_t acknowledged_generation;
    uint32_t commanded_generation;
    backend_scan_profile_t commanded_profile;
    backend_scan_profile_t reported_profile;
    int64_t convergence_started_ms;
    /* Distinguishes a real start at monotonic time zero from not-yet-sent. */
    bool convergence_started;
} backend_scanner_health_t;

typedef struct {
    backend_scan_profile_t desired[2];
    uint8_t eligible_mask;
    uint8_t converged_mask;
    bool converging;
    bool degraded;
    bool fatal;
} backend_scanner_plan_t;

bool backend_scanner_required_radio_healthy(
    backend_scan_profile_t profile,
    bool ble_healthy,
    bool wifi_healthy);

void backend_scanner_plan_compute(
    const backend_scanner_health_t health[2],
    int64_t system_boot_ms,
    int64_t now_ms,
    backend_scanner_plan_t *out);

int backend_scanner_ble_owner(const backend_scanner_plan_t *plan);

#ifdef __cplusplus
}
#endif
