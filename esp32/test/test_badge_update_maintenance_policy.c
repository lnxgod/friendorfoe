#include "badge_update_maintenance_policy.h"
#include "unity.h"

#include <limits.h>
#include <string.h>

static const char VALID_SESSION[] = "0123456789ABCDEF";

static badge_update_maintenance_marker_t preparing_marker(void)
{
    badge_update_maintenance_marker_t marker = {0};
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_prepare(
        &marker, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    return marker;
}

void test_badge_update_uplink_ota_begin_admission_is_session_and_mode_bound(void)
{
    typedef struct {
        bool canary_build;
        bool maintenance_active;
        size_t member_count;
        bool session_present;
        bool session_is_string;
        bool session_matches;
        badge_update_ota_begin_admission_t expected;
    } fixture_t;
    static const fixture_t FIXTURES[] = {
        {true, true, 11U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_ADMIT},
        {true, true, 10U, false, false, false,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, true, 11U, true, true, false,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, true, 11U, true, false, false,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, true, 12U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, false, 10U, false, false, false,
         BADGE_UPDATE_OTA_BEGIN_ADMIT},
        {true, false, 11U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION},
        {false, false, 10U, false, false, false,
         BADGE_UPDATE_OTA_BEGIN_ADMIT},
        {false, false, 11U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION},
        {false, true, 11U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION},
    };

    for (size_t i = 0U; i < sizeof(FIXTURES) / sizeof(FIXTURES[0]); ++i) {
        const fixture_t *fixture = &FIXTURES[i];
        TEST_ASSERT_EQUAL(
            fixture->expected,
            badge_update_uplink_ota_begin_admission_decide(
                fixture->canary_build,
                fixture->maintenance_active,
                fixture->member_count,
                fixture->session_present,
                fixture->session_is_string,
                fixture->session_matches));
    }
}

void test_badge_update_scanner_stage_begin_admission_is_session_and_mode_bound(
    void)
{
    typedef struct {
        bool canary_build;
        bool maintenance_active;
        size_t member_count;
        bool session_present;
        bool session_is_string;
        bool session_matches;
        badge_update_ota_begin_admission_t expected;
    } fixture_t;
    static const fixture_t FIXTURES[] = {
        {true, true, 12U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_ADMIT},
        {true, true, 11U, false, false, false,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, true, 12U, true, true, false,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, true, 12U, true, false, false,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, true, 13U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH},
        {true, false, 11U, false, false, false,
         BADGE_UPDATE_OTA_BEGIN_ADMIT},
        {true, false, 12U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION},
        {false, false, 11U, false, false, false,
         BADGE_UPDATE_OTA_BEGIN_ADMIT},
        {false, false, 12U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION},
        {false, true, 12U, true, true, true,
         BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION},
    };

    for (size_t i = 0U; i < sizeof(FIXTURES) / sizeof(FIXTURES[0]); ++i) {
        const fixture_t *fixture = &FIXTURES[i];
        TEST_ASSERT_EQUAL(
            fixture->expected,
            badge_update_scanner_stage_begin_admission_decide(
                fixture->canary_build,
                fixture->maintenance_active,
                fixture->member_count,
                fixture->session_present,
                fixture->session_is_string,
                fixture->session_matches));
    }
}

void test_badge_update_session_requires_exact_uppercase_nonzero_hex(void)
{
    TEST_ASSERT_TRUE(badge_update_session_valid(
        VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_TRUE(badge_update_session_valid(
        "FFFFFFFFFFFFFFFF", BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_FALSE(badge_update_session_valid(
        "0000000000000000", BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_FALSE(badge_update_session_valid(
        "0123456789abcDEF", BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_FALSE(badge_update_session_valid(
        "0123456789ABCDE", BADGE_UPDATE_SESSION_LENGTH - 1U));
    TEST_ASSERT_FALSE(badge_update_session_valid(
        "0123456789ABCDEFG", BADGE_UPDATE_SESSION_LENGTH + 1U));
    TEST_ASSERT_FALSE(badge_update_session_valid(
        "0123456789ABCDE ", BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_FALSE(badge_update_session_valid(
        "0123456789ABCDE-", BADGE_UPDATE_SESSION_LENGTH));

    char embedded_nul[BADGE_UPDATE_SESSION_LENGTH] = "01234567";
    memcpy(embedded_nul + 9U, "9ABCDEF", 7U);
    TEST_ASSERT_FALSE(badge_update_session_valid(
        embedded_nul, sizeof(embedded_nul)));
    TEST_ASSERT_FALSE(badge_update_session_valid(NULL, 0U));
}

void test_badge_update_preparing_marker_is_exact_and_checksummed(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_UPDATE_MAINTENANCE_MAGIC, marker.magic);
    TEST_ASSERT_EQUAL_UINT16(
        BADGE_UPDATE_MAINTENANCE_VERSION, marker.version);
    TEST_ASSERT_EQUAL_UINT16(sizeof(marker), marker.size);
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_PHASE_PREPARING, marker.phase);
    TEST_ASSERT_EQUAL_UINT8(0U, marker.boot_count);
    TEST_ASSERT_EQUAL_UINT32(0U, marker.expected_reboot_generation);
    TEST_ASSERT_EQUAL_STRING(VALID_SESSION, marker.session);
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));

    badge_update_maintenance_marker_t corrupted = marker;
    corrupted.session[3] ^= 1;
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_valid(&corrupted));
    corrupted = marker;
    corrupted.crc32 ^= 1U;
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_valid(&corrupted));
    corrupted = marker;
    corrupted.reserved = 1U;
    badge_update_maintenance_marker_seal(&corrupted);
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_valid(&corrupted));
}

void test_badge_update_marker_session_match_is_exact_and_conflict_safe(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();
    TEST_ASSERT_TRUE(badge_update_maintenance_session_matches(
        &marker, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_FALSE(badge_update_maintenance_session_matches(
        &marker, "1123456789ABCDEF", BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_FALSE(badge_update_maintenance_session_matches(
        &marker, "0123456789abcdef", BADGE_UPDATE_SESSION_LENGTH));

    badge_update_maintenance_marker_t corrupted = marker;
    corrupted.crc32 ^= 1U;
    TEST_ASSERT_FALSE(badge_update_maintenance_session_matches(
        &corrupted, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
}

void test_badge_update_abort_clears_only_exact_preparing_or_active_session(void)
{
    static const char OTHER_SESSION[] = "1123456789ABCDEF";
    static const char LOWER_SESSION[] = "0123456789abcdef";

    badge_update_maintenance_marker_t preparing = preparing_marker();
    badge_update_maintenance_marker_t original = preparing;
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_abort(
        &preparing, OTHER_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_EQUAL_MEMORY(&original, &preparing, sizeof(preparing));
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_abort(
        &preparing, LOWER_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_EQUAL_MEMORY(&original, &preparing, sizeof(preparing));
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_abort(
        &preparing, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH - 1U));
    TEST_ASSERT_EQUAL_MEMORY(&original, &preparing, sizeof(preparing));

    badge_update_maintenance_marker_t corrupted = preparing;
    corrupted.crc32 ^= 1U;
    original = corrupted;
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_abort(
        &corrupted, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_EQUAL_MEMORY(&original, &corrupted, sizeof(corrupted));

    TEST_ASSERT_TRUE(badge_update_maintenance_marker_abort(
        &preparing, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    badge_update_maintenance_marker_t cleared = {0};
    TEST_ASSERT_EQUAL_MEMORY(&cleared, &preparing, sizeof(preparing));

    badge_update_maintenance_marker_t active = preparing_marker();
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_arm_reboot(
        &active, 7U));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_activate(&active));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_abort(
        &active, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));
    TEST_ASSERT_EQUAL_MEMORY(&cleared, &active, sizeof(active));
}

void test_badge_update_preparing_abort_waits_for_safe_owned_restart(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();

    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_ABORT_WAIT_PREEMPTION,
        badge_update_preparing_abort_decide(
            &marker, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH,
            false, false));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_ABORT_WAIT_REBOOT_OWNER,
        badge_update_preparing_abort_decide(
            &marker, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH,
            true, false));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_ABORT_CLEAR_AND_REBOOT,
        badge_update_preparing_abort_decide(
            &marker, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH,
            true, true));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_abort(
        &marker, VALID_SESSION, BADGE_UPDATE_SESSION_LENGTH));

    badge_update_maintenance_marker_t cleared = {0};
    TEST_ASSERT_EQUAL_MEMORY(&cleared, &marker, sizeof(marker));
}

void test_badge_update_boot_requires_reboot_armed_exact_expected_generation(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_arm_reboot(
        &marker, 7U));
    TEST_ASSERT_EQUAL(BADGE_UPDATE_PHASE_REBOOT_ARMED, marker.phase);
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));

    badge_update_maintenance_marker_t mismatch = marker;
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_CLEAR,
        badge_update_maintenance_boot_decide(
            &mismatch, true, 8U, false));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_CLEAR,
        badge_update_maintenance_boot_decide(
            &mismatch, false, 7U, false));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_CLEAR,
        badge_update_maintenance_boot_decide(
            &mismatch, true, 7U, true));

    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_ENTER,
        badge_update_maintenance_boot_decide(
            &marker, true, 7U, false));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_activate(&marker));
    TEST_ASSERT_EQUAL(BADGE_UPDATE_PHASE_ACTIVE, marker.phase);
    TEST_ASSERT_EQUAL_UINT8(1U, marker.boot_count);
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));
}

