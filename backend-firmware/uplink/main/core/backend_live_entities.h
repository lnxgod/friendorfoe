#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_dashboard_event.h"
#include "backend_json_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keep the native-compatible entity projection bounded so FOF_STATUS stays
 * below the Lite USB frame ceiling even when the history ring is busy. */
#define BACKEND_LIVE_ENTITY_CAPACITY 8U

typedef struct {
    bool used;
    int64_t last_seen_ms;
    uint32_t event_count;
    int8_t best_rssi;
    backend_dashboard_event_t event;
} backend_live_entity_t;

typedef struct {
    backend_live_entity_t records[BACKEND_LIVE_ENTITY_CAPACITY];
} backend_live_entities_t;

void backend_live_entities_init(backend_live_entities_t *state);

bool backend_live_entities_ingest(
    backend_live_entities_t *state,
    const backend_dashboard_event_t *event,
    int64_t now_ms);

size_t backend_live_entities_active_count(
    const backend_live_entities_t *state,
    int64_t now_ms);

/* Appends one complete JSON array using the production badge entity field
 * contract. The caller owns synchronization around state. */
bool backend_live_entities_append_json(
    backend_json_writer_t *writer,
    const backend_live_entities_t *state,
    int64_t now_ms);

#ifdef __cplusplus
}
#endif
