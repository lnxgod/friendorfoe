#include "backend_ota_operation_id.h"

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE) && \
    !defined(FOF_BACKEND_PROFILE_BADGE_LITE)

#include <string.h>

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

bool backend_ota_operation_id_decode(
    const char *encoded, backend_ota_operation_id_t *out)
{
    if (encoded == NULL || out == NULL ||
        strlen(encoded) != BACKEND_OTA_OPERATION_ID_HEX_LENGTH) {
        return false;
    }

    backend_ota_operation_id_t decoded;
    for (size_t index = 0U; index < sizeof(decoded.bytes); ++index) {
        const int high = hex_value(encoded[index * 2U]);
        const int low = hex_value(encoded[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        decoded.bytes[index] = (uint8_t)((high << 4U) | low);
    }
    *out = decoded;
    return true;
}

bool backend_ota_operation_id_encode(
    const backend_ota_operation_id_t *operation_id,
    char *out,
    size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    if (operation_id == NULL || out == NULL ||
        capacity < BACKEND_OTA_OPERATION_ID_HEX_LENGTH + 1U) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(operation_id->bytes); ++index) {
        const uint8_t value = operation_id->bytes[index];
        out[index * 2U] = hex[value >> 4U];
        out[index * 2U + 1U] = hex[value & UINT8_C(0x0F)];
    }
    out[BACKEND_OTA_OPERATION_ID_HEX_LENGTH] = '\0';
    return true;
}

bool backend_ota_operation_id_equal(
    const backend_ota_operation_id_t *left,
    const backend_ota_operation_id_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    uint8_t different = 0U;
    for (size_t index = 0U; index < sizeof(left->bytes); ++index) {
        different |= (uint8_t)(left->bytes[index] ^ right->bytes[index]);
    }
    return different == 0U;
}

bool backend_ota_operation_id_is_zero(
    const backend_ota_operation_id_t *operation_id)
{
    if (operation_id == NULL) {
        return false;
    }
    uint8_t combined = 0U;
    for (size_t index = 0U; index < sizeof(operation_id->bytes); ++index) {
        combined |= operation_id->bytes[index];
    }
    return combined == 0U;
}

#endif
