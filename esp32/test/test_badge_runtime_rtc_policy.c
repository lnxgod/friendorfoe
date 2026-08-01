#include "badge_runtime_rtc_policy.h"

#include "badge_con_game.h"
#include "badge_update_maintenance_policy.h"
#include "unity.h"

#include <string.h>

static uint32_t get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint16_t get_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] |
           (uint16_t)((uint16_t)bytes[1] << 8U);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t test_crc32(const uint8_t *bytes, size_t byte_count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < byte_count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static void stamp_layout(uint8_t *bytes, uint16_t layout_size)
{
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_LAYOUT_MAGIC_OFFSET],
        BADGE_RUNTIME_RTC_LAYOUT_MAGIC);
    bytes[BADGE_RUNTIME_RTC_LAYOUT_VERSION_OFFSET] =
        (uint8_t)BADGE_RUNTIME_RTC_LAYOUT_VERSION;
    bytes[BADGE_RUNTIME_RTC_LAYOUT_VERSION_OFFSET + 1U] =
        (uint8_t)(BADGE_RUNTIME_RTC_LAYOUT_VERSION >> 8U);
    bytes[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET] = (uint8_t)layout_size;
    bytes[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET + 1U] =
        (uint8_t)(layout_size >> 8U);
}

static badge_con_game_state_t old_game_state(uint32_t last_decay_ms)
{
    return (badge_con_game_state_t) {
        .seed = BADGE_CON_ROLE_INFECTED,
        .role = BADGE_CON_ROLE_INFECTED,
        .active = true,
        .shield = 30U,
        .last_decay_ms = last_decay_ms,
    };
}

static void encode_old_game(
    const badge_con_game_state_t *game,
    uint32_t generation,
    uint8_t out[BADGE_CON_RTC_RECORD_BYTES])
{
    memset(out, 0, BADGE_CON_RTC_RECORD_BYTES);
    out[0] = 0xC4U;
    out[1] = 0x01U;
    out[2] = (uint8_t)game->seed;
    out[3] = (uint8_t)game->role;
    out[4] = game->active ? 1U : 0U;
    out[5] = game->shield;
    put_le32(&out[8], game->last_decay_ms);
    put_le32(&out[12], generation);
    put_le32(&out[16], test_crc32(out, 16U));
}

static void make_old_canary(
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE],
    uint32_t generation,
    uint32_t recovery_magic,
    bool armed_update_marker)
{
    memset(bytes, 0, BADGE_RUNTIME_RTC_CANARY_SIZE);
    badge_con_game_state_t game = old_game_state(1234U);
    encode_old_game(
        &game, generation,
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET]);
    if (armed_update_marker) {
        badge_update_maintenance_marker_t marker = {0};
        TEST_ASSERT_TRUE(badge_update_maintenance_marker_prepare(
            &marker, "0123456789ABCDEF", BADGE_UPDATE_SESSION_LENGTH));
        TEST_ASSERT_TRUE(badge_update_maintenance_marker_arm_reboot(
            &marker, generation));
        memcpy(
            &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET],
            &marker,
            sizeof(marker));
    }
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_RECOVERY_OFFSET],
        recovery_magic);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GENERATION_OFFSET],
        generation);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_MAGIC_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
}

void test_badge_rtc_current_layout_transition_preserves_stable_core(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_PRODUCTION_SIZE];
    memset(bytes, 0xa5, sizeof(bytes));
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET],
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC);
    put_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 42U);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);

    badge_runtime_rtc_boot_result_t result =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_PRODUCTION_SIZE);

    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_CURRENT, result.source);
    TEST_ASSERT_TRUE(result.expected_software_reset);
    TEST_ASSERT_EQUAL_UINT32(42U, result.consumed_generation);
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC,
        get_le32(&bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        42U, get_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_RTC_LAYOUT_MAGIC,
        get_le32(&bytes[BADGE_RUNTIME_RTC_LAYOUT_MAGIC_OFFSET]));
    TEST_ASSERT_EQUAL_UINT16(
        BADGE_RUNTIME_RTC_PRODUCTION_SIZE,
        get_le16(&bytes[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET]));
}

void test_badge_rtc_valid_current_header_is_authoritative_and_consumed(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE] = {0};
    put_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 43U);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
    stamp_layout(bytes, BADGE_RUNTIME_RTC_CANARY_SIZE);
    bytes[BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET] = 0x33U;

    badge_runtime_rtc_boot_result_t result =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_CURRENT, result.source);
    TEST_ASSERT_TRUE(result.prior_layout_valid);
    TEST_ASSERT_TRUE(result.expected_software_reset);
    TEST_ASSERT_EQUAL_UINT32(43U, result.consumed_generation);
    TEST_ASSERT_EQUAL_HEX8(
        0x33U, bytes[BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET]);
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET]));
}

void test_badge_rtc_original_v078_transition_is_one_hop_and_clears_extension(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    memset(bytes, 0x6d, sizeof(bytes));
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET],
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
    put_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);

    badge_runtime_rtc_boot_result_t first =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_LEGACY_V078, first.source);
    TEST_ASSERT_TRUE(first.expected_software_reset);
    TEST_ASSERT_EQUAL_UINT32(1U, first.consumed_generation);
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET]));
    for (size_t i = BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET;
         i < sizeof(bytes);
         ++i) {
        TEST_ASSERT_EQUAL_HEX8(0U, bytes[i]);
    }

    badge_runtime_rtc_boot_result_t replay =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, replay.source);
    TEST_ASSERT_FALSE(replay.expected_software_reset);
}

void test_badge_rtc_downgrade_retry_accepts_legacy_over_stale_valid_header(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE] = {0};
    badge_runtime_rtc_boot_result_t initialized =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), false,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, initialized.source);

    memset(
        &bytes[BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET],
        0x7e,
        sizeof(bytes) - BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
    put_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);

    badge_runtime_rtc_boot_result_t retry =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_LEGACY_V078, retry.source);
    TEST_ASSERT_TRUE(retry.expected_software_reset);
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    for (size_t i = BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET;
         i < sizeof(bytes);
         ++i) {
        TEST_ASSERT_EQUAL_HEX8(0U, bytes[i]);
    }

    badge_runtime_rtc_boot_result_t replay =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, replay.source);
}

void test_badge_rtc_old_canary_zero_marker_migrates_valid_game_and_recovery(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    uint8_t old_game[BADGE_CON_RTC_RECORD_BYTES];
    make_old_canary(
        bytes, 73U, BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC, false);
    memcpy(
        old_game,
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET],
        sizeof(old_game));

    badge_runtime_rtc_boot_result_t preinit =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY, preinit.source);
    TEST_ASSERT_TRUE(preinit.expected_software_reset);

    badge_runtime_rtc_boot_result_t migrated =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY, migrated.source);
    TEST_ASSERT_EQUAL_UINT32(73U, migrated.consumed_generation);
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC,
        get_le32(&bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        73U, get_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        old_game,
        &bytes[BADGE_RUNTIME_RTC_CANARY_GAME_OFFSET],
        sizeof(old_game));
    for (size_t i = BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET;
         i < BADGE_RUNTIME_RTC_CANARY_GAME_OFFSET;
         ++i) {
        TEST_ASSERT_EQUAL_HEX8(0U, bytes[i]);
    }
}

void test_badge_rtc_old_canary_requires_software_reset(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    make_old_canary(bytes, 79U, 0U, false);

    badge_runtime_rtc_boot_result_t result =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), false,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, result.source);
    TEST_ASSERT_FALSE(result.expected_software_reset);
}

void test_badge_rtc_old_canary_bound_marker_migrates_exact_bytes(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    uint8_t old_marker[sizeof(badge_update_maintenance_marker_t)];
    make_old_canary(bytes, 91U, 0U, true);
    memcpy(
        old_marker,
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET],
        sizeof(old_marker));

    badge_runtime_rtc_boot_result_t migrated =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY, migrated.source);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        old_marker,
        &bytes[BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET],
        sizeof(old_marker));
}

