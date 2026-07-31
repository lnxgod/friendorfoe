#include "unity.h"

#include "uplink_ota_policy.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define TEST_RUNNING_VERSION "0.64.77-badge-defcon34"
#define TEST_NEW_VERSION "0.64.78-badge-defcon34"
#define TEST_IMAGE_SIZE 9000U
#define TEST_PARTITION_SIZE 2097152U
#define TEST_CRC32 0x89abcdefU
#define TEST_SHA_LOWER \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define TEST_SHA_UPPER \
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
#define TEST_SHA_OTHER \
    "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static uplink_ota_manifest_t manifest_fixture(void)
{
    uplink_ota_manifest_t manifest = {0};
    strcpy(manifest.target, UPLINK_OTA_TARGET);
    strcpy(manifest.project, UPLINK_OTA_PROJECT);
    strcpy(manifest.hardware, UPLINK_OTA_HARDWARE);
    strcpy(manifest.version, TEST_NEW_VERSION);
    strcpy(manifest.sha256, TEST_SHA_LOWER);
    manifest.size = TEST_IMAGE_SIZE;
    manifest.crc32 = TEST_CRC32;
    return manifest;
}

static fof_firmware_image_identity_t identity_fixture(void)
{
    fof_firmware_image_identity_t identity = {0};
    strcpy(identity.version, TEST_NEW_VERSION);
    strcpy(identity.project, UPLINK_OTA_PROJECT);
    return identity;
}

static uplink_ota_policy_session_t begun_session(uint32_t image_size)
{
    uplink_ota_policy_session_t session;
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = "unchanged";
    manifest.size = image_size;
    uplink_ota_policy_init(&session);
    TEST_ASSERT_TRUE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_NULL(error);
    return session;
}

static void grant_and_write(uplink_ota_policy_session_t *session,
                            uint32_t length)
{
    uint32_t credit = 0;
    uint32_t durable = UINT32_MAX;
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        session, &credit, &durable, &error));
    TEST_ASSERT_EQUAL_UINT32(session->durable_written, durable);
    TEST_ASSERT_TRUE(length <= credit);
    TEST_ASSERT_TRUE(uplink_ota_policy_note_durable_write(
        session, length, durable + length, &error));
}

static uplink_ota_policy_session_t fully_written_session(void)
{
    uplink_ota_policy_session_t session = begun_session(TEST_IMAGE_SIZE);
    grant_and_write(&session, 4096U);
    grant_and_write(&session, 4096U);
    grant_and_write(&session, TEST_IMAGE_SIZE - 8192U);
    TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_SIZE, session.durable_written);
    TEST_ASSERT_FALSE(session.credit_outstanding);
    return session;
}

void test_uplink_ota_manifest_accepts_only_exact_identity_and_newer_version(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = "unchanged";
    TEST_ASSERT_TRUE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_NULL(error);

    const char *wrong_values[] = {
        "uplink-s3", "fof_uplink", "esp32-s3-devkitc-1"
    };
    char *fields[] = {manifest.target, manifest.project, manifest.hardware};
    const char *expected[] = {
        UPLINK_OTA_TARGET, UPLINK_OTA_PROJECT, UPLINK_OTA_HARDWARE
    };
    for (size_t i = 0; i < 3; ++i) {
        strcpy(fields[i], wrong_values[i]);
        TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
            &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false,
            &error));
        TEST_ASSERT_NOT_NULL(error);
        strcpy(fields[i], expected[i]);
    }
}

void test_uplink_ota_manifest_rejects_nulls_and_unterminated_fields(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        NULL, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, NULL, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, NULL));

    char *fields[] = {
        manifest.target, manifest.project, manifest.hardware,
        manifest.version, manifest.sha256
    };
    const size_t sizes[] = {
        sizeof(manifest.target), sizeof(manifest.project),
        sizeof(manifest.hardware), sizeof(manifest.version),
        sizeof(manifest.sha256)
    };
    for (size_t i = 0; i < 5; ++i) {
        manifest = manifest_fixture();
        memset(fields[i], 'A', sizes[i]);
        TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
            &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false,
            &error));
    }
}

