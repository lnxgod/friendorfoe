#include <stdbool.h>
#include <stdint.h>

#include <unity.h>

#include "backend_recovery_policy.h"
#include "backend_scanner_runtime.h"
#include "backend_uart_slot.h"
#include "time_sync_policy.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void complete_recovery_action(
    backend_recovery_policy_t *policy,
    backend_recovery_action_t action,
    int64_t now_ms)
{
    TEST_ASSERT_TRUE(backend_recovery_policy_complete_action(
        policy, action, true, now_ms));
}

void test_missing_status_uses_exact_probe_reinit_and_unavailable_deadlines(void)
{
    backend_recovery_policy_t policy;
    backend_recovery_policy_init(&policy, 1000);

    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 6999, false));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 7000, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 7000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 7999, false));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 8000, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 8000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 9000, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 9000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_REINIT_LOCAL_UART,
        backend_recovery_policy_tick(&policy, 10000, false));
    complete_recovery_action(
        &policy, BACKEND_RECOVERY_REINIT_LOCAL_UART, 10000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 15999, false));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_MARK_UNAVAILABLE,
        backend_recovery_policy_tick(&policy, 16000, false));
    TEST_ASSERT_FALSE(policy.unavailable);
    complete_recovery_action(
        &policy, BACKEND_RECOVERY_MARK_UNAVAILABLE, 16000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 17000, false));
    TEST_ASSERT_TRUE(policy.unavailable);
}

void test_valid_status_resets_local_recovery_and_changed_boot_is_reported(void)
{
    backend_recovery_policy_t policy;
    backend_recovery_policy_init(&policy, 0);

    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_FIRST,
        backend_recovery_policy_note_status(&policy, 77U, 100));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 6100, false));
    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_UNCHANGED,
        backend_recovery_policy_note_status(&policy, 77U, 6200));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 12199, false));
    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_CHANGED,
        backend_recovery_policy_note_status(&policy, 88U, 12200));
    TEST_ASSERT_EQUAL_UINT32(88U, policy.boot_id);
    TEST_ASSERT_FALSE(policy.unavailable);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 18199, false));
}

void test_delayed_polling_never_bursts_health_probes_faster_than_one_second(void)
{
    backend_recovery_policy_t policy;
    backend_recovery_policy_init(&policy, 0);

    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 9000, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 9000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 9999, false));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 10000, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 10000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 10999, false));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 11000, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 11000);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_REINIT_LOCAL_UART,
        backend_recovery_policy_tick(&policy, 11001, false));
    complete_recovery_action(
        &policy, BACKEND_RECOVERY_REINIT_LOCAL_UART, 11001);
}

void test_remote_radio_restart_is_boot_bound_monotonic_and_ota_deferred(void)
{
    backend_recovery_policy_t policy;
    backend_recovery_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_FIRST,
        backend_recovery_policy_note_status(&policy, 77U, 100));

    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_INVALID_BOOT,
        backend_recovery_policy_request_restart(&policy, 76U, 9U));
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_INVALID_GENERATION,
        backend_recovery_policy_request_restart(&policy, 77U, 0U));
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_APPLIED,
        backend_recovery_policy_request_restart(&policy, 77U, 9U));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 200, true));
    TEST_ASSERT_TRUE(policy.remote_restart_pending);
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_REFRESHED,
        backend_recovery_policy_request_restart(&policy, 77U, 9U));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_RESTART_RADIOS,
        backend_recovery_policy_tick(&policy, 201, false));
    TEST_ASSERT_TRUE(policy.remote_restart_pending);
    complete_recovery_action(
        &policy, BACKEND_RECOVERY_SEND_RESTART_RADIOS, 201);
    TEST_ASSERT_FALSE(policy.remote_restart_pending);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 202, false));
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_STALE,
        backend_recovery_policy_request_restart(&policy, 77U, 8U));

    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_CHANGED,
        backend_recovery_policy_note_status(&policy, 88U, 300));
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_APPLIED,
        backend_recovery_policy_request_restart(&policy, 88U, 1U));
}

