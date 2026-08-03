#ifndef BACKEND_OTA_OPERATION_ID_H
#define BACKEND_OTA_OPERATION_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_hardware_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE) && \
    !defined(FOF_BACKEND_PROFILE_BADGE_LITE)

#define BACKEND_OTA_OPERATION_ID_HEX_LENGTH 32U

typedef struct {
    uint8_t bytes[16];
} backend_ota_operation_id_t;

bool backend_ota_operation_id_decode(
    const char *encoded, backend_ota_operation_id_t *out);
bool backend_ota_operation_id_encode(
    const backend_ota_operation_id_t *operation_id,
    char *out,
    size_t capacity);
bool backend_ota_operation_id_equal(
    const backend_ota_operation_id_t *left,
    const backend_ota_operation_id_t *right);
bool backend_ota_operation_id_is_zero(
    const backend_ota_operation_id_t *operation_id);

#elif defined(FOF_BACKEND_PROFILE_BADGE_LITE) && \
      !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)

typedef uint32_t backend_ota_operation_id_t;

static inline bool backend_ota_operation_id_equal(
    const backend_ota_operation_id_t *left,
    const backend_ota_operation_id_t *right)
{
    return left != NULL && right != NULL && *left == *right;
}

static inline bool backend_ota_operation_id_is_zero(
    const backend_ota_operation_id_t *operation_id)
{
    return operation_id != NULL && *operation_id == 0U;
}

#else
#error "select exactly one backend hardware profile"
#endif

#ifdef __cplusplus
}
#endif

#endif
