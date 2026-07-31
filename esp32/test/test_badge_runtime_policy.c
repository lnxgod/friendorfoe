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

void test_badge_expected_reboot_generation_increments_and_never_uses_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(
        1U, badge_runtime_expected_reboot_next_generation(0U));
    TEST_ASSERT_EQUAL_UINT32(
        2U, badge_runtime_expected_reboot_next_generation(1U));
    TEST_ASSERT_EQUAL_UINT32(
        1U, badge_runtime_expected_reboot_next_generation(UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC + 1U,
        badge_runtime_expected_reboot_next_generation(
            BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC - 1U));
    TEST_ASSERT_EQUAL_UINT32(
        BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC + 1U,
        badge_runtime_expected_reboot_next_generation(
            BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC));
}

void test_badge_rollback_generation_is_v078_and_current_abi_compatible(void)
{
    const uint32_t original_v078_expected_magic = 0xF0F0B007U;
    uint32_t generation =
        badge_runtime_expected_reboot_generation_for_target(
            BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK,
            41U);

    /* Commit 755153e authenticates a planned software reset by reading +4. */
    TEST_ASSERT_EQUAL_HEX32(original_v078_expected_magic, generation);

    badge_runtime_expected_reboot_decision_t current =
        badge_runtime_expected_reboot_decide(
            true, generation, original_v078_expected_magic);
    TEST_ASSERT_TRUE(current.expected_software_reset);
    TEST_ASSERT_FALSE(current.legacy_v078);
    TEST_ASSERT_EQUAL_HEX32(
        original_v078_expected_magic, current.consumed_generation);
    TEST_ASSERT_TRUE(current.clear_generation);

    TEST_ASSERT_EQUAL_UINT32(
        42U,
        badge_runtime_expected_reboot_generation_for_target(
            BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT, 41U));
    TEST_ASSERT_EQUAL_UINT32(
        0U,
        badge_runtime_expected_reboot_generation_for_target(
            (badge_runtime_expected_reboot_target_t)99, 41U));
}

void test_badge_expected_reboot_lease_rejects_duplicate_without_losing_owner(void)
{
    badge_runtime_expected_reboot_arm_state_t state;
    badge_runtime_expected_reboot_lease_t first = {0};
    badge_runtime_expected_reboot_lease_t duplicate = {
        .owner_id = 0xAAAAAAAAU,
        .generation = 0xBBBBBBBBU,
    };
    badge_runtime_expected_reboot_arm_state_init(&state);

    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_reserve(
        &state, 7U, &first));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_is_preparing(
        &state, &first));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_reserve(
        &state, 8U, &duplicate));
    TEST_ASSERT_EQUAL_UINT32(0U, duplicate.owner_id);
    TEST_ASSERT_EQUAL_UINT32(0U, duplicate.generation);
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_is_preparing(
        &state, &first));
}

void test_badge_expected_reboot_lease_publish_and_release_require_owner(void)
{
    badge_runtime_expected_reboot_arm_state_t state;
    badge_runtime_expected_reboot_lease_t owner = {0};
    badge_runtime_expected_reboot_lease_t wrong = {
        .owner_id = 77U,
        .generation = 11U,
    };
    badge_runtime_expected_reboot_arm_state_init(&state);

    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_reserve(
        &state, 11U, &owner));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_publish(
        &state, &wrong));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_is_owned(
        &state, &owner));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_publish(
        &state, &owner));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_is_owned(
        &state, &owner));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_release(
        &state, &wrong));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_is_owned(
        &state, &owner));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_release(
        &state, &owner));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_is_owned(
        &state, &owner));
}

void test_badge_expected_reboot_failed_prepare_returns_idle_and_stale_is_rejected(void)
{
    badge_runtime_expected_reboot_arm_state_t state;
    badge_runtime_expected_reboot_lease_t failed = {0};
    badge_runtime_expected_reboot_lease_t next = {0};
    badge_runtime_expected_reboot_arm_state_init(&state);

    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_reserve(
        &state, 21U, &failed));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_cancel(
        &state, &failed));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_is_preparing(
        &state, &failed));

    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_reserve(
        &state, 22U, &next));
    TEST_ASSERT_NOT_EQUAL(failed.owner_id, next.owner_id);
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_cancel(
        &state, &failed));
    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_is_preparing(
        &state, &next));
}

void test_badge_expected_reboot_owner_id_exhaustion_fails_closed(void)
{
    badge_runtime_expected_reboot_arm_state_t state;
    badge_runtime_expected_reboot_lease_t owner = {0};
    badge_runtime_expected_reboot_arm_state_init(&state);
    state.last_owner_id = UINT32_MAX;

    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_reserve(
        &state, 31U, &owner));
    TEST_ASSERT_EQUAL_UINT32(0U, owner.owner_id);
    TEST_ASSERT_EQUAL_UINT32(0U, owner.generation);
}

void test_badge_expected_reboot_cold_bss_is_idle_and_fabricated_owner_is_rejected(void)
{
    badge_runtime_expected_reboot_arm_state_t cold_bss = {0};
    badge_runtime_expected_reboot_lease_t fabricated = {
        .owner_id = 1U,
        .generation = BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC,
    };
    badge_runtime_expected_reboot_lease_t owner = {0};

    TEST_ASSERT_EQUAL(
        BADGE_RUNTIME_EXPECTED_REBOOT_ARM_IDLE, cold_bss.phase);
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_is_preparing(
        &cold_bss, &fabricated));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_is_owned(
        &cold_bss, &fabricated));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_cancel(
        &cold_bss, &fabricated));
    TEST_ASSERT_FALSE(badge_runtime_expected_reboot_arm_release(
        &cold_bss, &fabricated));

    TEST_ASSERT_TRUE(badge_runtime_expected_reboot_arm_reserve(
        &cold_bss, 1U, &owner));
    TEST_ASSERT_EQUAL_UINT32(1U, owner.owner_id);
    TEST_ASSERT_EQUAL_UINT32(1U, owner.generation);
}

void test_badge_expected_reboot_token_accepts_only_exact_current_layout(void)
{
    const uint32_t magic = BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC;
    badge_runtime_expected_reboot_decision_t current =
        badge_runtime_expected_reboot_decide(
            true, 17U, magic);
    TEST_ASSERT_TRUE(current.expected_software_reset);
    TEST_ASSERT_FALSE(current.legacy_v078);
    TEST_ASSERT_EQUAL_UINT32(17U, current.consumed_generation);
    TEST_ASSERT_FALSE(current.clear_generation);

    badge_runtime_expected_reboot_decision_t missing_generation =
        badge_runtime_expected_reboot_decide(
            true, 0U, magic);
    TEST_ASSERT_FALSE(missing_generation.expected_software_reset);
    TEST_ASSERT_TRUE(missing_generation.clear_generation);

    badge_runtime_expected_reboot_decision_t wrong_magic =
        badge_runtime_expected_reboot_decide(
            true, 17U, magic ^ 1U);
    TEST_ASSERT_FALSE(wrong_magic.expected_software_reset);
    TEST_ASSERT_TRUE(wrong_magic.clear_generation);
}

void test_badge_expected_reboot_token_migrates_original_v078_once(void)
{
    const uint32_t magic = BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC;
    badge_runtime_expected_reboot_decision_t legacy =
        badge_runtime_expected_reboot_decide(
            true, magic, 0U);
    TEST_ASSERT_TRUE(legacy.expected_software_reset);
    TEST_ASSERT_TRUE(legacy.legacy_v078);
    TEST_ASSERT_EQUAL_UINT32(1U, legacy.consumed_generation);
    TEST_ASSERT_TRUE(legacy.clear_generation);

    badge_runtime_expected_reboot_decision_t initialized_layout =
        badge_runtime_expected_reboot_decide(
            true, magic, 0U);
    TEST_ASSERT_TRUE(initialized_layout.expected_software_reset);
    TEST_ASSERT_TRUE(initialized_layout.legacy_v078);

    badge_runtime_expected_reboot_decision_t non_software =
        badge_runtime_expected_reboot_decide(
            false, magic, 0U);
    TEST_ASSERT_FALSE(non_software.expected_software_reset);

    badge_runtime_expected_reboot_decision_t ambiguous =
        badge_runtime_expected_reboot_decide(
            true, magic, magic);
    TEST_ASSERT_TRUE(ambiguous.expected_software_reset);
    TEST_ASSERT_FALSE(ambiguous.legacy_v078);
    TEST_ASSERT_EQUAL_UINT32(magic, ambiguous.consumed_generation);
    TEST_ASSERT_TRUE(ambiguous.clear_generation);
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

void test_badge_normal_stability_is_host_independent_and_requires_both_uart_workers(void)
{
    TEST_ASSERT_TRUE(badge_runtime_normal_stability_satisfied(
        false, true, true, true, 64000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_normal_stability_satisfied(
        false, true, false, true, 64000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_normal_stability_satisfied(
        false, true, true, false, 64000, 90, 60));
}

void test_badge_normal_stability_rejects_safe_early_low_heap_or_dark_display(void)
{
    TEST_ASSERT_FALSE(badge_runtime_normal_stability_satisfied(
        true, true, true, true, 64000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_normal_stability_satisfied(
        false, true, true, true, 64000, 30, 60));
    TEST_ASSERT_FALSE(badge_runtime_normal_stability_satisfied(
        false, true, true, true, 8000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_normal_stability_satisfied(
        false, false, true, true, 64000, 90, 60));
}

void test_badge_uart_heartbeat_freshness_rejects_missing_future_and_stale_values(void)
{
    TEST_ASSERT_FALSE(badge_runtime_uart_heartbeat_fresh(0, 1000, 100));
    TEST_ASSERT_FALSE(badge_runtime_uart_heartbeat_fresh(1001, 1000, 100));
    TEST_ASSERT_TRUE(badge_runtime_uart_heartbeat_fresh(901, 1000, 100));
    TEST_ASSERT_FALSE(badge_runtime_uart_heartbeat_fresh(900, 1000, 100));
}

void test_badge_usb_recovery_token_is_consumed_only_by_expected_software_reset(void)
{
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RECOVERY_TOKEN_CONSUME_SAFE_USB,
                      badge_runtime_recovery_token_decide(
                          true, BADGE_RUNTIME_RESET_EXPECTED_SW));
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RECOVERY_TOKEN_CLEAR,
                      badge_runtime_recovery_token_decide(
                          false, BADGE_RUNTIME_RESET_EXPECTED_SW));
}

void test_badge_usb_recovery_token_is_cleared_for_mismatched_reset_causes(void)
{
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RECOVERY_TOKEN_CLEAR,
                      badge_runtime_recovery_token_decide(
                          true, BADGE_RUNTIME_RESET_CLEAN));
    TEST_ASSERT_EQUAL(BADGE_RUNTIME_RECOVERY_TOKEN_CLEAR,
                      badge_runtime_recovery_token_decide(
                          true, BADGE_RUNTIME_RESET_CRASH));
}

void test_badge_rollback_health_rejects_usb_task_heartbeat_without_response(void)
{
    TEST_ASSERT_FALSE(badge_runtime_rollback_health_satisfied(
        false, true, false, true, 64000, 90, 60));
    TEST_ASSERT_FALSE(badge_runtime_rollback_health_satisfied(
        true, true, false, false, 64000, 90, 60));
}

void test_badge_safe_usb_rollback_health_accepts_display_and_completed_response(void)
{
    TEST_ASSERT_TRUE(badge_runtime_rollback_health_satisfied(
        true, true, true, false, 64000, 90, 60));
}

void test_badge_normal_rollback_health_requires_uart_worker_heartbeat_only(void)
{
    TEST_ASSERT_FALSE(badge_runtime_rollback_health_satisfied(
        false, true, true, false, 64000, 90, 60));
    TEST_ASSERT_TRUE(badge_runtime_rollback_health_satisfied(
        false, true, true, true, 64000, 90, 60));
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
