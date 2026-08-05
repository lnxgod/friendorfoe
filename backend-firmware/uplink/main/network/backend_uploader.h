#ifndef BACKEND_UPLOADER_H
#define BACKEND_UPLOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "backend_http_policy.h"
#include "backend_upload_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_HEARTBEAT_INTERVAL_MS INT64_C(60000)

typedef struct {
    uint32_t queue_depth;
    int64_t last_backend_success_ms;
    uint32_t queued_count;
    uint32_t overflow_dropped_count;
    uint32_t ack_count;
    uint32_t retry_count;
    uint32_t quarantine_count;
    uint32_t in_flight_sequence;
    uint32_t in_flight_crc32;
    int64_t next_attempt_ms;
    uint8_t retry_exponent;
    bool in_flight;
    bool in_flight_orphaned;
} backend_uploader_state_t;

typedef struct {
    int64_t last_queued_ms;
    bool initialized;
} backend_heartbeat_state_t;

typedef enum {
    BACKEND_UPLOADER_QUEUE_UNCHANGED = 0,
    BACKEND_UPLOADER_QUEUE_POPPED,
    BACKEND_UPLOADER_QUEUE_QUARANTINED,
} backend_uploader_queue_result_t;

typedef enum {
    BACKEND_UPLOADER_IGNORED = 0,
    BACKEND_UPLOADER_ACKED,
    BACKEND_UPLOADER_RETRY,
    BACKEND_UPLOADER_QUARANTINED,
} backend_uploader_outcome_t;

void backend_uploader_state_init(backend_uploader_state_t *state);

/* Call under the FIFO lock after a successful push. If the push dropped the
 * oldest batch, pass the identity copied from the pre-push locked head. */
bool backend_uploader_note_enqueued(
    backend_uploader_state_t *state,
    uint32_t post_push_depth,
    bool dropped_oldest,
    uint32_t dropped_sequence,
    uint32_t dropped_crc32);

/* Call under the FIFO lock with the current borrowed peek. A true result
 * means the caller may copy this exact head into its immutable send buffer. */
bool backend_uploader_begin_head(
    backend_uploader_state_t *state,
    const backend_upload_batch_t *head,
    uint32_t actual_queue_depth,
    int64_t now_ms);

/* queue_result and actual_queue_depth must come from the locked exact
 * sequence+CRC removal attempt. Only BACKEND_UPLOADER_ACKED is eligible to
 * notify the generation-bound AP-success policy. */
backend_uploader_outcome_t backend_uploader_note_response(
    backend_uploader_state_t *state,
    uint32_t request_sequence,
    uint32_t request_crc32,
    backend_http_disposition_t disposition,
    int status_code,
    backend_uploader_queue_result_t queue_result,
    uint32_t actual_queue_depth,
    uint32_t random_value,
    int64_t now_ms);

void backend_heartbeat_init(
    backend_heartbeat_state_t *state,
    int64_t now_ms);
bool backend_heartbeat_due(
    const backend_heartbeat_state_t *state,
    int64_t now_ms);
void backend_heartbeat_mark_queued(
    backend_heartbeat_state_t *state,
    int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
