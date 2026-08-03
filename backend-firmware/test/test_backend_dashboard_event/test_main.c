#include <math.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_dashboard_event.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_detection_observation_t fixture_observation(uint8_t source)
{
    backend_detection_observation_t observation = {0};
    observation.timestamp_valid = true;
    observation.timestamp_epoch_ms = INT64_C(1785600000123);
    strcpy(observation.detection.drone_id, "event-1");
    strcpy(observation.detection.manufacturer, "Acme");
    strcpy(observation.detection.model, "Model One");
    strcpy(observation.detection.bssid, "AA:BB:CC:DD:EE:FF");
    observation.detection.source = source;
    observation.detection.confidence = 0.804f;
    observation.detection.fused_confidence = 0.806f;
    observation.detection.rssi = -55;
    observation.detection.latitude = 37.25;
    observation.detection.longitude = -122.5;
    observation.detection.operator_lat = 37.5;
    observation.detection.operator_lon = -122.75;
    observation.detection.scanner_slots_seen = UINT8_C(0x03);
    return observation;
}

void test_projector_prioritizes_each_meta_glasses_evidence_field(void)
{
    backend_detection_observation_t observations[3] = {
        fixture_observation(DETECTION_SRC_BLE_RID),
        fixture_observation(DETECTION_SRC_BLE_RID),
        fixture_observation(DETECTION_SRC_BLE_RID),
    };
    strcpy(observations[0].detection.manufacturer, "Ray-Ban");
    strcpy(observations[1].detection.model, "Meta Glasses 2");
    strcpy(observations[2].detection.class_reason, "name:meta_glasses");

    for (size_t index = 0U; index < 3U; ++index) {
        backend_dashboard_event_t event = {0};
        TEST_ASSERT_TRUE(backend_dashboard_event_project(
            &observations[index], &event));
        TEST_ASSERT_EQUAL_STRING("Meta Glasses", event.badge_label);
        TEST_ASSERT_EQUAL_STRING("meta_glasses", event.badge_class);
    }
}

void test_projector_maps_only_the_five_drone_sources(void)
{
    static const uint8_t drone_sources[] = {
        DETECTION_SRC_BLE_RID,
        DETECTION_SRC_WIFI_SSID,
        DETECTION_SRC_WIFI_DJI_IE,
        DETECTION_SRC_WIFI_BEACON,
        DETECTION_SRC_WIFI_OUI,
    };
    for (size_t index = 0U;
         index < sizeof(drone_sources) / sizeof(drone_sources[0]);
         ++index) {
        backend_detection_observation_t observation =
            fixture_observation(drone_sources[index]);
        backend_dashboard_event_t event = {0};
        TEST_ASSERT_TRUE(backend_dashboard_event_project(
            &observation, &event));
        TEST_ASSERT_EQUAL_STRING("Drone", event.badge_label);
        TEST_ASSERT_EQUAL_STRING("drone", event.badge_class);
    }

    backend_detection_observation_t other =
        fixture_observation(DETECTION_SRC_BLE_FINGERPRINT);
    backend_dashboard_event_t event = {0};
    TEST_ASSERT_TRUE(backend_dashboard_event_project(&other, &event));
    TEST_ASSERT_EQUAL_STRING("", event.badge_label);
    TEST_ASSERT_EQUAL_STRING("", event.badge_class);
}

void test_projector_uses_shared_identity_rounds_score_and_estimates_distance(void)
{
    backend_detection_observation_t observation =
        fixture_observation(DETECTION_SRC_WIFI_BEACON);
    backend_dashboard_event_t event = {0};

    TEST_ASSERT_TRUE(backend_dashboard_event_project(&observation, &event));
    TEST_ASSERT_EQUAL_STRING("event-1", event.id);
    TEST_ASSERT_EQUAL_STRING("Acme", event.manufacturer);
    TEST_ASSERT_EQUAL_STRING("Model One", event.model);
    TEST_ASSERT_EQUAL_STRING(
        "WIFI:AA:BB:CC:DD:EE:FF", event.badge_entity_key);
    TEST_ASSERT_EQUAL_UINT8(DETECTION_SRC_WIFI_BEACON, event.source);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.804f, event.confidence);
    TEST_ASSERT_EQUAL_UINT8(81U, event.threat_score);
    TEST_ASSERT_EQUAL_INT8(-55, event.rssi);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 1.0, event.distance_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.25, event.aircraft_lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.5, event.aircraft_lon);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.5, event.operator_lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.75, event.operator_lon);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x03), event.scanner_slot_mask);
    TEST_ASSERT_TRUE(event.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600000123), event.timestamp_epoch_ms);

    observation.detection.confidence = 1.25f;
    observation.detection.fused_confidence = -0.5f;
    TEST_ASSERT_TRUE(backend_dashboard_event_project(&observation, &event));
    TEST_ASSERT_EQUAL_UINT8(100U, event.threat_score);
    observation.detection.confidence = -0.25f;
    TEST_ASSERT_TRUE(backend_dashboard_event_project(&observation, &event));
    TEST_ASSERT_EQUAL_UINT8(0U, event.threat_score);
}