void test_badge_update_preparing_or_corrupt_marker_never_enters_maintenance(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_CLEAR,
        badge_update_maintenance_boot_decide(
            &marker, true, 1U, false));
    marker.crc32 ^= 1U;
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_CLEAR,
        badge_update_maintenance_boot_decide(
            &marker, true, 1U, false));
}

void test_badge_update_boot_count_is_bounded(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();
    for (uint8_t boot = 1U;
         boot <= BADGE_UPDATE_MAINTENANCE_MAX_BOOTS;
         ++boot) {
        TEST_ASSERT_TRUE(badge_update_maintenance_marker_arm_reboot(
            &marker, boot));
        TEST_ASSERT_EQUAL(
            BADGE_UPDATE_BOOT_ENTER,
            badge_update_maintenance_boot_decide(
                &marker, true, boot, false));
        TEST_ASSERT_TRUE(badge_update_maintenance_marker_activate(&marker));
        TEST_ASSERT_EQUAL_UINT8(boot, marker.boot_count);
    }
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_arm_reboot(
        &marker, 99U));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_BOOT_CLEAR,
        badge_update_maintenance_boot_decide(
            &marker, true, 99U, false));
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_activate(&marker));
}

void test_badge_update_committed_summary_is_canonical_and_exact(void)
{
    badge_update_maintenance_marker_t marker = preparing_marker();
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_arm_reboot(
        &marker, 1U));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_activate(&marker));
    const char sha[] =
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_commit_uplink(
        &marker, "0.64.80-badge-defcon34", sha, 1234567U,
        "ota_1"));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));
    TEST_ASSERT_TRUE(marker.uplink_committed);
    TEST_ASSERT_EQUAL_STRING("0.64.80-badge-defcon34",
                             marker.uplink_version);
    TEST_ASSERT_EQUAL_STRING(sha, marker.uplink_sha256);
    TEST_ASSERT_EQUAL_UINT32(1234567U, marker.uplink_size);
    TEST_ASSERT_EQUAL_UINT32(1234567U, marker.uplink_received);
    TEST_ASSERT_EQUAL_STRING("ota_1", marker.uplink_partition);

    badge_update_maintenance_marker_t bad = marker;
    bad.uplink_sha256[0] = 'A';
    badge_update_maintenance_marker_seal(&bad);
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_valid(&bad));
    bad = marker;
    bad.uplink_received--;
    badge_update_maintenance_marker_seal(&bad);
    TEST_ASSERT_FALSE(badge_update_maintenance_marker_valid(&bad));

    TEST_ASSERT_TRUE(
        badge_update_maintenance_marker_clear_uplink(&marker));
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_valid(&marker));
    TEST_ASSERT_FALSE(marker.uplink_committed);
    TEST_ASSERT_EQUAL_CHAR('\0', marker.uplink_version[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', marker.uplink_sha256[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', marker.uplink_partition[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, marker.uplink_size);
    TEST_ASSERT_EQUAL_UINT32(0U, marker.uplink_received);
}

void test_badge_update_prepare_decision_waits_only_for_owned_or_running_work(void)
{
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_PREPARE_WAITING_FOR_OWNER,
        badge_update_prepare_decide(true, false, false));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_PREPARE_BUSY,
        badge_update_prepare_decide(false, false, false));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_PREPARE_REBOOT_QUIESCED,
        badge_update_prepare_decide(false, true, true));
    TEST_ASSERT_EQUAL(
        BADGE_UPDATE_PREPARE_REBOOT_SAFE,
        badge_update_prepare_decide(false, true, false));
}

