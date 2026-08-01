#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_USB_HEALTH_OK = 0,
    BADGE_USB_HEALTH_WAITING,
    BADGE_USB_HEALTH_RESTART_SAFE_USB,
} badge_usb_health_action_t;

typedef struct {
    bool safe_usb;
    bool one_boot_recovery_consumed;
    bool task_started;
    bool host_connected;
    bool transaction_active;
    int64_t now_ms;
    int64_t task_heartbeat_ms;
    int64_t last_rx_ms;
    int64_t last_command_ms;
    int64_t last_response_ms;
    int64_t oldest_unanswered_command_ms;
    int64_t last_transaction_progress_ms;
    int64_t boot_grace_ms;
    int64_t stale_after_ms;
} badge_usb_health_inputs_t;

badge_usb_health_action_t badge_usb_health_decide(
    const badge_usb_health_inputs_t *inputs);

#ifdef __cplusplus
}
#endif
