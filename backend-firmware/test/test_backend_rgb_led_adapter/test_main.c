#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_fullsize_rgb_led.h"
#include "backend_rgb_led_pattern.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "../support/backend_test_main.h"

/* Compile the exact Fullsize component into this isolated host suite. */
#include "../../fullsize-components/backend_fullsize_led/backend_fullsize_rgb_led.c"

static led_strip_config_t observed_strip_config;
static led_strip_rmt_config_t observed_rmt_config;
static unsigned strip_create_count;
static unsigned strip_set_count;
static unsigned strip_refresh_count;
static unsigned strip_clear_count;
static unsigned strip_delete_count;
static bool strip_create_fails;
static unsigned strip_set_fail_on_call;
static unsigned strip_refresh_fail_on_call;
static unsigned strip_clear_fail_on_call;
static unsigned strip_delete_fail_on_call;
static uint32_t observed_red[4];
static uint32_t observed_green[4];
static uint32_t observed_blue[4];
static TaskFunction_t captured_task;
static void *captured_task_argument;
static unsigned task_create_count;
static bool task_create_fails;
static int delay_scenario;
static unsigned delay_count;
static unsigned fault_task_park_count;
static jmp_buf task_exit;
static struct led_strip_t {
    unsigned opaque;
} observed_strip;

esp_err_t led_strip_new_rmt_device(
    const led_strip_config_t *strip_config,
    const led_strip_rmt_config_t *rmt_config,
    led_strip_handle_t *ret_strip)
{
    ++strip_create_count;
    observed_strip_config = *strip_config;
    observed_rmt_config = *rmt_config;
    if (strip_create_fails) {
        return -1;
    }
    *ret_strip = &observed_strip;
    return ESP_OK;
}

esp_err_t led_strip_set_pixel(
    led_strip_handle_t strip,
    uint32_t index,
    uint32_t red,
    uint32_t green,
    uint32_t blue)
{
    TEST_ASSERT_EQUAL_PTR(&observed_strip, strip);
    TEST_ASSERT_EQUAL_UINT32(0, index);
    TEST_ASSERT_LESS_THAN_UINT(
        sizeof(observed_red) / sizeof(observed_red[0]), strip_set_count);
    observed_red[strip_set_count] = red;
    observed_green[strip_set_count] = green;
    observed_blue[strip_set_count++] = blue;
    if (strip_set_count == strip_set_fail_on_call) {
        return -1;
    }
    return ESP_OK;
}

esp_err_t led_strip_refresh(led_strip_handle_t strip)
{
    TEST_ASSERT_EQUAL_PTR(&observed_strip, strip);
    ++strip_refresh_count;
    if (strip_refresh_count == strip_refresh_fail_on_call) {
        return -1;
    }
    return ESP_OK;
}

esp_err_t led_strip_clear(led_strip_handle_t strip)
{
    TEST_ASSERT_EQUAL_PTR(&observed_strip, strip);
    ++strip_clear_count;
    if (strip_clear_count == strip_clear_fail_on_call) {
        return -1;
    }
    return ESP_OK;
}

esp_err_t led_strip_del(led_strip_handle_t strip)
{
    TEST_ASSERT_EQUAL_PTR(&observed_strip, strip);
    ++strip_delete_count;
    if (strip_delete_count == strip_delete_fail_on_call) {
        return -1;
    }
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
    if (task_create_fails) {
        return NULL;
    }
    return task_buffer;
}

void vTaskDelay(TickType_t ticks)
{
    ++delay_count;
    if (delay_scenario == 1 && ticks == 400U) {
        TEST_ASSERT_TRUE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));
        return;
    }
    if (delay_scenario == 0 && delay_count == 1U) {
        return;
    } else {
        longjmp(task_exit, 1);
    }
}

void vTaskSuspend(TaskHandle_t task)
{
    TEST_ASSERT_NULL(task);
    ++fault_task_park_count;
    longjmp(task_exit, 1);
}

