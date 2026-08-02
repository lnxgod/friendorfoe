#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_led_pattern.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void assert_pattern(
    backend_led_state_t state,
    const backend_led_step_t *expected,
    size_t expected_count)
{
    size_t actual_count = SIZE_MAX;
    const backend_led_step_t *actual =
        backend_led_pattern(state, &actual_count);

    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_UINT(expected_count, actual_count);
    for (size_t index = 0; index < expected_count; ++index) {
        TEST_ASSERT_EQUAL(expected[index].on, actual[index].on);
        TEST_ASSERT_EQUAL_UINT16(expected[index].duration_ms,
                                 actual[index].duration_ms);
    }
}

void test_led_priority_is_fatal_both_drone_meta_network_healthy(void)
{
    backend_led_inputs_t inputs = {0};
    TEST_ASSERT_EQUAL(BACKEND_LED_HEALTHY, backend_led_select(&inputs));

    inputs.network_degraded = true;
    TEST_ASSERT_EQUAL(BACKEND_LED_NETWORK_DEGRADED,
                      backend_led_select(&inputs));

    inputs.meta_live = true;
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_led_select(&inputs));

    inputs.drone_live = true;
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE_META, backend_led_select(&inputs));

    inputs.meta_live = false;
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE, backend_led_select(&inputs));

    inputs.meta_live = true;
    inputs.fatal = true;
    TEST_ASSERT_EQUAL(BACKEND_LED_FATAL, backend_led_select(&inputs));
}

void test_led_patterns_match_every_exact_timing_step(void)
{
    static const backend_led_step_t healthy[] = {
        {true, 80}, {false, 2920},
    };
    static const backend_led_step_t network_degraded[] = {
        {true, 300}, {false, 300}, {true, 300}, {false, 1800},
    };
    static const backend_led_step_t drone[] = {
        {true, 400}, {false, 120}, {true, 120}, {false, 1360},
    };
    static const backend_led_step_t meta[] = {
        {true, 100}, {false, 100}, {true, 100}, {false, 100},
        {true, 100}, {false, 100}, {true, 100}, {false, 1000},
    };
    static const backend_led_step_t drone_meta[] = {
        {true, 400}, {false, 120}, {true, 120}, {false, 1360},
        {true, 100}, {false, 100}, {true, 100}, {false, 100},
        {true, 100}, {false, 100}, {true, 100}, {false, 1000},
    };
    static const backend_led_step_t fatal[] = {
        {true, 120}, {false, 120}, {true, 120}, {false, 120},
        {true, 120}, {false, 800},
    };
    static const backend_led_step_t uart_lost[] = {
        {true, 1000}, {false, 1000},
    };

    assert_pattern(BACKEND_LED_HEALTHY, healthy,
                   sizeof(healthy) / sizeof(healthy[0]));
    assert_pattern(BACKEND_LED_NETWORK_DEGRADED, network_degraded,
                   sizeof(network_degraded) / sizeof(network_degraded[0]));
    assert_pattern(BACKEND_LED_DRONE, drone,
                   sizeof(drone) / sizeof(drone[0]));
    assert_pattern(BACKEND_LED_META, meta,
                   sizeof(meta) / sizeof(meta[0]));
    assert_pattern(BACKEND_LED_DRONE_META, drone_meta,
                   sizeof(drone_meta) / sizeof(drone_meta[0]));
    assert_pattern(BACKEND_LED_FATAL, fatal,
                   sizeof(fatal) / sizeof(fatal[0]));
    assert_pattern(BACKEND_LED_UART_LOST, uart_lost,
                   sizeof(uart_lost) / sizeof(uart_lost[0]));
}

void test_led_pattern_rejects_invalid_state_and_missing_count(void)
{
    size_t count = SIZE_MAX;
    TEST_ASSERT_NULL(backend_led_pattern((backend_led_state_t)99, &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_NULL(backend_led_pattern(BACKEND_LED_HEALTHY, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_led_priority_is_fatal_both_drone_meta_network_healthy);
    BACKEND_RUN_TEST(test_led_patterns_match_every_exact_timing_step);
    BACKEND_RUN_TEST(
        test_led_pattern_rejects_invalid_state_and_missing_count);
    return UNITY_END();
}