void test_uplink_ota_manifest_rejects_equal_without_explicit_recovery(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    strcpy(manifest.version, TEST_RUNNING_VERSION);
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_NOT_NULL(error);
}

void test_uplink_ota_manifest_accepts_equal_only_for_recovery(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    strcpy(manifest.version, TEST_RUNNING_VERSION);
    manifest.recovery_rewrite_same_version = true;
    TEST_ASSERT_TRUE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_NULL(error);

    strcpy(manifest.version, TEST_NEW_VERSION);
    TEST_ASSERT_TRUE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));

    strcpy(manifest.version, "0.64.76-badge-defcon34");
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    strcpy(manifest.version, "0.64.77-other");
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
}

void test_uplink_ota_manifest_rejects_older_unordered_and_malformed_versions(void)
{
    const char *versions[] = {
        "0.64.76-badge-defcon34", "0.64.77-other", "not-a-version", ""
    };
    const char *error = NULL;
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
        uplink_ota_manifest_t manifest = manifest_fixture();
        strcpy(manifest.version, versions[i]);
        TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
            &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false,
            &error));
        TEST_ASSERT_NOT_NULL(error);
    }
}

void test_uplink_ota_manifest_rejects_pending_verify(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, true, &error));
    TEST_ASSERT_NOT_NULL(error);
}

void test_uplink_ota_manifest_rejects_bad_size_crc_and_sha(void)
{
    const char *error = NULL;
    uplink_ota_manifest_t manifest = manifest_fixture();
    manifest.size = UPLINK_OTA_MIN_IMAGE_BYTES - 1U;
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    manifest = manifest_fixture();
    manifest.size = TEST_PARTITION_SIZE + 1U;
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    manifest = manifest_fixture();
    manifest.crc32 = 0;
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    manifest = manifest_fixture();
    strcpy(manifest.sha256, "abcd");
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    manifest = manifest_fixture();
    memset(manifest.sha256, '0', FOF_FIRMWARE_SHA256_HEX_LENGTH);
    manifest.sha256[FOF_FIRMWARE_SHA256_HEX_LENGTH] = '\0';
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
}

void test_uplink_ota_manifest_accepts_exact_size_boundaries_and_optional_error(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    manifest.size = UPLINK_OTA_MIN_IMAGE_BYTES;
    TEST_ASSERT_TRUE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, UPLINK_OTA_MIN_IMAGE_BYTES,
        false, NULL));
    manifest.size = TEST_PARTITION_SIZE;
    TEST_ASSERT_TRUE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, NULL));
}

void test_uplink_ota_manifest_handles_empty_and_bounded_unterminated_running_version(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    char unterminated[33];
    memset(unterminated, '7', sizeof(unterminated));
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, "", TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, unterminated, TEST_PARTITION_SIZE, false, &error));
}

void test_uplink_ota_init_and_begin_copy_manifest_and_reset_state(void)
{
    uplink_ota_policy_session_t session;
    memset(&session, 0xa5, sizeof(session));
    uplink_ota_policy_init(&session);
    TEST_ASSERT_EQUAL(UPLINK_OTA_IDLE, session.state);
    TEST_ASSERT_EQUAL_UINT32(0, session.durable_written);
    TEST_ASSERT_EQUAL_UINT32(0, session.next_credit_at);
    TEST_ASSERT_FALSE(session.credit_outstanding);
    TEST_ASSERT_NULL(session.last_error);

    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    memset(&manifest, 0, sizeof(manifest));
    TEST_ASSERT_EQUAL(UPLINK_OTA_RECEIVING, session.state);
    TEST_ASSERT_EQUAL_STRING(UPLINK_OTA_TARGET, session.manifest.target);
    TEST_ASSERT_EQUAL_STRING(TEST_NEW_VERSION, session.manifest.version);
}

void test_uplink_ota_invalid_or_repeated_begin_fails_closed_without_copy(void)
{
    uplink_ota_policy_session_t session;
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    uplink_ota_policy_init(&session);
    strcpy(manifest.target, "attacker");
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
    TEST_ASSERT_EQUAL_CHAR('\0', session.manifest.target[0]);

    session = begun_session(TEST_IMAGE_SIZE);
    manifest = manifest_fixture();
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
}

