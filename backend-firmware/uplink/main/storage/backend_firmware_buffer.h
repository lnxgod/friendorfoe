#ifndef BACKEND_FIRMWARE_BUFFER_H
#define BACKEND_FIRMWARE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_hardware_profile.h"
#include "backend_ota_operation_id.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_FIRMWARE_BUFFER_CAPACITY FOF_BACKEND_SCANNER_CACHE_CAPACITY

typedef void *(*backend_firmware_alloc_fn)(size_t size, void *context);

typedef struct {
    uint8_t *bytes;
    size_t capacity;
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    backend_ota_operation_id_t owner_operation_id;
    bool has_owner_operation_id;
#else
    uint32_t owner_generation;
#endif
    bool initialized;
    bool acquired;
} backend_firmware_buffer_t;

bool backend_firmware_buffer_init_once(
    backend_firmware_buffer_t *buffer,
    backend_firmware_alloc_fn psram_alloc,
    void *alloc_context);
bool backend_firmware_buffer_acquire(
    backend_firmware_buffer_t *buffer,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id);
#else
    uint32_t owner_generation);
#endif
void backend_firmware_buffer_release(
    backend_firmware_buffer_t *buffer,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    bool has_operation_id,
    const backend_ota_operation_id_t *operation_id);
#else
    uint32_t owner_generation);
#endif
uint8_t *backend_firmware_buffer_data(backend_firmware_buffer_t *buffer);
size_t backend_firmware_buffer_capacity(
    const backend_firmware_buffer_t *buffer);

#ifdef __cplusplus
}
#endif

#endif
