#include "backend_upload_fifo.h"

#include <string.h>

#include "backend_identity.h"

static bool batch_is_valid(const backend_upload_batch_t *batch)
{
    return batch && batch->sequence != 0U &&
           batch->json_len >= 2U &&
           batch->json_len <= BACKEND_UPLOAD_MAX_JSON &&
           batch->json[0] == '{' &&
           batch->json[batch->json_len - 1U] == '}' &&
           batch->json[batch->json_len] == '\0' &&
           batch->json_crc32 == backend_identity_crc32(
               batch->json, batch->json_len);
}

void backend_upload_fifo_init(
    backend_upload_fifo_t *fifo,
    backend_upload_batch_t *storage,
    uint16_t capacity)
{
    if (!fifo) {
        return;
    }
    memset(fifo, 0, sizeof(*fifo));
    if (!storage || capacity == 0U ||
        capacity > BACKEND_UPLOAD_FIFO_CAPACITY) {
        return;
    }
    fifo->storage = storage;
    fifo->capacity = capacity;
}

bool backend_upload_fifo_is_valid(const backend_upload_fifo_t *fifo)
{
    return fifo && fifo->storage && fifo->capacity > 0U &&
           fifo->capacity <= BACKEND_UPLOAD_FIFO_CAPACITY &&
           fifo->head < fifo->capacity && fifo->count <= fifo->capacity;
}

bool backend_upload_fifo_push(
    backend_upload_fifo_t *fifo,
    const backend_upload_batch_t *batch,
    bool *out_dropped_oldest)
{
    if (out_dropped_oldest) {
        *out_dropped_oldest = false;
    }
    if (!backend_upload_fifo_is_valid(fifo) || !batch_is_valid(batch)) {
        return false;
    }

    if (fifo->count == fifo->capacity) {
        fifo->head = (uint16_t)((fifo->head + 1U) % fifo->capacity);
        fifo->count--;
        fifo->dropped_batches++;
        if (out_dropped_oldest) {
            *out_dropped_oldest = true;
        }
    }
    const uint16_t tail = (uint16_t)(
        (fifo->head + fifo->count) % fifo->capacity);
    fifo->storage[tail] = *batch;
    fifo->count++;
    return true;
}

const backend_upload_batch_t *backend_upload_fifo_peek(
    const backend_upload_fifo_t *fifo)
{
    if (!backend_upload_fifo_is_valid(fifo) || fifo->count == 0U) {
        return NULL;
    }
    return &fifo->storage[fifo->head];
}

bool backend_upload_fifo_pop_acked(
    backend_upload_fifo_t *fifo,
    uint32_t expected_sequence)
{
    const backend_upload_batch_t *head = backend_upload_fifo_peek(fifo);
    if (!head || expected_sequence == 0U ||
        head->sequence != expected_sequence) {
        return false;
    }
    fifo->head = (uint16_t)((fifo->head + 1U) % fifo->capacity);
    fifo->count--;
    return true;
}
