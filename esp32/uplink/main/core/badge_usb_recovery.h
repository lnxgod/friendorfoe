#pragma once

#include "badge_runtime.h"
#include "badge_usb_recovery_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_USB_FIRMWARE_RESTART_PREPARE_FAILED = 0,
    BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY,
    BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED,
} badge_usb_firmware_restart_prepare_result_t;

/*
 * Acquire the expected-reboot lease before attempting the irreversible
 * firmware-operation restart reservation. If firmware work is busy, the
 * exact lease is released before BUSY is returned.
 */
badge_usb_firmware_restart_prepare_result_t
badge_usb_recovery_prepare_firmware_restart(
    const char *reason,
    badge_runtime_expected_reboot_target_t expected_target,
    badge_runtime_expected_reboot_lease_t *out_lease);

/*
 * Consume an already-owned lease. This is the only valid handoff after an
 * irreversible firmware restart reservation, so every failure path parks
 * safely and the function never returns.
 */
_Noreturn void badge_usb_recovery_restart_with_owned_lease(
    badge_usb_reset_target_t target,
    const char *reason,
    const badge_runtime_expected_reboot_lease_t *lease);

/*
 * Returns false without restarting when expected-reboot ownership cannot be
 * proved. On success the function does not return.
 */
bool badge_usb_recovery_restart(
    badge_usb_reset_target_t target, const char *reason);

#ifdef __cplusplus
}
#endif
