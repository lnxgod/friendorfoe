#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_FLOW_DETECTION_QUEUE_CAPACITY 64U
#define BACKEND_FLOW_PAUSE_DEPTH 48U
#define BACKEND_FLOW_RESUME_DEPTH 24U
#define BACKEND_FLOW_CONTROL_QUEUE_RESERVE 4U

typedef enum {
    BACKEND_FLOW_NO_CHANGE = 0,
    BACKEND_FLOW_PAUSE,
    BACKEND_FLOW_RESUME,
} backend_flow_decision_t;

typedef enum {
    BACKEND_FLOW_APPLIED = 0,
    BACKEND_FLOW_REFRESHED,
    BACKEND_FLOW_STALE,
    BACKEND_FLOW_CONFLICT,
    BACKEND_FLOW_INVALID_GENERATION,
    BACKEND_FLOW_INVALID_ARGUMENT,
} backend_flow_apply_result_t;

typedef struct {
    uint32_t generation;
    bool paused;
    bool ack_pending;
} backend_flow_state_t;

typedef struct {
    uint32_t generation;
    bool paused;
} backend_flow_ack_t;

backend_flow_decision_t backend_flow_policy_update(
    size_t decoded_queue_depth,
    bool currently_paused);

bool backend_flow_detection_enqueue_allowed(
    bool flow_paused,
    size_t detection_queue_depth);

bool backend_flow_control_enqueue_allowed(size_t control_queue_depth);

void backend_flow_state_init(backend_flow_state_t *state);

backend_flow_apply_result_t backend_flow_state_apply(
    backend_flow_state_t *state,
    uint32_t generation,
    bool paused);

bool backend_flow_state_take_ack(
    backend_flow_state_t *state,
    backend_flow_ack_t *out);

#ifdef __cplusplus
}
#endif
