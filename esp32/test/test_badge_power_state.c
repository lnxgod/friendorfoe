#include "unity.h"

#include "badge_power_state.h"

void test_badge_power_state_boots_active_without_persisted_quiet(void)
{
    badge_power_state_t state;
    badge_power_state_init(&state);

    TEST_ASSERT_FALSE(state.quiet);
    TEST_ASSERT_EQUAL_UINT32(0U, state.generation);
    TEST_ASSERT_FALSE(badge_power_state_converged(&state));
}

void test_badge_power_state_transition_increments_generation_once(void)
{
    badge_power_state_t state;
    badge_power_state_init(&state);

    TEST_ASSERT_TRUE(badge_power_state_request(&state, true));
    TEST_ASSERT_TRUE(state.quiet);
    TEST_ASSERT_EQUAL_UINT32(1U, state.generation);
    TEST_ASSERT_FALSE(badge_power_state_request(&state, true));
    TEST_ASSERT_EQUAL_UINT32(1U, state.generation);
    TEST_ASSERT_TRUE(badge_power_state_request(&state, false));
    TEST_ASSERT_EQUAL_UINT32(2U, state.generation);
}

void test_badge_power_state_quiet_ack_requires_current_generation_and_halted_radios(void)
{
    badge_power_state_t state;
    badge_power_state_init(&state);
    badge_power_state_note_identity(&state, 0);
    badge_power_state_note_identity(&state, 1);
    badge_power_state_request(&state, true);

    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 0U, false, false, true,
        true, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, true, true,
        false, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, false,
        true, false, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, false, false, true, false, false));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, false, true, 1U, false, false, true,
        true, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, true, false, true,
        true, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        false, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, false, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, false, false, false, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, true, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, false, true, true, false, true));
    TEST_ASSERT_TRUE(badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_converged(&state));
    TEST_ASSERT_TRUE(badge_power_state_note_ack(
        &state, 1, true, true, 1U, false, false, true,
        true, true, false, false, true, false, true));
    TEST_ASSERT_TRUE(badge_power_state_converged(&state));
}

void test_badge_power_state_wake_ack_and_identity_reassertion(void)
{
    badge_power_state_t state;
    badge_power_state_init(&state);
    badge_power_state_note_identity(&state, 0);
    badge_power_state_note_identity(&state, 1);
    badge_power_state_request(&state, true);
    badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, false, false, true, false, true);
    badge_power_state_note_ack(
        &state, 1, true, true, 1U, false, false, true,
        true, true, false, false, true, false, true);

    TEST_ASSERT_TRUE(badge_power_state_request(&state, false));
    TEST_ASSERT_FALSE(badge_power_state_converged(&state));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, true, 2U, false, false, true,
        true, true, false, false, true, false, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, false, 2U, true, false, false,
        true, false, false, true, false, true, true));
    TEST_ASSERT_FALSE(badge_power_state_note_ack(
        &state, 0, true, false, 2U, true, false, false,
        true, false, false, true, true, false, true));
    TEST_ASSERT_TRUE(badge_power_state_note_ack(
        &state, 0, true, false, 2U, true, false, false,
        true, false, false, true, true, true, true));
    TEST_ASSERT_TRUE(badge_power_state_note_ack(
        &state, 1, true, false, 2U, true, true, false,
        false, false, true, true, true, true, true));
    TEST_ASSERT_TRUE(badge_power_state_converged(&state));

    badge_power_state_note_identity(&state, 1);
    TEST_ASSERT_FALSE(badge_power_state_converged(&state));
    TEST_ASSERT_FALSE(state.scanners[1].acked);
}

void test_badge_power_state_disconnected_scanner_is_not_converged(void)
{
    badge_power_state_t state;
    badge_power_state_init(&state);
    badge_power_state_note_identity(&state, 0);
    badge_power_state_note_identity(&state, 1);
    badge_power_state_request(&state, true);
    badge_power_state_note_ack(
        &state, 0, true, true, 1U, false, false, true,
        true, true, false, false, true, false, true);
    badge_power_state_note_ack(
        &state, 1, true, true, 1U, false, false, true,
        true, true, false, false, true, false, true);
    TEST_ASSERT_TRUE(badge_power_state_converged(&state));

    badge_power_state_note_disconnected(&state, 1);
    TEST_ASSERT_FALSE(badge_power_state_converged(&state));
    TEST_ASSERT_FALSE(state.scanners[1].connected);
}