void test_badge_rtc_old_canary_rejects_corrupt_game_or_marker(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    make_old_canary(bytes, 101U, 0U, false);
    bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET + 16U] ^= 1U;
    badge_runtime_rtc_boot_result_t corrupt_game =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_NONE, corrupt_game.source);

    make_old_canary(bytes, 101U, 0U, false);
    bytes[BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET] = 1U;
    badge_runtime_rtc_boot_result_t corrupt_marker =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_NONE, corrupt_marker.source);

    make_old_canary(bytes, 101U, 0U, false);
    badge_update_maintenance_marker_t preparing = {0};
    TEST_ASSERT_TRUE(badge_update_maintenance_marker_prepare(
        &preparing, "0123456789ABCDEF", BADGE_UPDATE_SESSION_LENGTH));
    memcpy(
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET],
        &preparing,
        sizeof(preparing));
    badge_runtime_rtc_boot_result_t unbound_marker =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_NONE, unbound_marker.source);
}

void test_badge_rtc_corrupt_old_canary_transition_fails_closed(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    make_old_canary(
        bytes, 107U, BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC, false);
    bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET + 16U] ^= 1U;

    badge_runtime_rtc_boot_result_t result =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, result.source);
    TEST_ASSERT_FALSE(result.expected_software_reset);
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET]));
    for (size_t i = BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET;
         i < sizeof(bytes);
         ++i) {
        TEST_ASSERT_EQUAL_HEX8(0U, bytes[i]);
    }
}

void test_badge_rtc_old_canary_rejects_ambiguous_current_token(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    memset(bytes, 0, sizeof(bytes));
    badge_con_game_state_t game =
        old_game_state(BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
    encode_old_game(
        &game, 113U,
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET]);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_GENERATION_OFFSET], 113U);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_OLD_CANARY_MAGIC_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);

    badge_runtime_rtc_boot_result_t result =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_AMBIGUOUS, result.source);
    TEST_ASSERT_FALSE(result.expected_software_reset);
}

void test_badge_rtc_layout_size_changes_clear_extensions_without_moving_core(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    memset(bytes, 0x3c, sizeof(bytes));
    put_le32(&bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET], 0x11223344U);
    put_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET], 7U);
    put_le32(&bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET], 0U);
    stamp_layout(bytes, BADGE_RUNTIME_RTC_CANARY_SIZE);

    badge_runtime_rtc_boot_result_t production =
        badge_runtime_rtc_transition(
            bytes, BADGE_RUNTIME_RTC_PRODUCTION_SIZE, false,
            BADGE_RUNTIME_RTC_PRODUCTION_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, production.source);
    TEST_ASSERT_EQUAL_UINT32(
        0x11223344U,
        get_le32(&bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET]));
    TEST_ASSERT_EQUAL_UINT16(
        BADGE_RUNTIME_RTC_PRODUCTION_SIZE,
        get_le16(&bytes[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET]));

    badge_runtime_rtc_boot_result_t canary =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), false,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, canary.source);
    TEST_ASSERT_EQUAL_UINT16(
        BADGE_RUNTIME_RTC_CANARY_SIZE,
        get_le16(&bytes[BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET]));
    for (size_t i = BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET;
         i < sizeof(bytes);
         ++i) {
        TEST_ASSERT_EQUAL_HEX8(0U, bytes[i]);
    }
}

