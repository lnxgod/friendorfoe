#include "uplink_ota_policy.h"

#include "firmware_version_order.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(UPLINK_OTA_TARGET) <= 33,
               "uplink OTA target must fit manifest");
_Static_assert(sizeof(UPLINK_OTA_PROJECT) <= 33,
               "uplink OTA project must fit manifest");
_Static_assert(sizeof(UPLINK_OTA_HARDWARE) <= 33,
               "uplink OTA hardware must fit manifest");

/*
 * This pure policy deliberately does not own stream framing, idle timeouts,
 * leftover bytes, binary abort, terminal replies, or transport ownership.
 */

static const char ERR_INVALID_ARGUMENT[] = "invalid_argument";
static const char ERR_PENDING_VERIFY[] = "pending_verify";
static const char ERR_UNTERMINATED_FIELD[] = "unterminated_field";
static const char ERR_WRONG_TARGET[] = "target_mismatch";
static const char ERR_WRONG_PROJECT[] = "project_mismatch";
static const char ERR_WRONG_HARDWARE[] = "hardware_mismatch";
static const char ERR_INVALID_VERSION[] = "invalid_version";
static const char ERR_DOWNGRADE[] = "downgrade_forbidden";
static const char ERR_UNORDERED_VERSION[] = "unordered_version";
static const char ERR_EQUAL_VERSION[] = "equal_version_requires_recovery";
static const char ERR_IMAGE_TOO_SMALL[] = "image_too_small";
static const char ERR_IMAGE_TOO_LARGE[] = "image_too_large";
static const char ERR_INVALID_CRC[] = "invalid_crc32";
static const char ERR_INVALID_SHA[] = "invalid_sha256";
static const char ERR_INVALID_STATE[] = "invalid_state";
static const char ERR_CREDIT_OUTSTANDING[] = "credit_outstanding";
static const char ERR_IMAGE_COMPLETE[] = "image_complete";
static const char ERR_NO_CREDIT[] = "credit_required";
static const char ERR_ZERO_WRITE[] = "zero_write";
static const char ERR_COUNTER_OVERFLOW[] = "counter_overflow";
static const char ERR_TRANSPORT_MISMATCH[] = "transport_count_mismatch";
static const char ERR_CREDIT_OVERSHOOT[] = "credit_overshoot";
static const char ERR_IMAGE_OVERSHOOT[] = "image_overshoot";
static const char ERR_INCOMPLETE_IMAGE[] = "incomplete_image";
static const char ERR_CRC_MISMATCH[] = "crc32_mismatch";
static const char ERR_SHA_MISMATCH[] = "sha256_mismatch";
static const char ERR_IDENTITY_MISSING[] = "embedded_identity_missing";
static const char ERR_IDENTITY_FIELD[] = "embedded_identity_invalid";
static const char ERR_PROJECT_MISMATCH[] = "embedded_project_mismatch";
static const char ERR_VERSION_MISMATCH[] = "embedded_version_mismatch";
static const char ERR_TARGET_MARKER[] = "target_marker_missing";
static const char ERR_HARDWARE_MARKER[] = "hardware_marker_missing";
static const char ERR_VERIFICATION_REQUIRED[] = "verification_required";
static const char ERR_ABORTED[] = "aborted";

static bool bounded_string_is_terminated(const char *value, size_t capacity)
{
    return value && memchr(value, '\0', capacity) != NULL;
}

static bool return_error(const char **error, const char *literal)
{
    if (error) {
        *error = literal;
    }
    return false;
}

static bool session_error(uplink_ota_policy_session_t *session,
                          const char **error,
                          const char *literal)
{
    if (session) {
        if (session->state == UPLINK_OTA_COMMITTED) {
            return return_error(error, ERR_INVALID_STATE);
        }
        if (session->state == UPLINK_OTA_ERROR) {
            return return_error(error,
                                session->last_error
                                    ? session->last_error
                                    : ERR_INVALID_STATE);
        }
        session->state = UPLINK_OTA_ERROR;
        session->credit_outstanding = false;
        session->verification_passed = false;
        session->last_error = literal;
    }
    return return_error(error, literal);
}

static bool sha256_c_string_is_valid(const char *value)
{
    /* The digest contract is a fixed 65-byte buffer: 64 hex digits followed
     * by NUL. The shared validator examines exactly those 65 bytes, avoiding
     * an unbounded C-string scan on an untrusted computed digest. */
    if (!value || !fof_firmware_sha256_hex_is_valid(value)) {
        return false;
    }
    for (size_t i = 0; i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
        if (value[i] != '0') {
            return true;
        }
    }
    return false;
}

