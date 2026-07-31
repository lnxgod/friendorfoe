#include "badge_usb_recovery_policy.h"

badge_usb_reset_target_t badge_usb_recovery_target(bool flash_confirmed)
{
    return flash_confirmed
        ? BADGE_USB_RESET_ROM
        : BADGE_USB_RESET_APP;
}