void test_badge_rtc_invalid_buffers_are_rejected_without_mutation(void)
{
    uint8_t undersized[BADGE_RUNTIME_RTC_PRODUCTION_SIZE - 1U];
    uint8_t oversized[BADGE_RUNTIME_RTC_PRODUCTION_SIZE + 1U];
    uint8_t undersized_before[sizeof(undersized)];
    uint8_t oversized_before[sizeof(oversized)];
    memset(undersized, 0x52, sizeof(undersized));
    memset(oversized, 0xa7, sizeof(oversized));
    memcpy(undersized_before, undersized, sizeof(undersized));
    memcpy(oversized_before, oversized, sizeof(oversized));

    badge_runtime_rtc_boot_result_t null_result =
        badge_runtime_rtc_transition(
            NULL, BADGE_RUNTIME_RTC_PRODUCTION_SIZE, true,
            BADGE_RUNTIME_RTC_PRODUCTION_SIZE);
    badge_runtime_rtc_boot_result_t small_result =
        badge_runtime_rtc_transition(
            undersized, sizeof(undersized), true,
            BADGE_RUNTIME_RTC_PRODUCTION_SIZE);
    badge_runtime_rtc_boot_result_t large_result =
        badge_runtime_rtc_transition(
            oversized, sizeof(oversized), true,
            BADGE_RUNTIME_RTC_PRODUCTION_SIZE);

    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, null_result.source);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, small_result.source);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, large_result.source);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        undersized_before, undersized, sizeof(undersized));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        oversized_before, oversized, sizeof(oversized));
}

void test_badge_rtc_preinit_prepare_preserves_recovery_and_allows_fresh_arm(void)
{
    uint8_t bytes[BADGE_RUNTIME_RTC_CANARY_SIZE];
    memset(bytes, 0xd3, sizeof(bytes));
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET],
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC);

    badge_runtime_rtc_boot_result_t prepared =
        badge_runtime_rtc_transition(
            bytes, sizeof(bytes), false,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_NONE, prepared.source);
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC,
        get_le32(&bytes[BADGE_RUNTIME_RTC_RECOVERY_OFFSET]));
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_RTC_LAYOUT_MAGIC,
        get_le32(&bytes[BADGE_RUNTIME_RTC_LAYOUT_MAGIC_OFFSET]));

    uint32_t generation =
        badge_runtime_expected_reboot_next_generation(
            get_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    put_le32(&bytes[BADGE_RUNTIME_RTC_GENERATION_OFFSET], generation);
    put_le32(
        &bytes[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);

    badge_runtime_rtc_boot_result_t next_boot =
        badge_runtime_rtc_classify(
            bytes, sizeof(bytes), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RTC_SOURCE_CURRENT, next_boot.source);
    TEST_ASSERT_TRUE(next_boot.expected_software_reset);
    TEST_ASSERT_EQUAL_UINT32(generation, next_boot.consumed_generation);
}

void test_badge_rtc_consumed_reserved_generation_never_replays_as_v078(void)
{
    uint8_t current[BADGE_RUNTIME_RTC_CANARY_SIZE] = {0};
    put_le32(
        &current[BADGE_RUNTIME_RTC_GENERATION_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
    put_le32(
        &current[BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET],
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC);
    stamp_layout(current, BADGE_RUNTIME_RTC_CANARY_SIZE);

    badge_runtime_rtc_boot_result_t consumed_current =
        badge_runtime_rtc_transition(
            current, sizeof(current), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_CURRENT, consumed_current.source);
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC,
        consumed_current.consumed_generation);
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(&current[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_NONE,
        badge_runtime_rtc_classify(
            current, sizeof(current), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE).source);

    uint8_t old_canary[BADGE_RUNTIME_RTC_CANARY_SIZE];
    make_old_canary(
        old_canary, BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC, 0U, false);
    badge_runtime_rtc_boot_result_t consumed_old =
        badge_runtime_rtc_transition(
            old_canary, sizeof(old_canary), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE);
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY, consumed_old.source);
    TEST_ASSERT_EQUAL_UINT32(
        0U, get_le32(
            &old_canary[BADGE_RUNTIME_RTC_GENERATION_OFFSET]));
    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_RTC_SOURCE_NONE,
        badge_runtime_rtc_classify(
            old_canary, sizeof(old_canary), true,
            BADGE_RUNTIME_RTC_CANARY_SIZE).source);
}
