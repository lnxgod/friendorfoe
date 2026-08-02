#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_detection_sink.h"
#include "backend_feature_adapter.h"
#include "ble_fingerprint.h"
#include "ble_threat_detector.h"
#include "constants.h"
#include "detection_types.h"
#include "french_dri_parser.h"
#include "open_drone_id_parser.h"
#include "wifi_beacon_rid_parser.h"

static void append_tlv(uint8_t *payload, size_t *length, uint8_t type,
                       const char *value)
{
    size_t value_length = strlen(value);
    payload[(*length)++] = type;
    payload[(*length)++] = (uint8_t)value_length;
    memcpy(&payload[*length], value, value_length);
    *length += value_length;
}

void test_french_dri_fixture_preserves_identity_position_altitude_source_and_rssi(void)
{
    uint8_t payload[96] = {0x6A, 0x5C, 0x35, 0x01};
    size_t length = 4;
    append_tlv(payload, &length, 2, "FR-DRI-123");
    append_tlv(payload, &length, 4, "48.8566");
    append_tlv(payload, &length, 5, "2.3522");
    append_tlv(payload, &length, 6, "123.5");

    odid_state_t state;
    odid_state_init(&state, "12:34:56:78:9A:BC", 7000);
    state.rssi = -58;
    TEST_ASSERT_TRUE(french_dri_parse_ie(payload, length, &state));

    drone_detection_t detection;
    TEST_ASSERT_TRUE(odid_state_to_detection(
        &state, "fr_", DETECTION_SRC_WIFI_BEACON, &detection));
    TEST_ASSERT_EQUAL_STRING("fr_FR-DRI-123", detection.drone_id);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 48.8566, detection.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 2.3522, detection.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 123.5, detection.altitude_m);
    TEST_ASSERT_EQUAL_UINT8(DETECTION_SRC_WIFI_BEACON, detection.source);
    TEST_ASSERT_EQUAL_INT8(-58, detection.rssi);
}

void test_wifi_beacon_rid_fixture_preserves_identity_position_altitude_source_and_rssi(void)
{
    uint8_t payload[55] = {0};
    payload[0] = 0xFA;
    payload[1] = 0x0B;
    payload[2] = 0xBC;
    payload[3] = 0x0D;
    payload[4] = 2;
    payload[5] = (uint8_t)(ODID_MSG_TYPE_BASIC_ID << 4);
    payload[6] = (uint8_t)((1U << 4) | 2U);
    memcpy(&payload[7], "BEACON-RID-7", 12);

    uint8_t *location = &payload[30];
    location[0] = (uint8_t)(ODID_MSG_TYPE_LOCATION << 4);
    location[2] = 45;
    location[3] = 20;
    int32_t latitude_e7 = 515074000;
    int32_t longitude_e7 = -1278000;
    memcpy(&location[5], &latitude_e7, sizeof(latitude_e7));
    memcpy(&location[9], &longitude_e7, sizeof(longitude_e7));
    uint16_t altitude_wire = 2270;
    memcpy(&location[13], &altitude_wire, sizeof(altitude_wire));
    location[17] = 0xFF;
    location[18] = 0xFF;

    odid_state_t state;
    odid_state_init(&state, "22:33:44:55:66:77", 8000);
    state.rssi = -63;
    TEST_ASSERT_TRUE(wifi_beacon_rid_parse_ie(payload, sizeof(payload), &state));

    drone_detection_t detection;
    TEST_ASSERT_TRUE(odid_state_to_detection(
        &state, "wifi_", DETECTION_SRC_WIFI_BEACON, &detection));
    TEST_ASSERT_EQUAL_STRING("wifi_BEACON-RID-7", detection.drone_id);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 51.5074, detection.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -0.1278, detection.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 135.0, detection.altitude_m);
    TEST_ASSERT_EQUAL_UINT8(DETECTION_SRC_WIFI_BEACON, detection.source);
    TEST_ASSERT_EQUAL_INT8(-63, detection.rssi);
}

typedef struct {
    drone_detection_t detections[11];
    size_t count;
} matrix_capture_t;

