#ifndef BACKEND_FIRMWARE_STORE_H
#define BACKEND_FIRMWARE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_hardware_profile.h"
#include "backend_ota_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_FIRMWARE_STORE_PARTITION_LABEL "fw_scanner_be"
#define BACKEND_FIRMWARE_STORE_CAPACITY FOF_BACKEND_SCANNER_CACHE_CAPACITY
#define BACKEND_FIRMWARE_STORE_COPY_CHUNK 512U

typedef struct {
    void *context;
    bool (*erase)(void *context, const char *label, size_t capacity);
    bool (*write)(void *context, const char *label, size_t offset,
                  const uint8_t *bytes, size_t length);
    bool (*read)(void *context, const char *label, size_t offset,
                 uint8_t *output, size_t length);
} backend_firmware_store_partition_t;

typedef enum {
    BACKEND_FIRMWARE_STORE_OK = 0,
    BACKEND_FIRMWARE_STORE_INVALID_ARGUMENT,
    BACKEND_FIRMWARE_STORE_BUSY,
    BACKEND_FIRMWARE_STORE_REJECT_IDENTITY,
    BACKEND_FIRMWARE_STORE_REJECT_CAPACITY,
    BACKEND_FIRMWARE_STORE_IMAGE_INVALID,
    BACKEND_FIRMWARE_STORE_SOURCE_READ_FAILED,
    BACKEND_FIRMWARE_STORE_ERASE_FAILED,
    BACKEND_FIRMWARE_STORE_WRITE_FAILED,
    BACKEND_FIRMWARE_STORE_VERIFY_FAILED,
} backend_firmware_store_result_t;

typedef struct {
    backend_firmware_store_partition_t partition;
    backend_ota_manifest_t manifest;
    backend_ota_read_fn source_read;
    void *source_context;
    backend_ota_image_result_t last_image_result;
    uint32_t relay_session_id;
    uint32_t erase_count;
    uint32_t write_count;
    bool available;
    bool persisted;
    bool relay_claimed;
} backend_firmware_store_t;

void backend_firmware_store_init(
    backend_firmware_store_t *store,
    const backend_firmware_store_partition_t *partition);

/*
 * A non-persisted stage keeps a generation-bound view of the caller-owned,
 * immutable validation arena and performs no partition mutation. A persisted
 * stage copies only an already-completely-validated backend scanner image to
 * fw_scanner_be and validates the copied bytes again before exposing them.
 */
backend_firmware_store_result_t backend_firmware_store_stage(
    backend_firmware_store_t *store,
    const backend_ota_manifest_t *manifest,
    backend_ota_read_fn source_read,
    void *source_context,
    bool persist);

bool backend_firmware_store_matches(
    const backend_firmware_store_t *store,
    const backend_ota_manifest_t *manifest);

bool backend_firmware_store_read(
    const backend_firmware_store_t *store,
    uint32_t generation,
    size_t offset,
    uint8_t *output,
    size_t length);

bool backend_firmware_store_claim_relay(
    backend_firmware_store_t *store,
    uint32_t generation,
    uint32_t session_id);

bool backend_firmware_store_relay_claim_matches(
    const backend_firmware_store_t *store,
    uint32_t generation,
    uint32_t session_id);

void backend_firmware_store_release_relay(
    backend_firmware_store_t *store,
    uint32_t generation,
    uint32_t session_id);

bool backend_firmware_store_discard(
    backend_firmware_store_t *store,
    uint32_t generation);

uint32_t backend_firmware_store_image_mutation_count(
    const backend_firmware_store_t *store);

#ifdef __cplusplus
}
#endif

#endif
