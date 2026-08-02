#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_led_pattern.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "backend_test_main.h"

#ifndef BACKEND_YELLOW_LED_ADAPTER_SOURCE
#error "BACKEND_YELLOW_LED_ADAPTER_SOURCE must name one production adapter"
#endif

/* Compile the exact scanner or uplink adapter into this isolated host suite. */
#include BACKEND_YELLOW_LED_ADAPTER_SOURCE

static gpio_config_t observed_gpio_config;
static unsigned gpio_config_count;
static int observed_levels[8];
static size_t observed_level_count;
static TickType_t observed_delays[8];
static size_t observed_delay_count;
static TaskFunction_t captured_task;
static void *captured_task_argument;
static unsigned task_create_count;
static bool reenter_during_create;
static bool reentrant_init_result;
static int delay_scenario;
static unsigned snapshot_hook_count;
static jmp_buf task_exit;

void backend_yellow_led_test_snapshot_hook(void)
{
    ++snapshot_hook_count;
    if (delay_scenario == 3 && snapshot_hook_count == 1U) {
        TEST_ASSERT_TRUE(backend_yellow_led_set_state(BACKEND_LED_META));
    }
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    ++gpio_config_count;
    observed_gpio_config = *config;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    TEST_ASSERT_EQUAL_INT(GPIO_NUM_21, gpio_num);
    TEST_ASSERT_LESS_THAN(
        sizeof(observed_levels) / sizeof(observed_levels[0]),
        observed_level_count);
    observed_levels[observed_level_count++] = (int)level;
    return ESP_OK;
}

TaskHandle_t xTaskCreateStatic(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    StackType_t *stack_buffer,
    StaticTask_t *task_buffer)
{
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_GREATER_THAN_UINT32(0, stack_depth);
    TEST_ASSERT_GREATER_THAN_UINT32(0, priority);
    TEST_ASSERT_NOT_NULL(stack_buffer);
    TEST_ASSERT_NOT_NULL(task_buffer);
    ++task_create_count;
    captured_task = task;
    captured_task_argument = argument;
    if (reenter_during_create) {
        reentrant_init_result = backend_yellow_led_init(BACKEND_LED_META);
    }
    return task_buffer;
}

void vTaskDelay(TickType_t ticks)
{
    TEST_ASSERT_LESS_THAN(
        sizeof(observed_delays) / sizeof(observed_delays[0]),
        observed_delay_count);
    observed_delays[observed_delay_count++] = ticks;
    if (observed_delay_count == 1U) {
        if (delay_scenario == 1) {
            TEST_ASSERT_TRUE(
                backend_yellow_led_set_state(BACKEND_LED_META));
        } else if (delay_scenario == 2) {
            TEST_ASSERT_TRUE(
                backend_yellow_led_set_state(BACKEND_LED_META));
            TEST_ASSERT_TRUE(
                backend_yellow_led_set_state(BACKEND_LED_DRONE));
        }
        return;
    }
    longjmp(task_exit, 1);
}

void setUp(void)
{
    atomic_store_explicit(
        &s_state_revision, BACKEND_LED_UART_LOST, memory_order_relaxed);
    atomic_store_explicit(
        &s_lifecycle, BACKEND_LED_STOPPED, memory_order_relaxed);
    memset(&observed_gpio_config, 0, sizeof(observed_gpio_config));
    gpio_config_count = 0U;
    memset(observed_levels, 0, sizeof(observed_levels));
    observed_level_count = 0U;
    memset(observed_delays, 0, sizeof(observed_delays));
    observed_delay_count = 0U;
    captured_task = NULL;
    captured_task_argument = NULL;
    task_create_count = 0U;
    reenter_during_create = false;
    reentrant_init_result = true;
    delay_scenario = 0;
    snapshot_hook_count = 0U;
}

void tearDown(void)
{
}

static void reset_pattern_observations(void)
{
    observed_level_count = 0U;
    observed_delay_count = 0U;
    memset(observed_levels, 0, sizeof(observed_levels));
    memset(observed_delays, 0, sizeof(observed_delays));
}

void test_yellow_led_adapter_is_active_low_static_and_restarts_on_change(void)
{
    TEST_ASSERT_TRUE(backend_yellow_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, gpio_config_count);
    TEST_ASSERT_EQUAL_HEX64(
        UINT64_C(1) << GPIO_NUM_21, observed_gpio_config.pin_bit_mask);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, observed_gpio_config.mode);
    TEST_ASSERT_EQUAL_UINT(1, observed_level_count);
    TEST_ASSERT_EQUAL_INT(1, observed_levels[0]);
    TEST_ASSERT_EQUAL_UINT(1, task_create_count);
    TEST_ASSERT_NOT_NULL(captured_task);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE, backend_yellow_led_state());

    reset_pattern_observations();
    delay_scenario = 1;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }
    TEST_ASSERT_EQUAL_UINT(2, observed_delay_count);
    TEST_ASSERT_EQUAL_UINT32(400, observed_delays[0]);
    TEST_ASSERT_EQUAL_UINT32(100, observed_delays[1]);
    TEST_ASSERT_EQUAL_INT(0, observed_levels[0]);
    TEST_ASSERT_EQUAL_INT(0, observed_levels[1]);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_yellow_led_state());
}

void test_yellow_led_adapter_detects_rapid_a_b_a_transition(void)
{
    TEST_ASSERT_TRUE(backend_yellow_led_init(BACKEND_LED_DRONE));
    reset_pattern_observations();
    delay_scenario = 2;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }
    TEST_ASSERT_EQUAL_UINT(2, observed_delay_count);
    TEST_ASSERT_EQUAL_UINT32(400, observed_delays[0]);
    TEST_ASSERT_EQUAL_UINT32(400, observed_delays[1]);
    TEST_ASSERT_EQUAL_INT(0, observed_levels[0]);
    TEST_ASSERT_EQUAL_INT(0, observed_levels[1]);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE, backend_yellow_led_state());
}

void test_yellow_led_adapter_takes_a_coherent_revision_state_snapshot(void)
{
    TEST_ASSERT_TRUE(backend_yellow_led_init(BACKEND_LED_DRONE));
    reset_pattern_observations();
    delay_scenario = 3;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }
    TEST_ASSERT_EQUAL_UINT(2, observed_delay_count);
    TEST_ASSERT_EQUAL_UINT32(100, observed_delays[0]);
    TEST_ASSERT_EQUAL_UINT32(100, observed_delays[1]);
    TEST_ASSERT_EQUAL_INT(0, observed_levels[0]);
    TEST_ASSERT_EQUAL_INT(1, observed_levels[1]);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_yellow_led_state());
}

void test_yellow_led_adapter_prevents_double_static_task_creation(void)
{
    reenter_during_create = true;
    TEST_ASSERT_TRUE(backend_yellow_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_FALSE(reentrant_init_result);
    TEST_ASSERT_EQUAL_UINT(1, task_create_count);

    TEST_ASSERT_TRUE(backend_yellow_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, task_create_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_yellow_led_state());
    TEST_ASSERT_FALSE(
        backend_yellow_led_set_state((backend_led_state_t)99));
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_yellow_led_state());
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_yellow_led_adapter_is_active_low_static_and_restarts_on_change);
    BACKEND_RUN_TEST(
        test_yellow_led_adapter_detects_rapid_a_b_a_transition);
    BACKEND_RUN_TEST(
        test_yellow_led_adapter_takes_a_coherent_revision_state_snapshot);
    BACKEND_RUN_TEST(
        test_yellow_led_adapter_prevents_double_static_task_creation);
    return UNITY_END();
}
