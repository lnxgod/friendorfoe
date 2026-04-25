#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool fof_policy_probe_should_ignore_broadcast(const char *ssid);
float fof_policy_probe_confidence(bool hard_match);

bool fof_policy_is_priority_ble_fingerprint(const char *manufacturer);
bool fof_policy_ble_uuid128_is_calibration_le(const uint8_t uuid_le[16]);
bool fof_policy_ble_has_calibration_uuid_le(const uint8_t uuids[][16],
                                            uint8_t count);
bool fof_policy_ble_uuid128_matches_token_le(const uint8_t uuid_le[16],
                                             const char *uuid_token);
bool fof_policy_ble_has_exact_uuid128_le(const uint8_t uuids[][16],
                                         uint8_t count,
                                         const char *uuid_token);
bool fof_policy_ble_svc_raw_contains_uuid(const char *svc_raw,
                                          const char *uuid_token);
bool fof_policy_should_drop_low_value(uint8_t source,
                                      float confidence,
                                      const char *manufacturer,
                                      const uint8_t ble_svc_uuids_128[][16],
                                      uint8_t ble_svc_uuid_128_count);
bool fof_policy_is_controller_class_ble(uint8_t source,
                                        const char *manufacturer);
bool fof_policy_should_shed_low_priority(uint8_t source,
                                         const char *manufacturer,
                                         const uint8_t ble_svc_uuids_128[][16],
                                         uint8_t ble_svc_uuid_128_count,
                                         uint32_t queue_depth,
                                         uint32_t queue_capacity);
uint32_t fof_policy_queue_pressure_pct(uint32_t queue_depth,
                                       uint32_t queue_capacity);
void fof_policy_probe_rate_aux(uint32_t ie_hash,
                               const char *probed_ssids,
                               char *out,
                               size_t out_len);
bool fof_policy_detection_identity_key(const drone_detection_t *det,
                                       char *out,
                                       size_t out_len);
bool fof_policy_detection_dedupe_key(const drone_detection_t *det,
                                     int64_t timestamp_ms,
                                     uint32_t bucket_ms,
                                     char *out,
                                     size_t out_len);
const char *fof_policy_scan_profile_for_slot(uint8_t scanner_id,
                                             bool calibration_active);
const char *fof_policy_slot_role_for_slot(uint8_t scanner_id);

#ifdef __cplusplus
}
#endif
