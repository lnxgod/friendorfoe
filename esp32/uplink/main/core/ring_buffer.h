#pragma once

/**
 * Friend or Foe -- Thread-Safe Ring Buffer
 *
 * Fixed-capacity circular buffer for arbitrary item types.
 * When full, push() overwrites the oldest entry.
 * Thread-safety via FreeRTOS portMUX spinlock.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ring_buffer ring_buffer_t;

/**
 * Create a ring buffer.
 *
 * @param capacity   Maximum number of items
 * @param item_size  Size of each item in bytes
 * @return Allocated ring buffer, or NULL on failure
 */
ring_buffer_t *ring_buffer_create(int capacity, size_t item_size);

/**
 * Create a ring buffer with its storage in external PSRAM when available.
 * Falls back to internal SRAM (same as [ring_buffer_create]) when PSRAM
 * isn't present or the allocation fails.
 *
 * The header struct always lives in internal SRAM; only the data array
 * moves to PSRAM. Use for large offline queues and buffers where the
 * capacity would exhaust internal heap.
 */
ring_buffer_t *ring_buffer_create_psram(int capacity, size_t item_size);

/**
 * Push an item into the ring buffer.
 * If full, the oldest item is overwritten.
 *
 * @return true if an old item was overwritten (buffer was full)
 */
bool ring_buffer_push(ring_buffer_t *rb, const void *item);

/**
 * Pop the oldest item from the ring buffer.
 *
 * @param item  Output buffer (must be at least item_size bytes)
 * @return true if an item was popped, false if buffer was empty
 */
bool ring_buffer_pop(ring_buffer_t *rb, void *item);

/** Get the number of items currently in the buffer. */
int ring_buffer_count(const ring_buffer_t *rb);

/** Check if the buffer is full. */
bool ring_buffer_is_full(const ring_buffer_t *rb);

/** Check if the buffer is empty. */
bool ring_buffer_is_empty(const ring_buffer_t *rb);

/** Free the ring buffer and its storage. */
void ring_buffer_destroy(ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif
