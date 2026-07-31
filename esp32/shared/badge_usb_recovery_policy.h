#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_USB_RESET_APP = 0,
    BADGE_USB_RESET_ROM,
} badge_usb_reset_target_t;

badge_usb_reset_target_t badge_usb_recovery_target(bool flash_confirmed);

#ifdef __cplusplus
}
#endif
