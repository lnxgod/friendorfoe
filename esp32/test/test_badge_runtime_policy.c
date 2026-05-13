#include "badge_runtime_policy.h"
#include "unity.h"

void test_badge_default_network_is_usb_only(void)
{
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_NETWORK_OFF,
                      badge_runtime_default_network_mode(true));
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_NETWORK_BACKEND,
                      badge_runtime_default_network_mode(false));
}

void test_badge_network_ttl_defaults_and_off(void)
{
    TEST_ASSERT_EQUAL_INT(0, badge_runtime_network_ttl_s(
        BADGE_RUNTIME_NETWORK_OFF, 600));
    TEST_ASSERT_EQUAL_INT(600, badge_runtime_network_ttl_s(
        BADGE_RUNTIME_NETWORK_LOCAL_AP, 0));
    TEST_ASSERT_EQUAL_INT(900, badge_runtime_network_ttl_s(
        BADGE_RUNTIME_NETWORK_BACKEND, 0));
    TEST_ASSERT_EQUAL_INT(42, badge_runtime_network_ttl_s(
        BADGE_RUNTIME_NETWORK_BACKEND, 42));
    TEST_ASSERT_EQUAL_INT(0, badge_runtime_network_ttl_s(
        BADGE_RUNTIME_NETWORK_BACKEND, -1));
}

void test_badge_network_sessions_are_explicitly_allowed(void)
{
    TEST_ASSERT_TRUE(badge_runtime_badge_allows_network_mode(
        BADGE_RUNTIME_NETWORK_OFF));
    TEST_ASSERT_TRUE(badge_runtime_badge_allows_network_mode(
        BADGE_RUNTIME_NETWORK_LOCAL_AP));
    TEST_ASSERT_TRUE(badge_runtime_badge_allows_network_mode(
        BADGE_RUNTIME_NETWORK_BACKEND));
}

void test_badge_post_ota_network_hold_defaults_and_off(void)
{
    TEST_ASSERT_EQUAL_INT(0, badge_runtime_post_ota_hold_ttl_s(
        BADGE_RUNTIME_NETWORK_OFF, 300));
    TEST_ASSERT_EQUAL_INT(300, badge_runtime_post_ota_hold_ttl_s(
        BADGE_RUNTIME_NETWORK_LOCAL_AP, 0));
    TEST_ASSERT_EQUAL_INT(300, badge_runtime_post_ota_hold_ttl_s(
        BADGE_RUNTIME_NETWORK_BACKEND, 0));
    TEST_ASSERT_EQUAL_INT(45, badge_runtime_post_ota_hold_ttl_s(
        BADGE_RUNTIME_NETWORK_BACKEND, 45));
}

void test_badge_pending_verify_crash_forces_rollback(void)
{
    badge_runtime_boot_decision_t d = badge_runtime_boot_decide(
        BADGE_RUNTIME_RESET_CRASH,
        true,
        0,
        3
    );
    TEST_ASSERT_TRUE(d.force_ota_rollback);
    TEST_ASSERT_FALSE(d.enter_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(1, d.new_crash_count);
}

void test_badge_expected_software_reset_does_not_increment_crash_count(void)
{
    badge_runtime_boot_decision_t d = badge_runtime_boot_decide(
        BADGE_RUNTIME_RESET_EXPECTED_SW,
        true,
        2,
        3
    );
    TEST_ASSERT_FALSE(d.force_ota_rollback);
    TEST_ASSERT_FALSE(d.enter_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(2, d.new_crash_count);
}

void test_badge_unplanned_software_reset_counts_as_crash(void)
{
    badge_runtime_boot_decision_t d = badge_runtime_boot_decide(
        BADGE_RUNTIME_RESET_CRASH,
        false,
        2,
        3
    );
    TEST_ASSERT_FALSE(d.force_ota_rollback);
    TEST_ASSERT_TRUE(d.enter_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(3, d.new_crash_count);
}

void test_badge_validated_crash_loop_enters_safe_mode(void)
{
    badge_runtime_boot_decision_t d = badge_runtime_boot_decide(
        BADGE_RUNTIME_RESET_CRASH,
        false,
        2,
        3
    );
    TEST_ASSERT_FALSE(d.force_ota_rollback);
    TEST_ASSERT_TRUE(d.enter_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(3, d.new_crash_count);
}

void test_badge_health_gate_requires_display_usb_scanner_and_heap(void)
{
    TEST_ASSERT_FALSE(badge_runtime_can_mark_valid(
        false, true, true, true, 64000, 30, 60));
    TEST_ASSERT_FALSE(badge_runtime_can_mark_valid(
        false, true, true, false, 64000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_can_mark_valid(
        false, true, true, true, 8000, 90, 60));
    TEST_ASSERT_TRUE(badge_runtime_can_mark_valid(
        false, true, true, true, 64000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_can_mark_valid(
        true, true, true, true, 64000, 90, 60));
}

void test_badge_usb_recovery_waits_through_boot_grace(void)
{
    TEST_ASSERT_FALSE(badge_runtime_usb_recovery_due(
        false, false, -1, 119, 90, 120));
}

void test_badge_usb_recovery_ignores_safe_mode(void)
{
    TEST_ASSERT_FALSE(badge_runtime_usb_recovery_due(
        true, false, -1, 300, 90, 120));
}

void test_badge_usb_recovery_triggers_when_control_never_starts(void)
{
    TEST_ASSERT_TRUE(badge_runtime_usb_recovery_due(
        false, false, -1, 120, 90, 120));
}

void test_badge_usb_recovery_triggers_when_control_stale(void)
{
    TEST_ASSERT_TRUE(badge_runtime_usb_recovery_due(
        false, true, 90, 240, 90, 120));
}

void test_badge_usb_recovery_stays_clear_when_control_fresh(void)
{
    TEST_ASSERT_FALSE(badge_runtime_usb_recovery_due(
        false, true, 12, 240, 90, 120));
}
