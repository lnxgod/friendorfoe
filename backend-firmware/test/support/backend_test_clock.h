#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t backend_test_clock_now_us(void);
void backend_test_clock_set_us(int64_t now_us);

#ifdef __cplusplus
}
#endif