void test_uplink_ota_initial_and_final_short_credit_are_absolute(void)
{
    uplink_ota_policy_session_t session = begun_session(5000U);
    uint32_t credit = 0;
    uint32_t durable = UINT32_MAX;
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_EQUAL_UINT32(UPLINK_OTA_CREDIT_BYTES, credit);
    TEST_ASSERT_EQUAL_UINT32(0, durable);
    TEST_ASSERT_EQUAL_UINT32(4096U, session.next_credit_at);
    TEST_ASSERT_TRUE(uplink_ota_policy_note_durable_write(
        &session, 4096U, 4096U, &error));
    TEST_ASSERT_FALSE(session.credit_outstanding);
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_EQUAL_UINT32(904U, credit);
    TEST_ASSERT_EQUAL_UINT32(4096U, durable);
    TEST_ASSERT_EQUAL_UINT32(5000U, session.next_credit_at);
}

void test_uplink_ota_credit_waits_for_exact_durable_boundary(void)
{
    uplink_ota_policy_session_t session = begun_session(TEST_IMAGE_SIZE);
    uint32_t credit = 0;
    uint32_t durable = 0;
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_note_durable_write(
        &session, 2048U, 2048U, &error));
    TEST_ASSERT_TRUE(session.credit_outstanding);
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);

    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_note_durable_write(
        &session, 4096U, 4096U, &error));
    TEST_ASSERT_FALSE(session.credit_outstanding);
}

void test_uplink_ota_exact_final_write_enters_verifying_and_blocks_more_io(void)
{
    uplink_ota_policy_session_t session = begun_session(UPLINK_OTA_MIN_IMAGE_BYTES);
    uint32_t credit = 0, durable = 0;
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_note_durable_write(
        &session, UPLINK_OTA_MIN_IMAGE_BYTES,
        UPLINK_OTA_MIN_IMAGE_BYTES, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_VERIFYING, session.state);
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));

    session = begun_session(UPLINK_OTA_MIN_IMAGE_BYTES);
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_note_durable_write(
        &session, UPLINK_OTA_MIN_IMAGE_BYTES,
        UPLINK_OTA_MIN_IMAGE_BYTES, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 1U, UPLINK_OTA_MIN_IMAGE_BYTES + 1U, &error));
}

void test_uplink_ota_write_rejects_no_credit_zero_mismatch_and_overshoot(void)
{
    const char *error = NULL;
    uplink_ota_policy_session_t session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 1U, 1U, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);

    uint32_t credit = 0, durable = 0;
    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 0U, 0U, &error));

    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 1U, 2U, &error));

    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, UPLINK_OTA_CREDIT_BYTES + 1U,
        UPLINK_OTA_CREDIT_BYTES + 1U, &error));
}

void test_uplink_ota_durable_accounting_rejects_uint32_wrap(void)
{
    uplink_ota_policy_session_t session = begun_session(TEST_IMAGE_SIZE);
    const char *error = NULL;
    session.durable_written = UINT32_MAX - 1U;
    session.next_credit_at = UINT32_MAX;
    session.credit_outstanding = true;
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 2U, 0U, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
}

void test_uplink_ota_commit_accepts_exact_integrity_identity_and_markers(void)
{
    uplink_ota_policy_session_t session = fully_written_session();
    fof_firmware_image_identity_t identity = identity_fixture();
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_VERIFYING, session.state);
    TEST_ASSERT_NULL(error);
    TEST_ASSERT_TRUE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_COMMITTED, session.state);
}

void test_uplink_ota_commit_accepts_case_insensitive_sha(void)
{
    uplink_ota_policy_session_t session = fully_written_session();
    fof_firmware_image_identity_t identity = identity_fixture();
    const char *error = NULL;
    TEST_ASSERT_TRUE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_UPPER,
        &identity, true, true, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_VERIFYING, session.state);
    TEST_ASSERT_TRUE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_COMMITTED, session.state);
}

