#include "unity.h"

#include "firmware_relay_policy.h"

#include <stddef.h>
#include <stdint.h>

#define TEST_SESSION "r1234abcd"
#define TEST_OTHER_SESSION "rdeadbee"
#define TEST_MAC "e0:72:a1:f9:48:58"
#define TEST_OTHER_MAC "e0:72:a1:f9:48:59"
#define TEST_TARGET_VERSION "0.64.69-badge-defcon34"
#define TEST_SHA256 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define TEST_OTHER_SHA256 \
    "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define TEST_SIZE 1180737U
#define TEST_CRC32 0x1234abcdU
#define TEST_GENERATION 69U

typedef enum {
    MUTATION_NONE = 0,
    MUTATION_TYPE_MISSING,
    MUTATION_TYPE_WRONG,
    MUTATION_SESSION_MISSING,
    MUTATION_SESSION_WRONG,
    MUTATION_RECEIVED_MISSING,
    MUTATION_RECEIVED_WRONG,
    MUTATION_TOTAL_MISSING,
    MUTATION_TOTAL_WRONG,
    MUTATION_PERCENT_MISSING,
    MUTATION_PERCENT_WRONG,
    MUTATION_TARGET_MISSING,
    MUTATION_TARGET_WRONG,
    MUTATION_VERSION_MISSING,
    MUTATION_VERSION_WRONG,
    MUTATION_PROJECT_MISSING,
    MUTATION_PROJECT_WRONG,
    MUTATION_HARDWARE_MISSING,
    MUTATION_HARDWARE_WRONG,
    MUTATION_SHA256_MISSING,
    MUTATION_SHA256_WRONG,
    MUTATION_GENERATION_MISSING,
    MUTATION_GENERATION_WRONG,
    MUTATION_SIZE_MISSING,
    MUTATION_SIZE_WRONG,
    MUTATION_CRC32_MISSING,
    MUTATION_CRC32_WRONG,
    MUTATION_ALLOW_SAME_MISSING,
    MUTATION_ALLOW_SAME_WRONG,
} receipt_mutation_t;

typedef struct {
    const char *name;
    receipt_mutation_t mutation;
    bool expected;
} receipt_case_t;

static fof_firmware_receipt_view_t receipt_fixture(const char *type)
{
    fof_firmware_receipt_view_t receipt = {
        .type = type,
        .session_id = TEST_SESSION,
        .target_version = TEST_TARGET_VERSION,
        .firmware_name = FOF_LEGACY_READY_BADGE_TARGET,
        .project = FOF_LEGACY_READY_BADGE_PROJECT,
        .hardware = FOF_LEGACY_READY_BADGE_HARDWARE,
        .sha256 = TEST_SHA256,
        .has_generation = true,
        .generation = TEST_GENERATION,
        .has_size = true,
        .size = TEST_SIZE,
        .has_crc32 = true,
        .crc32 = TEST_CRC32,
        .has_allow_same_version = true,
        .allow_same_version = false,
        .has_received = true,
        .received = TEST_SIZE,
        .has_total = true,
        .total = TEST_SIZE,
        .has_percent = true,
        .percent = 100U,
    };
    return receipt;
}

static fof_firmware_strict_receipt_expectation_t strict_fixture(
    const char *type)
{
    fof_firmware_strict_receipt_expectation_t expected = {
        .type = type,
        .session_id = TEST_SESSION,
        .target_version = TEST_TARGET_VERSION,
        .firmware_name = FOF_LEGACY_READY_BADGE_TARGET,
        .project = FOF_LEGACY_READY_BADGE_PROJECT,
        .hardware = FOF_LEGACY_READY_BADGE_HARDWARE,
        .sha256 = TEST_SHA256,
        .generation = TEST_GENERATION,
        .size = TEST_SIZE,
        .crc32 = TEST_CRC32,
        .allow_same_version = false,
        .received = TEST_SIZE,
    };
    return expected;
}

