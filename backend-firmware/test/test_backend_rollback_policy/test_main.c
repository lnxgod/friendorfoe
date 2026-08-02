#include <unity.h>

#include "backend_recovery_policy.h"
#include "scanner_rollback.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static scanner_rollback_readiness_t ready_input(uint32_t boot_id)
{
    scanner_rollback_readiness_t readiness = {
        .boot_id = boot_id,
        .command_ingress_boot_id = boot_id,
        .role_boot_id = boot_id,
        .uptime_ms = SCANNER_ROLLBACK_MIN_UPTIME_MS,
        .expected_profile = BACKEND_SCAN_PROFILE_BLE_PRIMARY,
        .reported_profile = BACKEND_SCAN_PROFILE_BLE_PRIMARY,
        .watchdog_ready_mask = BACKEND_WORKER_UART_RX_CONTROL |
            BACKEND_WORKER_BLE_RADIO | BACKEND_WORKER_OTA,
        .command_ingress_healthy = true,
        .current_boot_role_acked = true,
        .required_radio_healthy = true,
    };
    return readiness;
}

void test_watchdog_readiness_tracks_the_assigned_radio_profile(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = ready_input(77U);
    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));

    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness.watchdog_ready_mask = BACKEND_WORKER_UART_RX_CONTROL |
        BACKEND_WORKER_WIFI_RADIO | BACKEND_WORKER_OTA;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));

    readiness.expected_profile = BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
    readiness.reported_profile = BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness.watchdog_ready_mask = BACKEND_WORKER_UART_RX_CONTROL |
        BACKEND_WORKER_BLE_RADIO | BACKEND_WORKER_OTA;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));

    readiness.expected_profile = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    readiness.reported_profile = BACKEND_SCAN_PROFILE_HYBRID_FAILOVER;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness.watchdog_ready_mask |= BACKEND_WORKER_WIFI_RADIO;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
}

void test_sixty_seconds_alone_cannot_clear_pending_verify(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = {
        .boot_id = 77U,
        .uptime_ms = SCANNER_ROLLBACK_MIN_UPTIME_MS,
    };

    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));
    TEST_ASSERT_EQUAL(
        SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    TEST_ASSERT_FALSE(policy.readiness_latched);
}

void test_every_current_boot_health_gate_is_required(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = ready_input(77U);
    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));

    TEST_ASSERT_EQUAL(
        SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    TEST_ASSERT_TRUE(policy.readiness_latched);

    readiness.command_ingress_healthy = false;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    TEST_ASSERT_FALSE(policy.readiness_latched);
    readiness = ready_input(77U);
    readiness.command_ingress_boot_id = 76U;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.current_boot_role_acked = false;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.role_boot_id = 76U;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.reported_profile = BACKEND_SCAN_PROFILE_WIFI_PRIMARY;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.required_radio_healthy = false;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.watchdog_ready_mask &= ~BACKEND_WORKER_OTA;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.uptime_ms = SCANNER_ROLLBACK_MIN_UPTIME_MS - 1;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
}

void test_crash_on_pending_verify_forces_rollback_before_readiness(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = ready_input(77U);
    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));
    TEST_ASSERT_EQUAL(
        SCANNER_ROLLBACK_FORCE_ROLLBACK,
        scanner_rollback_policy_evaluate(&policy, &readiness, true));
    TEST_ASSERT_FALSE(policy.readiness_latched);

    TEST_ASSERT_TRUE(scanner_rollback_policy_reset_boot(&policy, 78U, false));
    readiness = ready_input(78U);
    TEST_ASSERT_EQUAL(
        SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, true));
}

void test_boot_id_reset_discards_stale_role_ingress_and_watchdog_readiness(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = ready_input(77U);
    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));

    TEST_ASSERT_TRUE(scanner_rollback_policy_reset_boot(&policy, 78U, true));
    TEST_ASSERT_FALSE(policy.readiness_latched);
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));

    readiness.boot_id = 78U;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(78U);
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
}

void test_mark_valid_commit_requires_current_readiness_and_is_one_shot(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = ready_input(77U);
    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));
    TEST_ASSERT_FALSE(scanner_rollback_policy_mark_valid_committed(&policy));
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_MARK_VALID,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    TEST_ASSERT_TRUE(scanner_rollback_policy_mark_valid_committed(&policy));
    TEST_ASSERT_FALSE(policy.pending_verify);
    TEST_ASSERT_FALSE(policy.readiness_latched);
    TEST_ASSERT_FALSE(scanner_rollback_policy_mark_valid_committed(&policy));
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
}

void test_invalid_arguments_profiles_and_time_fail_closed(void)
{
    scanner_rollback_policy_t policy;
    scanner_rollback_readiness_t readiness = ready_input(77U);
    TEST_ASSERT_FALSE(scanner_rollback_policy_init(NULL, 77U, true));
    TEST_ASSERT_FALSE(scanner_rollback_policy_init(&policy, 0U, true));
    TEST_ASSERT_TRUE(scanner_rollback_policy_init(&policy, 77U, true));
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(NULL, &readiness, false));
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, NULL, false));
    readiness.uptime_ms = -1;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.expected_profile = BACKEND_SCAN_PROFILE_QUIESCENT;
    readiness.reported_profile = BACKEND_SCAN_PROFILE_QUIESCENT;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
    readiness = ready_input(77U);
    readiness.reported_profile = (backend_scan_profile_t)99;
    TEST_ASSERT_EQUAL(SCANNER_ROLLBACK_WAIT,
        scanner_rollback_policy_evaluate(&policy, &readiness, false));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_watchdog_readiness_tracks_the_assigned_radio_profile);
    BACKEND_RUN_TEST(
        test_sixty_seconds_alone_cannot_clear_pending_verify);
    BACKEND_RUN_TEST(test_every_current_boot_health_gate_is_required);
    BACKEND_RUN_TEST(
        test_crash_on_pending_verify_forces_rollback_before_readiness);
    BACKEND_RUN_TEST(
        test_boot_id_reset_discards_stale_role_ingress_and_watchdog_readiness);
    BACKEND_RUN_TEST(
        test_mark_valid_commit_requires_current_readiness_and_is_one_shot);
    BACKEND_RUN_TEST(
        test_invalid_arguments_profiles_and_time_fail_closed);
    return UNITY_END();
}
