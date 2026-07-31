#include "unity.h"

#include "badge_usb_health_policy.h"

static badge_usb_health_inputs_t healthy_inputs(void)
{
    badge_usb_health_inputs_t inputs = {
        .task_started = true,
        .now_ms = 10000,
        .task_heartbeat_ms = 10000,
        .last_rx_ms = -1,
        .last_command_ms = -1,
        .last_response_ms = -1,
        .oldest_unanswered_command_ms = -1,
        .last_transaction_progress_ms = -1,
        .boot_grace_ms = 2000,
        .stale_after_ms = 3000,
    };
    return inputs;
}

void test_badge_usb_health_fresh_heartbeat_is_healthy_without_host_io(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_OK, badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_restarts_stale_task(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.task_heartbeat_ms = 6000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_restarts_unanswered_command(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.last_rx_ms = 5000;
    inputs.last_command_ms = 6000;
    inputs.last_response_ms = 5000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_continuous_commands_cannot_hide_oldest_unanswered(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.host_connected = true;
    inputs.last_rx_ms = 9900;
    inputs.last_command_ms = 9900;
    inputs.last_response_ms = 5000;
    inputs.oldest_unanswered_command_ms = 6000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_restarts_stale_host_rx_without_response(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.host_connected = true;
    inputs.last_response_ms = 5000;
    inputs.last_rx_ms = 5000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_OK, badge_usb_health_decide(&inputs));

    inputs.last_rx_ms = 6000;
    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_allows_fresh_transaction_progress(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.transaction_active = true;
    inputs.task_heartbeat_ms = 6000;
    inputs.last_transaction_progress_ms = 8000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_OK, badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_restarts_stalled_transaction_progress(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.transaction_active = true;
    inputs.last_transaction_progress_ms = 6000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_safe_boot_does_not_reboot_loop(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.safe_usb = true;
    inputs.one_boot_recovery_consumed = true;
    inputs.task_heartbeat_ms = 6000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_WAITING,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_waits_through_boot_grace(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.now_ms = 1000;
    inputs.task_heartbeat_ms = -1;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_WAITING,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_restarts_unstarted_task_after_boot_grace(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.task_started = false;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_safe_mode_holds_unstarted_task_after_boot_grace(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.task_started = false;
    inputs.safe_usb = true;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_WAITING,
                      badge_usb_health_decide(&inputs));
}

void test_badge_usb_health_restarts_when_response_timestamp_is_future(void)
{
    badge_usb_health_inputs_t inputs = healthy_inputs();
    inputs.last_command_ms = 6000;
    inputs.last_response_ms = 11000;

    TEST_ASSERT_EQUAL(BADGE_USB_HEALTH_RESTART_SAFE_USB,
                      badge_usb_health_decide(&inputs));
}
