#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <unity.h>

#include "backend_flow_policy.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_flow_queue_contract_is_fixed_and_control_is_reserved(void)
{
    TEST_ASSERT_EQUAL_UINT32(64U, BACKEND_FLOW_DETECTION_QUEUE_CAPACITY);
    TEST_ASSERT_EQUAL_UINT32(48U, BACKEND_FLOW_PAUSE_DEPTH);
    TEST_ASSERT_EQUAL_UINT32(24U, BACKEND_FLOW_RESUME_DEPTH);
    TEST_ASSERT_EQUAL_UINT32(4U, BACKEND_FLOW_CONTROL_QUEUE_RESERVE);

    TEST_ASSERT_TRUE(backend_flow_detection_enqueue_allowed(false, 63U));
    TEST_ASSERT_FALSE(backend_flow_detection_enqueue_allowed(false, 64U));
    TEST_ASSERT_FALSE(backend_flow_detection_enqueue_allowed(true, 0U));
    TEST_ASSERT_TRUE(backend_flow_control_enqueue_allowed(3U));
    TEST_ASSERT_FALSE(backend_flow_control_enqueue_allowed(4U));
}

void test_flow_policy_pauses_at_48_and_not_at_47(void)
{
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_NO_CHANGE,
        backend_flow_policy_update(47U, false));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_PAUSE,
        backend_flow_policy_update(48U, false));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_PAUSE,
        backend_flow_policy_update(64U, false));
}

void test_flow_policy_resumes_at_24_and_not_at_25(void)
{
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_NO_CHANGE,
        backend_flow_policy_update(25U, true));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_RESUME,
        backend_flow_policy_update(24U, true));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_RESUME,
        backend_flow_policy_update(0U, true));
}

void test_flow_generation_is_monotonic_and_equal_command_refreshes_ack(void)
{
    backend_flow_state_t state;
    backend_flow_ack_t ack = {0};
    backend_flow_state_init(&state);

    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_APPLIED,
        backend_flow_state_apply(&state, 6U, true));
    TEST_ASSERT_TRUE(state.paused);
    TEST_ASSERT_TRUE(backend_flow_state_take_ack(&state, &ack));
    TEST_ASSERT_EQUAL_UINT32(6U, ack.generation);
    TEST_ASSERT_TRUE(ack.paused);
    TEST_ASSERT_FALSE(backend_flow_state_take_ack(&state, &ack));

    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_REFRESHED,
        backend_flow_state_apply(&state, 6U, true));
    TEST_ASSERT_TRUE(backend_flow_state_take_ack(&state, &ack));
    TEST_ASSERT_EQUAL_UINT32(6U, ack.generation);
    TEST_ASSERT_TRUE(ack.paused);

    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_CONFLICT,
        backend_flow_state_apply(&state, 6U, false));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_STALE,
        backend_flow_state_apply(&state, 5U, false));
    TEST_ASSERT_TRUE(state.paused);
    TEST_ASSERT_FALSE(backend_flow_state_take_ack(&state, &ack));
}

void test_larger_flow_generation_applies_and_zero_is_invalid(void)
{
    backend_flow_state_t state;
    backend_flow_state_init(&state);

    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_INVALID_GENERATION,
        backend_flow_state_apply(&state, 0U, true));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_APPLIED,
        backend_flow_state_apply(&state, 1U, true));
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_APPLIED,
        backend_flow_state_apply(&state, 2U, false));
    TEST_ASSERT_FALSE(state.paused);
    TEST_ASSERT_EQUAL_UINT32(2U, state.generation);
}

void test_flow_helpers_fail_closed_for_invalid_arguments(void)
{
    backend_flow_ack_t ack = {0};

    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_INVALID_ARGUMENT,
        backend_flow_state_apply(NULL, 1U, true));
    TEST_ASSERT_FALSE(backend_flow_state_take_ack(NULL, &ack));

    backend_flow_state_t state;
    backend_flow_state_init(&state);
    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_APPLIED,
        backend_flow_state_apply(&state, 1U, true));
    TEST_ASSERT_FALSE(backend_flow_state_take_ack(&state, NULL));
    TEST_ASSERT_TRUE(state.ack_pending);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_flow_queue_contract_is_fixed_and_control_is_reserved);
    BACKEND_RUN_TEST(test_flow_policy_pauses_at_48_and_not_at_47);
    BACKEND_RUN_TEST(test_flow_policy_resumes_at_24_and_not_at_25);
    BACKEND_RUN_TEST(
        test_flow_generation_is_monotonic_and_equal_command_refreshes_ack);
    BACKEND_RUN_TEST(test_larger_flow_generation_applies_and_zero_is_invalid);
    BACKEND_RUN_TEST(test_flow_helpers_fail_closed_for_invalid_arguments);
    return UNITY_END();
}
