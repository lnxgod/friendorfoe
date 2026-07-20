#include "firmware_relay_policy.h"

#include "firmware_image_contract.h"
#include "firmware_version_order.h"

#include <stddef.h>
#include <string.h>

static bool strings_equal(const char *left, const char *right)
{
    return left && right && strcmp(left, right) == 0;
}

static char lowercase_hex(char value)
{
    return value >= 'A' && value <= 'F'
        ? (char)(value - 'A' + 'a') : value;
}

static bool sha256_equal(const char *left, const char *right)
{
    if (!fof_firmware_sha256_hex_is_valid(left) ||
        !fof_firmware_sha256_hex_is_valid(right)) {
        return false;
    }
    for (size_t i = 0; i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
        if (lowercase_hex(left[i]) != lowercase_hex(right[i])) {
            return false;
        }
    }
    return true;
}

static bool receipt_type_and_session_match(
    const fof_firmware_receipt_view_t *receipt,
    const char *type,
    const char *session_id)
{
    return receipt &&
        strings_equal(receipt->type, type) &&
        strings_equal(receipt->session_id, session_id) &&
        session_id[0] != '\0';
}

bool fof_firmware_legacy_ack_matches(
    const fof_firmware_receipt_view_t *receipt,
    const char *session_id)
{
    return receipt_type_and_session_match(
        receipt, "ota_ack", session_id);
}

bool fof_firmware_legacy_progress_matches(
    const fof_firmware_receipt_view_t *receipt,
    const char *session_id,
    uint32_t staged_size)
{
    return receipt_type_and_session_match(
               receipt, "ota_progress", session_id) &&
        receipt->has_received && receipt->received == staged_size &&
        receipt->has_total && receipt->total == staged_size &&
        receipt->has_percent && receipt->percent == 100U;
}

bool fof_firmware_legacy_done_matches(
    const fof_firmware_receipt_view_t *receipt,
    const char *session_id,
    uint32_t staged_size)
{
    return receipt_type_and_session_match(
               receipt, "ota_done", session_id) &&
        receipt->has_received && receipt->received == staged_size;
}

bool fof_firmware_stop_ack_matches(
    const fof_firmware_receipt_view_t *receipt)
{
    return receipt && strings_equal(receipt->type, "stop_ack");
}

bool fof_firmware_strict_receipt_matches(
    const fof_firmware_receipt_view_t *receipt,
    const fof_firmware_strict_receipt_expectation_t *expected)
{
    return receipt && expected &&
        receipt_type_and_session_match(
            receipt, expected->type, expected->session_id) &&
        strings_equal(receipt->target_version,
                      expected->target_version) &&
        strings_equal(receipt->firmware_name,
                      expected->firmware_name) &&
        strings_equal(receipt->project, expected->project) &&
        strings_equal(receipt->hardware, expected->hardware) &&
        sha256_equal(receipt->sha256, expected->sha256) &&
        receipt->has_generation &&
        receipt->generation == expected->generation &&
        receipt->has_size && receipt->size == expected->size &&
        receipt->has_crc32 && receipt->crc32 == expected->crc32 &&
        receipt->has_allow_same_version &&
        receipt->allow_same_version == expected->allow_same_version &&
        receipt->has_received &&
        receipt->received == expected->received;
}

bool fof_firmware_legacy_relay_authorized(
    const fof_legacy_relay_authorization_view_t *authorization)
{
    if (!authorization || !authorization->automatic_bound) {
        return false;
    }
    const fof_legacy_identity_view_t *identity =
        &authorization->identity;
    const fof_legacy_manifest_view_t *manifest =
        &authorization->manifest;

    return identity->received &&
        fof_firmware_hardware_id_is_canonical(
            authorization->bound_hardware_id) &&
        fof_firmware_hardware_id_is_canonical(identity->hardware_id) &&
        strings_equal(identity->hardware_id,
                      authorization->bound_hardware_id) &&
        strings_equal(identity->version,
                      FOF_LEGACY_READY_BOOTSTRAP_VERSION) &&
        strings_equal(identity->board, FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(identity->firmware_name,
                      FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(identity->project,
                      FOF_LEGACY_READY_BADGE_PROJECT) &&
        strings_equal(identity->hardware,
                      FOF_LEGACY_READY_BADGE_HARDWARE) &&
        strings_equal(manifest->target,
                      FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(manifest->project,
                      FOF_LEGACY_READY_BADGE_PROJECT) &&
        strings_equal(manifest->hardware,
                      FOF_LEGACY_READY_BADGE_HARDWARE) &&
        strings_equal(identity->board, manifest->target) &&
        strings_equal(identity->firmware_name, manifest->target) &&
        strings_equal(identity->project, manifest->project) &&
        strings_equal(identity->hardware, manifest->hardware) &&
        fof_firmware_sha256_hex_is_valid(manifest->sha256) &&
        manifest->size > 0U &&
        fof_firmware_version_compare(
            manifest->version, identity->version) == FOF_VERSION_NEWER;
}

bool fof_firmware_post_reboot_boot_id_proved(uint32_t before_boot_id,
                                             uint32_t after_boot_id)
{
    return after_boot_id != 0U &&
           (before_boot_id == 0U || after_boot_id != before_boot_id);
}
