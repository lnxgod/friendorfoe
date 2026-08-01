#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "firmware_legacy_ready.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *type;
    const char *session_id;
    const char *target_version;
    const char *firmware_name;
    const char *project;
    const char *hardware;
    const char *sha256;
    bool has_generation;
    uint32_t generation;
    bool has_size;
    uint32_t size;
    bool has_crc32;
    uint32_t crc32;
    bool has_allow_same_version;
    bool allow_same_version;
    bool has_received;
    uint32_t received;
    bool has_total;
    uint32_t total;
    bool has_percent;
    uint32_t percent;
} fof_firmware_receipt_view_t;

typedef struct {
    const char *type;
    const char *session_id;
    const char *target_version;
    const char *firmware_name;
    const char *project;
    const char *hardware;
    const char *sha256;
    uint32_t generation;
    uint32_t size;
    uint32_t crc32;
    bool allow_same_version;
    uint32_t received;
} fof_firmware_strict_receipt_expectation_t;

typedef struct {
    bool automatic_bound;
    const char *bound_hardware_id;
    fof_legacy_identity_view_t identity;
    fof_legacy_manifest_view_t manifest;
} fof_legacy_relay_authorization_view_t;

typedef struct {
    uint32_t expected_generation;
    const char *expected_hardware_id;
    uint32_t staged_generation;
    bool live_identity_received;
    const char *live_hardware_id;
} fof_firmware_bound_relay_view_t;

bool fof_firmware_legacy_ack_matches(
    const fof_firmware_receipt_view_t *receipt,
    const char *session_id);

bool fof_firmware_legacy_progress_matches(
    const fof_firmware_receipt_view_t *receipt,
    const char *session_id,
    uint32_t staged_size);

bool fof_firmware_legacy_done_matches(
    const fof_firmware_receipt_view_t *receipt,
    const char *session_id,
    uint32_t staged_size);

bool fof_firmware_stop_ack_matches(
    const fof_firmware_receipt_view_t *receipt);

bool fof_firmware_strict_receipt_matches(
    const fof_firmware_receipt_view_t *receipt,
    const fof_firmware_strict_receipt_expectation_t *expected);

bool fof_firmware_legacy_relay_authorized(
    const fof_legacy_relay_authorization_view_t *authorization);

bool fof_firmware_bound_relay_request_matches(
    const fof_firmware_bound_relay_view_t *request);

/**
 * Prove that a scanner crossed a reboot boundary after a relay.
 *
 * A pre-update value of zero is the migration case for firmware that predates
 * boot IDs.  The updated scanner must still report a non-zero value.  Once a
 * scanner supports boot IDs, every subsequent relay must observe a different
 * value.
 */
bool fof_firmware_post_reboot_boot_id_proved(uint32_t before_boot_id,
                                             uint32_t after_boot_id);

#ifdef __cplusplus
}
#endif
