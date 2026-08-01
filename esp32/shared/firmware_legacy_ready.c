#include "firmware_legacy_ready.h"

#include "firmware_image_contract.h"
#include "firmware_version_order.h"

#include <stddef.h>
#include <string.h>

static bool hardware_id_hex_digit_is_valid(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

bool fof_firmware_hardware_id_is_canonical(const char *hardware_id)
{
    if (!hardware_id || strlen(hardware_id) != 17U) {
        return false;
    }

    for (size_t i = 0; i < 17U; ++i) {
        if ((i + 1U) % 3U == 0U) {
            if (hardware_id[i] != ':') {
                return false;
            }
        } else if (!hardware_id_hex_digit_is_valid(hardware_id[i])) {
            return false;
        }
    }
    return true;
}

static bool strings_equal(const char *left, const char *right)
{
    return left && right && strcmp(left, right) == 0;
}

bool fof_firmware_legacy_ready_authorized(
    const fof_legacy_ready_view_t *ready,
    const fof_legacy_identity_view_t *identity,
    const fof_legacy_manifest_view_t *manifest)
{
    return ready && identity && manifest &&
        ready->strict_fields_absent && identity->received &&
        fof_firmware_hardware_id_is_canonical(identity->hardware_id) &&
        strings_equal(ready->current_version,
                      FOF_LEGACY_READY_BOOTSTRAP_VERSION) &&
        strings_equal(identity->version, ready->current_version) &&
        strings_equal(ready->board, FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(identity->board, FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(identity->firmware_name,
                      FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(manifest->target, FOF_LEGACY_READY_BADGE_TARGET) &&
        strings_equal(identity->project, FOF_LEGACY_READY_BADGE_PROJECT) &&
        strings_equal(manifest->project, FOF_LEGACY_READY_BADGE_PROJECT) &&
        strings_equal(identity->hardware, FOF_LEGACY_READY_BADGE_HARDWARE) &&
        strings_equal(manifest->hardware, FOF_LEGACY_READY_BADGE_HARDWARE) &&
        strings_equal(ready->board, manifest->target) &&
        strings_equal(identity->board, manifest->target) &&
        strings_equal(identity->firmware_name, manifest->target) &&
        strings_equal(identity->project, manifest->project) &&
        strings_equal(identity->hardware, manifest->hardware) &&
        strings_equal(ready->target_version, manifest->version) &&
        ready->size == manifest->size && ready->crc32 == manifest->crc32 &&
        fof_firmware_sha256_hex_is_valid(manifest->sha256) &&
        fof_firmware_version_compare(manifest->version,
                                     ready->current_version) ==
            FOF_VERSION_NEWER;
}
