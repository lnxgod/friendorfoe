#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_LED_HEALTHY = 0,
    BACKEND_LED_NETWORK_DEGRADED,
    BACKEND_LED_DRONE,
    BACKEND_LED_META,
    BACKEND_LED_DRONE_META,
    BACKEND_LED_FATAL,
    BACKEND_LED_UART_LOST,
} backend_led_state_t;

typedef struct {
    bool fatal;
    bool network_degraded;
    bool drone_live;
    bool meta_live;
} backend_led_inputs_t;

typedef struct {
    bool on;
    uint16_t duration_ms;
} backend_led_step_t;

backend_led_state_t backend_led_select(const backend_led_inputs_t *inputs);

const backend_led_step_t *backend_led_pattern(
    backend_led_state_t state, size_t *step_count);

#ifdef __cplusplus
}
#endif
