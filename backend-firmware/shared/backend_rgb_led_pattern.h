#pragma once

#include <stddef.h>
#include <stdint.h>

#include "backend_led_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint16_t duration_ms;
} backend_rgb_led_step_t;

const backend_rgb_led_step_t *backend_rgb_led_pattern(
    backend_led_state_t state, size_t *count);

#ifdef __cplusplus
}
#endif