void test_failed_recovery_delivery_remains_pending_until_success(void)
{
    backend_recovery_policy_t policy;
    backend_recovery_policy_init(&policy, 0);

    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 6000, false));
    TEST_ASSERT_FALSE(backend_recovery_policy_complete_action(
        &policy, BACKEND_RECOVERY_SEND_PROBE, true, 5999));
    TEST_ASSERT_TRUE(backend_recovery_policy_complete_action(
        &policy, BACKEND_RECOVERY_SEND_PROBE, false, 6000));
    TEST_ASSERT_EQUAL_UINT8(0U, policy.probes_sent);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_PROBE,
        backend_recovery_policy_tick(&policy, 6001, false));
    complete_recovery_action(&policy, BACKEND_RECOVERY_SEND_PROBE, 6001);
    TEST_ASSERT_EQUAL_UINT8(1U, policy.probes_sent);

    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_FIRST,
        backend_recovery_policy_note_status(&policy, 77U, 6100));
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_APPLIED,
        backend_recovery_policy_request_restart(&policy, 77U, 9U));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_RESTART_RADIOS,
        backend_recovery_policy_tick(&policy, 6101, false));
    TEST_ASSERT_TRUE(backend_recovery_policy_complete_action(
        &policy, BACKEND_RECOVERY_SEND_RESTART_RADIOS, false, 6101));
    TEST_ASSERT_TRUE(policy.remote_restart_pending);
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_SEND_RESTART_RADIOS,
        backend_recovery_policy_tick(&policy, 6102, false));
    complete_recovery_action(
        &policy, BACKEND_RECOVERY_SEND_RESTART_RADIOS, 6102);
    TEST_ASSERT_EQUAL(
        BACKEND_REMOTE_RECOVERY_REFRESHED,
        backend_recovery_policy_request_restart(&policy, 77U, 9U));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 6103, false));
}

void test_status_and_coordinator_cadence_have_exact_boundaries(void)
{
    backend_status_cadence_t status;
    backend_command_cadence_t command;
    backend_status_cadence_init(&status);
    backend_command_cadence_init(&command);

    TEST_ASSERT_TRUE(backend_status_cadence_due(&status, 100, false));
    TEST_ASSERT_TRUE(backend_status_cadence_due(&status, 101, false));
    TEST_ASSERT_TRUE(backend_status_cadence_mark_sent(&status, 100));
    TEST_ASSERT_FALSE(backend_status_cadence_due(&status, 2099, false));
    TEST_ASSERT_TRUE(backend_status_cadence_due(&status, 2100, false));
    TEST_ASSERT_TRUE(backend_status_cadence_mark_sent(&status, 2100));
    TEST_ASSERT_TRUE(backend_status_cadence_due(&status, 2101, true));
    TEST_ASSERT_TRUE(backend_status_cadence_due(&status, 2102, false));
    TEST_ASSERT_TRUE(backend_status_cadence_mark_sent(&status, 2102));
    TEST_ASSERT_FALSE(backend_status_cadence_due(&status, 4101, false));
    TEST_ASSERT_TRUE(backend_status_cadence_due(&status, 4102, false));
    TEST_ASSERT_TRUE(backend_status_cadence_mark_sent(&status, 4102));

    TEST_ASSERT_TRUE(backend_command_cadence_due(&command, 500, false));
    TEST_ASSERT_TRUE(backend_command_cadence_due(&command, 501, false));
    TEST_ASSERT_TRUE(backend_command_cadence_mark_sent(&command, 500));
    TEST_ASSERT_FALSE(backend_command_cadence_due(&command, 10499, false));
    TEST_ASSERT_TRUE(backend_command_cadence_due(&command, 10500, false));
    TEST_ASSERT_TRUE(backend_command_cadence_mark_sent(&command, 10500));
    TEST_ASSERT_TRUE(backend_command_cadence_due(&command, 10501, true));
    TEST_ASSERT_TRUE(backend_command_cadence_due(&command, 10502, false));
    TEST_ASSERT_TRUE(backend_command_cadence_mark_sent(&command, 10502));
    TEST_ASSERT_FALSE(backend_command_cadence_due(&command, 20501, false));
    TEST_ASSERT_TRUE(backend_command_cadence_due(&command, 20502, false));
    TEST_ASSERT_TRUE(backend_command_cadence_mark_sent(&command, 20502));
}

