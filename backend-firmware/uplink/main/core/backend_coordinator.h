#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_detection_router.h"
#include "backend_led_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_LED_MIRROR_TTL_MS UINT32_C(6000)
#define BACKEND_LED_MIRROR_REFRESH_MS INT64_C(2000)

typedef bool (*backend_coordinator_upload_sink_fn)(
    void *context,
    const backend_detection_observation_t *observation);

typedef struct {
    bool valid;
    int64_t arrival_monotonic_ms;
    uint64_t insertion_order;
    backend_detection_observation_t observation;
} backend_coordinator_retained_t;

typedef struct {
    backend_led_state_t state;
    uint32_t generation;
    int64_t last_mirror_ms;
    uint8_t connected_mask;
    bool initialized;
    bool generation_exhausted;
    backend_detection_router_t detection_router;
    backend_coordinator_upload_sink_fn upload_sink;
    void *upload_sink_context;
    bool pending_upload_valid;
    backend_detection_observation_t pending_upload;
    backend_coordinator_retained_t retained[2];
    uint64_t next_retained_order;
    bool flow_paused;
} backend_coordinator_t;

typedef struct {
    uint8_t send_mask;
    backend_led_command_t command;
} backend_led_mirror_output_t;

typedef struct {
    bool consumed;
    bool accepted_for_upload;
    bool update_local_threat;
    bool retained_for_retry;
    bool flow_paused;
} backend_coordinator_ingest_result_t;

void backend_coordinator_init(backend_coordinator_t *coordinator);

void backend_coordinator_set_upload_sink(
    backend_coordinator_t *coordinator,
    backend_coordinator_upload_sink_fn sink,
    void *context);

backend_coordinator_ingest_result_t backend_coordinator_ingest_detection(
    backend_coordinator_t *coordinator,
    uint8_t slot,
    const backend_detection_observation_t *observation,
    int64_t monotonic_now_ms);

size_t backend_coordinator_tick_detections(
    backend_coordinator_t *coordinator,
    int64_t monotonic_now_ms);

bool backend_coordinator_retry_one(
    backend_coordinator_t *coordinator,
    uint8_t *out_slot,
    backend_detection_observation_t *out_threat_copy);

bool backend_coordinator_flow_paused(
    const backend_coordinator_t *coordinator);

bool backend_coordinator_retained_detection(
    const backend_coordinator_t *coordinator,
    uint8_t slot,
    backend_detection_observation_t *out);

bool backend_coordinator_generation_exhausted(
    const backend_coordinator_t *coordinator);

void backend_coordinator_update_led(
    backend_coordinator_t *coordinator,
    backend_led_state_t selected_state,
    uint8_t connected_mask,
    int64_t monotonic_now_ms,
    backend_led_mirror_output_t *out);

#ifdef __cplusplus
}
#endif
