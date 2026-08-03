#ifndef BACKEND_OTA_IDENTITY_H
#define BACKEND_OTA_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    uint32_t image_size;
    uint32_t crc32;
    char sha256[65];
    uint32_t generation;
    bool allow_same_version;
} backend_ota_manifest_t;

typedef enum {
    BACKEND_OTA_ADMIT = 0,
    BACKEND_OTA_REJECT_ARGUMENT,
    BACKEND_OTA_REJECT_IDENTITY,
    BACKEND_OTA_REJECT_VERSION,
    BACKEND_OTA_REJECT_DIGEST,
    BACKEND_OTA_REJECT_SIZE,
    BACKEND_OTA_REJECT_GENERATION,
    BACKEND_OTA_REJECT_CAPACITY,
} backend_ota_admission_result_t;

typedef enum {
    BACKEND_OTA_IMAGE_OK = 0,
    BACKEND_OTA_IMAGE_READ_ERROR,
    BACKEND_OTA_IMAGE_FORMAT_ERROR,
    BACKEND_OTA_IMAGE_DIGEST_MISMATCH,
    BACKEND_OTA_IMAGE_CRC_MISMATCH,
    BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH,
    BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
} backend_ota_image_result_t;

/*
 * Return true only when the complete requested range belongs to the staged
 * artifact. A read beginning at the artifact's exact end must return false;
 * the validator uses that bound to reject bytes trailing manifest.image_size.
 */
typedef bool (*backend_ota_read_fn)(
    void *context, size_t offset, uint8_t *output, size_t length);

/* Shared one-shot SHA-256 for canonical OTA protocol bodies. */
bool backend_ota_sha256(
    const uint8_t *bytes, size_t length, uint8_t output[32]);

backend_ota_admission_result_t backend_ota_manifest_admit(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    const char *running_version,
    size_t partition_capacity);

/*
 * Decode the exact /nodes/firmware/latest/{backend-target} response into an
 * immutable manifest. Catalog generation and recovery policy are local,
 * trusted inputs because neither is delegated to HTTP metadata.
 */
bool backend_ota_manifest_decode_metadata(
    const char *json,
    size_t length,
    uint32_t catalog_generation,
    bool allow_same_version,
    backend_ota_manifest_t *out);

backend_ota_image_result_t backend_ota_image_validate(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    backend_ota_read_fn read_fn,
    void *read_context);

#ifdef __cplusplus
}
#endif

#endif
