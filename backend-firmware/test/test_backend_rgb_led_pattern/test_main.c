#include <stddef.h>
#include <stdint.h>

#include <unity.h>

#include "backend_rgb_led_pattern.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void assert_pattern(
    backend_led_state_t state,
    const backend_rgb_led_step_t *expected,
    size_t expected_count)
{
    size_t actual_count = SIZE_MAX;
    const backend_rgb_led_step_t *actual =
        backend_rgb_led_pattern(state, &actual_count);

    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_UINT(expected_count, actual_count);
    for (size_t index = 0U; index < expected_count; ++index) {
        TEST_ASSERT_EQUAL_UINT8(expected[index].red, actual[index].red);
        TEST_ASSERT_EQUAL_UINT8(expected[index].green, actual[index].green);
        TEST_ASSERT_EQUAL_UINT8(expected[index].blue, actual[index].blue);
        TEST_ASSERT_EQUAL_UINT16(
            expected[index].duration_ms, actual[index].duration_ms);
    }
}

void test_rgb_led_patterns_render_every_threat_with_exact_colors_and_timing(void)
{
    static const backend_rgb_led_step_t healthy[] = {
        {0, 32, 0, 80}, {0, 0, 0, 2920},
    };
    static const backend_rgb_led_step_t degraded[] = {
        {32, 12, 0, 300}, {0, 0, 0, 300},
        {32, 12, 0, 300}, {0, 0, 0, 1800},
    };
    static const backend_rgb_led_step_t drone[] = {
        {24, 0, 32, 400}, {0, 0, 0, 120},
        {32, 8, 0, 120}, {0, 0, 0, 1360},
    };
    static const backend_rgb_led_step_t meta[] = {
        {32, 0, 0, 100}, {0, 0, 0, 100},
        {0, 0, 32, 100}, {0, 0, 0, 100},
        {32, 0, 0, 100}, {0, 0, 0, 100},
        {0, 0, 32, 100}, {0, 0, 0, 1000},
    };
    static const backend_rgb_led_step_t drone_meta[] = {
        {24, 0, 32, 400}, {0, 0, 0, 120},
        {32, 8, 0, 120}, {0, 0, 0, 1360},
        {32, 0, 0, 100}, {0, 0, 0, 100},
        {0, 0, 32, 100}, {0, 0, 0, 100},
        {32, 0, 0, 100}, {0, 0, 0, 100},
        {0, 0, 32, 100}, {0, 0, 0, 1000},
    };
    static const backend_rgb_led_step_t uart_lost[] = {
        {32, 24, 0, 1000}, {0, 0, 0, 1000},
    };
    static const backend_rgb_led_step_t fatal[] = {
        {32, 0, 0, 120}, {0, 0, 0, 120},
        {32, 0, 0, 120}, {0, 0, 0, 120},
        {32, 0, 0, 120}, {0, 0, 0, 800},
    };

    assert_pattern(BACKEND_LED_HEALTHY, healthy,
                   sizeof(healthy) / sizeof(healthy[0]));
    assert_pattern(BACKEND_LED_NETWORK_DEGRADED, degraded,
                   sizeof(degraded) / sizeof(degraded[0]));
    assert_pattern(BACKEND_LED_DRONE, drone,
                   sizeof(drone) / sizeof(drone[0]));
    assert_pattern(BACKEND_LED_META, meta, sizeof(meta) / sizeof(meta[0]));
    assert_pattern(BACKEND_LED_DRONE_META, drone_meta,
                   sizeof(drone_meta) / sizeof(drone_meta[0]));
    assert_pattern(BACKEND_LED_UART_LOST, uart_lost,
                   sizeof(uart_lost) / sizeof(uart_lost[0]));
    assert_pattern(BACKEND_LED_FATAL, fatal,
                   sizeof(fatal) / sizeof(fatal[0]));
}

void test_rgb_led_pattern_rejects_invalid_state_and_missing_count(void)
{
    size_t count = SIZE_MAX;
    TEST_ASSERT_NULL(
        backend_rgb_led_pattern((backend_led_state_t)99, &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_NULL(backend_rgb_led_pattern(BACKEND_LED_HEALTHY, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_rgb_led_patterns_render_every_threat_with_exact_colors_and_timing);
    BACKEND_RUN_TEST(test_rgb_led_pattern_rejects_invalid_state_and_missing_count);
    return UNITY_END();
}
