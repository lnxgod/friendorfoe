#pragma once

#include <stdint.h>

#include "backend_test_clock.h"

static inline int64_t esp_timer_get_time(void)
{
    return backend_test_clock_now_us();
}
