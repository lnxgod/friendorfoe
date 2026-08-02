#pragma once

#include <stdbool.h>

#include "backend_led_pattern.h"
#include "backend_threat_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_HEALTH_HEALTHY = 0,
    BACKEND_HEALTH_DEGRADED,
    BACKEND_HEALTH_FATAL,
} backend_health_level_t;

typedef struct {
    bool scanner_usable[2];
    bool wifi_connected;
    bool backend_reachable;
    bool fatal_runtime;
    backend_threat_snapshot_t threats;
} backend_health_inputs_t;

typedef struct {
    backend_health_level_t level;
    backend_led_state_t led_state;
} backend_health_snapshot_t;

void backend_health_evaluate(
    const backend_health_inputs_t *inputs,
    backend_health_snapshot_t *out);

#ifdef __cplusplus
}
#endif
