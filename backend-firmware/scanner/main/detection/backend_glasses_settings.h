#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool backend_glasses_settings_is_enabled(void);
void backend_glasses_settings_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
