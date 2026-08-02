#include "backend_feature_adapter.h"

#include <stdio.h>
#include <string.h>

#include "backend_detection_sink.h"
#include "detection_types.h"
#include "rssi_distance.h"

static bool mac_is_present(const uint8_t mac[6])
{
    if (mac == NULL) {
        return false;
    }
    uint8_t combined = 0;
    for (size_t index = 0; index < 6; ++index) {
        combined |= mac[index];
    }
    return combined != 0;
}

static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void copy_text(char *destination, size_t size, const char *source)
{
    if (destination == NULL || size == 0) {
        return;
    }
    snprintf(destination, size, "%s", source != NULL ? source : "");
}

static bool confidence_is_valid(float confidence)
{
    return confidence > 0.0f && confidence <= 1.0f;
}

bool backend_feature_emit_ble(
    const backend_ble_feature_observation_t *observation)
{
    if (observation == NULL || observation->fingerprint == NULL ||
        !mac_is_present(observation->mac) || observation->observed_ms <= 0 ||
        observation->first_seen_ms <= 0 ||
        observation->first_seen_ms > observation->observed_ms) {
        return false;
    }

    const ble_fingerprint_t *fingerprint = observation->fingerprint;
    const ble_threat_signal_t *threat = observation->threat;
    float confidence = threat != NULL ? threat->confidence : observation->confidence;
    if (!confidence_is_valid(confidence)) {
        return false;
    }

    uint8_t threat_kind = BLE_THREAT_KIND_NONE;
    const char *identity_prefix = "BLE";
    const char *manufacturer = fingerprint->type_name != NULL
        ? fingerprint->type_name : ble_device_type_name(fingerprint->device_type);
    const char *reason = fingerprint->class_reason[0] != '\0'
        ? fingerprint->class_reason : "ble_fingerprint";
    if (threat != NULL) {
        if (threat->kind == BLE_THREAT_PAIRING_SPAM) {
            threat_kind = BLE_THREAT_KIND_PAIRING_SPAM;
            identity_prefix = "BLE-PAIR";
            manufacturer = "Pairing Spam";
            reason = "behavioral:pairing_spam";
        } else if (threat->kind == BLE_THREAT_SERIAL_SKIMMER) {
#if FOF_SERIAL_SKIMMER_DETECTION_ENABLED
            threat_kind = BLE_THREAT_KIND_SERIAL_SKIMMER;
            identity_prefix = "BLE-SERIAL";
            manufacturer = "Possible Skimmer";
            reason = "behavioral:serial_skimmer";
#else
            return false;
#endif
        } else {
            return false;
        }
    }

    drone_detection_t detection = {0};
    char mac[18];
    format_mac(observation->mac, mac);
    snprintf(detection.drone_id, sizeof(detection.drone_id),
             "%s:%s", identity_prefix, mac);
    detection.source = DETECTION_SRC_BLE_FINGERPRINT;
    detection.confidence = confidence;
    detection.fused_confidence = confidence;
    detection.rssi = observation->rssi;
    detection.estimated_distance_m = rssi_distance_estimate_m(observation->rssi);
    detection.first_seen_ms = observation->first_seen_ms;
    detection.last_updated_ms = observation->observed_ms;
    copy_text(detection.bssid, sizeof(detection.bssid), mac);
    copy_text(detection.manufacturer, sizeof(detection.manufacturer), manufacturer);
    snprintf(detection.model, sizeof(detection.model), "FP:%08lX",
             (unsigned long)fingerprint->hash);
    copy_text(detection.class_reason, sizeof(detection.class_reason), reason);
    copy_text(detection.ble_name, sizeof(detection.ble_name), fingerprint->local_name);
    detection.ble_company_id = fingerprint->company_id;
    detection.ble_apple_type = fingerprint->apple_type;
    detection.ble_ad_type_count = fingerprint->ad_type_count;
    detection.ble_payload_len = fingerprint->payload_len;
    detection.ble_addr_type = 1;
    memcpy(detection.ble_apple_auth, fingerprint->apple_auth,
           sizeof(detection.ble_apple_auth));
    detection.ble_apple_activity = fingerprint->apple_activity;
    detection.ble_apple_flags = fingerprint->apple_flags;
    detection.ble_raw_mfr_len = fingerprint->raw_mfr_len;
    memcpy(detection.ble_raw_mfr, fingerprint->raw_mfr,
           sizeof(detection.ble_raw_mfr));
    detection.ble_svc_uuid_count = fingerprint->svc_uuid_count;
    memcpy(detection.ble_service_uuids, fingerprint->service_uuids,
           sizeof(detection.ble_service_uuids));
    detection.ble_svc_uuid_128_count = fingerprint->svc_uuid_128_count;
    memcpy(detection.ble_service_uuids_128, fingerprint->service_uuids_128,
           sizeof(detection.ble_service_uuids_128));

    if (threat != NULL) {
        detection.ble_threat_kind = threat_kind;
        detection.ble_prompt_family_mask = threat->prompt_family_mask;
        detection.ble_unique_macs = threat->unique_macs;
        detection.ble_observation_count = threat->observation_count;
        detection.ble_serial_service_uuid = threat->serial_service_uuid;
        detection.ble_threat_evidence_mask = threat->evidence_mask;
    }
    return backend_detection_sink_emit(&detection, observation->observed_ms);
}