void test_uplink_ota_commit_rejects_unterminated_computed_sha_without_overread(void)
{
    uplink_ota_policy_session_t session = fully_written_session();
    fof_firmware_image_identity_t identity = identity_fixture();
    const char *error = NULL;
    long page_size = sysconf(_SC_PAGESIZE);
    TEST_ASSERT_TRUE(page_size > 65);

    size_t mapping_size = (size_t)page_size * 2U;
    unsigned char *mapping = mmap(NULL, mapping_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        TEST_FAIL_MESSAGE("guard-page mmap failed");
    }
    TEST_ASSERT_EQUAL_INT(0,
                          mprotect(mapping + page_size, (size_t)page_size,
                                   PROT_NONE));

    char *computed_sha = (char *)(mapping + page_size - 65);
    memset(computed_sha, 'a', 65);
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, computed_sha,
        &identity, true, true, &error));
    TEST_ASSERT_EQUAL_STRING("invalid_sha256", error);
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);

    TEST_ASSERT_EQUAL_INT(0, munmap(mapping, mapping_size));
}

void test_uplink_ota_commit_rejects_byte_count_crc_and_sha_failures(void)
{
    fof_firmware_image_identity_t identity = identity_fixture();
    const char *error = NULL;
    uplink_ota_policy_session_t session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE - 1U, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);

    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32 + 1U, TEST_SHA_LOWER,
        &identity, true, true, &error));
    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_OTHER,
        &identity, true, true, &error));
    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, "bad-sha",
        &identity, true, true, &error));
}

void test_uplink_ota_commit_rejects_embedded_identity_and_marker_failures(void)
{
    const char *error = NULL;
    fof_firmware_image_identity_t identity = identity_fixture();
    uplink_ota_policy_session_t session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        NULL, true, true, &error));

    session = fully_written_session();
    strcpy(identity.project, "fof_uplink");
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    identity = identity_fixture();
    session = fully_written_session();
    strcpy(identity.version, TEST_RUNNING_VERSION);
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    identity = identity_fixture();
    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, false, true, &error));
    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, false, &error));

    session = fully_written_session();
    memset(identity.project, 'P', sizeof(identity.project));
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));

    identity = identity_fixture();
    session = fully_written_session();
    memset(identity.version, 'V', sizeof(identity.version));
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
}

void test_uplink_ota_terminal_states_reject_begin_write_credit_and_commit(void)
{
    uplink_ota_policy_session_t session = fully_written_session();
    fof_firmware_image_identity_t identity = identity_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    uint32_t credit = 0, durable = 0;
    TEST_ASSERT_TRUE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 1U, TEST_IMAGE_SIZE + 1U, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
}

void test_uplink_ota_committed_and_error_states_are_latched_until_init(void)
{
    uplink_ota_policy_session_t session = fully_written_session();
    fof_firmware_image_identity_t identity = identity_fixture();
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    uint32_t credit = 0, durable = 0;
    TEST_ASSERT_TRUE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_VERIFYING, session.state);
    TEST_ASSERT_TRUE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_COMMITTED, session.state);
    TEST_ASSERT_NULL(session.last_error);
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        &session, 1U, TEST_IMAGE_SIZE + 1U, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        &session, &credit, &durable, &error));
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    uplink_ota_policy_fail(&session, "late caller failure");
    TEST_ASSERT_EQUAL(UPLINK_OTA_COMMITTED, session.state);
    TEST_ASSERT_NULL(session.last_error);

    uplink_ota_policy_init(&session);
    strcpy(manifest.target, "wrong");
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
    const char *first_error = session.last_error;
    uplink_ota_policy_fail(&session, "replacement");
    TEST_ASSERT_EQUAL_PTR(first_error, session.last_error);
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_EQUAL_PTR(first_error, session.last_error);
    uplink_ota_policy_init(&session);
    TEST_ASSERT_EQUAL(UPLINK_OTA_IDLE, session.state);
    TEST_ASSERT_NULL(session.last_error);
}

void test_uplink_ota_mark_committed_requires_verified_integrity_latch(void)
{
    uplink_ota_policy_session_t session;
    fof_firmware_image_identity_t identity = identity_fixture();
    const char *error = NULL;

    uplink_ota_policy_init(&session);
    TEST_ASSERT_FALSE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_EQUAL_STRING("invalid_state", error);
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);

    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_EQUAL_STRING("verification_required", error);
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);

    session = fully_written_session();
    TEST_ASSERT_TRUE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, &error));
    TEST_ASSERT_TRUE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_COMMITTED, session.state);
    TEST_ASSERT_FALSE(uplink_ota_policy_mark_committed(&session, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_COMMITTED, session.state);
}

