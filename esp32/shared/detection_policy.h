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
bool fof_policy_ssid_is_notable(const char *ssid);
const char *fof_policy_notable_ssid_label(const char *ssid);

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
uint32_t fof_policy_ble_fingerprint_reemit_ms(const char *manufacturer);
bool fof_policy_ble_meta_should_reacquire(bool ble_scanning,
                                          bool host_synced,
                                          int64_t meta_age_s,
                                          uint32_t adv_seen_delta,
                                          bool calibration_active,
                                          bool ota_active);
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
bool fof_policy_scan_profile_allows_source(const char *scan_profile,
                                           uint8_t source);

#define FOF_POLICY_EVIL_TWIN_SSID_SLOTS 16
#define FOF_POLICY_EVIL_TWIN_AP_SLOTS   4
#define FOF_POLICY_EVIL_TWIN_MAX_SSID_LEN 33

typedef struct {
    bool in_use;
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth_mode;
    int64_t last_seen_ms;
    int64_t last_alert_ms;
} fof_policy_evil_twin_ap_t;

typedef struct {
    bool in_use;
    char ssid[33];
    int64_t last_used_ms;
    fof_policy_evil_twin_ap_t aps[FOF_POLICY_EVIL_TWIN_AP_SLOTS];
} fof_policy_evil_twin_ssid_t;

typedef struct {
    fof_policy_evil_twin_ssid_t ssids[FOF_POLICY_EVIL_TWIN_SSID_SLOTS];
} fof_policy_evil_twin_state_t;

typedef struct {
    char ssid[33];
    uint8_t suspect_bssid[6];
    uint8_t reference_bssid[6];
    int8_t suspect_rssi;
    uint8_t suspect_channel;
    uint8_t suspect_auth_mode;
    uint8_t reference_auth_mode;
    bool mixed_open;
    bool strong_clone;
    char detail[48];
} fof_policy_evil_twin_alert_t;

void fof_policy_evil_twin_state_init(fof_policy_evil_twin_state_t *state);
bool fof_policy_evil_twin_observe(fof_policy_evil_twin_state_t *state,
                                  const char *ssid,
                                  const uint8_t bssid[6],
                                  int8_t rssi,
                                  uint8_t channel,
                                  uint8_t auth_mode,
                                  int64_t now_ms,
                                  fof_policy_evil_twin_alert_t *out);
const char *fof_policy_wifi_auth_label(uint8_t auth_mode);
uint8_t fof_policy_wifi_beacon_auth_mode(const uint8_t *frame, size_t frame_len);

#ifdef __cplusplus
}
#endif
