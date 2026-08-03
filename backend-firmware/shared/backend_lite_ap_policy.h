#ifndef BACKEND_LITE_AP_POLICY_H
#define BACKEND_LITE_AP_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "backend_ap_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_LITE_AP_REASON_NONE = 0,
    BACKEND_LITE_AP_REASON_WIFI_UNCONFIGURED,
    BACKEND_LITE_AP_REASON_WIFI_JOIN_FAILED,
} backend_lite_ap_reason_t;

typedef struct {
    bool wifi_configured;
    bool wifi_connected;
    bool wifi_join_failed;
    bool usb_live_confirmed;
} backend_lite_ap_input_t;

typedef struct {
    bool running;
    backend_lite_ap_reason_t reason;
} backend_lite_ap_policy_t;

void backend_lite_ap_policy_init(backend_lite_ap_policy_t *policy);
backend_ap_action_t backend_lite_ap_policy_tick(
    backend_lite_ap_policy_t *policy,
    backend_lite_ap_input_t input);
backend_lite_ap_reason_t backend_lite_ap_policy_reason(
    const backend_lite_ap_policy_t *policy);
bool backend_lite_network_can_use_sta(
    uint8_t network_count,
    bool manager_initialized,
    bool station_connected);

#ifdef __cplusplus
}
#endif

#endif
