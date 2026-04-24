#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool scanner_calibration_mode_start(const char *session_id,
                                    const char *advertise_uuid);
void scanner_calibration_mode_stop(const char *reason);
bool scanner_calibration_mode_is_active(void);
const char *scanner_calibration_mode_uuid(void);
const char *scanner_calibration_mode_session_id(void);
const char *scanner_calibration_mode_label(void);
bool scanner_calibration_mode_allows_detection(const drone_detection_t *detection);
bool scanner_calibration_mode_allows_ble_uuid128(const uint8_t uuids[][16],
                                                 uint8_t count);

#ifdef __cplusplus
}
#endif
