#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_live_entities.h"
#include "backend_threat_policy.h"
#include "backend_usb_protocol.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_dashboard_event_t fixture_event(
    const char *key,
    const char *id,
    uint8_t source,
    const char *threat_class)
{
    backend_dashboard_event_t event = {
        .source = source,
        .confidence = 0.904f,
        .threat_score = 90U,
        .rssi = -61,
        .distance_m = 2.0,
        .aircraft_lat = 37.7749,
        .aircraft_lon = -122.4194,
        .altitude_m = 42.5,
        .operator_lat = 37.7750,
        .operator_lon = -122.4195,
    };
    snprintf(event.badge_entity_key, sizeof(event.badge_entity_key),
             "%s", key);
    snprintf(event.id, sizeof(event.id), "%s", id);
    snprintf(event.manufacturer, sizeof(event.manufacturer), "FOF Sim");
    snprintf(event.model, sizeof(event.model), "Orbit");
    snprintf(event.badge_label, sizeof(event.badge_label), "%s",
             strcmp(threat_class, "drone") == 0 ? "Drone" : "Meta Glasses");
    snprintf(event.badge_class, sizeof(event.badge_class), "%s",
             threat_class);
    snprintf(event.operator_id, sizeof(event.operator_id), "operator-1");
    return event;
}

static size_t encode(
    const backend_live_entities_t *state,
    int64_t now_ms,
    char *output,
    size_t capacity)
{
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    if (!backend_live_entities_append_json(&writer, state, now_ms)) {
        return 0U;
    }
    return backend_json_writer_finish(&writer);
}

void test_ingest_deduplicates_and_preserves_best_rssi(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    backend_dashboard_event_t event = fixture_event(
        "ID:FOF-SIM-001", "FOF-SIM-001", DETECTION_SRC_BLE_RID,
        "drone");
    TEST_ASSERT_TRUE(backend_live_entities_ingest(
        &state, &event, INT64_C(1000)));
    event.rssi = -72;
    TEST_ASSERT_TRUE(backend_live_entities_ingest(
        &state, &event, INT64_C(2000)));

    TEST_ASSERT_EQUAL_size_t(
        1U, backend_live_entities_active_count(&state, INT64_C(2000)));
    char output[2048] = {0};
    TEST_ASSERT_NOT_EQUAL(0U, encode(
        &state, INT64_C(2500), output, sizeof(output)));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"events\":2"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"rssi\":-72"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"best_rssi\":-61"));
}

void test_reappearing_entity_starts_a_new_live_session(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    backend_dashboard_event_t event = fixture_event(
        "ID:FOF-SIM-001", "FOF-SIM-001", DETECTION_SRC_BLE_RID,
        "drone");
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &event, 0));
    event.rssi = -75;
    TEST_ASSERT_TRUE(backend_live_entities_ingest(
        &state, &event, BACKEND_REMOTE_ID_LIVE_WINDOW_MS + 1));

    char output[2048] = {0};
    TEST_ASSERT_NOT_EQUAL(0U, encode(
        &state, BACKEND_REMOTE_ID_LIVE_WINDOW_MS + 1,
        output, sizeof(output)));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"events\":1"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"best_rssi\":-75"));
}

void test_live_windows_match_backend_threat_policy(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    backend_dashboard_event_t remote = fixture_event(
        "ID:rid", "rid", DETECTION_SRC_BLE_RID, "drone");
    backend_dashboard_event_t ssid = fixture_event(
        "WIFI:ssid", "ssid", DETECTION_SRC_WIFI_SSID, "drone");
    backend_dashboard_event_t meta = fixture_event(
        "BLE:meta", "meta", DETECTION_SRC_BLE_FINGERPRINT, "meta");
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &remote, 0));
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &ssid, 0));
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &meta, 0));

    TEST_ASSERT_EQUAL_size_t(3U, backend_live_entities_active_count(
        &state, BACKEND_DRONE_SSID_LIVE_WINDOW_MS));
    TEST_ASSERT_EQUAL_size_t(2U, backend_live_entities_active_count(
        &state, BACKEND_DRONE_SSID_LIVE_WINDOW_MS + 1));
    TEST_ASSERT_EQUAL_size_t(2U, backend_live_entities_active_count(
        &state, BACKEND_REMOTE_ID_LIVE_WINDOW_MS));
    TEST_ASSERT_EQUAL_size_t(0U, backend_live_entities_active_count(
        &state, BACKEND_REMOTE_ID_LIVE_WINDOW_MS + 1));
}

void test_drone_and_meta_with_same_identity_remain_distinct(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    backend_dashboard_event_t drone = fixture_event(
        "BLE:shared", "shared", DETECTION_SRC_BLE_RID, "drone");
    backend_dashboard_event_t meta = fixture_event(
        "BLE:shared", "shared", DETECTION_SRC_BLE_FINGERPRINT,
        "meta_glasses");
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &drone, 10));
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &meta, 20));
    TEST_ASSERT_EQUAL_size_t(
        2U, backend_live_entities_active_count(&state, 20));

    char output[4096] = {0};
    TEST_ASSERT_NOT_EQUAL(0U, encode(&state, 20, output, sizeof(output)));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"class\":\"drone\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"class\":\"meta\""));
}

