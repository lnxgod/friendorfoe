/**
 * Friend or Foe -- Thread-Safe Ring Buffer Implementation
 */

#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

struct ring_buffer {
    uint8_t       *storage;
    int            capacity;
    size_t         item_size;
    int            head;        /* next write position */
    int            tail;        /* next read position  */
    int            count;
    portMUX_TYPE   lock;
};

ring_buffer_t *ring_buffer_create(int capacity, size_t item_size)
{
    if (capacity <= 0 || item_size == 0) {
        return NULL;
    }

    ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) {
        return NULL;
    }

    rb->storage = calloc(capacity, item_size);
    if (!rb->storage) {
        free(rb);
        return NULL;
    }

    rb->capacity  = capacity;
    rb->item_size = item_size;
    rb->head      = 0;
    rb->tail      = 0;
    rb->count     = 0;
    portMUX_INITIALIZE(&rb->lock);

    return rb;
}

bool ring_buffer_push(ring_buffer_t *rb, const void *item)
{
    if (!rb || !item) {
        return false;
    }

    bool overwritten = false;

    portENTER_CRITICAL(&rb->lock);

    /* Copy item into head position */
    memcpy(rb->storage + (rb->head * rb->item_size), item, rb->item_size);
    rb->head = (rb->head + 1) % rb->capacity;

    if (rb->count == rb->capacity) {
        /* Buffer was full -- advance tail to drop oldest */
        rb->tail = (rb->tail + 1) % rb->capacity;
        overwritten = true;
    } else {
        rb->count++;
    }

    portEXIT_CRITICAL(&rb->lock);

    return overwritten;
}

bool ring_buffer_pop(ring_buffer_t *rb, void *item)
{
    if (!rb || !item) {
        return false;
    }

    bool success = false;

    portENTER_CRITICAL(&rb->lock);

    if (rb->count > 0) {
        memcpy(item, rb->storage + (rb->tail * rb->item_size), rb->item_size);
        rb->tail = (rb->tail + 1) % rb->capacity;
        rb->count--;
        success = true;
    }

    portEXIT_CRITICAL(&rb->lock);

    return success;
}

int ring_buffer_count(const ring_buffer_t *rb)
{
    if (!rb) {
        return 0;
    }
    /* Single word read -- no lock needed on 32-bit MCU */
    return rb->count;
}

bool ring_buffer_is_full(const ring_buffer_t *rb)
{
    if (!rb) {
        return false;
    }
    return rb->count == rb->capacity;
}

bool ring_buffer_is_empty(const ring_buffer_t *rb)
{
    if (!rb) {
        return true;
    }
    return rb->count == 0;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) {
        return;
    }
    free(rb->storage);
    free(rb);
}