static void mutate_receipt(fof_firmware_receipt_view_t *receipt,
                           receipt_mutation_t mutation,
                           const char *wrong_type)
{
    switch (mutation) {
        case MUTATION_NONE:
            break;
        case MUTATION_TYPE_MISSING:
            receipt->type = NULL;
            break;
        case MUTATION_TYPE_WRONG:
            receipt->type = wrong_type;
            break;
        case MUTATION_SESSION_MISSING:
            receipt->session_id = NULL;
            break;
        case MUTATION_SESSION_WRONG:
            receipt->session_id = TEST_OTHER_SESSION;
            break;
        case MUTATION_RECEIVED_MISSING:
            receipt->has_received = false;
            break;
        case MUTATION_RECEIVED_WRONG:
            receipt->received = TEST_SIZE - 1U;
            break;
        case MUTATION_TOTAL_MISSING:
            receipt->has_total = false;
            break;
        case MUTATION_TOTAL_WRONG:
            receipt->total = TEST_SIZE - 1U;
            break;
        case MUTATION_PERCENT_MISSING:
            receipt->has_percent = false;
            break;
        case MUTATION_PERCENT_WRONG:
            receipt->percent = 99U;
            break;
        case MUTATION_TARGET_MISSING:
            receipt->firmware_name = NULL;
            break;
        case MUTATION_TARGET_WRONG:
            receipt->firmware_name = "scanner-s3-combo";
            break;
        case MUTATION_VERSION_MISSING:
            receipt->target_version = NULL;
            break;
        case MUTATION_VERSION_WRONG:
            receipt->target_version = "0.64.70-badge-defcon34";
            break;
        case MUTATION_PROJECT_MISSING:
            receipt->project = NULL;
            break;
        case MUTATION_PROJECT_WRONG:
            receipt->project = "friendorfoe_scanner";
            break;
        case MUTATION_HARDWARE_MISSING:
            receipt->hardware = NULL;
            break;
        case MUTATION_HARDWARE_WRONG:
            receipt->hardware = "esp32-s3-devkitc-1";
            break;
        case MUTATION_SHA256_MISSING:
            receipt->sha256 = NULL;
            break;
        case MUTATION_SHA256_WRONG:
            receipt->sha256 = TEST_OTHER_SHA256;
            break;
        case MUTATION_GENERATION_MISSING:
            receipt->has_generation = false;
            break;
        case MUTATION_GENERATION_WRONG:
            receipt->generation = TEST_GENERATION + 1U;
            break;
        case MUTATION_SIZE_MISSING:
            receipt->has_size = false;
            break;
        case MUTATION_SIZE_WRONG:
            receipt->size = TEST_SIZE - 1U;
            break;
        case MUTATION_CRC32_MISSING:
            receipt->has_crc32 = false;
            break;
        case MUTATION_CRC32_WRONG:
            receipt->crc32 = TEST_CRC32 + 1U;
            break;
        case MUTATION_ALLOW_SAME_MISSING:
            receipt->has_allow_same_version = false;
            break;
        case MUTATION_ALLOW_SAME_WRONG:
            receipt->allow_same_version = true;
            break;
    }
}

void test_relay_policy_legacy_ack_is_exact_type_and_session_table(void)
{
    static const receipt_case_t cases[] = {
        {"exact ack", MUTATION_NONE, true},
        {"missing type", MUTATION_TYPE_MISSING, false},
        {"ota_ack_extra", MUTATION_TYPE_WRONG, false},
        {"missing session", MUTATION_SESSION_MISSING, false},
        {"wrong session", MUTATION_SESSION_WRONG, false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fof_firmware_receipt_view_t receipt = receipt_fixture("ota_ack");
        mutate_receipt(&receipt, cases[i].mutation, "ota_ack_extra");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            fof_firmware_legacy_ack_matches(&receipt, TEST_SESSION),
            cases[i].name);
    }
}

