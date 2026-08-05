#include "backend_identity.h"

#include <string.h>

static const backend_firmware_identity_t BACKEND_UPLINK_IDENTITY = {
    .product_family = FOF_BACKEND_PRODUCT_FAMILY,
    .firmware_line = FOF_BACKEND_FIRMWARE_LINE,
    .component = "uplink",
    .target = FOF_BACKEND_UPLINK_TARGET,
    .project = FOF_BACKEND_UPLINK_PROJECT,
    .hardware = FOF_BACKEND_HARDWARE,
    .version = FOF_VERSION_BACKEND,
};

static const backend_firmware_identity_t BACKEND_SCANNER_IDENTITY = {
    .product_family = FOF_BACKEND_PRODUCT_FAMILY,
    .firmware_line = FOF_BACKEND_FIRMWARE_LINE,
    .component = "scanner",
    .target = FOF_BACKEND_SCANNER_TARGET,
    .project = FOF_BACKEND_SCANNER_PROJECT,
    .hardware = FOF_BACKEND_HARDWARE,
    .version = FOF_VERSION_BACKEND,
};

static bool zero_padded_field_matches(
    const char *field,
    size_t field_size,
    const char *expected)
{
    size_t expected_size = strlen(expected);
    if (expected_size >= field_size ||
        memcmp(field, expected, expected_size) != 0) {
        return false;
    }
    for (size_t index = expected_size; index < field_size; ++index) {
        if (field[index] != '\0') {
            return false;
        }
    }
    return true;
}

const backend_firmware_identity_t *
backend_identity_for_image(backend_image_kind_t kind)
{
    switch (kind) {
    case BACKEND_IMAGE_UPLINK:
        return &BACKEND_UPLINK_IDENTITY;
    case BACKEND_IMAGE_SCANNER:
        return &BACKEND_SCANNER_IDENTITY;
    default:
        return NULL;
    }
}

bool backend_identity_matches(
    const backend_firmware_identity_t *expected,
    const char *target,
    const char *project,
    const char *hardware)
{
    return expected != NULL && target != NULL && project != NULL &&
           hardware != NULL && strcmp(expected->target, target) == 0 &&
           strcmp(expected->project, project) == 0 &&
           strcmp(expected->hardware, hardware) == 0;
}

uint32_t backend_identity_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_C(0xFFFFFFFF);

    if (bytes == NULL && size != 0) {
        return 0;
    }
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & UINT32_C(1));
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

bool backend_identity_record_build(
    backend_image_kind_t kind,
    backend_embedded_identity_record_t *out)
{
    const backend_firmware_identity_t *identity = backend_identity_for_image(kind);
    if (identity == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->magic = FOF_BACKEND_IDENTITY_MAGIC;
    out->schema = FOF_BACKEND_IDENTITY_SCHEMA;
    out->image_kind = (uint16_t)kind;
    memcpy(out->target, identity->target, strlen(identity->target));
    memcpy(out->project, identity->project, strlen(identity->project));
    memcpy(out->hardware, identity->hardware, strlen(identity->hardware));
    memcpy(out->version, identity->version, strlen(identity->version));
    out->crc32 = backend_identity_crc32(
        out, offsetof(backend_embedded_identity_record_t, crc32));
    return true;
}

bool backend_identity_record_validate(
    const backend_embedded_identity_record_t *record)
{
    const backend_firmware_identity_t *identity;
    if (record == NULL || record->magic != FOF_BACKEND_IDENTITY_MAGIC ||
        record->schema != FOF_BACKEND_IDENTITY_SCHEMA ||
        record->image_kind > BACKEND_IMAGE_SCANNER ||
        record->crc32 != backend_identity_crc32(
            record, offsetof(backend_embedded_identity_record_t, crc32))) {
        return false;
    }

    identity = backend_identity_for_image((backend_image_kind_t)record->image_kind);
    return identity != NULL && zero_padded_field_matches(
               record->target, sizeof(record->target), identity->target) &&
           zero_padded_field_matches(
               record->project, sizeof(record->project), identity->project) &&
           zero_padded_field_matches(
               record->hardware, sizeof(record->hardware), identity->hardware) &&
           zero_padded_field_matches(
               record->version, sizeof(record->version), identity->version);
}