void test_watchdog_readiness_requires_each_worker_own_iteration(void)
{
    uint32_t completed = 0U;
    const uint32_t required = BACKEND_WATCHDOG_UPLINK_REQUIRED_MASK;

    TEST_ASSERT_EQUAL_INT64(30000, BACKEND_WORKER_WATCHDOG_BUDGET_MS);
    TEST_ASSERT_FALSE(backend_watchdog_ready(required, completed));
    TEST_ASSERT_TRUE(backend_watchdog_mark_iteration(
        &completed, BACKEND_WORKER_UART_RX_CONTROL));
    TEST_ASSERT_TRUE(backend_watchdog_mark_iteration(
        &completed, BACKEND_WORKER_COORDINATOR));
    TEST_ASSERT_TRUE(backend_watchdog_mark_iteration(
        &completed, BACKEND_WORKER_UPLOADER));
    TEST_ASSERT_TRUE(backend_watchdog_mark_iteration(
        &completed, BACKEND_WORKER_COMMAND_CLIENT));
    TEST_ASSERT_FALSE(backend_watchdog_ready(required, completed));
    TEST_ASSERT_TRUE(backend_watchdog_mark_iteration(
        &completed, BACKEND_WORKER_OTA));
    TEST_ASSERT_TRUE(backend_watchdog_ready(required, completed));
    TEST_ASSERT_FALSE(backend_watchdog_mark_iteration(&completed, 0U));
    TEST_ASSERT_FALSE(backend_watchdog_mark_iteration(&completed, 3U));
    TEST_ASSERT_FALSE(backend_watchdog_mark_iteration(NULL, BACKEND_WORKER_OTA));
}

void test_time_validity_uses_the_vendored_epoch_policy(void)
{
    TEST_ASSERT_FALSE(backend_coordinator_epoch_valid(INT64_C(1699999999999)));
    TEST_ASSERT_FALSE(backend_coordinator_epoch_valid(INT64_C(1700000000000)));
    TEST_ASSERT_TRUE(backend_coordinator_epoch_valid(INT64_C(1700000000001)));
    TEST_ASSERT_EQUAL(
        fof_time_epoch_is_valid(INT64_C(1700000000001)),
        backend_coordinator_epoch_valid(INT64_C(1700000000001)));
}