void test_relay_policy_legacy_progress_requires_every_exact_field_table(void)
{
    static const receipt_case_t cases[] = {
        {"exact progress", MUTATION_NONE, true},
        {"missing type", MUTATION_TYPE_MISSING, false},
        {"wrong type", MUTATION_TYPE_WRONG, false},
        {"missing session", MUTATION_SESSION_MISSING, false},
        {"wrong session", MUTATION_SESSION_WRONG, false},
        {"missing received", MUTATION_RECEIVED_MISSING, false},
        {"wrong received", MUTATION_RECEIVED_WRONG, false},
        {"missing total", MUTATION_TOTAL_MISSING, false},
        {"wrong total", MUTATION_TOTAL_WRONG, false},
        {"missing percent", MUTATION_PERCENT_MISSING, false},
        {"wrong percent", MUTATION_PERCENT_WRONG, false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fof_firmware_receipt_view_t receipt =
            receipt_fixture("ota_progress");
        mutate_receipt(&receipt, cases[i].mutation, "ota_progress_extra");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            fof_firmware_legacy_progress_matches(
                &receipt, TEST_SESSION, TEST_SIZE),
            cases[i].name);
    }
}

void test_relay_policy_legacy_done_and_stop_ack_are_exact_table(void)
{
    static const receipt_case_t done_cases[] = {
        {"exact done", MUTATION_NONE, true},
        {"missing type", MUTATION_TYPE_MISSING, false},
        {"wrong type", MUTATION_TYPE_WRONG, false},
        {"missing session", MUTATION_SESSION_MISSING, false},
        {"wrong session", MUTATION_SESSION_WRONG, false},
        {"missing received", MUTATION_RECEIVED_MISSING, false},
        {"wrong received", MUTATION_RECEIVED_WRONG, false},
    };
    for (size_t i = 0;
         i < sizeof(done_cases) / sizeof(done_cases[0]); ++i) {
        fof_firmware_receipt_view_t receipt = receipt_fixture("ota_done");
        mutate_receipt(&receipt, done_cases[i].mutation, "ota_done_extra");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            done_cases[i].expected,
            fof_firmware_legacy_done_matches(
                &receipt, TEST_SESSION, TEST_SIZE),
            done_cases[i].name);
    }

    static const receipt_case_t stop_cases[] = {
        {"exact stop_ack", MUTATION_NONE, true},
        {"missing stop_ack type", MUTATION_TYPE_MISSING, false},
        {"stop_ack_extra", MUTATION_TYPE_WRONG, false},
    };
    for (size_t i = 0;
         i < sizeof(stop_cases) / sizeof(stop_cases[0]); ++i) {
        fof_firmware_receipt_view_t receipt = receipt_fixture("stop_ack");
        mutate_receipt(&receipt, stop_cases[i].mutation, "stop_ack_extra");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            stop_cases[i].expected,
            fof_firmware_stop_ack_matches(&receipt),
            stop_cases[i].name);
    }
}

void test_relay_policy_strict_receipt_requires_full_manifest_table(void)
{
    static const receipt_case_t cases[] = {
        {"exact strict receipt", MUTATION_NONE, true},
        {"missing type", MUTATION_TYPE_MISSING, false},
        {"wrong type", MUTATION_TYPE_WRONG, false},
        {"missing session", MUTATION_SESSION_MISSING, false},
        {"wrong session", MUTATION_SESSION_WRONG, false},
        {"missing target", MUTATION_TARGET_MISSING, false},
        {"wrong target", MUTATION_TARGET_WRONG, false},
        {"missing version", MUTATION_VERSION_MISSING, false},
        {"wrong version", MUTATION_VERSION_WRONG, false},
        {"missing project", MUTATION_PROJECT_MISSING, false},
        {"wrong project", MUTATION_PROJECT_WRONG, false},
        {"missing hardware", MUTATION_HARDWARE_MISSING, false},
        {"wrong hardware", MUTATION_HARDWARE_WRONG, false},
        {"missing sha256", MUTATION_SHA256_MISSING, false},
        {"wrong sha256", MUTATION_SHA256_WRONG, false},
        {"missing generation", MUTATION_GENERATION_MISSING, false},
        {"wrong generation", MUTATION_GENERATION_WRONG, false},
        {"missing size", MUTATION_SIZE_MISSING, false},
        {"wrong size", MUTATION_SIZE_WRONG, false},
        {"missing crc32", MUTATION_CRC32_MISSING, false},
        {"wrong crc32", MUTATION_CRC32_WRONG, false},
        {"missing allow_same", MUTATION_ALLOW_SAME_MISSING, false},
        {"wrong allow_same", MUTATION_ALLOW_SAME_WRONG, false},
        {"missing received", MUTATION_RECEIVED_MISSING, false},
        {"wrong received", MUTATION_RECEIVED_WRONG, false},
    };
    fof_firmware_strict_receipt_expectation_t expected =
        strict_fixture("ota_done");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fof_firmware_receipt_view_t receipt = receipt_fixture("ota_done");
        mutate_receipt(&receipt, cases[i].mutation, "ota_done_extra");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            fof_firmware_strict_receipt_matches(&receipt, &expected),
            cases[i].name);
    }
}

