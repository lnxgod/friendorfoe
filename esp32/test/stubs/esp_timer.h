#pragma once

/**
 * Stub esp_timer.h for native unit tests.
 */

#include <stdint.h>

static inline int64_t esp_timer_get_time(void) { return 0; }
