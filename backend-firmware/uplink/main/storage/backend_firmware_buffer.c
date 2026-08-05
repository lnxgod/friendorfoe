#include "backend_firmware_buffer.h"

#include <string.h>

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
               buffer->capacity == FOF_BACKEND_SCANNER_CACHE_CAPACITY;
    }
    if (!psram_alloc) {
        return false;
    }

    buffer->initialized = true;
    buffer->bytes = psram_alloc(
        FOF_BACKEND_SCANNER_CACHE_CAPACITY, alloc_context);
    if (!buffer->bytes) {
        buffer->capacity = 0U;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        memset(&buffer->owner_operation_id, 0,
               sizeof(buffer->owner_operation_id));
        buffer->has_owner_operation_id = false;
#else
        buffer->owner_generation = 0U;
#endif
        buffer->acquired = false;
        return false;
    }
    buffer->capacity = FOF_BACKEND_SCANNER_CACHE_CAPACITY;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    memset(&buffer->owner_operation_id, 0,
           sizeof(buffer->owner_operation_id));
    buffer->has_owner_operation_id = false;
#else
    buffer->owner_generation = 0U;
#endif
    buffer->acquired = false;
    return true;
}

bool backend_firmware_buffer_acquire(
    backend_firmware_buffer_t *buffer,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id)
#else
    uint32_t owner_generation)
#endif
{
    if (!buffer || !buffer->initialized || !buffer->bytes ||
        buffer->capacity != FOF_BACKEND_SCANNER_CACHE_CAPACITY ||
        buffer->acquired
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        || !has_operation_id || operation_id == NULL
#else
        || owner_generation == 0U
#endif
        ) {
        return false;
    }
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    buffer->owner_operation_id = *operation_id;
    buffer->has_owner_operation_id = true;
#else
    buffer->owner_generation = owner_generation;
#endif
    buffer->acquired = true;
    return true;
}

void backend_firmware_buffer_release(
    backend_firmware_buffer_t *buffer,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id)
#else
    uint32_t owner_generation)
#endif
{
    if (!buffer || !buffer->acquired
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        || !buffer->has_owner_operation_id || !has_operation_id ||
        operation_id == NULL || !backend_ota_operation_id_equal(
            &buffer->owner_operation_id, operation_id)
#else
        || owner_generation == 0U ||
        buffer->owner_generation != owner_generation
#endif
        ) {
        return;
    }
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    memset(&buffer->owner_operation_id, 0,
           sizeof(buffer->owner_operation_id));
    buffer->has_owner_operation_id = false;
#else
    buffer->owner_generation = 0U;
#endif
    buffer->acquired = false;
}

uint8_t *backend_firmware_buffer_data(backend_firmware_buffer_t *buffer)
{
    if (!buffer || !buffer->initialized ||
        buffer->capacity != FOF_BACKEND_SCANNER_CACHE_CAPACITY) {
        return NULL;
    }
    return buffer->bytes;
}

size_t backend_firmware_buffer_capacity(
    const backend_firmware_buffer_t *buffer)
{
    if (!buffer || !buffer->initialized || !buffer->bytes ||
        buffer->capacity != FOF_BACKEND_SCANNER_CACHE_CAPACITY) {
        return 0U;
    }
    return buffer->capacity;
}
