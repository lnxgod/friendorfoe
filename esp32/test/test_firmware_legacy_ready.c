#include "unity.h"

#include "firmware_legacy_ready.h"

#include <stddef.h>

#define BADGE_SCANNER_TARGET FOF_LEGACY_READY_BADGE_TARGET
#define BADGE_SCANNER_PROJECT FOF_LEGACY_READY_BADGE_PROJECT
#define BADGE_SCANNER_HARDWARE FOF_LEGACY_READY_BADGE_HARDWARE
#define BADGE_SCANNER_TARGET_VERSION "0.64.69-badge-defcon34"
#define BADGE_SCANNER_SIZE 1192736U
#define BADGE_SCANNER_CRC32 0x89ABCDEFU
#define BADGE_SCANNER_SHA256 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static fof_legacy_ready_view_t legacy_ready_fixture(void)
{
    fof_legacy_ready_view_t ready = {
        .strict_fields_absent = true,
        .board = BADGE_SCANNER_TARGET,
        .current_version = FOF_LEGACY_READY_BOOTSTRAP_VERSION,
        .target_version = BADGE_SCANNER_TARGET_VERSION,
        .size = BADGE_SCANNER_SIZE,
        .crc32 = BADGE_SCANNER_CRC32,
    };
    return ready;
}

static fof_legacy_identity_view_t legacy_identity_fixture(void)
{
    fof_legacy_identity_view_t identity = {
        .received = true,
        .version = FOF_LEGACY_READY_BOOTSTRAP_VERSION,
        .board = BADGE_SCANNER_TARGET,
        .firmware_name = BADGE_SCANNER_TARGET,
        .project = BADGE_SCANNER_PROJECT,
        .hardware = BADGE_SCANNER_HARDWARE,
        .hardware_id = "e0:72:a1:f9:48:58",
    };
    return identity;
}

static fof_legacy_manifest_view_t legacy_manifest_fixture(void)
{
    fof_legacy_manifest_view_t manifest = {
        .target = BADGE_SCANNER_TARGET,
        .version = BADGE_SCANNER_TARGET_VERSION,
        .project = BADGE_SCANNER_PROJECT,
        .hardware = BADGE_SCANNER_HARDWARE,
        .sha256 = BADGE_SCANNER_SHA256,
        .size = BADGE_SCANNER_SIZE,
        .crc32 = BADGE_SCANNER_CRC32,
    };
    return manifest;
}

static bool legacy_ready_authorized(const fof_legacy_ready_view_t *ready,
                                    const fof_legacy_identity_view_t *identity,
                                    const fof_legacy_manifest_view_t *manifest)
{
    return fof_firmware_legacy_ready_authorized(ready, identity, manifest);
}

void test_legacy_ready_authorizes_only_exact_06468_identity_and_manifest(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    TEST_ASSERT_TRUE(legacy_ready_authorized(&ready, &identity, &manifest));

    ready.strict_fields_absent = false;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_wrong_source_version(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.current_version = "0.64.67-badge-live-follow";
    identity.version = ready.current_version;

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_requires_received_identity(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.received = false;

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_requires_canonical_hardware_id(void)
{
    static const char *invalid_hardware_ids[] = {
        "",
        "e0:72:a1:f9:48",
        "e0:72:a1:f9:48:580",
        "e0-72-a1-f9-48-58",
        "e0:72:a1:f9:48:5g",
        "e0:72:a1:f9::48:58",
    };
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.hardware_id = "E0:72:A1:F9:48:58";
    TEST_ASSERT_TRUE(legacy_ready_authorized(&ready, &identity, &manifest));

    for (size_t i = 0;
         i < sizeof(invalid_hardware_ids) / sizeof(invalid_hardware_ids[0]);
         ++i) {
        identity.hardware_id = invalid_hardware_ids[i];
        TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    }
}

void test_legacy_ready_rejects_target_identity_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.firmware_name = "scanner-s3-combo";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_project_identity_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.project = "fof_scanner";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_coherent_production_scanner_contract(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.board = "scanner-s3-combo";
    identity.board = "scanner-s3-combo";
    identity.firmware_name = "scanner-s3-combo";
    identity.project = "fof_scanner";
    manifest.target = "scanner-s3-combo";
    manifest.project = "fof_scanner";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_hardware_identity_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.hardware = "esp32s3_generic";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_ready_board_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.board = "scanner-s3-combo";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_identity_board_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.board = "scanner-s3-combo";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_current_version_identity_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    identity.version = "0.64.67-badge-live-follow";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_target_version_manifest_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.target_version = "0.64.70-badge-defcon34";

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_size_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.size--;

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_crc_mismatch(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.crc32 ^= 1U;

    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_rejects_empty_or_invalid_manifest_sha256(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    manifest.sha256 = "";
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));

    manifest.sha256 =
        "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}

void test_legacy_ready_requires_strictly_newer_manifest_version(void)
{
    static const char *not_newer_versions[] = {
        FOF_LEGACY_READY_BOOTSTRAP_VERSION,
        "0.64.67-badge-live-follow",
        "0.64.68-badge-defcon34",
    };
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    for (size_t i = 0;
         i < sizeof(not_newer_versions) / sizeof(not_newer_versions[0]);
         ++i) {
        ready.target_version = not_newer_versions[i];
        manifest.version = not_newer_versions[i];
        TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    }
}

void test_legacy_ready_rejects_null_views(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    TEST_ASSERT_FALSE(legacy_ready_authorized(NULL, &identity, &manifest));
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, NULL, &manifest));
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, NULL));
}

void test_legacy_ready_rejects_null_string_members(void)
{
    fof_legacy_ready_view_t ready = legacy_ready_fixture();
    fof_legacy_identity_view_t identity = legacy_identity_fixture();
    fof_legacy_manifest_view_t manifest = legacy_manifest_fixture();

    ready.board = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    ready = legacy_ready_fixture();
    ready.current_version = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    ready = legacy_ready_fixture();
    ready.target_version = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));

    ready = legacy_ready_fixture();
    identity = legacy_identity_fixture();
    identity.version = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    identity = legacy_identity_fixture();
    identity.board = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    identity = legacy_identity_fixture();
    identity.firmware_name = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    identity = legacy_identity_fixture();
    identity.project = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    identity = legacy_identity_fixture();
    identity.hardware = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    identity = legacy_identity_fixture();
    identity.hardware_id = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));

    identity = legacy_identity_fixture();
    manifest = legacy_manifest_fixture();
    manifest.target = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    manifest = legacy_manifest_fixture();
    manifest.version = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    manifest = legacy_manifest_fixture();
    manifest.project = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    manifest = legacy_manifest_fixture();
    manifest.hardware = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
    manifest = legacy_manifest_fixture();
    manifest.sha256 = NULL;
    TEST_ASSERT_FALSE(legacy_ready_authorized(&ready, &identity, &manifest));
}
