#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "backend_upload_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    backend_upload_batch_t *storage;
    uint16_t capacity;
    uint16_t head;
    uint16_t count;
    uint32_t dropped_batches;
} backend_upload_fifo_t;

void backend_upload_fifo_init(
    backend_upload_fifo_t *fifo,
    backend_upload_batch_t *storage,
    uint16_t capacity);

bool backend_upload_fifo_is_valid(const backend_upload_fifo_t *fifo);

bool backend_upload_fifo_push(
    backend_upload_fifo_t *fifo,
    const backend_upload_batch_t *batch,
    bool *out_dropped_oldest);

const backend_upload_batch_t *backend_upload_fifo_peek(
    const backend_upload_fifo_t *fifo);

bool backend_upload_fifo_pop_acked(
    backend_upload_fifo_t *fifo,
    uint32_t expected_sequence);

#ifdef __cplusplus
}
#endif
