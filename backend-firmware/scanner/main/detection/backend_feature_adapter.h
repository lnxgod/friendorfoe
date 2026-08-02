#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_fingerprint.h"
#include "ble_threat_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mac[6];
    const ble_fingerprint_t *fingerprint;
    const ble_threat_signal_t *threat;
    uint8_t addr_type;
    int8_t rssi;
    float confidence;
    int64_t first_seen_ms;
    int64_t observed_ms;
} backend_ble_feature_observation_t;

typedef enum {
    BACKEND_WIFI_FEATURE_AP = 0,
    BACKEND_WIFI_FEATURE_PROBE,
    BACKEND_WIFI_FEATURE_ASSOCIATION,
    BACKEND_WIFI_FEATURE_ANOMALY,
    BACKEND_WIFI_FEATURE_LOCKON,
} backend_wifi_feature_kind_t;

typedef struct {
    backend_wifi_feature_kind_t kind;
    uint8_t bssid[6];
    uint8_t peer_mac[6];
    const char *ssid;
    const char *manufacturer;
    const char *class_reason;
    const char *evidence;
    int8_t rssi;
    float confidence;
    int32_t freq_mhz;
    int32_t channel_width_mhz;
    uint8_t wifi_auth_mode;
    uint8_t wifi_generation;
    uint32_t evidence_hash;
    int64_t observed_ms;
} backend_wifi_feature_observation_t;

bool backend_feature_emit_ble(
    const backend_ble_feature_observation_t *observation);

bool backend_feature_emit_wifi(
    const backend_wifi_feature_observation_t *observation);

#ifdef __cplusplus
}
#endif
