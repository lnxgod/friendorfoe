#ifndef BACKEND_IDENTITY_H
#define BACKEND_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_version.h"

#define FOF_BACKEND_UPLINK_TARGET "uplink-s3-backend"
#define FOF_BACKEND_UPLINK_PROJECT "fof_backend_uplink"
#define FOF_BACKEND_SCANNER_TARGET "scanner-s3-combo-backend"
#define FOF_BACKEND_SCANNER_PROJECT "fof_backend_scanner"
#define FOF_BACKEND_HARDWARE "seeed_xiao_esp32s3"

#define FOF_BACKEND_IDENTITY_MAGIC UINT32_C(0x42464F46)
#define FOF_BACKEND_IDENTITY_SCHEMA UINT16_C(1)

typedef enum {
    BACKEND_IMAGE_UPLINK = 0,
    BACKEND_IMAGE_SCANNER = 1,
} backend_image_kind_t;

typedef struct {
    const char *target;
    const char *project;
    const char *hardware;
    const char *version;
} backend_firmware_identity_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t image_kind;
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    uint32_t crc32;
} backend_embedded_identity_record_t;

_Static_assert(sizeof(backend_embedded_identity_record_t) == 164,
               "backend identity record must be exactly 164 bytes");
_Static_assert(offsetof(backend_embedded_identity_record_t, magic) == 0,
               "unexpected identity magic offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, schema) == 4,
               "unexpected identity schema offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, image_kind) == 6,
               "unexpected identity image kind offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, target) == 8,
               "unexpected identity target offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, project) == 48,
               "unexpected identity project offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, hardware) == 88,
               "unexpected identity hardware offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, version) == 128,
               "unexpected identity version offset");
_Static_assert(offsetof(backend_embedded_identity_record_t, crc32) == 160,
               "unexpected identity CRC offset");

const backend_firmware_identity_t *
backend_identity_for_image(backend_image_kind_t kind);

bool backend_identity_matches(
    const backend_firmware_identity_t *expected,
    const char *target,
    const char *project,
    const char *hardware);

uint32_t backend_identity_crc32(const void *data, size_t size);

bool backend_identity_record_build(
    backend_image_kind_t kind,
    backend_embedded_identity_record_t *out);

bool backend_identity_record_validate(
    const backend_embedded_identity_record_t *record);

extern const backend_embedded_identity_record_t fof_backend_embedded_identity;

#endif
