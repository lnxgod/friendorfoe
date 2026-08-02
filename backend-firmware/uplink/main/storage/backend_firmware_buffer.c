#include "backend_firmware_buffer.h"

bool backend_firmware_buffer_init_once(
    backend_firmware_buffer_t *buffer,
    backend_firmware_alloc_fn psram_alloc,
    void *alloc_context)
{
    if (!buffer) {
        return false;
    }
    if (buffer->initialized) {
        return buffer->bytes != NULL &&
               buffer->capacity == BACKEND_FIRMWARE_BUFFER_CAPACITY;
    }
    if (!psram_alloc) {
        return false;
    }

    buffer->initialized = true;
    buffer->bytes = psram_alloc(
        BACKEND_FIRMWARE_BUFFER_CAPACITY, alloc_context);
    if (!buffer->bytes) {
        buffer->capacity = 0U;
        buffer->owner_generation = 0U;
        buffer->acquired = false;
        return false;
    }
    buffer->capacity = BACKEND_FIRMWARE_BUFFER_CAPACITY;
    buffer->owner_generation = 0U;
    buffer->acquired = false;
    return true;
}

bool backend_firmware_buffer_acquire(
    backend_firmware_buffer_t *buffer,
    uint32_t owner_generation)
{
    if (!buffer || !buffer->initialized || !buffer->bytes ||
        buffer->capacity != BACKEND_FIRMWARE_BUFFER_CAPACITY ||
        buffer->acquired || owner_generation == 0U) {
        return false;
    }
    buffer->owner_generation = owner_generation;
    buffer->acquired = true;
    return true;
}

void backend_firmware_buffer_release(
    backend_firmware_buffer_t *buffer,
    uint32_t owner_generation)
{
    if (!buffer || !buffer->acquired || owner_generation == 0U ||
        buffer->owner_generation != owner_generation) {
        return;
    }
    buffer->owner_generation = 0U;
    buffer->acquired = false;
}

uint8_t *backend_firmware_buffer_data(backend_firmware_buffer_t *buffer)
{
    if (!buffer || !buffer->initialized ||
        buffer->capacity != BACKEND_FIRMWARE_BUFFER_CAPACITY) {
        return NULL;
    }
    return buffer->bytes;
}

size_t backend_firmware_buffer_capacity(
    const backend_firmware_buffer_t *buffer)
{
    if (!buffer || !buffer->initialized || !buffer->bytes ||
        buffer->capacity != BACKEND_FIRMWARE_BUFFER_CAPACITY) {
        return 0U;
    }
    return buffer->capacity;
}
