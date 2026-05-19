#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "badge_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

void badge_theme_runtime_init(void);
const badge_theme_t *badge_theme_runtime_get(void);
uint32_t badge_theme_runtime_hash(void);
bool badge_theme_runtime_set(const badge_theme_t *theme, bool persist);
void badge_theme_runtime_reset(bool persist);
size_t badge_theme_runtime_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* FOF_BADGE_VARIANT */
