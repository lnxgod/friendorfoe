#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_MODE_BACKEND = 0,
    BADGE_MODE_LOCAL_AP,
    BADGE_MODE_USB_ONLY,
} badge_mode_t;

badge_mode_t badge_mode_get(void);
bool badge_mode_set(badge_mode_t mode);
bool badge_mode_parse(const char *value, badge_mode_t *out);
const char *badge_mode_to_string(badge_mode_t mode);
const char *badge_mode_display_name(badge_mode_t mode);
bool badge_mode_backend_enabled(badge_mode_t mode);
bool badge_mode_ap_enabled(badge_mode_t mode);

#ifdef __cplusplus
}
#endif