typedef enum {
    AUTH_NONE = 0,
    AUTH_MANUAL_CALLER,
    AUTH_BOUND_MISSING,
    AUTH_SOURCE_NOT_68,
    AUTH_IDENTITY_INCOMPLETE,
    AUTH_LIVE_MAC_NONCANONICAL,
    AUTH_BOUND_MAC_NONCANONICAL,
    AUTH_BOUND_LIVE_MAC_MISMATCH,
    AUTH_IDENTITY_BOARD_MISMATCH,
    AUTH_IDENTITY_TARGET_MISMATCH,
    AUTH_IDENTITY_PROJECT_MISMATCH,
    AUTH_IDENTITY_HARDWARE_MISMATCH,
    AUTH_MANIFEST_TARGET_MISMATCH,
    AUTH_MANIFEST_PROJECT_MISMATCH,
    AUTH_MANIFEST_HARDWARE_MISMATCH,
    AUTH_MANIFEST_VERSION_NOT_NEWER,
    AUTH_MANIFEST_SHA_INVALID,
} authorization_mutation_t;

typedef struct {
    const char *name;
    authorization_mutation_t mutation;
    bool expected;
} authorization_case_t;

static fof_legacy_relay_authorization_view_t authorization_fixture(void)
{
    fof_legacy_relay_authorization_view_t authorization = {
        .automatic_bound = true,
        .bound_hardware_id = TEST_MAC,
        .identity = {
            .received = true,
            .version = FOF_LEGACY_READY_BOOTSTRAP_VERSION,
            .board = FOF_LEGACY_READY_BADGE_TARGET,
            .firmware_name = FOF_LEGACY_READY_BADGE_TARGET,
            .project = FOF_LEGACY_READY_BADGE_PROJECT,
            .hardware = FOF_LEGACY_READY_BADGE_HARDWARE,
            .hardware_id = TEST_MAC,
        },
        .manifest = {
            .target = FOF_LEGACY_READY_BADGE_TARGET,
            .version = TEST_TARGET_VERSION,
            .project = FOF_LEGACY_READY_BADGE_PROJECT,
            .hardware = FOF_LEGACY_READY_BADGE_HARDWARE,
            .sha256 = TEST_SHA256,
            .size = TEST_SIZE,
            .crc32 = TEST_CRC32,
        },
    };
    return authorization;
}

