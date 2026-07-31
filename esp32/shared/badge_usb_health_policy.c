#include "badge_usb_health_policy.h"

static bool timestamp_stale(int64_t now_ms, int64_t timestamp_ms,
                            int64_t stale_after_ms)
{
    return timestamp_ms < 0 || now_ms < timestamp_ms ||
           now_ms - timestamp_ms >= stale_after_ms;
}

static badge_usb_health_action_t failure_action(
    const badge_usb_health_inputs_t *inputs)
{
    if (inputs->safe_usb || inputs->one_boot_recovery_consumed) {
        return BADGE_USB_HEALTH_WAITING;
    }
    return BADGE_USB_HEALTH_RESTART_SAFE_USB;
}

badge_usb_health_action_t badge_usb_health_decide(
    const badge_usb_health_inputs_t *inputs)
{
    if (!inputs || inputs->now_ms < 0 ||
        inputs->stale_after_ms <= 0 || inputs->boot_grace_ms < 0 ||
        inputs->now_ms < inputs->boot_grace_ms) {
        return BADGE_USB_HEALTH_WAITING;
    }

    if (!inputs->task_started) {
        return failure_action(inputs);
    }

    int64_t last_response_ms = inputs->last_response_ms > inputs->now_ms
        ? -1
        : inputs->last_response_ms;

    if (inputs->transaction_active) {
        if (!timestamp_stale(inputs->now_ms,
                             inputs->last_transaction_progress_ms,
                             inputs->stale_after_ms)) {
            return BADGE_USB_HEALTH_OK;
        }
        return failure_action(inputs);
    }

    /* This timestamp is latched on the first required response failure and
     * is not moved forward by newer traffic. A continuous stream of commands
     * therefore cannot hide an older command whose reply never drained. */
    if (inputs->oldest_unanswered_command_ms >= 0 &&
        timestamp_stale(inputs->now_ms,
                        inputs->oldest_unanswered_command_ms,
                        inputs->stale_after_ms)) {
        return failure_action(inputs);
    }

    if (inputs->host_connected && inputs->last_rx_ms >= 0 &&
        last_response_ms < inputs->last_rx_ms &&
        timestamp_stale(inputs->now_ms, inputs->last_rx_ms,
                        inputs->stale_after_ms)) {
        return failure_action(inputs);
    }

    if (inputs->last_command_ms >= 0 &&
        last_response_ms < inputs->last_command_ms &&
        timestamp_stale(inputs->now_ms, inputs->last_command_ms,
                        inputs->stale_after_ms)) {
        return failure_action(inputs);
    }

    if (timestamp_stale(inputs->now_ms, inputs->task_heartbeat_ms,
                        inputs->stale_after_ms)) {
        return failure_action(inputs);
    }

    /* No host traffic is healthy-idle; RX only matters after it needs a reply. */
    return BADGE_USB_HEALTH_OK;
}