void test_uart_slot_records_match_selected_profile_and_each_has_bounded_framer(void)
{
    backend_uart_slot_config_t slot0 = {0};
    backend_uart_slot_config_t slot1 = {0};
    backend_uart_slots_t slots;

    TEST_ASSERT_TRUE(backend_uart_slot_config(0U, &slot0));
    TEST_ASSERT_TRUE(backend_uart_slot_config(1U, &slot1));
    TEST_ASSERT_FALSE(backend_uart_slot_config(2U, &slot0));
    TEST_ASSERT_FALSE(backend_uart_slot_config(0U, NULL));
    TEST_ASSERT_EQUAL_INT(1, slot0.uart);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_EQUAL_INT(2, slot0.rx_gpio);
    TEST_ASSERT_EQUAL_INT(1, slot0.tx_gpio);
    TEST_ASSERT_EQUAL_INT(2, slot1.uart);
    TEST_ASSERT_EQUAL_INT(4, slot1.rx_gpio);
    TEST_ASSERT_EQUAL_INT(3, slot1.tx_gpio);
#else
    TEST_ASSERT_EQUAL_INT(18, slot0.rx_gpio);
    TEST_ASSERT_EQUAL_INT(17, slot0.tx_gpio);
    TEST_ASSERT_EQUAL_INT(2, slot1.uart);
    TEST_ASSERT_EQUAL_INT(16, slot1.rx_gpio);
    TEST_ASSERT_EQUAL_INT(15, slot1.tx_gpio);
#endif
    TEST_ASSERT_EQUAL_INT(921600, slot0.baud);
    TEST_ASSERT_EQUAL_INT(8, slot0.data_bits);
    TEST_ASSERT_EQUAL_INT(1, slot0.stop_bits);
    TEST_ASSERT_EQUAL_INT(0, slot0.parity);
    TEST_ASSERT_FALSE(slot0.flow_control);

    TEST_ASSERT_TRUE(backend_uart_slots_init(&slots));
    TEST_ASSERT_EQUAL_UINT32(0U, slots.slot[0].framer.byte_len);
    TEST_ASSERT_EQUAL_UINT32(0U, slots.slot[1].framer.byte_len);
    TEST_ASSERT_EQUAL_PTR(slots.slot[0].storage, slots.slot[0].framer.buffer);
    TEST_ASSERT_EQUAL_PTR(slots.slot[1].storage, slots.slot[1].framer.buffer);

    static const uint8_t partial[] = {'{', '"', 't'};
    size_t consumed = 0U;
    TEST_ASSERT_EQUAL(
        SCANNER_UART_LINE_EVENT_NONE,
        backend_uart_slot_consume(
            &slots, 0U, partial, sizeof(partial), &consumed).kind);
    TEST_ASSERT_EQUAL_UINT(sizeof(partial), consumed);
    TEST_ASSERT_TRUE(scanner_uart_line_framer_has_partial(
        &slots.slot[0].framer));
    TEST_ASSERT_TRUE(backend_uart_slot_driver_reinit(&slots, 0U));
    TEST_ASSERT_FALSE(scanner_uart_line_framer_has_partial(
        &slots.slot[0].framer));
    TEST_ASSERT_TRUE(backend_uart_slot_driver_reinit(&slots, 0U));
    TEST_ASSERT_FALSE(backend_uart_slot_driver_reinit(NULL, 0U));
    TEST_ASSERT_FALSE(backend_uart_slot_driver_reinit(&slots, 2U));
}

