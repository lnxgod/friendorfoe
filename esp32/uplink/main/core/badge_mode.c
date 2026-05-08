#include "badge_mode.h"

#include "nvs_config.h"

#include <string.h>

#define BADGE_MODE_NVS_KEY "badge_mode"

bool badge_mode_parse(const char *value, badge_mode_t *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "backend") == 0) {
        *out = BADGE_MODE_BACKEND;
        return true;
    }
    if (strcmp(value, "local_ap") == 0 || strcmp(value, "ap") == 0) {
        *out = BADGE_MODE_LOCAL_AP;
        return true;
    }
    if (strcmp(value, "usb_only") == 0 || strcmp(value, "usb") == 0) {
        *out = BADGE_MODE_USB_ONLY;
        return true;
    }
    return false;
}

const char *badge_mode_to_string(badge_mode_t mode)
{
    switch (mode) {
        case BADGE_MODE_BACKEND:  return "backend";
        case BADGE_MODE_USB_ONLY: return "usb_only";
        case BADGE_MODE_LOCAL_AP:
        default:                  return "local_ap";
    }
}

const char *badge_mode_display_name(badge_mode_t mode)
{
    switch (mode) {
        case BADGE_MODE_BACKEND:  return "Backend";
        case BADGE_MODE_USB_ONLY: return "USB Only";
        case BADGE_MODE_LOCAL_AP:
        default:                  return "Local AP";
    }
}

badge_mode_t badge_mode_get(void)
{
#ifdef FOF_BADGE_VARIANT
    badge_mode_t mode = BADGE_MODE_USB_ONLY;
#else
    badge_mode_t mode = BADGE_MODE_BACKEND;
#endif

    char value[16] = {0};
    if (nvs_config_get_string(BADGE_MODE_NVS_KEY, value, sizeof(value))) {
        (void)badge_mode_parse(value, &mode);
    }
    return mode;
}

bool badge_mode_set(badge_mode_t mode)
{
    return nvs_config_set_string(BADGE_MODE_NVS_KEY,
                                 badge_mode_to_string(mode));
}

bool badge_mode_backend_enabled(badge_mode_t mode)
{
    return mode == BADGE_MODE_BACKEND;
}

bool badge_mode_ap_enabled(badge_mode_t mode)
{
    return mode == BADGE_MODE_LOCAL_AP || mode == BADGE_MODE_BACKEND;
}
