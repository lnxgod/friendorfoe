#include "backend_test_clock.h"

static int64_t s_now_us;

int64_t backend_test_clock_now_us(void)
{
    return s_now_us;
}

void backend_test_clock_set_us(int64_t now_us)
{
    s_now_us = now_us;
}
