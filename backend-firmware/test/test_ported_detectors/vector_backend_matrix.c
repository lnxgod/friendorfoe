#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_detection_sink.h"
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

static drone_detection_t matrix_detection(const char *id, uint8_t source,
                                           const char *manufacturer,
                                           const char *reason)
{
    drone_detection_t detection = {0};
    strncpy(detection.drone_id, id, sizeof(detection.drone_id) - 1);
    detection.source = source;
    detection.confidence = 0.75f;
    detection.rssi = -45;
    strncpy(detection.bssid, "02:00:00:00:00:01",
            sizeof(detection.bssid) - 1);
    strncpy(detection.manufacturer, manufacturer,
            sizeof(detection.manufacturer) - 1);
    strncpy(detection.class_reason, reason,
            sizeof(detection.class_reason) - 1);
    detection.first_seen_ms = 1000;
    detection.last_updated_ms = 1000;
    return detection;
}

static void emit_matrix_fingerprint(matrix_capture_t *capture,
                                    const uint8_t *advertisement, size_t length,
                                    const char *id)
{
    ble_fingerprint_t fingerprint;
    ble_fingerprint_compute(advertisement, (int)length, 1, 0, &fingerprint);
    drone_detection_t detection = matrix_detection(
        id, DETECTION_SRC_BLE_FINGERPRINT, fingerprint.type_name,
        fingerprint.class_reason);
    TEST_ASSERT_TRUE(backend_detection_sink_emit(&detection, 1000));
    TEST_ASSERT_TRUE(capture->count > 0);
}

void test_backend_feature_matrix_emits_complete_detection_snapshots(void)
{
    matrix_capture_t capture = {0};
    backend_detection_sink_register(capture_matrix_detection, &capture);

    static const uint8_t generic_ble[] = {
        5, 0x09, 'B', 'T', '0', '5'
    };
    static const uint8_t meta[] = {
        13, 0x09, 'R', 'a', 'y', '-', 'B', 'a', 'n', ' ', 'M', 'e', 't', 'a'
    };
    static const uint8_t tracker[] = {3, 0x03, 0x44, 0xFD};
    static const uint8_t venue[] = {
        26, 0xFF, 0x4C, 0x00, 0x02, 0x15,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        0, 1, 0, 2, 0xC5
    };
    emit_matrix_fingerprint(&capture, generic_ble, sizeof(generic_ble), "ble");
    emit_matrix_fingerprint(&capture, meta, sizeof(meta), "meta");
    emit_matrix_fingerprint(&capture, tracker, sizeof(tracker), "tracker");
    emit_matrix_fingerprint(&capture, venue, sizeof(venue), "venue");

    drone_detection_t pairing = matrix_detection(
        "pairing", DETECTION_SRC_BLE_FINGERPRINT, "Pairing Spam",
        "behavioral:pairing_spam");
    pairing.ble_threat_kind = BLE_THREAT_KIND_PAIRING_SPAM;
    TEST_ASSERT_TRUE(backend_detection_sink_emit(&pairing, 1000));

    TEST_ASSERT_EQUAL_INT(1, FOF_SERIAL_SKIMMER_DETECTION_ENABLED);
    drone_detection_t serial = matrix_detection(
        "serial", DETECTION_SRC_BLE_FINGERPRINT, "Possible Skimmer",
        "behavioral:serial_skimmer");
    serial.ble_threat_kind = BLE_THREAT_KIND_SERIAL_SKIMMER;
    serial.ble_serial_service_uuid = 0xFFE0;
    TEST_ASSERT_TRUE(backend_detection_sink_emit(&serial, 1000));

    const struct {
        const char *id;
        uint8_t source;
        const char *manufacturer;
        const char *reason;
    } wifi[] = {
        {"ap", DETECTION_SRC_WIFI_AP_INVENTORY, "WiFi AP", "ap_inventory"},
        {"probe", DETECTION_SRC_WIFI_PROBE_REQUEST, "WiFi Probe", "probe"},
        {"assoc", DETECTION_SRC_WIFI_ASSOC, "WiFi Assoc", "association"},
        {"anomaly", DETECTION_SRC_WIFI_ASSOC, "Evil Twin", "anomaly"},
        {"lockon", DETECTION_SRC_WIFI_OUI, "Lock-on", "lock_on"},
    };
    for (size_t index = 0; index < sizeof(wifi) / sizeof(wifi[0]); ++index) {
        drone_detection_t detection = matrix_detection(
            wifi[index].id, wifi[index].source,
            wifi[index].manufacturer, wifi[index].reason);
        TEST_ASSERT_TRUE(backend_detection_sink_emit(&detection, 1000));
    }

    TEST_ASSERT_EQUAL_size_t(11, capture.count);
    TEST_ASSERT_EQUAL_UINT8(
        BLE_THREAT_KIND_PAIRING_SPAM, capture.detections[4].ble_threat_kind);
    TEST_ASSERT_EQUAL_UINT8(
        BLE_THREAT_KIND_SERIAL_SKIMMER, capture.detections[5].ble_threat_kind);
    backend_detection_sink_register(NULL, NULL);
}
