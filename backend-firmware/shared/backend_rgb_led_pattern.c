#include "backend_rgb_led_pattern.h"

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

#define STEP(red_value, green_value, blue_value, milliseconds) \
    {red_value, green_value, blue_value, milliseconds}

static const backend_rgb_led_step_t HEALTHY[] = {
    STEP(0, 32, 0, 80), STEP(0, 0, 0, 2920),
};

static const backend_rgb_led_step_t NETWORK_DEGRADED[] = {
    STEP(32, 12, 0, 300), STEP(0, 0, 0, 300),
    STEP(32, 12, 0, 300), STEP(0, 0, 0, 1800),
};

static const backend_rgb_led_step_t DRONE[] = {
    STEP(24, 0, 32, 400), STEP(0, 0, 0, 120),
    STEP(32, 8, 0, 120), STEP(0, 0, 0, 1360),
};

static const backend_rgb_led_step_t META[] = {
    STEP(32, 0, 0, 100), STEP(0, 0, 0, 100),
    STEP(0, 0, 32, 100), STEP(0, 0, 0, 100),
    STEP(32, 0, 0, 100), STEP(0, 0, 0, 100),
    STEP(0, 0, 32, 100), STEP(0, 0, 0, 1000),
};

static const backend_rgb_led_step_t DRONE_META[] = {
    STEP(24, 0, 32, 400), STEP(0, 0, 0, 120),
    STEP(32, 8, 0, 120), STEP(0, 0, 0, 1360),
    STEP(32, 0, 0, 100), STEP(0, 0, 0, 100),
    STEP(0, 0, 32, 100), STEP(0, 0, 0, 100),
    STEP(32, 0, 0, 100), STEP(0, 0, 0, 100),
    STEP(0, 0, 32, 100), STEP(0, 0, 0, 1000),
};

static const backend_rgb_led_step_t FATAL[] = {
    STEP(32, 0, 0, 120), STEP(0, 0, 0, 120),
    STEP(32, 0, 0, 120), STEP(0, 0, 0, 120),
    STEP(32, 0, 0, 120), STEP(0, 0, 0, 800),
};

static const backend_rgb_led_step_t UART_LOST[] = {
    STEP(32, 24, 0, 1000), STEP(0, 0, 0, 1000),
};

const backend_rgb_led_step_t *backend_rgb_led_pattern(
    backend_led_state_t state, size_t *count)
{
    if (count == NULL) {
        return NULL;
    }
    switch (state) {
    case BACKEND_LED_HEALTHY:
        *count = ARRAY_COUNT(HEALTHY);
        return HEALTHY;
    case BACKEND_LED_NETWORK_DEGRADED:
        *count = ARRAY_COUNT(NETWORK_DEGRADED);
        return NETWORK_DEGRADED;
    case BACKEND_LED_DRONE:
        *count = ARRAY_COUNT(DRONE);
        return DRONE;
    case BACKEND_LED_META:
        *count = ARRAY_COUNT(META);
        return META;
    case BACKEND_LED_DRONE_META:
        *count = ARRAY_COUNT(DRONE_META);
        return DRONE_META;
    case BACKEND_LED_FATAL:
        *count = ARRAY_COUNT(FATAL);
        return FATAL;
    case BACKEND_LED_UART_LOST:
        *count = ARRAY_COUNT(UART_LOST);
        return UART_LOST;
    default:
        *count = 0U;
        return NULL;
    }
}
