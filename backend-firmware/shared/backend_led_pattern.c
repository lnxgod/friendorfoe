#include "backend_led_pattern.h"

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

static const backend_led_step_t HEALTHY[] = {
    {true, 80}, {false, 2920},
};

static const backend_led_step_t NETWORK_DEGRADED[] = {
    {true, 300}, {false, 300}, {true, 300}, {false, 1800},
};

static const backend_led_step_t DRONE[] = {
    {true, 400}, {false, 120}, {true, 120}, {false, 1360},
};

static const backend_led_step_t META[] = {
    {true, 100}, {false, 100}, {true, 100}, {false, 100},
    {true, 100}, {false, 100}, {true, 100}, {false, 1000},
};

static const backend_led_step_t DRONE_META[] = {
    {true, 400}, {false, 120}, {true, 120}, {false, 1360},
    {true, 100}, {false, 100}, {true, 100}, {false, 100},
    {true, 100}, {false, 100}, {true, 100}, {false, 1000},
};

static const backend_led_step_t FATAL[] = {
    {true, 120}, {false, 120}, {true, 120}, {false, 120},
    {true, 120}, {false, 800},
};

static const backend_led_step_t UART_LOST[] = {
    {true, 1000}, {false, 1000},
};

backend_led_state_t backend_led_select(const backend_led_inputs_t *inputs)
{
    if (inputs == NULL || inputs->fatal) {
        return BACKEND_LED_FATAL;
    }
    if (inputs->drone_live && inputs->meta_live) {
        return BACKEND_LED_DRONE_META;
    }
    if (inputs->drone_live) {
        return BACKEND_LED_DRONE;
    }
    if (inputs->meta_live) {
        return BACKEND_LED_META;
    }
    if (inputs->network_degraded) {
        return BACKEND_LED_NETWORK_DEGRADED;
    }
    return BACKEND_LED_HEALTHY;
}

const backend_led_step_t *backend_led_pattern(
    backend_led_state_t state, size_t *step_count)
{
    if (step_count == NULL) {
        return NULL;
    }
    switch (state) {
    case BACKEND_LED_HEALTHY:
        *step_count = ARRAY_COUNT(HEALTHY);
        return HEALTHY;
    case BACKEND_LED_NETWORK_DEGRADED:
        *step_count = ARRAY_COUNT(NETWORK_DEGRADED);
        return NETWORK_DEGRADED;
    case BACKEND_LED_DRONE:
        *step_count = ARRAY_COUNT(DRONE);
        return DRONE;
    case BACKEND_LED_META:
        *step_count = ARRAY_COUNT(META);
        return META;
    case BACKEND_LED_DRONE_META:
        *step_count = ARRAY_COUNT(DRONE_META);
        return DRONE_META;
    case BACKEND_LED_FATAL:
        *step_count = ARRAY_COUNT(FATAL);
        return FATAL;
    case BACKEND_LED_UART_LOST:
        *step_count = ARRAY_COUNT(UART_LOST);
        return UART_LOST;
    default:
        *step_count = 0;
        return NULL;
    }
}
