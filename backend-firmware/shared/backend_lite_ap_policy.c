#include "backend_lite_ap_policy.h"

#include <string.h>

void backend_lite_ap_policy_init(backend_lite_ap_policy_t *policy)
{
    if (!policy) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
}

backend_ap_action_t backend_lite_ap_policy_tick(
    backend_lite_ap_policy_t *policy,
    backend_lite_ap_input_t input)
{
    if (!policy) {
        return BACKEND_AP_NO_CHANGE;
    }

    if (!input.wifi_configured) {
        policy->reason = BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED;
    } else if (input.wifi_join_failed) {
        policy->reason = BACKEND_LITE_AP_REASON_WIFI_JOIN_FAILED;
    } else {
        policy->reason = BACKEND_LITE_AP_REASON_NONE;
    }

    const bool eligible = !input.wifi_configured || input.wifi_join_failed;
    const bool desired = eligible && !input.wifi_connected &&
        !input.usb_live_confirmed;
    if (desired == policy->running) {
        return BACKEND_AP_NO_CHANGE;
    }
    policy->running = desired;
    return desired ? BACKEND_AP_START : BACKEND_AP_STOP;
}

backend_lite_ap_reason_t backend_lite_ap_policy_reason(
    const backend_lite_ap_policy_t *policy)
{
    return policy ? policy->reason : BACKEND_LITE_AP_REASON_NONE;
}
