#include "backend_fullsize_rgb_led.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_hardware_profile.h"
#include "backend_rgb_led_pattern.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#if !defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#error "backend_fullsize_led is only valid for the s3_fullsize profile"
#endif

#define BACKEND_RGB_LED_STACK_DEPTH 2048U
#define BACKEND_RGB_LED_TASK_PRIORITY 1U
#define BACKEND_LED_STATE_BITS 3U
#define BACKEND_LED_STATE_MASK \
    (((uint_fast32_t)1U << BACKEND_LED_STATE_BITS) - (uint_fast32_t)1U)
#define BACKEND_LED_REVISION_INCREMENT \
    ((uint_fast32_t)1U << BACKEND_LED_STATE_BITS)

static StaticTask_t s_led_task_buffer;
static StackType_t s_led_task_stack[BACKEND_RGB_LED_STACK_DEPTH];
static atomic_uint_fast32_t s_state_revision = BACKEND_LED_UART_LOST;
static led_strip_handle_t s_strip;

typedef enum {
    BACKEND_RGB_LED_STOPPED = 0,
    BACKEND_RGB_LED_STARTING,
    BACKEND_RGB_LED_STARTED,
    BACKEND_RGB_LED_FAULTED,
} backend_rgb_led_lifecycle_t;

static atomic_int s_lifecycle = BACKEND_RGB_LED_STOPPED;

static bool state_valid(backend_led_state_t state)
{
    size_t step_count = 0U;
    return backend_rgb_led_pattern(state, &step_count) != NULL &&
           step_count > 0U;
}

static backend_led_state_t snapshot_state(uint_fast32_t snapshot)
{
    return (backend_led_state_t)(snapshot & BACKEND_LED_STATE_MASK);
}

static void release_strip(void)
{
    led_strip_handle_t strip = s_strip;
    s_strip = NULL;
    if (strip != NULL) {
        (void)led_strip_clear(strip);
        (void)led_strip_del(strip);
    }
}

static void fault_stop(void)
{
    atomic_store_explicit(
        &s_lifecycle, BACKEND_RGB_LED_FAULTED, memory_order_release);
    release_strip();
    atomic_store_explicit(
        &s_lifecycle, BACKEND_RGB_LED_STOPPED, memory_order_release);
}

static bool drive_led(const backend_rgb_led_step_t *step)
{
    if (step->red == 0U && step->green == 0U && step->blue == 0U) {
        return led_strip_clear(s_strip) == ESP_OK;
    }
    if (led_strip_set_pixel(
            s_strip, 0U, step->red, step->green, step->blue) != ESP_OK) {
        return false;
    }
    return led_strip_refresh(s_strip) == ESP_OK;
}

static void rgb_led_task(void *argument)
{
    (void)argument;
    for (;;) {
        const uint_fast32_t snapshot = atomic_load_explicit(
            &s_state_revision, memory_order_acquire);
        const backend_led_state_t state = snapshot_state(snapshot);
        size_t step_count = 0U;
        const backend_rgb_led_step_t *steps =
            backend_rgb_led_pattern(state, &step_count);
        if (steps == NULL || step_count == 0U) {
            (void)backend_fullsize_rgb_led_set_state(BACKEND_LED_FATAL);
            continue;
        }
        for (size_t index = 0U; index < step_count; ++index) {
            if (!drive_led(&steps[index])) {
                fault_stop();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(steps[index].duration_ms));
            if (snapshot != atomic_load_explicit(
                    &s_state_revision, memory_order_acquire)) {
                break;
            }
        }
    }
}

bool backend_fullsize_rgb_led_init(backend_led_state_t initial_state)
{
    if (!state_valid(initial_state)) {
        return false;
    }
    int expected = BACKEND_RGB_LED_STOPPED;
    if (!atomic_compare_exchange_strong_explicit(
            &s_lifecycle,
            &expected,
            BACKEND_RGB_LED_STARTING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        if (expected != BACKEND_RGB_LED_STARTED) {
            return false;
        }
        return backend_fullsize_rgb_led_set_state(initial_state);
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = FOF_BACKEND_STATUS_LED_GPIO,
        .max_leds = 1U,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10U * 1000U * 1000U,
        .flags.with_dma = false,
    };
    s_strip = NULL;
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) !=
        ESP_OK) {
        atomic_store_explicit(
            &s_lifecycle, BACKEND_RGB_LED_STOPPED, memory_order_release);
        return false;
    }
    if (led_strip_clear(s_strip) != ESP_OK) {
        fault_stop();
        return false;
    }

    atomic_store_explicit(
        &s_state_revision, (uint_fast32_t)initial_state,
        memory_order_release);
    if (xTaskCreateStatic(
            rgb_led_task,
            "be_rgb_led",
            BACKEND_RGB_LED_STACK_DEPTH,
            NULL,
            BACKEND_RGB_LED_TASK_PRIORITY,
            s_led_task_stack,
            &s_led_task_buffer) == NULL) {
        fault_stop();
        return false;
    }
    atomic_store_explicit(
        &s_lifecycle, BACKEND_RGB_LED_STARTED, memory_order_release);
    return true;
}

bool backend_fullsize_rgb_led_set_state(backend_led_state_t state)
{
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) !=
            BACKEND_RGB_LED_STARTED ||
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

backend_led_state_t backend_fullsize_rgb_led_state(void)
{
    return snapshot_state(atomic_load_explicit(
        &s_state_revision, memory_order_acquire));
}