void setUp(void)
{
    atomic_store_explicit(
        &s_state_revision, BACKEND_LED_UART_LOST, memory_order_relaxed);
    atomic_store_explicit(
        &s_lifecycle, BACKEND_RGB_LED_STOPPED, memory_order_relaxed);
    s_strip = NULL;
    memset(&observed_strip_config, 0, sizeof(observed_strip_config));
    memset(&observed_rmt_config, 0, sizeof(observed_rmt_config));
    strip_create_count = 0U;
    strip_set_count = 0U;
    strip_refresh_count = 0U;
    strip_clear_count = 0U;
    strip_delete_count = 0U;
    strip_create_fails = false;
    strip_set_fail_on_call = 0U;
    strip_refresh_fail_on_call = 0U;
    strip_clear_fail_on_call = 0U;
    strip_delete_fail_on_call = 0U;
    memset(observed_red, 0, sizeof(observed_red));
    memset(observed_green, 0, sizeof(observed_green));
    memset(observed_blue, 0, sizeof(observed_blue));
    captured_task = NULL;
    captured_task_argument = NULL;
    task_create_count = 0U;
    task_create_fails = false;
    delay_scenario = 0;
    delay_count = 0U;
    fault_task_park_count = 0U;
}

void tearDown(void)
{
}

void test_fullsize_rgb_led_uses_one_grb_ws2812_on_gpio48(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
    TEST_ASSERT_EQUAL_INT(48, observed_strip_config.strip_gpio_num);
    TEST_ASSERT_EQUAL_UINT32(1, observed_strip_config.max_leds);
    TEST_ASSERT_EQUAL_UINT32(
        LED_STRIP_COLOR_COMPONENT_FMT_GRB.format_id,
        observed_strip_config.color_component_format.format_id);
    TEST_ASSERT_EQUAL_UINT(1, strip_clear_count);
    TEST_ASSERT_EQUAL_UINT(1, task_create_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE, backend_fullsize_rgb_led_state());
}

void test_fullsize_rgb_led_sets_color_then_refreshes_and_clears_off_steps(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }
    TEST_ASSERT_EQUAL_UINT(1, strip_set_count);
    TEST_ASSERT_EQUAL_UINT32(24, observed_red[0]);
    TEST_ASSERT_EQUAL_UINT32(0, observed_green[0]);
    TEST_ASSERT_EQUAL_UINT32(32, observed_blue[0]);
    TEST_ASSERT_EQUAL_UINT(1, strip_refresh_count);
    TEST_ASSERT_EQUAL_UINT(2, strip_clear_count);
}

void test_fullsize_rgb_led_propagates_strip_initialization_failure(void)
{
    strip_create_fails = true;
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(0, task_create_count);
}

void test_fullsize_rgb_led_releases_after_initial_clear_failure_before_retry(void)
{
    strip_clear_fail_on_call = 1U;
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(2, strip_clear_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NULL(s_strip);
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));

    strip_clear_fail_on_call = 0U;
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(2, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(1, task_create_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_fullsize_rgb_led_state());
}

void test_fullsize_rgb_led_releases_after_task_creation_failure_before_retry(void)
{
    task_create_fails = true;
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(2, strip_clear_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NULL(s_strip);
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));

    task_create_fails = false;
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(2, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(2, task_create_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_fullsize_rgb_led_state());
}

void test_fullsize_rgb_led_locks_out_after_delete_failure_during_initial_cleanup(void)
{
    strip_clear_fail_on_call = 1U;
    strip_delete_fail_on_call = 1U;
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NOT_NULL(s_strip);
    TEST_ASSERT_NOT_EQUAL(
        BACKEND_RGB_LED_STOPPED,
        atomic_load_explicit(&s_lifecycle, memory_order_relaxed));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
}

void test_fullsize_rgb_led_locks_out_after_delete_failure_during_task_cleanup(void)
{
    task_create_fails = true;
    strip_delete_fail_on_call = 1U;
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NOT_NULL(s_strip);
    TEST_ASSERT_NOT_EQUAL(
        BACKEND_RGB_LED_STOPPED,
        atomic_load_explicit(&s_lifecycle, memory_order_relaxed));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
}

