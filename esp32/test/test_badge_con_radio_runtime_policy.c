#include "unity.h"

#include "badge_con_radio_runtime_policy.h"

void test_badge_con_radio_runtime_controller_admission_requires_every_gate(void)
{
    typedef struct {
        bool firmware_operation_allows_radio;
        bool identity_valid;
        bool game_snapshot_valid;
        bool game_active;
        bool ota_pending_verify;
        bool expected;
    } admission_case_t;

    static const admission_case_t cases[] = {
        {true,  true,  true,  true,  false, true},
        {false, true,  true,  true,  false, false},
        {true,  false, true,  true,  false, false},
        {true,  true,  false, true,  false, false},
        {true,  true,  true,  false, false, false},
        {true,  true,  true,  true,  true,  false},
        {false, false, false, false, true,  false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL(
            cases[i].expected,
            badge_con_radio_runtime_controller_init_allowed(
                cases[i].firmware_operation_allows_radio,
                cases[i].identity_valid,
                cases[i].game_snapshot_valid,
                cases[i].game_active,
                cases[i].ota_pending_verify));
    }
}

void test_badge_con_radio_runtime_memory_gate_is_inclusive_and_fail_closed(void)
{
    typedef struct {
        uint32_t internal_free;
        uint32_t internal_largest;
        bool psram_initialized;
        uint32_t psram_total;
        uint32_t psram_free;
        badge_con_radio_memory_gate_t expected;
    } memory_case_t;

    static const memory_case_t cases[] = {
        {24576U, 16384U, true, 8388608U, 5242880U,
         BADGE_CON_RADIO_MEMORY_OK},
        {24575U, 16384U, true, 8388608U, 5242880U,
         BADGE_CON_RADIO_MEMORY_INTERNAL},
        {24576U, 16383U, true, 8388608U, 5242880U,
         BADGE_CON_RADIO_MEMORY_INTERNAL},
        {24576U, 16384U, false, 8388608U, 5242880U,
         BADGE_CON_RADIO_MEMORY_PSRAM},
        {24576U, 16384U, true, 8388607U, 5242880U,
         BADGE_CON_RADIO_MEMORY_PSRAM},
        {24576U, 16384U, true, 8388608U, 5242879U,
         BADGE_CON_RADIO_MEMORY_PSRAM},
        {0U, 0U, false, 0U, 0U,
         BADGE_CON_RADIO_MEMORY_PSRAM},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL_INT(
            cases[i].expected,
            badge_con_radio_runtime_memory_gate(
                cases[i].internal_free,
                cases[i].internal_largest,
                cases[i].psram_initialized,
                cases[i].psram_total,
                cases[i].psram_free));
    }
}

void test_badge_con_radio_runtime_tracks_only_nonzero_boot_changes(void)
{
    badge_con_radio_runtime_policy_t policy;
    badge_con_radio_runtime_policy_init(&policy);

    TEST_ASSERT_FALSE(badge_con_radio_runtime_observe_boot_id(
        &policy, 0, 0U));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_observe_boot_id(
        &policy, 1, 0U));

    TEST_ASSERT_TRUE(badge_con_radio_runtime_observe_boot_id(
        &policy, 0, 101U));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_observe_boot_id(
        &policy, 0, 101U));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_observe_boot_id(
        &policy, 0, 0U));

    TEST_ASSERT_TRUE(badge_con_radio_runtime_observe_boot_id(
        &policy, 1, 202U));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_observe_boot_id(
        &policy, 1, 202U));
    TEST_ASSERT_TRUE(badge_con_radio_runtime_observe_boot_id(
        &policy, 1, 203U));
}

void test_badge_con_radio_runtime_resets_and_retries_self_delivery(void)
{
    badge_con_radio_runtime_policy_t policy;
    badge_con_radio_runtime_policy_init(&policy);

    TEST_ASSERT_EQUAL_INT(0,
        badge_con_radio_runtime_next_unsent_lane(&policy));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_all_self_sent(&policy));

    badge_con_radio_runtime_note_self_sent(&policy, 0, 100U);
    TEST_ASSERT_EQUAL_INT(1,
        badge_con_radio_runtime_next_unsent_lane(&policy));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_all_self_sent(&policy));

    /* A failed lane-1 write records nothing, so lane 1 remains next. */
    TEST_ASSERT_EQUAL_INT(1,
        badge_con_radio_runtime_next_unsent_lane(&policy));
    badge_con_radio_runtime_note_self_sent(&policy, 1, 200U);
    TEST_ASSERT_EQUAL_INT(-1,
        badge_con_radio_runtime_next_unsent_lane(&policy));
    TEST_ASSERT_TRUE(badge_con_radio_runtime_all_self_sent(&policy));

    /* Either scanner's new nonzero boot identity invalidates both sends. */
    TEST_ASSERT_TRUE(badge_con_radio_runtime_observe_boot_id(
        &policy, 1, 777U));
    TEST_ASSERT_EQUAL_INT(0,
        badge_con_radio_runtime_next_unsent_lane(&policy));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_all_self_sent(&policy));
}

void test_badge_con_radio_runtime_retries_both_lanes_after_ack_timeout(void)
{
    badge_con_radio_runtime_policy_t policy;
    badge_con_radio_runtime_policy_init(&policy);

    badge_con_radio_runtime_note_self_sent(&policy, 0, 100U);
    badge_con_radio_runtime_note_self_sent(&policy, 1, 200U);
    TEST_ASSERT_TRUE(badge_con_radio_runtime_all_self_sent(&policy));

    TEST_ASSERT_FALSE(badge_con_radio_runtime_retry_self_due(
        &policy, false, 200U + BADGE_CON_SELF_ACK_RETRY_MS - 1U));
    TEST_ASSERT_TRUE(badge_con_radio_runtime_all_self_sent(&policy));
    TEST_ASSERT_TRUE(badge_con_radio_runtime_retry_self_due(
        &policy, false, 200U + BADGE_CON_SELF_ACK_RETRY_MS));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_all_self_sent(&policy));
    TEST_ASSERT_EQUAL_INT(0,
        badge_con_radio_runtime_next_unsent_lane(&policy));

    badge_con_radio_runtime_note_self_sent(&policy, 0, UINT32_MAX - 10U);
    badge_con_radio_runtime_note_self_sent(&policy, 1, UINT32_MAX - 5U);
    TEST_ASSERT_FALSE(badge_con_radio_runtime_retry_self_due(
        &policy, false,
        UINT32_MAX - 5U + BADGE_CON_SELF_ACK_RETRY_MS - 1U));
    TEST_ASSERT_TRUE(badge_con_radio_runtime_retry_self_due(
        &policy, false,
        UINT32_MAX - 5U + BADGE_CON_SELF_ACK_RETRY_MS));
}

void test_badge_con_radio_runtime_exact_ack_cancels_retry(void)
{
    badge_con_radio_runtime_policy_t policy;
    badge_con_radio_runtime_policy_init(&policy);

    badge_con_radio_runtime_note_self_sent(&policy, 0, 100U);
    badge_con_radio_runtime_note_self_sent(&policy, 1, 200U);
    TEST_ASSERT_FALSE(badge_con_radio_runtime_retry_self_due(
        &policy, true, 200U + BADGE_CON_SELF_ACK_RETRY_MS));
    TEST_ASSERT_TRUE(badge_con_radio_runtime_all_self_sent(&policy));
    TEST_ASSERT_FALSE(badge_con_radio_runtime_retry_self_due(
        &policy, false, 200U + (2U * BADGE_CON_SELF_ACK_RETRY_MS)));
}