static bool capture_matrix_detection(void *context,
                                     const drone_detection_t *detection,
                                     int64_t observed_monotonic_ms)
{
    matrix_capture_t *capture = context;
    TEST_ASSERT_NOT_NULL(detection);
    TEST_ASSERT_TRUE(observed_monotonic_ms > 0);
    TEST_ASSERT_TRUE(detection->drone_id[0] != '\0');
    TEST_ASSERT_TRUE(detection->confidence > 0.0f);
    TEST_ASSERT_TRUE(detection->first_seen_ms > 0);
    TEST_ASSERT_TRUE(detection->last_updated_ms >= detection->first_seen_ms);
    TEST_ASSERT_TRUE(detection->bssid[0] != '\0');
    TEST_ASSERT_TRUE(detection->manufacturer[0] != '\0');
    TEST_ASSERT_TRUE(detection->class_reason[0] != '\0');
    TEST_ASSERT_TRUE(capture->count < 11);
    capture->detections[capture->count++] = *detection;
    return true;
}

static ble_fingerprint_t emit_ble_observation(
    const uint8_t *advertisement,
    size_t length,
    const uint8_t mac[6],
    uint8_t addr_type,
    int8_t rssi,
    float confidence,
    int64_t observed_ms,
    const ble_threat_signal_t *threat)
{
    ble_fingerprint_t fingerprint;
    ble_fingerprint_compute(
        advertisement, (int)length, addr_type, 0, &fingerprint);
    const backend_ble_feature_observation_t observation = {
        .fingerprint = &fingerprint,
        .threat = threat,
        .addr_type = addr_type,
        .rssi = rssi,
        .confidence = confidence,
        .first_seen_ms = observed_ms - 100,
        .observed_ms = observed_ms,
    };
    backend_ble_feature_observation_t with_mac = observation;
    memcpy(with_mac.mac, mac, sizeof(with_mac.mac));
    TEST_ASSERT_TRUE(backend_feature_emit_ble(&with_mac));
    return fingerprint;
}

static bool observe_pairing_burst(ble_threat_signal_t *signal)
{
    bool emitted = false;
    ble_threat_detector_reset();
    for (int index = 0; index < 12; ++index) {
        for (int packet = 0; packet < 2; ++packet) {
            ble_threat_observation_t observation = {0};
            observation.mac[0] = 0x02;
            observation.mac[5] = (uint8_t)(0x40 + index);
            observation.observed_ms = 10000 + ((index * 2 + packet) * 300);
            observation.rssi = -48;
            observation.structural_hash = 0x1234;
            observation.prompt_family = BLE_PROMPT_SWIFT_PAIR;
            observation.company_id = 0x0006;
            if (ble_threat_detector_observe(&observation, signal)) {
                emitted = true;
            }
        }
    }
    return emitted;
}

static bool observe_serial_skimmer(ble_threat_signal_t *signal)
{
    ble_threat_detector_reset();
    ble_threat_observation_t observation = {0};
    const uint8_t mac[6] = {0xC0, 0x98, 0xE5, 0x00, 0x00, 0x01};
    memcpy(observation.mac, mac, sizeof(mac));
    observation.rssi = -45;
    observation.connectable = true;
    observation.structural_hash = 0xFFE0;
    observation.service_uuids[0] = 0xFFE0;
    observation.service_uuid_count = 1;
    observation.local_name = "BT";
    observation.trusted_identity = false;
    observation.observed_ms = 20000;
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, signal));
    observation.observed_ms = 22500;
    TEST_ASSERT_FALSE(ble_threat_detector_observe(&observation, signal));
    observation.observed_ms = 25100;
    return ble_threat_detector_observe(&observation, signal);
}