void test_scanner_runtime_boots_quiescent_and_preserves_control_during_quiet_modes(void)
{
    backend_scanner_runtime_t runtime;

    TEST_ASSERT_TRUE(backend_scanner_runtime_init(&runtime, 77U));
    TEST_ASSERT_EQUAL(
        BACKEND_SCAN_PROFILE_QUIESCENT,
        backend_scanner_runtime_profile(&runtime));
    TEST_ASSERT_EQUAL_INT(1, BACKEND_SCANNER_UART_PORT);
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    TEST_ASSERT_EQUAL_INT(1, BACKEND_SCANNER_UART_TX_GPIO);
    TEST_ASSERT_EQUAL_INT(2, BACKEND_SCANNER_UART_RX_GPIO);
#else
    TEST_ASSERT_EQUAL_INT(17, BACKEND_SCANNER_UART_TX_GPIO);
    TEST_ASSERT_EQUAL_INT(18, BACKEND_SCANNER_UART_RX_GPIO);
#endif
    TEST_ASSERT_EQUAL_INT(921600, BACKEND_SCANNER_UART_BAUD);
    TEST_ASSERT_TRUE(backend_scanner_runtime_status_due(&runtime, 0));
    TEST_ASSERT_TRUE(backend_scanner_runtime_status_due(&runtime, 1));
    TEST_ASSERT_TRUE(backend_scanner_runtime_status_sent(&runtime, 1));
    TEST_ASSERT_FALSE(backend_scanner_runtime_status_due(&runtime, 2));
    TEST_ASSERT_FALSE(backend_scanner_runtime_status_sent(&runtime, 2));

    TEST_ASSERT_EQUAL(
        BACKEND_ROLE_APPLIED,
        backend_scanner_runtime_apply_role(
            &runtime, 77U, 4U, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_TRUE(backend_scanner_runtime_status_due(&runtime, 1));
    TEST_ASSERT_TRUE(backend_scanner_runtime_enqueue_detection(&runtime));

    TEST_ASSERT_EQUAL(
        BACKEND_FLOW_APPLIED,
        backend_scanner_runtime_apply_flow(&runtime, 6U, true));
    TEST_ASSERT_FALSE(backend_scanner_runtime_enqueue_detection(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_enqueue_control(&runtime));

    backend_scanner_runtime_set_ota_active(&runtime, true);
    TEST_ASSERT_FALSE(backend_scanner_runtime_enqueue_detection(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_enqueue_control(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_enqueue_control(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_enqueue_control(&runtime));
    TEST_ASSERT_FALSE(backend_scanner_runtime_enqueue_control(&runtime));
}

void test_scanner_runtime_time_is_monotonic_and_restart_mask_tracks_role(void)
{
    backend_scanner_runtime_t runtime;
    backend_scanner_time_ack_t ack = {0};
    backend_scanner_runtime_init(&runtime, 77U);

    TEST_ASSERT_EQUAL(
        BACKEND_SCANNER_TIME_INVALID,
        backend_scanner_runtime_apply_time(
            &runtime, 1U, true, INT64_C(1700000000000), "sntp"));
    TEST_ASSERT_FALSE(backend_scanner_runtime_take_time_ack(&runtime, &ack));
    TEST_ASSERT_EQUAL(
        BACKEND_SCANNER_TIME_APPLIED,
        backend_scanner_runtime_apply_time(
            &runtime, 1U, true, INT64_C(1700000000001), "sntp"));
    TEST_ASSERT_TRUE(backend_scanner_runtime_take_time_ack(&runtime, &ack));
    TEST_ASSERT_EQUAL_UINT32(1U, ack.generation);
    TEST_ASSERT_TRUE(ack.valid);
    TEST_ASSERT_EQUAL_INT64(INT64_C(1700000000001), ack.epoch_ms);
    TEST_ASSERT_EQUAL_STRING("sntp", ack.source);
    TEST_ASSERT_EQUAL(
        BACKEND_SCANNER_TIME_REFRESHED,
        backend_scanner_runtime_apply_time(
            &runtime, 1U, true, INT64_C(1700000000001), "sntp"));
    TEST_ASSERT_TRUE(backend_scanner_runtime_take_time_ack(&runtime, &ack));
    TEST_ASSERT_FALSE(backend_scanner_runtime_take_time_ack(&runtime, &ack));
    TEST_ASSERT_EQUAL(
        BACKEND_SCANNER_TIME_CONFLICT,
        backend_scanner_runtime_apply_time(
            &runtime, 1U, true, INT64_C(1700000000002), "sntp"));
    TEST_ASSERT_EQUAL(
        BACKEND_SCANNER_TIME_STALE,
        backend_scanner_runtime_apply_time(
            &runtime, 0U, false, 0, "none"));

    TEST_ASSERT_EQUAL_HEX8(
        0U, backend_scanner_runtime_required_restart_mask(&runtime));
    TEST_ASSERT_EQUAL(
        BACKEND_ROLE_APPLIED,
        backend_scanner_runtime_apply_role(
            &runtime, 77U, 1U, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_EQUAL_HEX8(
        BACKEND_SCANNER_RADIO_BLE | BACKEND_SCANNER_RADIO_WIFI,
        backend_scanner_runtime_required_restart_mask(&runtime));
}

void test_scanner_runtime_watchdog_readiness_tracks_assigned_profile(void)
{
    backend_scanner_runtime_t runtime;
    backend_scanner_runtime_init(&runtime, 77U);

    TEST_ASSERT_FALSE(backend_scanner_runtime_worker_iteration(&runtime, 0U));
    TEST_ASSERT_FALSE(backend_scanner_runtime_worker_iteration(&runtime, 3U));
    TEST_ASSERT_EQUAL_HEX32(0U, runtime.watchdog_completed_mask);
    TEST_ASSERT_FALSE(backend_scanner_runtime_rollback_ready(&runtime));

    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_UART_RX_CONTROL));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_OTA));
    TEST_ASSERT_FALSE(backend_scanner_runtime_rollback_ready(&runtime));

    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED,
        backend_scanner_runtime_apply_role(
            &runtime, 77U, 1U, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_FALSE(backend_scanner_runtime_rollback_ready(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_BLE_RADIO));
    TEST_ASSERT_TRUE(backend_scanner_runtime_rollback_ready(&runtime));

    backend_scanner_runtime_init(&runtime, 88U);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED,
        backend_scanner_runtime_apply_role(
            &runtime, 88U, 1U, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_UART_RX_CONTROL));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_OTA));
    TEST_ASSERT_FALSE(backend_scanner_runtime_rollback_ready(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_WIFI_RADIO));
    TEST_ASSERT_TRUE(backend_scanner_runtime_rollback_ready(&runtime));

    backend_scanner_runtime_init(&runtime, 99U);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED,
        backend_scanner_runtime_apply_role(
            &runtime, 99U, 1U, BACKEND_SCAN_PROFILE_HYBRID_FAILOVER));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_UART_RX_CONTROL));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_OTA));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_BLE_RADIO));
    TEST_ASSERT_FALSE(backend_scanner_runtime_rollback_ready(&runtime));
    TEST_ASSERT_TRUE(backend_scanner_runtime_worker_iteration(
        &runtime, BACKEND_WORKER_WIFI_RADIO));
    TEST_ASSERT_TRUE(backend_scanner_runtime_rollback_ready(&runtime));
}

