#include "backend_flow_policy.h"

backend_flow_decision_t backend_flow_policy_update(
    size_t decoded_queue_depth,
    bool currently_paused)
{
    if (!currently_paused &&
        decoded_queue_depth >= BACKEND_FLOW_PAUSE_DEPTH) {
        return BACKEND_FLOW_PAUSE;
    }
    if (currently_paused &&
        decoded_queue_depth <= BACKEND_FLOW_RESUME_DEPTH) {
        return BACKEND_FLOW_RESUME;
    }
    return BACKEND_FLOW_NO_CHANGE;
}

bool backend_flow_detection_enqueue_allowed(
    bool flow_paused,
    size_t detection_queue_depth)
{
    return !flow_paused &&
           detection_queue_depth < BACKEND_FLOW_DETECTION_QUEUE_CAPACITY;
}

bool backend_flow_control_enqueue_allowed(size_t control_queue_depth)
{
    return control_queue_depth < BACKEND_FLOW_CONTROL_QUEUE_RESERVE;
}

void backend_flow_state_init(backend_flow_state_t *state)
{
    if (!state) {
        return;
    }
    state->generation = 0U;
    state->paused = false;
    state->ack_pending = false;
}

backend_flow_apply_result_t backend_flow_state_apply(
    backend_flow_state_t *state,
    uint32_t generation,
    bool paused)
{
    if (!state) {
        return BACKEND_FLOW_INVALID_ARGUMENT;
    }
    if (generation == 0U) {
        return BACKEND_FLOW_INVALID_GENERATION;
    }
    if (generation < state->generation) {
        return BACKEND_FLOW_STALE;
    }
    if (generation == state->generation) {
        if (paused != state->paused) {
            return BACKEND_FLOW_CONFLICT;
        }
        state->ack_pending = true;
        return BACKEND_FLOW_REFRESHED;
    }

    state->generation = generation;
    state->paused = paused;
    state->ack_pending = true;
    return BACKEND_FLOW_APPLIED;
}

bool backend_flow_state_take_ack(
    backend_flow_state_t *state,
    backend_flow_ack_t *out)
{
    if (!state || !out || !state->ack_pending) {
        return false;
    }
    out->generation = state->generation;
    out->paused = state->paused;
    state->ack_pending = false;
    return true;
}
