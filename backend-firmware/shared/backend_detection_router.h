#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_detection_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_DEDUPE_BUCKET_CAPACITY 64U
#define BACKEND_DEDUPE_READY_CAPACITY 64U
#define BACKEND_DEDUPE_WINDOW_MS INT64_C(500)

typedef struct {
    bool used;
    char key[192];
    int64_t opened_monotonic_ms;
    uint64_t insertion_order;
    backend_detection_observation_t observation;
} backend_dedupe_bucket_t;

typedef struct {
    bool accepted_for_upload;
    bool update_local_threat;
    bool backpressure;
} backend_detection_route_result_t;

typedef struct {
    backend_dedupe_bucket_t buckets[BACKEND_DEDUPE_BUCKET_CAPACITY];
    backend_detection_observation_t ready[BACKEND_DEDUPE_READY_CAPACITY];
    uint8_t ready_head;
    uint8_t ready_count;
    uint64_t next_insertion_order;
} backend_detection_router_t;

void backend_observation_resolve(
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *scanner_stamp,
    int64_t uplink_epoch_ms,
    backend_detection_observation_t *out);

void backend_detection_router_init(backend_detection_router_t *router);

backend_detection_route_result_t backend_detection_router_ingest(
    backend_detection_router_t *router,
    const backend_detection_observation_t *observation,
    int64_t monotonic_now_ms);

size_t backend_detection_router_tick(
    backend_detection_router_t *router,
    int64_t monotonic_now_ms);

bool backend_detection_router_next_upload(
    backend_detection_router_t *router,
    backend_detection_observation_t *out);

#ifdef __cplusplus
}
#endif