void test_recovery_helpers_fail_closed_on_invalid_time_or_arguments(void)
{
    backend_recovery_policy_t policy;
    backend_recovery_policy_init(&policy, 100);

    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_INVALID,
        backend_recovery_policy_note_status(NULL, 1U, 101));
    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_INVALID,
        backend_recovery_policy_note_status(&policy, 0U, 101));
    TEST_ASSERT_EQUAL(
        BACKEND_STATUS_BOOT_INVALID,
        backend_recovery_policy_note_status(&policy, 1U, 99));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(NULL, 100, false));
    TEST_ASSERT_EQUAL(
        BACKEND_RECOVERY_NONE,
        backend_recovery_policy_tick(&policy, 99, false));
    TEST_ASSERT_FALSE(backend_recovery_policy_complete_action(
        &policy, BACKEND_RECOVERY_SEND_PROBE, true, 101));
    TEST_ASSERT_FALSE(backend_status_cadence_mark_sent(NULL, 1));
    TEST_ASSERT_FALSE(backend_command_cadence_mark_sent(NULL, 1));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_missing_status_uses_exact_probe_reinit_and_unavailable_deadlines);
    BACKEND_RUN_TEST(
        test_valid_status_resets_local_recovery_and_changed_boot_is_reported);
    BACKEND_RUN_TEST(
        test_delayed_polling_never_bursts_health_probes_faster_than_one_second);
    BACKEND_RUN_TEST(
        test_remote_radio_restart_is_boot_bound_monotonic_and_ota_deferred);
    BACKEND_RUN_TEST(
        test_failed_recovery_delivery_remains_pending_until_success);
    BACKEND_RUN_TEST(test_status_and_coordinator_cadence_have_exact_boundaries);
    BACKEND_RUN_TEST(
        test_watchdog_readiness_requires_each_worker_own_iteration);
    BACKEND_RUN_TEST(test_time_validity_uses_the_vendored_epoch_policy);
    BACKEND_RUN_TEST(
        test_uart_slot_records_match_selected_profile_and_each_has_bounded_framer);
    BACKEND_RUN_TEST(
        test_scanner_runtime_boots_quiescent_and_preserves_control_during_quiet_modes);
    BACKEND_RUN_TEST(
        test_scanner_runtime_time_is_monotonic_and_restart_mask_tracks_role);
    BACKEND_RUN_TEST(
        test_scanner_runtime_watchdog_readiness_tracks_assigned_profile);
    BACKEND_RUN_TEST(
        test_recovery_helpers_fail_closed_on_invalid_time_or_arguments);
    return UNITY_END();
}