static bool hex_equal_case_insensitive(const char *left, const char *right)
{
    if (!sha256_c_string_is_valid(left) ||
        !sha256_c_string_is_valid(right)) {
        return false;
    }
    for (size_t i = 0; i < FOF_FIRMWARE_SHA256_HEX_LENGTH; ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'F') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'F') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool uplink_ota_policy_manifest_allowed(
    const uplink_ota_manifest_t *manifest,
    const char *running_version,
    uint32_t partition_size,
    bool running_pending_verify,
    const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!manifest || !running_version) {
        return return_error(error, ERR_INVALID_ARGUMENT);
    }
    if (running_pending_verify) {
        return return_error(error, ERR_PENDING_VERIFY);
    }
    if (!bounded_string_is_terminated(running_version,
                                      sizeof(manifest->version)) ||
        running_version[0] == '\0') {
        return return_error(error, ERR_INVALID_VERSION);
    }
    if (!bounded_string_is_terminated(manifest->target,
                                      sizeof(manifest->target)) ||
        !bounded_string_is_terminated(manifest->project,
                                      sizeof(manifest->project)) ||
        !bounded_string_is_terminated(manifest->hardware,
                                      sizeof(manifest->hardware)) ||
        !bounded_string_is_terminated(manifest->version,
                                      sizeof(manifest->version)) ||
        !bounded_string_is_terminated(manifest->sha256,
                                      sizeof(manifest->sha256))) {
        return return_error(error, ERR_UNTERMINATED_FIELD);
    }
    if (strcmp(manifest->target, UPLINK_OTA_TARGET) != 0) {
        return return_error(error, ERR_WRONG_TARGET);
    }
    if (strcmp(manifest->project, UPLINK_OTA_PROJECT) != 0) {
        return return_error(error, ERR_WRONG_PROJECT);
    }
    if (strcmp(manifest->hardware, UPLINK_OTA_HARDWARE) != 0) {
        return return_error(error, ERR_WRONG_HARDWARE);
    }

    fof_firmware_version_relation_t relation =
        fof_firmware_version_compare(manifest->version, running_version);
    if (relation == FOF_VERSION_INVALID) {
        return return_error(error, ERR_INVALID_VERSION);
    }
    if (relation == FOF_VERSION_OLDER) {
        return return_error(error, ERR_DOWNGRADE);
    }
    if (relation == FOF_VERSION_UNORDERED) {
        return return_error(error, ERR_UNORDERED_VERSION);
    }
    if (relation == FOF_VERSION_EQUAL &&
        !manifest->recovery_rewrite_same_version) {
        return return_error(error, ERR_EQUAL_VERSION);
    }
    if (manifest->size < UPLINK_OTA_MIN_IMAGE_BYTES) {
        return return_error(error, ERR_IMAGE_TOO_SMALL);
    }
    if (partition_size < UPLINK_OTA_MIN_IMAGE_BYTES ||
        manifest->size > partition_size) {
        return return_error(error, ERR_IMAGE_TOO_LARGE);
    }
    if (manifest->crc32 == 0U) {
        return return_error(error, ERR_INVALID_CRC);
    }
    if (!sha256_c_string_is_valid(manifest->sha256)) {
        return return_error(error, ERR_INVALID_SHA);
    }
    return true;
}

void uplink_ota_policy_init(uplink_ota_policy_session_t *session)
{
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->state = UPLINK_OTA_IDLE;
}

bool uplink_ota_policy_begin(
    uplink_ota_policy_session_t *session,
    const uplink_ota_manifest_t *manifest,
    const char *running_version,
    uint32_t partition_size,
    bool running_pending_verify,
    const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!session) {
        return return_error(error, ERR_INVALID_ARGUMENT);
    }
    if (session->state != UPLINK_OTA_IDLE) {
        return session_error(session, error, ERR_INVALID_STATE);
    }
    const char *manifest_error = NULL;
    if (!uplink_ota_policy_manifest_allowed(
            manifest, running_version, partition_size,
            running_pending_verify, &manifest_error)) {
        return session_error(session, error,
                             manifest_error ? manifest_error
                                            : ERR_INVALID_ARGUMENT);
    }
    memcpy(&session->manifest, manifest, sizeof(session->manifest));
    session->durable_written = 0;
    session->next_credit_at = 0;
    session->credit_outstanding = false;
    session->verification_passed = false;
    session->last_error = NULL;
    session->state = UPLINK_OTA_RECEIVING;
    return true;
}

bool uplink_ota_policy_grant_credit(
    uplink_ota_policy_session_t *session,
    uint32_t *credit_bytes,
    uint32_t *durable_received,
    const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!session || !credit_bytes || !durable_received) {
        return session_error(session, error, ERR_INVALID_ARGUMENT);
    }
    if (session->state != UPLINK_OTA_RECEIVING) {
        return session_error(session, error, ERR_INVALID_STATE);
    }
    if (session->credit_outstanding) {
        return session_error(session, error, ERR_CREDIT_OUTSTANDING);
    }
    if (session->durable_written >= session->manifest.size) {
        return session_error(session, error, ERR_IMAGE_COMPLETE);
    }
    uint32_t remaining = session->manifest.size - session->durable_written;
    uint32_t credit = remaining < UPLINK_OTA_CREDIT_BYTES
        ? remaining : UPLINK_OTA_CREDIT_BYTES;
    if (credit > UINT32_MAX - session->durable_written) {
        return session_error(session, error, ERR_COUNTER_OVERFLOW);
    }
    session->next_credit_at = session->durable_written + credit;
    session->credit_outstanding = true;
    *credit_bytes = credit;
    *durable_received = session->durable_written;
    return true;
}