void test_projector_rejects_nonfinite_confidence_and_coordinates(void)
{
    backend_detection_observation_t observation =
        fixture_observation(DETECTION_SRC_BLE_RID);
    backend_dashboard_event_t event;
    memset(&event, 0xA5, sizeof(event));
    const backend_dashboard_event_t cleared = {0};

    observation.detection.confidence = NAN;
    TEST_ASSERT_FALSE(backend_dashboard_event_project(&observation, &event));
    TEST_ASSERT_EQUAL_MEMORY(&cleared, &event, sizeof(event));
    observation = fixture_observation(DETECTION_SRC_BLE_RID);
    observation.detection.fused_confidence = INFINITY;
    TEST_ASSERT_FALSE(backend_dashboard_event_project(&observation, &event));
    observation = fixture_observation(DETECTION_SRC_BLE_RID);
    observation.detection.latitude = NAN;
    TEST_ASSERT_FALSE(backend_dashboard_event_project(&observation, &event));
    observation = fixture_observation(DETECTION_SRC_BLE_RID);
    observation.detection.longitude = INFINITY;
    TEST_ASSERT_FALSE(backend_dashboard_event_project(&observation, &event));
    observation = fixture_observation(DETECTION_SRC_BLE_RID);
    observation.detection.operator_lat = -INFINITY;
    TEST_ASSERT_FALSE(backend_dashboard_event_project(&observation, &event));
    observation = fixture_observation(DETECTION_SRC_BLE_RID);
    observation.detection.operator_lon = NAN;
    TEST_ASSERT_FALSE(backend_dashboard_event_project(&observation, &event));
}

void test_dashboard_json_escapes_every_text_field_exactly(void)
{
    backend_dashboard_event_t event = {
        .sequence = UINT64_C(7),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1785600000123),
        .source = DETECTION_SRC_WIFI_SSID,
        .confidence = 0.75f,
        .threat_score = 75U,
        .rssi = -60,
        .distance_m = 2.5,
        .aircraft_lat = 1.25,
        .aircraft_lon = -2.5,
        .operator_lat = 3.75,
        .operator_lon = -4.5,
        .scanner_slot_mask = UINT8_C(0x03),
    };
    strcpy(event.id, "id\"\\\n");
    strcpy(event.manufacturer, "M\tCorp");
    strcpy(event.model, "Model\rOne");
    strcpy(event.badge_label, "Drone");
    strcpy(event.badge_class, "drone");
    strcpy(event.badge_entity_key, "WIFI:AA\\BB");
    char encoded[1024] = {0};

    const size_t length = backend_dashboard_event_encode_json(
        &event, encoded, sizeof(encoded));

    static const char expected[] =
        "{\"sequence\":7,\"timestamp_valid\":true,"
        "\"timestamp_epoch_ms\":1785600000123,"
        "\"id\":\"id\\\"\\\\\\n\",\"manufacturer\":\"M\\tCorp\","
        "\"model\":\"Model\\rOne\",\"badge_label\":\"Drone\","
        "\"badge_class\":\"drone\","
        "\"badge_entity_key\":\"WIFI:AA\\\\BB\",\"source\":1,"
        "\"confidence\":0.75,\"threat_score\":75,\"rssi\":-60,"
        "\"distance_m\":2.5,\"aircraft_lat\":1.25,"
        "\"aircraft_lon\":-2.5,\"operator_lat\":3.75,"
        "\"operator_lon\":-4.5,\"scanner_slot_mask\":3}";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), length);
    TEST_ASSERT_EQUAL_STRING(expected, encoded);

    char too_small[16] = "not-empty";
    TEST_ASSERT_EQUAL_size_t(0U, backend_dashboard_event_encode_json(
        &event, too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_STRING("", too_small);
}

void test_fof_det_has_exact_compatibility_fields_and_bounded_size(void)
{
    backend_dashboard_event_t event = {
        .source = DETECTION_SRC_BLE_RID,
        .confidence = 0.875f,
        .threat_score = 88U,
        .rssi = -72,
    };
    strcpy(event.id, "usb-1");
    strcpy(event.manufacturer, "Meta");
    strcpy(event.badge_label, "Meta Glasses");
    strcpy(event.badge_class, "meta_glasses");
    strcpy(event.badge_entity_key, "ID:usb-1");
    char encoded[1535] = {0};

    const size_t length = backend_dashboard_event_encode_fof_det(
        &event, encoded, sizeof(encoded));

    static const char expected[] =
        "FOF_DET:{\"id\":\"usb-1\",\"manufacturer\":\"Meta\","
        "\"badge_label\":\"Meta Glasses\","
        "\"badge_class\":\"meta_glasses\","
        "\"badge_entity_key\":\"ID:usb-1\",\"source\":0,"
        "\"confidence\":0.875,\"threat_score\":88,\"rssi\":-72}\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), length);
    TEST_ASSERT_EQUAL_STRING(expected, encoded);
    TEST_ASSERT_LESS_THAN_size_t(1535U, length);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_projector_prioritizes_each_meta_glasses_evidence_field);
    BACKEND_RUN_TEST(test_projector_maps_only_the_five_drone_sources);
    BACKEND_RUN_TEST(
        test_projector_uses_shared_identity_rounds_score_and_estimates_distance);
    BACKEND_RUN_TEST(
        test_projector_rejects_nonfinite_confidence_and_coordinates);
    BACKEND_RUN_TEST(test_dashboard_json_escapes_every_text_field_exactly);
    BACKEND_RUN_TEST(
        test_fof_det_has_exact_compatibility_fields_and_bounded_size);
    return UNITY_END();
}