void test_backend_pairing_spam_identity_survives_rotating_macs(void)
{
    matrix_capture_t capture = {0};
    backend_detection_sink_register(capture_matrix_detection, &capture);

    ble_threat_signal_t pairing_signal = {0};
    TEST_ASSERT_TRUE(observe_pairing_burst(&pairing_signal));
    TEST_ASSERT_EQUAL_HEX32(0xFE69A532, pairing_signal.entity_hash);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, pairing_signal.confidence);

    static const uint8_t pairing_adv[] = {
        5, 0xFF, 0x06, 0x00, 0x01, 0x02
    };
    static const uint8_t rotating_macs[2][6] = {
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x71},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x72},
    };
    (void)emit_ble_observation(
        pairing_adv, sizeof(pairing_adv), rotating_macs[0], 2, -51,
        pairing_signal.confidence, 18000, &pairing_signal);
    (void)emit_ble_observation(
        pairing_adv, sizeof(pairing_adv), rotating_macs[1], 3, -49,
        pairing_signal.confidence, 18100, &pairing_signal);

    TEST_ASSERT_EQUAL_size_t(2, capture.count);
    TEST_ASSERT_EQUAL_STRING("BLE-PAIR:FE69A532",
                             capture.detections[0].drone_id);
    TEST_ASSERT_EQUAL_STRING("BLE-PAIR:FE69A532",
                             capture.detections[1].drone_id);
    TEST_ASSERT_EQUAL_STRING("FP:FE69A532", capture.detections[0].model);
    TEST_ASSERT_EQUAL_STRING("FP:FE69A532", capture.detections[1].model);
    TEST_ASSERT_EQUAL_STRING("02:00:00:00:00:71", capture.detections[0].bssid);
    TEST_ASSERT_EQUAL_STRING("02:00:00:00:00:72", capture.detections[1].bssid);
    TEST_ASSERT_EQUAL_INT8(-51, capture.detections[0].rssi);
    TEST_ASSERT_EQUAL_INT8(-49, capture.detections[1].rssi);
    TEST_ASSERT_EQUAL_UINT8(2, capture.detections[0].ble_addr_type);
    TEST_ASSERT_EQUAL_UINT8(3, capture.detections[1].ble_addr_type);
}