bool backend_feature_emit_wifi(
    const backend_wifi_feature_observation_t *observation)
{
    if (observation == NULL || observation->kind > BACKEND_WIFI_FEATURE_LOCKON ||
        !mac_is_present(observation->bssid) || observation->observed_ms <= 0 ||
        observation->freq_mhz <= 0 || observation->channel_width_mhz <= 0 ||
        !confidence_is_valid(observation->confidence)) {
        return false;
    }

    drone_detection_t detection = {0};
    char bssid[18];
    char peer[18] = {0};
    format_mac(observation->bssid, bssid);
    if (mac_is_present(observation->peer_mac)) {
        format_mac(observation->peer_mac, peer);
    }

    const char *prefix;
    const char *default_manufacturer;
    const char *default_reason;
    switch (observation->kind) {
    case BACKEND_WIFI_FEATURE_AP:
        prefix = "AP";
        default_manufacturer = "WiFi AP";
        default_reason = "ap_inventory";
        detection.source = DETECTION_SRC_WIFI_AP_INVENTORY;
        break;
    case BACKEND_WIFI_FEATURE_PROBE:
        prefix = "PROBE";
        default_manufacturer = "WiFi Probe";
        default_reason = "probe_request";
        detection.source = DETECTION_SRC_WIFI_PROBE_REQUEST;
        break;
    case BACKEND_WIFI_FEATURE_ASSOCIATION:
        if (peer[0] == '\0') {
            return false;
        }
        prefix = "STA";
        default_manufacturer = "WiFi Association";
        default_reason = "association";
        detection.source = DETECTION_SRC_WIFI_ASSOC;
        break;
    case BACKEND_WIFI_FEATURE_ANOMALY:
        if (peer[0] == '\0') {
            return false;
        }
        prefix = "ANOMALY";
        default_manufacturer = "Evil Twin";
        default_reason = "anomaly";
        detection.source = DETECTION_SRC_WIFI_ASSOC;
        break;
    case BACKEND_WIFI_FEATURE_LOCKON:
        prefix = "LOCKON";
        default_manufacturer = "Lock-on";
        default_reason = "lock_on";
        detection.source = DETECTION_SRC_WIFI_OUI;
        break;
    default:
        return false;
    }

    if (observation->kind == BACKEND_WIFI_FEATURE_ASSOCIATION) {
        snprintf(detection.drone_id, sizeof(detection.drone_id),
                 "%s:%s:AP:%s", prefix, peer, bssid);
        snprintf(detection.probed_ssids, sizeof(detection.probed_ssids),
                 "peer %s", peer);
    } else {
        snprintf(detection.drone_id, sizeof(detection.drone_id),
                 "%s:%s", prefix, bssid);
        copy_text(detection.probed_ssids, sizeof(detection.probed_ssids),
                  observation->evidence);
    }
    detection.confidence = observation->confidence;
    detection.fused_confidence = observation->confidence;
    detection.rssi = observation->rssi;
    detection.estimated_distance_m = rssi_distance_estimate_m(observation->rssi);
    detection.freq_mhz = observation->freq_mhz;
    detection.channel_width_mhz = observation->channel_width_mhz;
    detection.wifi_auth_mode = observation->wifi_auth_mode;
    detection.wifi_generation = observation->wifi_generation;
    detection.probe_ie_hash = observation->evidence_hash;
    detection.first_seen_ms = observation->observed_ms;
    detection.last_updated_ms = observation->observed_ms;
    copy_text(detection.bssid, sizeof(detection.bssid), bssid);
    copy_text(detection.ssid, sizeof(detection.ssid), observation->ssid);
    copy_text(detection.manufacturer, sizeof(detection.manufacturer),
              observation->manufacturer != NULL
                  ? observation->manufacturer : default_manufacturer);
    copy_text(detection.model, sizeof(detection.model), default_manufacturer);
    copy_text(detection.class_reason, sizeof(detection.class_reason),
              observation->class_reason != NULL
                  ? observation->class_reason : default_reason);
    return backend_detection_sink_emit(&detection, observation->observed_ms);
}
