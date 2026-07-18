#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdbool.h>

#include "badge_easter_egg.h"

#ifdef __cplusplus
extern "C" {
#endif

void badge_easter_egg_runtime_init(void);
bool badge_easter_egg_runtime_trigger(badge_easter_egg_source_t source);
bool badge_easter_egg_runtime_dismiss(void);
bool badge_easter_egg_runtime_snapshot(badge_easter_egg_machine_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FOF_BADGE_VARIANT */