void test_badge_update_inactivity_is_wrap_safe_and_exact(void)
{
    TEST_ASSERT_FALSE(badge_update_maintenance_inactivity_due(
        119999U, 0U));
    TEST_ASSERT_TRUE(badge_update_maintenance_inactivity_due(
        BADGE_UPDATE_MAINTENANCE_INACTIVITY_MS, 0U));
    TEST_ASSERT_FALSE(badge_update_maintenance_inactivity_due(
        98U, UINT32_MAX - 20U));
    TEST_ASSERT_TRUE(badge_update_maintenance_inactivity_due(
        120100U, 100U));
}

void test_badge_update_pending_verify_health_requires_every_maintenance_gate(void)
{
    TEST_ASSERT_TRUE(badge_update_maintenance_health_satisfied(
        10000U, true, true, true, true, false, 24576U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        9999U, true, true, true, true, false, 24576U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        10000U, true, false, true, true, false, 24576U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        10000U, true, true, false, true, false, 24576U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        10000U, true, true, true, false, false, 24576U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        10000U, true, true, true, true, true, 24576U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        10000U, true, true, true, true, false, 24575U, 16384U));
    TEST_ASSERT_FALSE(badge_update_maintenance_health_satisfied(
        10000U, true, true, true, true, false, 24576U, 16383U));
}