void test_backend_feature_matrix_emits_complete_detection_snapshots(void)
{
    matrix_capture_t capture = {0};
    backend_detection_sink_register(capture_matrix_detection, &capture);

    static const uint8_t privacy[] = {
        8, 0x09, 'H', 'I', 'D', 'V', 'C', 'A', 'M'
    };
    static const uint8_t meta[] = {
        13, 0x09, 'R', 'a', 'y', '-', 'B', 'a', 'n', ' ', 'M', 'e', 't', 'a'
    };
    static const uint8_t tracker[] = {3, 0x03, 0x44, 0xFD};
    static const uint8_t venue[] = {
        2, 0x01, 0x06,
        26, 0xFF, 0x4C, 0x00, 0x02, 0x15,
        0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
        0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
        0x12, 0x34, 0xAB, 0xCD, 0xC5
    };
    static const uint8_t macs[6][6] = {
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x02},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x03},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x04},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x05},
        {0xC0, 0x98, 0xE5, 0x00, 0x00, 0x01},
    };
    ble_fingerprint_t privacy_fp = emit_ble_observation(
        privacy, sizeof(privacy), macs[0], 0, -43, 0.80f, 1100, NULL);
    ble_fingerprint_t meta_fp = emit_ble_observation(
        meta, sizeof(meta), macs[1], 1, -47, 0.95f, 1200, NULL);
    ble_fingerprint_t tracker_fp = emit_ble_observation(
        tracker, sizeof(tracker), macs[2], 2, -52, 0.85f, 1300, NULL);
    ble_fingerprint_t venue_fp = emit_ble_observation(
        venue, sizeof(venue), macs[3], 3, -55, 0.70f, 1400, NULL);

    TEST_ASSERT_EQUAL(BLE_DEV_HIDDEN_CAMERA, privacy_fp.device_type);
    TEST_ASSERT_EQUAL(BLE_DEV_META_GLASSES, meta_fp.device_type);
    TEST_ASSERT_EQUAL(BLE_DEV_APPLE_FINDMY, tracker_fp.device_type);
    TEST_ASSERT_EQUAL(BLE_DEV_VENUE_BEACON, venue_fp.device_type);

    ble_threat_signal_t pairing_signal = {0};
    TEST_ASSERT_TRUE(observe_pairing_burst(&pairing_signal));
    TEST_ASSERT_EQUAL(BLE_THREAT_PAIRING_SPAM, pairing_signal.kind);
    static const uint8_t pairing_adv[] = {
        5, 0xFF, 0x06, 0x00, 0x01, 0x02
    };
    (void)emit_ble_observation(
        pairing_adv, sizeof(pairing_adv), macs[4], 2, -48,
        pairing_signal.confidence, 17000, &pairing_signal);

    TEST_ASSERT_EQUAL_INT(1, FOF_SERIAL_SKIMMER_DETECTION_ENABLED);
    ble_threat_signal_t serial_signal = {0};
    TEST_ASSERT_TRUE(observe_serial_skimmer(&serial_signal));
    TEST_ASSERT_EQUAL(BLE_THREAT_SERIAL_SKIMMER, serial_signal.kind);
    static const uint8_t serial_adv[] = {
        3, 0x03, 0xE0, 0xFF, 3, 0x09, 'B', 'T'
    };
    (void)emit_ble_observation(
        serial_adv, sizeof(serial_adv), macs[5], 0, -60,
        serial_signal.confidence, 25100, &serial_signal);

    const backend_wifi_feature_observation_t wifi[] = {
        {
            .kind = BACKEND_WIFI_FEATURE_AP,
            .bssid = {0x10, 0x20, 0x30, 0x40, 0x50, 0x01},
            .ssid = "BackendAP",
            .manufacturer = "WiFi AP",
            .class_reason = "ap_inventory",
            .rssi = -50,
            .confidence = 0.62f,
            .freq_mhz = 2412,
            .channel_width_mhz = 20,
            .wifi_auth_mode = 3,
            .wifi_generation = 6,
            .observed_ms = 30000,
        },
        {
            .kind = BACKEND_WIFI_FEATURE_PROBE,
            .bssid = {0x10, 0x20, 0x30, 0x40, 0x50, 0x02},
            .ssid = "DJI-FPV",
            .manufacturer = "WiFi Probe",
            .class_reason = "probe_request",
            .evidence = "DJI-FPV,HOME",
            .rssi = -61,
            .confidence = 0.55f,
            .freq_mhz = 2437,
            .channel_width_mhz = 20,
            .wifi_auth_mode = 0xFF,
            .wifi_generation = 5,
            .evidence_hash = 0x11223344,
            .observed_ms = 30100,
        },
        {
            .kind = BACKEND_WIFI_FEATURE_ASSOCIATION,
            .bssid = {0x10, 0x20, 0x30, 0x40, 0x50, 0x03},
            .peer_mac = {0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
            .manufacturer = "WiFi Association",
            .class_reason = "association",
            .rssi = -58,
            .confidence = 0.40f,
            .freq_mhz = 2462,
            .channel_width_mhz = 20,
            .wifi_auth_mode = 3,
            .wifi_generation = 6,
            .observed_ms = 30200,
        },
        {
            .kind = BACKEND_WIFI_FEATURE_ANOMALY,
            .bssid = {0x10, 0x20, 0x30, 0x40, 0x50, 0x04},
            .peer_mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
            .ssid = "CorpNet",
            .manufacturer = "Evil Twin",
            .class_reason = "mixed-security clone",
            .evidence = "reference AA:BB:CC:DD:EE:FF",
            .rssi = -42,
            .confidence = 0.82f,
            .freq_mhz = 5180,
            .channel_width_mhz = 80,
            .wifi_auth_mode = 0,
            .wifi_generation = 6,
            .evidence_hash = 0xAABBCCDD,
            .observed_ms = 30300,
        },
        {
            .kind = BACKEND_WIFI_FEATURE_LOCKON,
            .bssid = {0x10, 0x20, 0x30, 0x40, 0x50, 0x05},
            .ssid = "TargetAP",
            .manufacturer = "Lock-on",
            .class_reason = "lock_on channel 11",
            .evidence = "target channel 11 frames 42",
            .rssi = -39,
            .confidence = 0.90f,
            .freq_mhz = 2462,
            .channel_width_mhz = 20,
            .wifi_auth_mode = 3,
            .wifi_generation = 6,
            .evidence_hash = 42,
            .observed_ms = 30400,
        },
    };
    for (size_t index = 0; index < sizeof(wifi) / sizeof(wifi[0]); ++index) {
        TEST_ASSERT_TRUE(backend_feature_emit_wifi(&wifi[index]));
    }

    TEST_ASSERT_EQUAL_size_t(11, capture.count);
    static const char *expected_ids[11] = {
        "BLE:02:00:00:00:00:01", "BLE:02:00:00:00:00:02",
        "BLE:02:00:00:00:00:03", "BLE:02:00:00:00:00:04",
        "BLE-PAIR:FE69A532", "BLE-SERIAL:15A42060",
        "AP:10:20:30:40:50:01", "PROBE:10:20:30:40:50:02",
        "STA:22:33:44:55:66:77:AP:10:20:30:40:50:03",
        "ANOMALY:10:20:30:40:50:04", "LOCKON:10:20:30:40:50:05",
    };
    static const uint8_t expected_sources[11] = {
        DETECTION_SRC_BLE_FINGERPRINT, DETECTION_SRC_BLE_FINGERPRINT,
        DETECTION_SRC_BLE_FINGERPRINT, DETECTION_SRC_BLE_FINGERPRINT,
        DETECTION_SRC_BLE_FINGERPRINT, DETECTION_SRC_BLE_FINGERPRINT,
        DETECTION_SRC_WIFI_AP_INVENTORY, DETECTION_SRC_WIFI_PROBE_REQUEST,
        DETECTION_SRC_WIFI_ASSOC, DETECTION_SRC_WIFI_ASSOC,
        DETECTION_SRC_WIFI_OUI,
    };
    static const float expected_confidences[11] = {
        0.80f, 0.95f, 0.85f, 0.70f, 1.0f, 1.0f,
        0.62f, 0.55f, 0.40f, 0.82f, 0.90f,
    };
    for (size_t index = 0; index < 11; ++index) {
        TEST_ASSERT_EQUAL_STRING(expected_ids[index], capture.detections[index].drone_id);
        TEST_ASSERT_EQUAL_UINT8(expected_sources[index], capture.detections[index].source);
        TEST_ASSERT_FLOAT_WITHIN(
            0.0001f, expected_confidences[index],
            capture.detections[index].confidence);
    }
    TEST_ASSERT_EQUAL_STRING("Hidden Camera (suspect)", capture.detections[0].manufacturer);
    TEST_ASSERT_EQUAL_UINT8(0, capture.detections[0].ble_addr_type);
    TEST_ASSERT_EQUAL_STRING("Meta Glasses", capture.detections[1].manufacturer);
    TEST_ASSERT_EQUAL_HEX16(0xFD44, capture.detections[2].ble_service_uuids[0]);
    TEST_ASSERT_EQUAL_HEX16(0x004C, capture.detections[3].ble_company_id);
    TEST_ASSERT_EQUAL_UINT8(BLE_THREAT_KIND_PAIRING_SPAM,
                            capture.detections[4].ble_threat_kind);
    TEST_ASSERT_EQUAL_STRING("FP:FE69A532", capture.detections[4].model);
    TEST_ASSERT_EQUAL_UINT8(BLE_PROMPT_SWIFT_PAIR,
                            capture.detections[4].ble_prompt_family_mask);
    TEST_ASSERT_EQUAL_UINT8(BLE_THREAT_KIND_SERIAL_SKIMMER,
                            capture.detections[5].ble_threat_kind);
    TEST_ASSERT_EQUAL_STRING("FP:15A42060", capture.detections[5].model);
    TEST_ASSERT_EQUAL_INT8(-45, capture.detections[5].rssi);
    TEST_ASSERT_EQUAL_HEX16(0xFFE0,
                           capture.detections[5].ble_serial_service_uuid);
    TEST_ASSERT_TRUE(capture.detections[5].ble_threat_evidence_mask != 0);
    TEST_ASSERT_EQUAL_STRING("BackendAP", capture.detections[6].ssid);
    TEST_ASSERT_EQUAL_UINT8(3, capture.detections[6].wifi_auth_mode);
    TEST_ASSERT_EQUAL_STRING("DJI-FPV,HOME", capture.detections[7].probed_ssids);
    TEST_ASSERT_EQUAL_HEX32(0x11223344, capture.detections[7].probe_ie_hash);
    TEST_ASSERT_EQUAL_STRING("peer 22:33:44:55:66:77",
                             capture.detections[8].probed_ssids);
    TEST_ASSERT_EQUAL_STRING("mixed-security clone",
                             capture.detections[9].class_reason);
    TEST_ASSERT_EQUAL_HEX32(0xAABBCCDD, capture.detections[9].probe_ie_hash);
    TEST_ASSERT_EQUAL_STRING("target channel 11 frames 42",
                             capture.detections[10].probed_ssids);
    TEST_ASSERT_EQUAL_INT32(2462, capture.detections[10].freq_mhz);
    backend_detection_sink_register(NULL, NULL);
}
