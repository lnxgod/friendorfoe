#include "backend_coordinator.h"

#include <limits.h>
#include <string.h>

static bool selectable_state(backend_led_state_t state)
{
    return state >= BACKEND_LED_HEALTHY && state <= BACKEND_LED_FATAL;
}

static bool has_retained_detection(
    const backend_coordinator_t *coordinator)
{
    return coordinator->retained[0].valid || coordinator->retained[1].valid;
}

static void refresh_flow_paused(backend_coordinator_t *coordinator)
{
    coordinator->flow_paused = coordinator->pending_upload_valid ||
        has_retained_detection(coordinator) ||
        (coordinator->upload_sink == NULL &&
         coordinator->detection_router.ready_count != 0U);
}

static bool drain_uploads(backend_coordinator_t *coordinator)
{
    if (coordinator->upload_sink == NULL) {
        refresh_flow_paused(coordinator);
        return !coordinator->pending_upload_valid &&
               coordinator->detection_router.ready_count == 0U;
    }

    for (;;) {
        if (!coordinator->pending_upload_valid) {
            backend_detection_observation_t next = {0};
            if (!backend_detection_router_next_upload(
                    &coordinator->detection_router, &next)) {
                refresh_flow_paused(coordinator);
                return true;
            }
            coordinator->pending_upload = next;
            coordinator->pending_upload_valid = true;
        }

        if (!coordinator->upload_sink(
                coordinator->upload_sink_context,
                &coordinator->pending_upload)) {
            refresh_flow_paused(coordinator);
            return false;
        }
        memset(&coordinator->pending_upload,
               0,
               sizeof(coordinator->pending_upload));
        coordinator->pending_upload_valid = false;
    }
}

static bool retain_detection(
    backend_coordinator_t *coordinator,
    uint8_t slot,
    const backend_detection_observation_t *observation,
    int64_t monotonic_now_ms)
{
    backend_coordinator_retained_t *retained = &coordinator->retained[slot];
    if (retained->valid) {
        return false;
    }
    retained->valid = true;
    retained->arrival_monotonic_ms = monotonic_now_ms;
    retained->insertion_order = coordinator->next_retained_order++;
    if (coordinator->next_retained_order == 0U) {
        coordinator->next_retained_order = 1U;
    }
    retained->observation = *observation;
    refresh_flow_paused(coordinator);
    return true;
}

static bool oldest_retained_slot(
    const backend_coordinator_t *coordinator,
    uint8_t *out_slot)
{
    bool found = false;
    uint8_t slot = 0U;
    for (uint8_t candidate = 0U; candidate < 2U; ++candidate) {
        if (!coordinator->retained[candidate].valid) {
            continue;
        }
        if (!found || coordinator->retained[candidate].insertion_order <
                          coordinator->retained[slot].insertion_order) {
            found = true;
            slot = candidate;
        }
    }
    if (found) {
        *out_slot = slot;
    }
    return found;
}

void backend_coordinator_init(backend_coordinator_t *coordinator)
{
    if (coordinator != NULL) {
        memset(coordinator, 0, sizeof(*coordinator));
        backend_detection_router_init(&coordinator->detection_router);
        coordinator->next_retained_order = 1U;
    }
}

void backend_coordinator_set_upload_sink(
    backend_coordinator_t *coordinator,
    backend_coordinator_upload_sink_fn sink,
    void *context)
{
    if (coordinator != NULL) {
        coordinator->upload_sink = sink;
        coordinator->upload_sink_context = context;
        refresh_flow_paused(coordinator);
    }
}

backend_coordinator_ingest_result_t backend_coordinator_ingest_detection(
    backend_coordinator_t *coordinator,
    uint8_t slot,
    const backend_detection_observation_t *observation,
    int64_t monotonic_now_ms)
{
    backend_coordinator_ingest_result_t result = {0};
    if (coordinator == NULL || observation == NULL || slot >= 2U ||
        monotonic_now_ms < 0) {
        result.flow_paused = coordinator != NULL && coordinator->flow_paused;
        return result;
    }
    const bool drained = drain_uploads(coordinator);
    if (coordinator->retained[slot].valid) {
        refresh_flow_paused(coordinator);
        result.flow_paused = true;
        return result;
    }

    if (!drained) {
        result.retained_for_retry = retain_detection(
            coordinator, slot, observation, monotonic_now_ms);
        result.consumed = result.retained_for_retry;
        result.flow_paused = coordinator->flow_paused;
        return result;
    }

    const backend_detection_route_result_t routed =
        backend_detection_router_ingest(
            &coordinator->detection_router,
            observation,
            monotonic_now_ms);
    if (routed.backpressure) {
        result.retained_for_retry = retain_detection(
            coordinator, slot, observation, monotonic_now_ms);
        result.consumed = result.retained_for_retry;
    } else {
        result.accepted_for_upload = routed.accepted_for_upload;
        result.update_local_threat = routed.update_local_threat;
        result.consumed = routed.accepted_for_upload;
    }

    (void)drain_uploads(coordinator);
    refresh_flow_paused(coordinator);
    result.flow_paused = coordinator->flow_paused;
    return result;
}

