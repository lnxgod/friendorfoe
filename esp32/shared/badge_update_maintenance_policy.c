#include "badge_update_maintenance_policy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(badge_update_maintenance_marker_t) == 160U,
               "update-maintenance RTC marker layout drifted");

static uint32_t marker_crc32(const void *bytes, size_t byte_len)
{
    const uint8_t *p = (const uint8_t *)bytes;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < byte_len; ++i) {
        crc ^= p[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static bool uppercase_hex(uint8_t byte)
{
    return (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
           (byte >= (uint8_t)'A' && byte <= (uint8_t)'F');
}

static bool lowercase_hex(uint8_t byte)
{
    return (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
           (byte >= (uint8_t)'a' && byte <= (uint8_t)'f');
}

static bool bounded_token(const char *value, size_t capacity)
{
    if (!value || capacity < 2U) {
        return false;
    }
    size_t length = strnlen(value, capacity);
    if (length == 0U || length >= capacity) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        uint8_t byte = (uint8_t)value[i];
        if (byte < 0x21U || byte > 0x7eU ||
            byte == (uint8_t)'"' || byte == (uint8_t)'\\') {
            return false;
        }
    }
    return true;
}

static bool partition_token(const char *value)
{
    if (!bounded_token(value, BADGE_UPDATE_UPLINK_PARTITION_CAPACITY)) {
        return false;
    }
    for (size_t i = 0U; value[i] != '\0'; ++i) {
        uint8_t byte = (uint8_t)value[i];
        if (!((byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
              (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
              byte == (uint8_t)'_')) {
            return false;
        }
    }
    return true;
}

static bool canonical_sha256(const char *value)
{
    if (!value ||
        strnlen(value, BADGE_UPDATE_UPLINK_SHA256_CAPACITY) != 64U) {
        return false;
    }
    for (size_t i = 0U; i < 64U; ++i) {
        if (!lowercase_hex((uint8_t)value[i])) {
            return false;
        }
    }
    return value[64] == '\0';
}

bool badge_update_session_valid(const char *session, size_t byte_len)
{
    if (!session || byte_len != BADGE_UPDATE_SESSION_LENGTH) {
        return false;
    }
    bool nonzero = false;
    for (size_t i = 0U; i < BADGE_UPDATE_SESSION_LENGTH; ++i) {
        uint8_t byte = (uint8_t)session[i];
        if (!uppercase_hex(byte)) {
            return false;
        }
        nonzero = nonzero || byte != (uint8_t)'0';
    }
    return nonzero;
}

void badge_update_maintenance_marker_seal(
    badge_update_maintenance_marker_t *marker)
{
    if (!marker) {
        return;
    }
    marker->crc32 = 0U;
    marker->crc32 = marker_crc32(
        marker, offsetof(badge_update_maintenance_marker_t, crc32));
}

bool badge_update_maintenance_marker_valid(
    const badge_update_maintenance_marker_t *marker)
{
    if (!marker ||
        marker->magic != BADGE_UPDATE_MAINTENANCE_MAGIC ||
        marker->version != BADGE_UPDATE_MAINTENANCE_VERSION ||
        marker->size != sizeof(*marker) ||
        marker->reserved != 0U ||
        marker->phase < BADGE_UPDATE_PHASE_PREPARING ||
        marker->phase > BADGE_UPDATE_PHASE_ACTIVE ||
        marker->boot_count > BADGE_UPDATE_MAINTENANCE_MAX_BOOTS ||
        marker->session[BADGE_UPDATE_SESSION_LENGTH] != '\0' ||
        !badge_update_session_valid(
            marker->session, BADGE_UPDATE_SESSION_LENGTH) ||
        marker->crc32 != marker_crc32(
            marker, offsetof(badge_update_maintenance_marker_t, crc32))) {
        return false;
    }
    if ((marker->phase == BADGE_UPDATE_PHASE_PREPARING) !=
        (marker->expected_reboot_generation == 0U)) {
        return false;
    }
    if (marker->uplink_committed > 1U) {
        return false;
    }
    if (marker->uplink_committed == 0U) {
        return marker->uplink_version[0] == '\0' &&
               marker->uplink_sha256[0] == '\0' &&
               marker->uplink_partition[0] == '\0' &&
               marker->uplink_size == 0U &&
               marker->uplink_received == 0U;
    }
    return bounded_token(
               marker->uplink_version,
               BADGE_UPDATE_UPLINK_VERSION_CAPACITY) &&
           canonical_sha256(marker->uplink_sha256) &&
           partition_token(marker->uplink_partition) &&
           marker->uplink_size > 0U &&
           marker->uplink_received == marker->uplink_size;
}

bool badge_update_maintenance_marker_prepare(
    badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len)
{
    if (!marker ||
        !badge_update_session_valid(session, session_byte_len)) {
        return false;
    }
    memset(marker, 0, sizeof(*marker));
    marker->magic = BADGE_UPDATE_MAINTENANCE_MAGIC;
    marker->version = BADGE_UPDATE_MAINTENANCE_VERSION;
    marker->size = (uint16_t)sizeof(*marker);
    marker->phase = BADGE_UPDATE_PHASE_PREPARING;
    memcpy(marker->session, session, BADGE_UPDATE_SESSION_LENGTH);
    marker->session[BADGE_UPDATE_SESSION_LENGTH] = '\0';
    badge_update_maintenance_marker_seal(marker);
    return badge_update_maintenance_marker_valid(marker);
}

bool badge_update_maintenance_session_matches(
    const badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len)
{
    return badge_update_maintenance_marker_valid(marker) &&
           badge_update_session_valid(session, session_byte_len) &&
           memcmp(marker->session, session,
                  BADGE_UPDATE_SESSION_LENGTH) == 0;
}

bool badge_update_maintenance_marker_abort(
    badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len)
{
    if (!badge_update_maintenance_session_matches(
            marker, session, session_byte_len) ||
        (marker->phase != BADGE_UPDATE_PHASE_PREPARING &&
         marker->phase != BADGE_UPDATE_PHASE_ACTIVE)) {
        return false;
    }
    memset(marker, 0, sizeof(*marker));
    return true;
}

badge_update_abort_action_t badge_update_preparing_abort_decide(
    const badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len,
    bool preemption_safe,
    bool reboot_owner_acquired)
{
    if (!badge_update_maintenance_session_matches(
            marker, session, session_byte_len) ||
        marker->phase != BADGE_UPDATE_PHASE_PREPARING) {
        return BADGE_UPDATE_ABORT_CANCEL;
    }
    if (!preemption_safe) {
        return BADGE_UPDATE_ABORT_WAIT_PREEMPTION;
    }
    return reboot_owner_acquired
        ? BADGE_UPDATE_ABORT_CLEAR_AND_REBOOT
        : BADGE_UPDATE_ABORT_WAIT_REBOOT_OWNER;
}

bool badge_update_maintenance_marker_arm_reboot(
    badge_update_maintenance_marker_t *marker,
    uint32_t expected_reboot_generation)
{
    if (!badge_update_maintenance_marker_valid(marker) ||
        expected_reboot_generation == 0U ||
        (marker->phase != BADGE_UPDATE_PHASE_PREPARING &&
         marker->phase != BADGE_UPDATE_PHASE_ACTIVE)) {
        return false;
    }
    marker->phase = BADGE_UPDATE_PHASE_REBOOT_ARMED;
    marker->expected_reboot_generation = expected_reboot_generation;
    badge_update_maintenance_marker_seal(marker);
    return badge_update_maintenance_marker_valid(marker);
}

badge_update_maintenance_boot_action_t
badge_update_maintenance_boot_decide(
    const badge_update_maintenance_marker_t *marker,
    bool expected_software_reset,
    uint32_t expected_reboot_generation,
    bool emergency_safe_mode)
{
    if (!badge_update_maintenance_marker_valid(marker) ||
        marker->phase != BADGE_UPDATE_PHASE_REBOOT_ARMED ||
        !expected_software_reset ||
        emergency_safe_mode ||
        expected_reboot_generation == 0U ||
        marker->expected_reboot_generation !=
            expected_reboot_generation ||
        marker->boot_count >= BADGE_UPDATE_MAINTENANCE_MAX_BOOTS) {
        return BADGE_UPDATE_BOOT_CLEAR;
    }
    return BADGE_UPDATE_BOOT_ENTER;
}

bool badge_update_maintenance_marker_activate(
    badge_update_maintenance_marker_t *marker)
{
    if (!badge_update_maintenance_marker_valid(marker) ||
        marker->phase != BADGE_UPDATE_PHASE_REBOOT_ARMED ||
        marker->boot_count >= BADGE_UPDATE_MAINTENANCE_MAX_BOOTS) {
        return false;
    }
    marker->boot_count++;
    marker->phase = BADGE_UPDATE_PHASE_ACTIVE;
    badge_update_maintenance_marker_seal(marker);
    return badge_update_maintenance_marker_valid(marker);
}

bool badge_update_maintenance_marker_commit_uplink(
    badge_update_maintenance_marker_t *marker,
    const char *version,
    const char *sha256,
    uint32_t size,
    const char *partition)
{
    if (!badge_update_maintenance_marker_valid(marker) ||
        !bounded_token(version, BADGE_UPDATE_UPLINK_VERSION_CAPACITY) ||
        !canonical_sha256(sha256) ||
        size == 0U ||
        !partition_token(partition)) {
        return false;
    }
    marker->uplink_committed = 1U;
    snprintf(marker->uplink_version,
             sizeof(marker->uplink_version), "%s", version);
    snprintf(marker->uplink_sha256,
             sizeof(marker->uplink_sha256), "%s", sha256);
    snprintf(marker->uplink_partition,
             sizeof(marker->uplink_partition), "%s", partition);
    marker->uplink_size = size;
    marker->uplink_received = size;
    badge_update_maintenance_marker_seal(marker);
    return badge_update_maintenance_marker_valid(marker);
}

bool badge_update_maintenance_marker_clear_uplink(
    badge_update_maintenance_marker_t *marker)
{
    if (!badge_update_maintenance_marker_valid(marker) ||
        marker->phase != BADGE_UPDATE_PHASE_ACTIVE) {
        return false;
    }
    marker->uplink_committed = 0U;
    memset(marker->uplink_version, 0, sizeof(marker->uplink_version));
    memset(marker->uplink_sha256, 0, sizeof(marker->uplink_sha256));
    memset(marker->uplink_partition, 0, sizeof(marker->uplink_partition));
    marker->uplink_size = 0U;
    marker->uplink_received = 0U;
    badge_update_maintenance_marker_seal(marker);
    return badge_update_maintenance_marker_valid(marker);
}

badge_update_prepare_action_t badge_update_prepare_decide(
    bool firmware_operation_active,
    bool coordinator_suspended,
    bool radio_quiesced)
{
    if (firmware_operation_active) {
        return BADGE_UPDATE_PREPARE_WAITING_FOR_OWNER;
    }
    if (!coordinator_suspended) {
        return BADGE_UPDATE_PREPARE_BUSY;
    }
    return radio_quiesced
        ? BADGE_UPDATE_PREPARE_REBOOT_QUIESCED
        : BADGE_UPDATE_PREPARE_REBOOT_SAFE;
}

bool badge_update_maintenance_inactivity_due(
    uint32_t now_ms,
    uint32_t last_activity_ms)
{
    return (uint32_t)(now_ms - last_activity_ms) >=
           BADGE_UPDATE_MAINTENANCE_INACTIVITY_MS;
}

bool badge_update_maintenance_health_satisfied(
    uint32_t uptime_ms,
    bool display_alive,
    bool completed_usb_response,
    bool scanner_uart_zero_alive,
    bool scanner_uart_one_alive,
    bool emergency_safe_mode,
    uint32_t free_internal_heap,
    uint32_t largest_internal_block)
{
    return uptime_ms >= 10000U &&
           display_alive &&
           completed_usb_response &&
           scanner_uart_zero_alive &&
           scanner_uart_one_alive &&
           !emergency_safe_mode &&
           free_internal_heap >= 24576U &&
           largest_internal_block >= 16384U;
}

badge_update_ota_begin_admission_t
badge_update_uplink_ota_begin_admission_decide(
    bool canary_build,
    bool maintenance_active,
    size_t member_count,
    bool session_present,
    bool session_is_string,
    bool session_matches)
{
    if (!canary_build || !maintenance_active) {
        return member_count == 10U && !session_present
            ? BADGE_UPDATE_OTA_BEGIN_ADMIT
            : BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION;
    }
    return member_count == 11U &&
           session_present &&
           session_is_string &&
           session_matches
        ? BADGE_UPDATE_OTA_BEGIN_ADMIT
        : BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH;
}

badge_update_ota_begin_admission_t
badge_update_scanner_stage_begin_admission_decide(
    bool canary_build,
    bool maintenance_active,
    size_t member_count,
    bool session_present,
    bool session_is_string,
    bool session_matches)
{
    if (!canary_build || !maintenance_active) {
        return member_count == 11U && !session_present
            ? BADGE_UPDATE_OTA_BEGIN_ADMIT
            : BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION;
    }
    return member_count == 12U &&
           session_present &&
           session_is_string &&
           session_matches
        ? BADGE_UPDATE_OTA_BEGIN_ADMIT
        : BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH;
}
