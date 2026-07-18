#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_LEGACY_READY_BOOTSTRAP_VERSION \
    "0.64.68-badge-live-follow"
#define FOF_LEGACY_READY_BADGE_TARGET \
    "scanner-s3-combo-fof_badge"
#define FOF_LEGACY_READY_BADGE_PROJECT \
    "fof_badge_scanner"
#define FOF_LEGACY_READY_BADGE_HARDWARE \
    "seeed_xiao_esp32s3"

typedef struct {
    bool strict_fields_absent;
    const char *board;
    const char *current_version;
    const char *target_version;
    uint32_t size;
    uint32_t crc32;
} fof_legacy_ready_view_t;

typedef struct {
    bool received;
    const char *version;
    const char *board;
    const char *firmware_name;
    const char *project;
    const char *hardware;
    const char *hardware_id;
} fof_legacy_identity_view_t;

typedef struct {
    const char *target;
    const char *version;
    const char *project;
    const char *hardware;
    const char *sha256;
    uint32_t size;
    uint32_t crc32;
} fof_legacy_manifest_view_t;

bool fof_firmware_legacy_ready_authorized(
    const fof_legacy_ready_view_t *ready,
    const fof_legacy_identity_view_t *identity,
    const fof_legacy_manifest_view_t *manifest);

#ifdef __cplusplus
}
#endif