size_t backend_coordinator_tick_detections(
    backend_coordinator_t *coordinator,
    int64_t monotonic_now_ms)
{
    if (coordinator == NULL || monotonic_now_ms < 0 ||
        !drain_uploads(coordinator)) {
        return 0U;
    }
    const size_t moved = backend_detection_router_tick(
        &coordinator->detection_router, monotonic_now_ms);
    (void)drain_uploads(coordinator);
    refresh_flow_paused(coordinator);
    return moved;
}

bool backend_coordinator_retry_one(
    backend_coordinator_t *coordinator,
    uint8_t *out_slot,
    backend_detection_observation_t *out_threat_copy)
{
    if (coordinator == NULL || out_slot == NULL ||
        out_threat_copy == NULL || !drain_uploads(coordinator)) {
        return false;
    }

    uint8_t slot = 0U;
    if (!oldest_retained_slot(coordinator, &slot)) {
        refresh_flow_paused(coordinator);
        return false;
    }
    backend_coordinator_retained_t *retained = &coordinator->retained[slot];
    const backend_detection_route_result_t routed =
        backend_detection_router_ingest(
            &coordinator->detection_router,
            &retained->observation,
            retained->arrival_monotonic_ms);
    if (!routed.accepted_for_upload || routed.backpressure) {
        refresh_flow_paused(coordinator);
        return false;
    }

    *out_slot = slot;
    *out_threat_copy = retained->observation;
    memset(retained, 0, sizeof(*retained));
    (void)drain_uploads(coordinator);
    refresh_flow_paused(coordinator);
    return true;
}

bool backend_coordinator_flow_paused(
    const backend_coordinator_t *coordinator)
{
    return coordinator != NULL && coordinator->flow_paused;
}

bool backend_coordinator_retained_detection(
    const backend_coordinator_t *coordinator,
    uint8_t slot,
    backend_detection_observation_t *out)
{
    if (coordinator == NULL || slot >= 2U || out == NULL ||
        !coordinator->retained[slot].valid) {
        return false;
    }
    *out = coordinator->retained[slot].observation;
    return true;
}

bool backend_coordinator_generation_exhausted(
    const backend_coordinator_t *coordinator)
{
    return coordinator != NULL && coordinator->generation_exhausted;
}

void backend_coordinator_update_led(
    backend_coordinator_t *coordinator,
    backend_led_state_t selected_state,
    uint8_t connected_mask,
    int64_t monotonic_now_ms,
    backend_led_mirror_output_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (coordinator == NULL || !selectable_state(selected_state) ||
        monotonic_now_ms < 0) {
        return;
    }
    connected_mask &= UINT8_C(0x03);

    if (coordinator->generation_exhausted) {
        out->command.state = coordinator->state;
        out->command.generation = coordinator->generation;
        out->command.ttl_ms = BACKEND_LED_MIRROR_TTL_MS;
        return;
    }

    bool changed = false;
    if (!coordinator->initialized) {
        coordinator->initialized = true;
        coordinator->state = selected_state;
        coordinator->generation = 1U;
        coordinator->last_mirror_ms = monotonic_now_ms;
        changed = true;
    } else if (coordinator->state != selected_state) {
        if (coordinator->generation == UINT32_MAX) {
            coordinator->generation_exhausted = true;
            out->command.state = coordinator->state;
            out->command.generation = coordinator->generation;
            out->command.ttl_ms = BACKEND_LED_MIRROR_TTL_MS;
            return;
        }
        coordinator->state = selected_state;
        ++coordinator->generation;
        coordinator->last_mirror_ms = monotonic_now_ms;
        changed = true;
    }

    const uint8_t newly_connected = (uint8_t)(
        connected_mask & (uint8_t)~coordinator->connected_mask);
    bool periodic = false;
    if (!changed && monotonic_now_ms >= coordinator->last_mirror_ms &&
        monotonic_now_ms - coordinator->last_mirror_ms >=
            BACKEND_LED_MIRROR_REFRESH_MS) {
        coordinator->last_mirror_ms = monotonic_now_ms;
        periodic = true;
    }

    out->command.state = coordinator->state;
    out->command.generation = coordinator->generation;
    out->command.ttl_ms = BACKEND_LED_MIRROR_TTL_MS;
    if (changed || periodic) {
        out->send_mask = connected_mask;
    } else {
        out->send_mask = newly_connected;
    }
    coordinator->connected_mask = connected_mask;
}