static void mutate_authorization(
    fof_legacy_relay_authorization_view_t *authorization,
    authorization_mutation_t mutation)
{
    switch (mutation) {
        case AUTH_NONE:
            break;
        case AUTH_MANUAL_CALLER:
            authorization->automatic_bound = false;
            break;
        case AUTH_BOUND_MISSING:
            authorization->bound_hardware_id = NULL;
            break;
        case AUTH_SOURCE_NOT_68:
            authorization->identity.version =
                "0.64.67-badge-live-follow";
            break;
        case AUTH_IDENTITY_INCOMPLETE:
            authorization->identity.received = false;
            break;
        case AUTH_LIVE_MAC_NONCANONICAL:
            authorization->identity.hardware_id =
                "e0-72-a1-f9-48-58";
            break;
        case AUTH_BOUND_MAC_NONCANONICAL:
            authorization->bound_hardware_id =
                "e0-72-a1-f9-48-58";
            break;
        case AUTH_BOUND_LIVE_MAC_MISMATCH:
            authorization->identity.hardware_id = TEST_OTHER_MAC;
            break;
        case AUTH_IDENTITY_BOARD_MISMATCH:
            authorization->identity.board = "scanner-s3-combo";
            break;
        case AUTH_IDENTITY_TARGET_MISMATCH:
            authorization->identity.firmware_name =
                "scanner-s3-combo";
            break;
        case AUTH_IDENTITY_PROJECT_MISMATCH:
            authorization->identity.project = "friendorfoe_scanner";
            break;
        case AUTH_IDENTITY_HARDWARE_MISMATCH:
            authorization->identity.hardware =
                "esp32-s3-devkitc-1";
            break;
        case AUTH_MANIFEST_TARGET_MISMATCH:
            authorization->manifest.target = "scanner-s3-combo";
            break;
        case AUTH_MANIFEST_PROJECT_MISMATCH:
            authorization->manifest.project = "friendorfoe_scanner";
            break;
        case AUTH_MANIFEST_HARDWARE_MISMATCH:
            authorization->manifest.hardware =
                "esp32-s3-devkitc-1";
            break;
        case AUTH_MANIFEST_VERSION_NOT_NEWER:
            authorization->manifest.version =
                FOF_LEGACY_READY_BOOTSTRAP_VERSION;
            break;
        case AUTH_MANIFEST_SHA_INVALID:
            authorization->manifest.sha256 = "invalid";
            break;
    }
}

void test_relay_policy_legacy_authorization_is_automatic_bound_only_table(void)
{
    static const authorization_case_t cases[] = {
        {"exact automatic bound .68", AUTH_NONE, true},
        {"manual caller", AUTH_MANUAL_CALLER, false},
        {"unbound caller", AUTH_BOUND_MISSING, false},
        {"non .68 source", AUTH_SOURCE_NOT_68, false},
        {"incomplete identity", AUTH_IDENTITY_INCOMPLETE, false},
        {"noncanonical live MAC", AUTH_LIVE_MAC_NONCANONICAL, false},
        {"noncanonical bound MAC", AUTH_BOUND_MAC_NONCANONICAL, false},
        {"changed bound MAC", AUTH_BOUND_LIVE_MAC_MISMATCH, false},
        {"identity board mismatch", AUTH_IDENTITY_BOARD_MISMATCH, false},
        {"identity target mismatch", AUTH_IDENTITY_TARGET_MISMATCH, false},
        {"identity project mismatch", AUTH_IDENTITY_PROJECT_MISMATCH, false},
        {"identity hardware mismatch", AUTH_IDENTITY_HARDWARE_MISMATCH, false},
        {"manifest target mismatch", AUTH_MANIFEST_TARGET_MISMATCH, false},
        {"manifest project mismatch", AUTH_MANIFEST_PROJECT_MISMATCH, false},
        {"manifest hardware mismatch", AUTH_MANIFEST_HARDWARE_MISMATCH, false},
        {"manifest not newer", AUTH_MANIFEST_VERSION_NOT_NEWER, false},
        {"manifest SHA invalid", AUTH_MANIFEST_SHA_INVALID, false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fof_legacy_relay_authorization_view_t authorization =
            authorization_fixture();
        mutate_authorization(&authorization, cases[i].mutation);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].expected,
            fof_firmware_legacy_relay_authorized(&authorization),
            cases[i].name);
    }

    TEST_ASSERT_FALSE(fof_firmware_legacy_relay_authorized(NULL));
}

void test_relay_policy_post_reboot_proof_requires_a_new_nonzero_boot_id(void)
{
    TEST_ASSERT_TRUE(fof_firmware_post_reboot_boot_id_proved(0U, 1U));
    TEST_ASSERT_TRUE(fof_firmware_post_reboot_boot_id_proved(
        0x12345678U, 0x87654321U));

    TEST_ASSERT_FALSE(fof_firmware_post_reboot_boot_id_proved(0U, 0U));
    TEST_ASSERT_FALSE(fof_firmware_post_reboot_boot_id_proved(
        0x12345678U, 0U));
    TEST_ASSERT_FALSE(fof_firmware_post_reboot_boot_id_proved(
        0x12345678U, 0x12345678U));
}