void test_uplink_ota_all_validation_failures_enter_error(void)
{
    uplink_ota_policy_session_t session;
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    strcpy(manifest.target, "wrong");
    uplink_ota_policy_init(&session);
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
    TEST_ASSERT_EQUAL_PTR(error, session.last_error);

    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, 0, 0, NULL, NULL, false, false, &error));
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
}

void test_uplink_ota_representative_error_codes_are_wire_safe_and_stable(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    const char *error = NULL;
    strcpy(manifest.target, "wrong");
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_EQUAL_STRING("target_mismatch", error);

    manifest = manifest_fixture();
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, true, &error));
    TEST_ASSERT_EQUAL_STRING("pending_verify", error);

    manifest = manifest_fixture();
    strcpy(manifest.version, TEST_RUNNING_VERSION);
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, &error));
    TEST_ASSERT_EQUAL_STRING("equal_version_requires_recovery", error);

    uplink_ota_policy_session_t session = fully_written_session();
    fof_firmware_image_identity_t identity = identity_fixture();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_OTHER,
        &identity, true, true, &error));
    TEST_ASSERT_EQUAL_STRING("sha256_mismatch", error);
    TEST_ASSERT_EQUAL_PTR(error, session.last_error);
}

void test_uplink_ota_fail_and_init_use_stable_owned_error_state(void)
{
    uplink_ota_policy_session_t session = begun_session(TEST_IMAGE_SIZE);
    char caller_stack_error[] = "caller stack lifetime";
    uplink_ota_policy_fail(&session, caller_stack_error);
    TEST_ASSERT_EQUAL(UPLINK_OTA_ERROR, session.state);
    TEST_ASSERT_NOT_NULL(session.last_error);
    TEST_ASSERT_FALSE(session.last_error == caller_stack_error);
    caller_stack_error[0] = 'X';
    TEST_ASSERT_EQUAL_STRING("aborted", session.last_error);
    uplink_ota_policy_init(&session);
    TEST_ASSERT_EQUAL(UPLINK_OTA_IDLE, session.state);
    TEST_ASSERT_NULL(session.last_error);
}

void test_uplink_ota_null_function_arguments_are_safe(void)
{
    uplink_ota_manifest_t manifest = manifest_fixture();
    uplink_ota_policy_session_t session;
    const char *error = NULL;
    uint32_t credit = 0, durable = 0;
    fof_firmware_image_identity_t identity = identity_fixture();

    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        NULL, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE, false, NULL));
    TEST_ASSERT_FALSE(uplink_ota_policy_manifest_allowed(
        &manifest, NULL, TEST_PARTITION_SIZE, false, NULL));
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        NULL, &manifest, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, NULL));
    uplink_ota_policy_init(&session);
    TEST_ASSERT_FALSE(uplink_ota_policy_begin(
        &session, NULL, TEST_RUNNING_VERSION, TEST_PARTITION_SIZE,
        false, NULL));

    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        NULL, &credit, &durable, NULL));
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        &session, NULL, &durable, NULL));
    session = begun_session(TEST_IMAGE_SIZE);
    TEST_ASSERT_FALSE(uplink_ota_policy_grant_credit(
        &session, &credit, NULL, NULL));
    TEST_ASSERT_FALSE(uplink_ota_policy_note_durable_write(
        NULL, 1U, 1U, NULL));

    session = fully_written_session();
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        NULL, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        &identity, true, true, NULL));
    TEST_ASSERT_FALSE(uplink_ota_policy_verify_complete(
        &session, TEST_IMAGE_SIZE, TEST_CRC32, TEST_SHA_LOWER,
        NULL, true, true, NULL));
    TEST_ASSERT_FALSE(uplink_ota_policy_mark_committed(NULL, NULL));
    uplink_ota_policy_fail(NULL, "safe");
    uplink_ota_policy_init(NULL);
}
