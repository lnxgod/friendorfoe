#include "backend_yellow_led.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BACKEND_YELLOW_LED_STACK_DEPTH 2048U
#define BACKEND_YELLOW_LED_TASK_PRIORITY 1U
#define BACKEND_LED_STATE_BITS 3U
#define BACKEND_LED_STATE_MASK \
    (((uint_fast32_t)1U << BACKEND_LED_STATE_BITS) - (uint_fast32_t)1U)
#define BACKEND_LED_REVISION_INCREMENT \
    ((uint_fast32_t)1U << BACKEND_LED_STATE_BITS)

static StaticTask_t s_led_task_buffer;
static StackType_t s_led_task_stack[BACKEND_YELLOW_LED_STACK_DEPTH];
static atomic_uint_fast32_t s_state_revision = BACKEND_LED_UART_LOST;

typedef enum {
    BACKEND_LED_STOPPED = 0,
    BACKEND_LED_STARTING,
    BACKEND_LED_STARTED,
} backend_led_lifecycle_t;

static atomic_int s_lifecycle = BACKEND_LED_STOPPED;

#ifdef UNIT_TESTING
void backend_yellow_led_test_snapshot_hook(void);
#endif

static bool state_valid(backend_led_state_t state)
{
    size_t step_count = 0;
    return backend_led_pattern(state, &step_count) != NULL &&
           step_count > 0U;
}

static void drive_led(bool on)
{
    gpio_set_level(GPIO_NUM_21, on ? 0 : 1);
}

static backend_led_state_t snapshot_state(uint_fast32_t snapshot)
{
    return (backend_led_state_t)(snapshot & BACKEND_LED_STATE_MASK);
}

static void yellow_led_task(void *argument)
{
    (void)argument;
    for (;;) {
#ifdef UNIT_TESTING
        backend_yellow_led_test_snapshot_hook();
#endif
        const uint_fast32_t snapshot = atomic_load_explicit(
            &s_state_revision, memory_order_acquire);
        const backend_led_state_t state = snapshot_state(snapshot);
        size_t step_count = 0;
        const backend_led_step_t *steps =
            backend_led_pattern(state, &step_count);
        if (steps == NULL || step_count == 0U) {
            (void)backend_yellow_led_set_state(BACKEND_LED_FATAL);
            continue;
        }
        for (size_t index = 0; index < step_count; ++index) {
            drive_led(steps[index].on);
            vTaskDelay(pdMS_TO_TICKS(steps[index].duration_ms));
            if (snapshot != atomic_load_explicit(
                    &s_state_revision, memory_order_acquire)) {
                break;
            }
        }
    }
}

bool backend_yellow_led_init(backend_led_state_t initial_state)
{
    if (!state_valid(initial_state)) {
        return false;
    }
    int expected = BACKEND_LED_STOPPED;
    if (!atomic_compare_exchange_strong_explicit(
            &s_lifecycle,
            &expected,
            BACKEND_LED_STARTING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        if (expected != BACKEND_LED_STARTED) {
            return false;
        }
        return backend_yellow_led_set_state(initial_state);
    }

    const gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << GPIO_NUM_21,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        atomic_store_explicit(
            &s_lifecycle, BACKEND_LED_STOPPED, memory_order_release);
        return false;
    }
    drive_led(false);

    atomic_store_explicit(
        &s_state_revision, (uint_fast32_t)initial_state,
        memory_order_release);
    if (xTaskCreateStatic(
            yellow_led_task,
            "be_uplink_led",
            BACKEND_YELLOW_LED_STACK_DEPTH,
            NULL,
            BACKEND_YELLOW_LED_TASK_PRIORITY,
            s_led_task_stack,
            &s_led_task_buffer) == NULL) {
        atomic_store_explicit(
            &s_lifecycle, BACKEND_LED_STOPPED, memory_order_release);
        return false;
    }
    atomic_store_explicit(
        &s_lifecycle, BACKEND_LED_STARTED, memory_order_release);
    return true;
}

bool backend_yellow_led_set_state(backend_led_state_t state)
{
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) !=
            BACKEND_LED_STARTED ||
        !state_valid(state)) {
        return false;
    }
    uint_fast32_t previous = atomic_load_explicit(
        &s_state_revision, memory_order_acquire);
    for (;;) {
        if (snapshot_state(previous) == state) {
            return true;
        }
        const uint_fast32_t desired =
            ((previous + BACKEND_LED_REVISION_INCREMENT) &
             ~BACKEND_LED_STATE_MASK) |
            (uint_fast32_t)state;
        if (atomic_compare_exchange_weak_explicit(
                &s_state_revision,
                &previous,
                desired,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return true;
        }
    }
}

backend_led_state_t backend_yellow_led_state(void)
{
    return snapshot_state(atomic_load_explicit(
        &s_state_revision, memory_order_acquire));
}
