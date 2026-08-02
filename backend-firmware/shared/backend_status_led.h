#pragma once

#include <stdbool.h>

#include "backend_led_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

bool backend_status_led_init(backend_led_state_t initial);
bool backend_status_led_set_state(backend_led_state_t state);
backend_led_state_t backend_status_led_state(void);

#ifdef __cplusplus
}
#endif
