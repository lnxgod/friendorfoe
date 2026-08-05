#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the serialized scanner radio/profile transition state. */
void scanner_calibration_mode_init(void);
bool scanner_calibration_mode_start(const char *session_id,
                                    const char *advertise_uuid);
void scanner_calibration_mode_stop(const char *reason);
bool scanner_calibration_mode_is_active(void);
/**
 * Authoritative scanner quiet state.  The state flag is published before
 * detection TX or either radio is halted so every asynchronous profile/retry
 * path can fail closed while the UART command task remains alive.
 */
bool scanner_quiet_mode_set(bool enabled, uint32_t generation);
bool scanner_quiet_mode_is_active(void);
uint32_t scanner_quiet_mode_generation(void);
bool scanner_quiet_mode_radios_ready(void);
bool scanner_quiet_mode_tx_restored(void);
const char *scanner_quiet_mode_last_error(void);
const char *scanner_calibration_mode_uuid(void);
const char *scanner_calibration_mode_session_id(void);
const char *scanner_calibration_mode_label(void);
void scanner_scan_profile_set(const char *profile);
void scanner_scan_profile_apply(void);
const char *scanner_scan_profile_label(void);
/** True after an explicit profile command arrived from the uplink. */
bool scanner_scan_profile_assignment_seen(void);
/** Fence profile commands from touching radio drivers before boot init. */
void scanner_scan_profile_runtime_ready_set(bool ready);
/**
 * Atomically validate and persist a fixed-slot role/profile command.
 * Both strings must be the same exact fixed role, and an existing durable
 * role cannot be changed.  A command arriving after the boot allocation
 * window claims one serialized in-place radio recovery via the out parameter.
 */
bool scanner_slot_role_command_apply(const char *slot_role,
                                     const char *profile,
                                     bool *late_recovery_required);
/** Close the boot allocation window and return whether a role was assigned. */
bool scanner_slot_role_boot_window_close(void);
const char *scanner_slot_role_label(void);
bool scanner_slot_role_assignment_seen(void);
/** True only after a valid, durable role command arrived this boot. */
bool scanner_slot_role_command_seen(void);
bool scanner_calibration_mode_allows_detection(const drone_detection_t *detection);
bool scanner_calibration_mode_allows_ble_uuid128(const uint8_t uuids[][16],
                                                 uint8_t count);

#ifdef __cplusplus
}
#endif