bool uplink_ota_policy_note_durable_write(
    uplink_ota_policy_session_t *session,
    uint32_t length,
    uint32_t transport_received,
    const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!session) {
        return return_error(error, ERR_INVALID_ARGUMENT);
    }
    if (session->state != UPLINK_OTA_RECEIVING) {
        return session_error(session, error, ERR_INVALID_STATE);
    }
    if (!session->credit_outstanding) {
        return session_error(session, error, ERR_NO_CREDIT);
    }
    if (length == 0U) {
        return session_error(session, error, ERR_ZERO_WRITE);
    }
    if (length > UINT32_MAX - session->durable_written) {
        return session_error(session, error, ERR_COUNTER_OVERFLOW);
    }
    uint32_t new_durable = session->durable_written + length;
    if (transport_received != new_durable) {
        return session_error(session, error, ERR_TRANSPORT_MISMATCH);
    }
    if (new_durable > session->manifest.size) {
        return session_error(session, error, ERR_IMAGE_OVERSHOOT);
    }
    if (session->next_credit_at < session->durable_written ||
        new_durable > session->next_credit_at) {
        return session_error(session, error, ERR_CREDIT_OVERSHOOT);
    }
    session->durable_written = new_durable;
    if (new_durable == session->next_credit_at) {
        session->credit_outstanding = false;
    }
    if (new_durable == session->manifest.size) {
        session->state = UPLINK_OTA_VERIFYING;
    }
    return true;
}

bool uplink_ota_policy_verify_complete(
    uplink_ota_policy_session_t *session,
    uint32_t transport_received,
    uint32_t computed_crc32,
    const char *computed_sha256,
    const fof_firmware_image_identity_t *embedded_identity,
    bool target_marker_seen,
    bool hardware_marker_seen,
    const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!session) {
        return return_error(error, ERR_INVALID_ARGUMENT);
    }
    if (session->state != UPLINK_OTA_VERIFYING) {
        return session_error(session, error, ERR_INVALID_STATE);
    }
    if (session->credit_outstanding ||
        session->durable_written != session->manifest.size ||
        transport_received != session->manifest.size ||
        transport_received != session->durable_written) {
        return session_error(session, error, ERR_INCOMPLETE_IMAGE);
    }
    if (computed_crc32 != session->manifest.crc32) {
        return session_error(session, error, ERR_CRC_MISMATCH);
    }
    if (!computed_sha256 ||
        !hex_equal_case_insensitive(computed_sha256,
                                    session->manifest.sha256)) {
        return session_error(session, error,
                             sha256_c_string_is_valid(computed_sha256)
                                 ? ERR_SHA_MISMATCH : ERR_INVALID_SHA);
    }
    if (!embedded_identity) {
        return session_error(session, error, ERR_IDENTITY_MISSING);
    }
    if (!bounded_string_is_terminated(embedded_identity->project,
                                      sizeof(embedded_identity->project)) ||
        !bounded_string_is_terminated(embedded_identity->version,
                                      sizeof(embedded_identity->version))) {
        return session_error(session, error, ERR_IDENTITY_FIELD);
    }
    if (strcmp(embedded_identity->project, session->manifest.project) != 0) {
        return session_error(session, error, ERR_PROJECT_MISMATCH);
    }
    if (strcmp(embedded_identity->version, session->manifest.version) != 0) {
        return session_error(session, error, ERR_VERSION_MISMATCH);
    }
    if (!target_marker_seen) {
        return session_error(session, error, ERR_TARGET_MARKER);
    }
    if (!hardware_marker_seen) {
        return session_error(session, error, ERR_HARDWARE_MARKER);
    }
    session->verification_passed = true;
    session->last_error = NULL;
    return true;
}

bool uplink_ota_policy_mark_committed(
    uplink_ota_policy_session_t *session,
    const char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!session) {
        return return_error(error, ERR_INVALID_ARGUMENT);
    }
    if (session->state != UPLINK_OTA_VERIFYING) {
        return session_error(session, error, ERR_INVALID_STATE);
    }
    if (!session->verification_passed) {
        return session_error(session, error, ERR_VERIFICATION_REQUIRED);
    }
    session->state = UPLINK_OTA_COMMITTED;
    session->last_error = NULL;
    return true;
}

void uplink_ota_policy_fail(uplink_ota_policy_session_t *session,
                            const char *error)
{
    (void)error;
    if (!session) {
        return;
    }
    if (session->state == UPLINK_OTA_COMMITTED ||
        session->state == UPLINK_OTA_ERROR) {
        return;
    }
    session->state = UPLINK_OTA_ERROR;
    session->credit_outstanding = false;
    session->verification_passed = false;
    session->last_error = ERR_ABORTED;
}
