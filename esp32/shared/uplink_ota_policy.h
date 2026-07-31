#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "firmware_image_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_OTA_TARGET "uplink-s3-fof_badge"
#define UPLINK_OTA_PROJECT "fof_badge_uplink"
#define UPLINK_OTA_HARDWARE "seeed_xiao_esp32s3"
#define UPLINK_OTA_MIN_IMAGE_BYTES 1024U
#define UPLINK_OTA_CREDIT_BYTES 4096U

typedef enum {
    UPLINK_OTA_IDLE = 0,
    UPLINK_OTA_RECEIVING,
    UPLINK_OTA_VERIFYING,
    UPLINK_OTA_COMMITTED,
    UPLINK_OTA_ERROR,
} uplink_ota_state_t;

typedef struct {
    char target[33];
    char project[33];
    char hardware[33];
    char version[33];
    char sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
    uint32_t size;
    uint32_t crc32;
    bool recovery_rewrite_same_version;
} uplink_ota_manifest_t;

bool uplink_ota_policy_manifest_allowed(
    const uplink_ota_manifest_t *manifest,
    const char *running_version,
    uint32_t partition_size,
    bool running_pending_verify,
    const char **error); /* error is an optional stable snake_case code. */

typedef struct {
    uplink_ota_state_t state;
    uplink_ota_manifest_t manifest;
    uint32_t durable_written;
    uint32_t next_credit_at;
    bool credit_outstanding;
    bool verification_passed;
    const char *last_error;
} uplink_ota_policy_session_t;

void uplink_ota_policy_init(uplink_ota_policy_session_t *session);
bool uplink_ota_policy_begin(
    uplink_ota_policy_session_t *session,
    const uplink_ota_manifest_t *manifest,
    const char *running_version,
    uint32_t partition_size,
    bool running_pending_verify,
    const char **error);
bool uplink_ota_policy_note_durable_write(
    uplink_ota_policy_session_t *session,
    uint32_t length,
    uint32_t transport_received,
    const char **error);
bool uplink_ota_policy_grant_credit(
    uplink_ota_policy_session_t *session,
    uint32_t *credit_bytes,
    uint32_t *durable_received,
    const char **error);
bool uplink_ota_policy_verify_complete(
    uplink_ota_policy_session_t *session,
    uint32_t transport_received,
    uint32_t computed_crc32,
    const char *computed_sha256,
    const fof_firmware_image_identity_t *embedded_identity,
    bool target_marker_seen,
    bool hardware_marker_seen,
    const char **error);
bool uplink_ota_policy_mark_committed(
    uplink_ota_policy_session_t *session,
    const char **error);
void uplink_ota_policy_fail(uplink_ota_policy_session_t *session,
                            const char *error);

#ifdef __cplusplus
}
#endif
