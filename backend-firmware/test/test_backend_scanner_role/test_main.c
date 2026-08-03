#include <stdbool.h>
#include <stdint.h>

#include <unity.h>

#include "backend_scanner_role.h"
#include "../support/backend_test_main.h"

static backend_scanner_role_result_t test_role_apply(
    backend_scanner_role_state_t *state,
    uint32_t boot_id,
    uint32_t generation,
    backend_scan_profile_t profile)
{
    return backend_scanner_role_apply(
        state, boot_id, generation,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        19U,
#endif
        profile);
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_scanner_stays_quiescent_until_current_boot_generation(void)
{
    backend_scanner_role_state_t state;

    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT,
                      backend_scanner_role_effective(&state));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY,
                      backend_scanner_role_effective(&state));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_STALE, test_role_apply(
        &state, 77, 3, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_BOOT, test_role_apply(
        &state, 76, 5, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_BOOT, test_role_apply(
        &state, 78, 5, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY,
                      backend_scanner_role_effective(&state));
}

void test_equal_role_generation_is_an_idempotent_ack_refresh(void)
{
    backend_scanner_role_state_t state;
    backend_scanner_role_ack_t ack;

    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_TRUE(backend_scanner_role_take_ack(
        &state, true, false, &ack));
    uint32_t transitions = state.radio_transition_count;

    TEST_ASSERT_EQUAL(BACKEND_ROLE_REFRESHED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(transitions, state.radio_transition_count);
    TEST_ASSERT_TRUE(state.ack_pending);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_CONFLICT, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_TRUE(state.ack_pending);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, state.effective);
    TEST_ASSERT_EQUAL_UINT32(4, state.generation);
}

void test_larger_generation_only_transitions_radios_when_profile_changes(void)
{
    backend_scanner_role_state_t state;

    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL_UINT32(0, state.radio_transition_count);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 1, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_transition_count);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 2, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_transition_count);
    TEST_ASSERT_EQUAL_UINT32(2, state.generation);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 9, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_EQUAL_UINT32(2, state.radio_transition_count);
    TEST_ASSERT_EQUAL_UINT32(9, state.generation);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, state.effective);
}

void test_stale_conflicting_and_invalid_commands_do_not_mutate_state(void)
{
    backend_scanner_role_state_t state;
    backend_scanner_role_ack_t ack;

    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_TRUE(backend_scanner_role_take_ack(
        &state, false, true, &ack));
    TEST_ASSERT_FALSE(state.ack_pending);

    TEST_ASSERT_EQUAL(BACKEND_ROLE_STALE, test_role_apply(
        &state, 77, 3, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_CONFLICT, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_BOOT, test_role_apply(
        &state, 88, 5, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_PROFILE, test_role_apply(
        &state, 77, 5, (backend_scan_profile_t)99));

    TEST_ASSERT_EQUAL_UINT32(77, state.boot_id);
    TEST_ASSERT_EQUAL_UINT32(4, state.generation);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, state.effective);
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_transition_count);
    TEST_ASSERT_FALSE(state.ack_pending);
}

void test_applied_and_refreshed_commands_each_queue_one_complete_ack(void)
{
    backend_scanner_role_state_t state;
    backend_scanner_role_ack_t ack = {0};

    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_FALSE(backend_scanner_role_take_ack(
        &state, true, false, NULL));
    TEST_ASSERT_TRUE(state.ack_pending);
    TEST_ASSERT_TRUE(backend_scanner_role_take_ack(
        &state, true, false, &ack));
    TEST_ASSERT_EQUAL_UINT32(77, ack.boot_id);
    TEST_ASSERT_EQUAL_UINT32(4, ack.generation);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, ack.profile);
    TEST_ASSERT_TRUE(ack.ble_healthy);
    TEST_ASSERT_FALSE(ack.wifi_healthy);
    TEST_ASSERT_FALSE(ack.radio_healthy);
    TEST_ASSERT_FALSE(state.ack_pending);
    TEST_ASSERT_FALSE(backend_scanner_role_take_ack(
        &state, true, true, &ack));

    TEST_ASSERT_EQUAL(BACKEND_ROLE_REFRESHED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_TRUE(backend_scanner_role_take_ack(
        &state, true, true, &ack));
    TEST_ASSERT_TRUE(ack.ble_healthy);
    TEST_ASSERT_TRUE(ack.wifi_healthy);
    TEST_ASSERT_TRUE(ack.radio_healthy);
    TEST_ASSERT_FALSE(backend_scanner_role_take_ack(
        &state, true, true, &ack));
}

void test_every_declared_profile_is_accepted_and_unknown_profile_is_rejected(void)
{
    backend_scanner_role_state_t state;

    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 1, BACKEND_SCAN_PROFILE_QUIESCENT));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 2, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 3, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, test_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_PROFILE, test_role_apply(
        &state, 77, 5, (backend_scan_profile_t)-1));
    TEST_ASSERT_EQUAL_UINT32(4, state.generation);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, state.effective);
}

void test_role_helpers_fail_closed_for_null_state(void)
{
    backend_scanner_role_ack_t ack = {0};

    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT,
                      backend_scanner_role_effective(NULL));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_BOOT, test_role_apply(
        NULL, 77, 1, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_FALSE(backend_scanner_role_take_ack(
        NULL, true, true, &ack));
}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
void test_fullsize_role_generation_cannot_reassign_topology(void)
{
    backend_scanner_role_state_t state;
    backend_scanner_role_init(&state, 77U);

    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, backend_scanner_role_apply(
        &state, 77U, 4U, 19U, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(19U, state.topology_generation);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, backend_scanner_role_apply(
        &state, 77U, 5U, 19U, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(19U, state.topology_generation);

    TEST_ASSERT_EQUAL(BACKEND_ROLE_STALE, backend_scanner_role_apply(
        &state, 77U, 6U, 18U, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_CONFLICT, backend_scanner_role_apply(
        &state, 77U, 6U, 20U, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(19U, state.topology_generation);
    TEST_ASSERT_EQUAL_UINT32(5U, state.generation);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, state.effective);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_scanner_stays_quiescent_until_current_boot_generation);
    BACKEND_RUN_TEST(
        test_equal_role_generation_is_an_idempotent_ack_refresh);
    BACKEND_RUN_TEST(
        test_larger_generation_only_transitions_radios_when_profile_changes);
    BACKEND_RUN_TEST(
        test_stale_conflicting_and_invalid_commands_do_not_mutate_state);
    BACKEND_RUN_TEST(
        test_applied_and_refreshed_commands_each_queue_one_complete_ack);
    BACKEND_RUN_TEST(
        test_every_declared_profile_is_accepted_and_unknown_profile_is_rejected);
    BACKEND_RUN_TEST(test_role_helpers_fail_closed_for_null_state);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    BACKEND_RUN_TEST(test_fullsize_role_generation_cannot_reassign_topology);
#endif
    return UNITY_END();
}