void test_fullsize_rgb_led_faults_without_refresh_after_set_pixel_failure(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    strip_set_fail_on_call = 1U;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }

    TEST_ASSERT_EQUAL_UINT(1, strip_set_count);
    TEST_ASSERT_EQUAL_UINT(0, strip_refresh_count);
    TEST_ASSERT_EQUAL_UINT(2, strip_clear_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NULL(s_strip);
    TEST_ASSERT_EQUAL_UINT(1, fault_task_park_count);
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
}

void test_fullsize_rgb_led_faults_after_refresh_failure(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    strip_refresh_fail_on_call = 1U;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }

    TEST_ASSERT_EQUAL_UINT(1, strip_set_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_refresh_count);
    TEST_ASSERT_EQUAL_UINT(2, strip_clear_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NULL(s_strip);
    TEST_ASSERT_EQUAL_UINT(1, fault_task_park_count);
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
}

void test_fullsize_rgb_led_faults_after_off_step_clear_failure(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    strip_clear_fail_on_call = 2U;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }

    TEST_ASSERT_EQUAL_UINT(1, strip_set_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_refresh_count);
    TEST_ASSERT_EQUAL_UINT(3, strip_clear_count);
    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NULL(s_strip);
    TEST_ASSERT_EQUAL_UINT(1, fault_task_park_count);
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
}

void test_fullsize_rgb_led_parks_and_locks_out_after_runtime_delete_failure(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    strip_set_fail_on_call = 1U;
    strip_delete_fail_on_call = 1U;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }

    TEST_ASSERT_EQUAL_UINT(1, strip_delete_count);
    TEST_ASSERT_NOT_NULL(s_strip);
    TEST_ASSERT_EQUAL_UINT(1, fault_task_park_count);
    TEST_ASSERT_NOT_EQUAL(
        BACKEND_RGB_LED_STOPPED,
        atomic_load_explicit(&s_lifecycle, memory_order_relaxed));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_set_state(BACKEND_LED_META));
    TEST_ASSERT_FALSE(backend_fullsize_rgb_led_init(BACKEND_LED_META));
    TEST_ASSERT_EQUAL_UINT(1, strip_create_count);
}

void test_fullsize_rgb_led_interrupts_a_step_after_an_atomic_state_revision(void)
{
    TEST_ASSERT_TRUE(backend_fullsize_rgb_led_init(BACKEND_LED_DRONE));
    delay_scenario = 1;
    if (setjmp(task_exit) == 0) {
        captured_task(captured_task_argument);
    }
    TEST_ASSERT_EQUAL_UINT(2, strip_set_count);
    TEST_ASSERT_EQUAL_UINT32(24, observed_red[0]);
    TEST_ASSERT_EQUAL_UINT32(32, observed_red[1]);
    TEST_ASSERT_EQUAL_UINT32(0, observed_green[1]);
    TEST_ASSERT_EQUAL_UINT32(0, observed_blue[1]);
    TEST_ASSERT_EQUAL_UINT(2, strip_refresh_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, backend_fullsize_rgb_led_state());
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_fullsize_rgb_led_uses_one_grb_ws2812_on_gpio48);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_sets_color_then_refreshes_and_clears_off_steps);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_propagates_strip_initialization_failure);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_releases_after_initial_clear_failure_before_retry);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_releases_after_task_creation_failure_before_retry);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_locks_out_after_delete_failure_during_initial_cleanup);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_locks_out_after_delete_failure_during_task_cleanup);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_faults_without_refresh_after_set_pixel_failure);
    BACKEND_RUN_TEST(test_fullsize_rgb_led_faults_after_refresh_failure);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_faults_after_off_step_clear_failure);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_parks_and_locks_out_after_runtime_delete_failure);
    BACKEND_RUN_TEST(
        test_fullsize_rgb_led_interrupts_a_step_after_an_atomic_state_revision);
    return UNITY_END();
}
