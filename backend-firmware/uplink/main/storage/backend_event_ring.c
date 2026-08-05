#include "backend_event_ring.h"

#include <string.h>

static bool ring_valid(const backend_event_ring_t *ring)
{
    return ring != NULL && ring->records != NULL &&
           ring->capacity == BACKEND_EVENT_RING_CAPACITY &&
           ring->start < ring->capacity && ring->count <= ring->capacity;
}

bool backend_event_ring_init(
    backend_event_ring_t *ring,
    backend_dashboard_event_t *storage,
    size_t capacity)
{
    if (ring == NULL) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    if (storage == NULL || capacity != BACKEND_EVENT_RING_CAPACITY) {
        return false;
    }
    ring->records = storage;
    ring->capacity = capacity;
    ring->next_sequence = UINT64_C(1);
    return true;
}

bool backend_event_ring_append(
    backend_event_ring_t *ring,
    const backend_dashboard_event_t *event)
{
    if (!ring_valid(ring) || event == NULL) {
        return false;
    }
    if (ring->next_sequence == 0U || ring->next_sequence == UINT64_MAX) {
        ring->start = 0U;
        ring->count = 0U;
        ring->next_sequence = UINT64_C(1);
    }

    const size_t slot = (ring->start + ring->count) % ring->capacity;
    if (ring->count == ring->capacity) {
        ring->start = (ring->start + 1U) % ring->capacity;
    } else {
        ++ring->count;
    }
    ring->records[slot] = *event;
    ring->records[slot].sequence = ring->next_sequence++;
    return true;
}

bool backend_event_ring_snapshot(
    const backend_event_ring_t *ring,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *output,
    size_t output_capacity,
    backend_event_ring_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!ring_valid(ring) ||
        (output_capacity > 0U && output == NULL)) {
        return false;
    }
    if (ring->count == 0U) {
        return true;
    }

    const uint64_t oldest = ring->records[ring->start].sequence;
    const size_t newest_slot =
        (ring->start + ring->count - 1U) % ring->capacity;
    const uint64_t newest = ring->records[newest_slot].sequence;
    snapshot->oldest_sequence = oldest;
    snapshot->newest_sequence = newest;

    if (oldest > 0U && after < oldest - 1U) {
        snapshot->cursor_reset = true;
    }
    if (after >= newest || limit == 0U ||
        output_capacity == 0U) {
        return true;
    }

    size_t first_offset = 0U;
    if (!snapshot->cursor_reset && after >= oldest) {
        first_offset = (size_t)(after - oldest + 1U);
    }
    if (first_offset >= ring->count) {
        return true;
    }

    size_t copy_count = ring->count - first_offset;
    if (copy_count > limit) {
        copy_count = limit;
    }
    if (copy_count > output_capacity) {
        copy_count = output_capacity;
    }
    for (size_t index = 0U; index < copy_count; ++index) {
        const size_t slot =
            (ring->start + first_offset + index) % ring->capacity;
        output[index] = ring->records[slot];
    }
    snapshot->count = copy_count;
    return true;
}