void test_json_uses_native_badge_fields_and_position_contract(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    backend_dashboard_event_t event = fixture_event(
        "ID:FOF-SIM-001", "FOF-SIM-001", DETECTION_SRC_BLE_RID,
        "drone");
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &event, 1000));

    char output[2048] = {0};
    TEST_ASSERT_NOT_EQUAL(0U, encode(&state, 3500, output, sizeof(output)));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"class\":\"drone\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"category\":\"DRONE\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"code\":\"DRN\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"source\":\"ble_rid\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"source_id\":0"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"display_id\":\"FOF-SIM-001\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"last_seen_s\":2"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"lat\":37.7749000"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"lon\":-122.4194000"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"operator_id\":\"operator-1\""));
}

void test_meta_is_canonical_and_zero_position_is_omitted(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    backend_dashboard_event_t event = fixture_event(
        "BLE:meta", "meta-1", DETECTION_SRC_BLE_FINGERPRINT,
        "meta_glasses");
    event.aircraft_lat = 0.0;
    event.aircraft_lon = 0.0;
    event.operator_lat = 0.0;
    event.operator_lon = 0.0;
    TEST_ASSERT_TRUE(backend_live_entities_ingest(&state, &event, 1000));

    char output[2048] = {0};
    TEST_ASSERT_NOT_EQUAL(0U, encode(&state, 1000, output, sizeof(output)));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"class\":\"meta\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"category\":\"GLASS\""));
    TEST_ASSERT_NULL(strstr(output, "\"lat\":"));
    TEST_ASSERT_NULL(strstr(output, "\"operator_lat\":"));
}

void test_capacity_evicts_oldest_entity_and_encoder_fails_closed(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    for (size_t index = 0U; index <= BACKEND_LIVE_ENTITY_CAPACITY; ++index) {
        char key[32];
        char id[32];
        snprintf(key, sizeof(key), "ID:%u", (unsigned)index);
        snprintf(id, sizeof(id), "entity-%u", (unsigned)index);
        backend_dashboard_event_t event = fixture_event(
            key, id, DETECTION_SRC_BLE_RID, "drone");
        TEST_ASSERT_TRUE(backend_live_entities_ingest(
            &state, &event, (int64_t)index));
    }
    TEST_ASSERT_EQUAL_size_t(BACKEND_LIVE_ENTITY_CAPACITY,
        backend_live_entities_active_count(&state, 10));
    char output[8192] = {0};
    TEST_ASSERT_NOT_EQUAL(0U, encode(&state, 10, output, sizeof(output)));
    TEST_ASSERT_NULL(strstr(output, "\"display_id\":\"entity-0\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"display_id\":\"entity-8\""));

    char too_small[16] = "not-empty";
    TEST_ASSERT_EQUAL_size_t(0U, encode(
        &state, 10, too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_STRING("", too_small);
}

void test_eight_maximally_escaped_entities_fit_status_budget(void)
{
    backend_live_entities_t state;
    backend_live_entities_init(&state);
    for (size_t index = 0U; index < BACKEND_LIVE_ENTITY_CAPACITY; ++index) {
        char key[32];
        snprintf(key, sizeof(key), "ID:escape-%u", (unsigned)index);
        backend_dashboard_event_t event = fixture_event(
            key, "id", DETECTION_SRC_BLE_RID, "drone");
        memset(event.id, 1, sizeof(event.id) - 1U);
        memset(event.manufacturer, 2, sizeof(event.manufacturer) - 1U);
        memset(event.model, 3, sizeof(event.model) - 1U);
        memset(event.badge_label, 4, sizeof(event.badge_label) - 1U);
        memset(event.operator_id, 5, sizeof(event.operator_id) - 1U);
        event.altitude_m = DBL_MAX;
        TEST_ASSERT_TRUE(backend_live_entities_ingest(
            &state, &event, (int64_t)index));
    }
    static char output[BACKEND_USB_STATUS_MAX] = {0};
    const size_t length = encode(&state, 10, output, sizeof(output));
    TEST_ASSERT_NOT_EQUAL(0U, length);
    TEST_ASSERT_LESS_THAN_size_t(BACKEND_USB_STATUS_MAX - 3500U, length);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_ingest_deduplicates_and_preserves_best_rssi);
    BACKEND_RUN_TEST(test_reappearing_entity_starts_a_new_live_session);
    BACKEND_RUN_TEST(test_live_windows_match_backend_threat_policy);
    BACKEND_RUN_TEST(test_drone_and_meta_with_same_identity_remain_distinct);
    BACKEND_RUN_TEST(test_json_uses_native_badge_fields_and_position_contract);
    BACKEND_RUN_TEST(test_meta_is_canonical_and_zero_position_is_omitted);
    BACKEND_RUN_TEST(
        test_capacity_evicts_oldest_entity_and_encoder_fails_closed);
    BACKEND_RUN_TEST(
        test_eight_maximally_escaped_entities_fit_status_budget);
    return UNITY_END();
}
