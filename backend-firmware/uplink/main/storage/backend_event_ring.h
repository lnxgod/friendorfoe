#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_dashboard_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_EVENT_RING_CAPACITY 128U

typedef struct {
    backend_dashboard_event_t *records;
    size_t capacity;
    size_t start;
    size_t count;
    uint64_t next_sequence;
    uint64_t dropped_contention;
} backend_event_ring_t;

typedef struct {
    size_t count;
    uint64_t oldest_sequence;
    uint64_t newest_sequence;
    bool cursor_reset;
} backend_event_ring_snapshot_t;

bool backend_event_ring_init(
    backend_event_ring_t *ring,
    backend_dashboard_event_t *storage,
    size_t capacity);

bool backend_event_ring_append(
    backend_event_ring_t *ring,
    const backend_dashboard_event_t *event);

bool backend_event_ring_snapshot(
    const backend_event_ring_t *ring,
    uint64_t after,
    size_t limit,
    backend_dashboard_event_t *output,
    size_t output_capacity,
    backend_event_ring_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
