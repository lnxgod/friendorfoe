#include "unity.h"

#include "badge_usb_recovery_policy.h"

void test_badge_usb_recovery_selects_rom_after_explicit_ok_confirmation(void)
{
    TEST_ASSERT_EQUAL(BADGE_USB_RESET_ROM,
                      badge_usb_recovery_target(true));
}

void test_badge_usb_recovery_uses_app_reset_without_ok_confirmation(void)
{
    TEST_ASSERT_EQUAL(BADGE_USB_RESET_APP,
                      badge_usb_recovery_target(false));
}
