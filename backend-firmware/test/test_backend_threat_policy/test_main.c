#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_coordinator.h"
#include "backend_health.h"
#include "backend_threat_policy.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static drone_detection_t fixture_detection(uint8_t source)
{
    drone_detection_t detection = {0};
    detection.source = source;
    detection.rssi = -60;
    detection.confidence = 0.80f;
    strcpy(detection.drone_id, "fixture");
    return detection;
}

typedef struct {
    bool accept;
    size_t attempts;
    size_t accepted_count;
    backend_detection_observation_t last_attempt;
    backend_detection_observation_t accepted[8];
} upload_sink_fixture_t;

static bool upload_sink_capture(
    void *context,
    const backend_detection_observation_t *observation)
{
    upload_sink_fixture_t *fixture = context;
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_NOT_NULL(observation);
    ++fixture->attempts;
    fixture->last_attempt = *observation;
    if (!fixture->accept) {
        return false;
    }
    TEST_ASSERT_LESS_THAN_UINT32(8U, fixture->accepted_count);
    fixture->accepted[fixture->accepted_count++] = *observation;
    return true;
}

static backend_detection_observation_t coordinator_observation(
    const char *identity,
    uint8_t slot,
    int8_t rssi)
{
    backend_detection_observation_t observation = {0};
    observation.detection = fixture_detection(DETECTION_SRC_BLE_RID);
    snprintf(observation.detection.drone_id,
             sizeof(observation.detection.drone_id),
             "%s",
             identity);
    observation.detection.scanner_slot = slot;
    observation.detection.scanner_slots_seen =
        (uint8_t)(UINT8_C(1) << slot);
    observation.detection.rssi = rssi;
    observation.timestamp_valid = true;
    observation.timestamp_epoch_ms = 1785600000100LL + slot;
    return observation;
}

void test_drone_ssid_and_remote_id_keep_their_distinct_live_windows(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t ssid = fixture_detection(DETECTION_SRC_WIFI_SSID);
    strcpy(ssid.ssid, "DJI-TEST");
    backend_threat_ingest(&state, &ssid, 1000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 16000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_INT64(15000, snapshot.drone_last_seen_age_ms);
    backend_threat_snapshot(&state, 16001, &snapshot);
    TEST_ASSERT_FALSE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot.drone_count);

    drone_detection_t rid = fixture_detection(DETECTION_SRC_BLE_RID);
    backend_threat_ingest(&state, &rid, 20000);
    backend_threat_snapshot(&state, 110000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_INT64(90000, snapshot.drone_last_seen_age_ms);
    backend_threat_snapshot(&state, 110001, &snapshot);
    TEST_ASSERT_FALSE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot.drone_count);
}

void test_remote_id_outlives_stale_ssid_and_age_uses_newest_live_evidence(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t ssid = fixture_detection(DETECTION_SRC_WIFI_SSID);
    strcpy(ssid.ssid, "Autel-SSID");
    drone_detection_t rid = fixture_detection(DETECTION_SRC_WIFI_BEACON);
    backend_threat_ingest(&state, &ssid, 1000);
    backend_threat_ingest(&state, &rid, 2000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 2000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.drone_count);

    backend_threat_snapshot(&state, 17001, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.drone_count);
    TEST_ASSERT_EQUAL_INT64(15001, snapshot.drone_last_seen_age_ms);
}

void test_duplicate_identity_refreshes_one_live_entity(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t meta = fixture_detection(DETECTION_SRC_BLE_FINGERPRINT);
    meta.ble_company_id = UINT16_C(0x0D53);
    strcpy(meta.bssid, "AA:BB:CC:DD:EE:FF");
    strcpy(meta.model, "FP:1234ABCD");

    backend_threat_ingest(&state, &meta, 1000);
    backend_threat_ingest(&state, &meta, 2000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 92000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.meta_count);
    TEST_ASSERT_EQUAL_INT64(90000, snapshot.meta_last_seen_age_ms);
    backend_threat_snapshot(&state, 92001, &snapshot);
    TEST_ASSERT_FALSE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot.meta_count);
}

void test_far_valid_drone_and_meta_are_not_display_filtered(void)
{
    backend_threat_state_t drone_state;
    backend_threat_init(&drone_state);
    drone_detection_t drone = fixture_detection(DETECTION_SRC_WIFI_SSID);
    drone.rssi = -100;
    strcpy(drone.ssid, "DJI-FAR");
    strcpy(drone.drone_id, "far-drone");
    backend_threat_ingest(&drone_state, &drone, 1000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&drone_state, 1000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.drone_count);

    backend_threat_state_t meta_state;
    backend_threat_init(&meta_state);
    drone_detection_t meta = fixture_detection(DETECTION_SRC_BLE_FINGERPRINT);
    meta.rssi = -100;
    meta.ble_company_id = UINT16_C(0x0D53);
    strcpy(meta.bssid, "11:22:33:44:55:66");
    strcpy(meta.model, "FP:89ABCDEF");
    backend_threat_ingest(&meta_state, &meta, 2000);
    backend_threat_snapshot(&meta_state, 2000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.meta_count);
}

void test_overlapping_drone_and_meta_evidence_uses_badge_drone_precedence(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t overlap =
        fixture_detection(DETECTION_SRC_BLE_FINGERPRINT);
    strcpy(overlap.manufacturer, "DJI");
    strcpy(overlap.bssid, "22:33:44:55:66:77");
    strcpy(overlap.model, "FP:ABCDEF12");
    overlap.ble_company_id = UINT16_C(0x0D53);

    backend_threat_ingest(&state, &overlap, 1000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 1000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_FALSE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.drone_count);
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot.meta_count);
}

void test_meta_requires_badge_derived_identity_evidence_and_expires_at_90s(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t weak = fixture_detection(DETECTION_SRC_BLE_FINGERPRINT);
    strcpy(weak.drone_id, "status:ble:meta");
    strcpy(weak.class_reason, "status:meta weak_meta glasses_detector");
    backend_threat_ingest(&state, &weak, 1000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 1000, &snapshot);
    TEST_ASSERT_FALSE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_INT64(-1, snapshot.meta_last_seen_age_ms);

    drone_detection_t strong = fixture_detection(DETECTION_SRC_BLE_FINGERPRINT);
    strong.ble_company_id = UINT16_C(0x0D53);
    strcpy(strong.bssid, "AA:BB:CC:DD:EE:FF");
    strcpy(strong.model, "FP:1234ABCD");
    backend_threat_ingest(&state, &strong, 2000);
    backend_threat_snapshot(&state, 92000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.meta_count);
    TEST_ASSERT_EQUAL_INT64(90000, snapshot.meta_last_seen_age_ms);
    backend_threat_snapshot(&state, 92001, &snapshot);
    TEST_ASSERT_FALSE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot.meta_count);
}

void test_duplicate_drone_does_not_inflate_count_and_backward_clock_age_is_safe(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t rid = fixture_detection(DETECTION_SRC_WIFI_DJI_IE);
    backend_threat_ingest(&state, &rid, 5000);
    backend_threat_ingest(&state, &rid, 5001);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 4000, &snapshot);
    TEST_ASSERT_TRUE(snapshot.drone_live);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot.drone_count);
    TEST_ASSERT_EQUAL_INT64(0, snapshot.drone_last_seen_age_ms);
}

void test_non_threat_detection_never_changes_liveness(void)
{
    backend_threat_state_t state;
    backend_threat_init(&state);
    drone_detection_t ordinary =
        fixture_detection(DETECTION_SRC_WIFI_AP_INVENTORY);
    strcpy(ordinary.ssid, "ordinary-ap");
    backend_threat_ingest(&state, &ordinary, 1000);

    backend_threat_snapshot_t snapshot = {0};
    backend_threat_snapshot(&state, 1000, &snapshot);
    TEST_ASSERT_FALSE(snapshot.drone_live);
    TEST_ASSERT_FALSE(snapshot.meta_live);
    TEST_ASSERT_EQUAL_INT64(-1, snapshot.drone_last_seen_age_ms);
    TEST_ASSERT_EQUAL_INT64(-1, snapshot.meta_last_seen_age_ms);
}

void test_health_maps_scanner_and_network_failures_then_applies_threat_priority(void)
{
    backend_health_inputs_t inputs = {
        .scanner_usable = {true, true},
        .wifi_connected = true,
        .backend_reachable = true,
    };
    backend_health_snapshot_t health = {0};
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_HEALTH_HEALTHY, health.level);
    TEST_ASSERT_EQUAL(BACKEND_LED_HEALTHY, health.led_state);

    inputs.scanner_usable[1] = false;
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_HEALTH_DEGRADED, health.level);
    TEST_ASSERT_EQUAL(BACKEND_LED_NETWORK_DEGRADED, health.led_state);

    inputs.threats.drone_live = true;
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE, health.led_state);
    inputs.threats.meta_live = true;
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE_META, health.led_state);

    inputs.scanner_usable[0] = false;
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_HEALTH_FATAL, health.level);
    TEST_ASSERT_EQUAL(BACKEND_LED_FATAL, health.led_state);

    inputs.scanner_usable[0] = true;
    inputs.scanner_usable[1] = true;
    inputs.fatal_runtime = true;
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_HEALTH_FATAL, health.level);
    TEST_ASSERT_EQUAL(BACKEND_LED_FATAL, health.led_state);
}

void test_wifi_or_backend_outage_is_degraded_without_stopping_local_threats(void)
{
    backend_health_inputs_t inputs = {
        .scanner_usable = {true, true},
        .wifi_connected = false,
        .backend_reachable = true,
        .threats = {.meta_live = true},
    };
    backend_health_snapshot_t health = {0};
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_HEALTH_DEGRADED, health.level);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, health.led_state);

    inputs.wifi_connected = true;
    inputs.backend_reachable = false;
    backend_health_evaluate(&inputs, &health);
    TEST_ASSERT_EQUAL(BACKEND_HEALTH_DEGRADED, health.level);
    TEST_ASSERT_EQUAL(BACKEND_LED_META, health.led_state);
}

void test_coordinator_mirrors_on_change_new_connection_and_exact_two_second_refresh(void)
{
    backend_coordinator_t coordinator;
    backend_coordinator_init(&coordinator);
    backend_led_mirror_output_t output = {0};

    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_HEALTHY, UINT8_C(0x03), 1000, &output);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x03), output.send_mask);
    TEST_ASSERT_EQUAL_UINT32(1U, output.command.generation);
    TEST_ASSERT_EQUAL_UINT32(6000U, output.command.ttl_ms);
    TEST_ASSERT_EQUAL(BACKEND_LED_HEALTHY, output.command.state);

    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_HEALTHY, UINT8_C(0x03), 2999, &output);
    TEST_ASSERT_EQUAL_HEX8(0U, output.send_mask);
    TEST_ASSERT_EQUAL_UINT32(1U, output.command.generation);
    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_HEALTHY, UINT8_C(0x03), 3000, &output);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x03), output.send_mask);
    TEST_ASSERT_EQUAL_UINT32(1U, output.command.generation);

    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_DRONE, UINT8_C(0x03), 3001, &output);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x03), output.send_mask);
    TEST_ASSERT_EQUAL_UINT32(2U, output.command.generation);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE, output.command.state);

    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_DRONE, UINT8_C(0x01), 3002, &output);
    TEST_ASSERT_EQUAL_HEX8(0U, output.send_mask);
    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_DRONE, UINT8_C(0x03), 3003, &output);
    TEST_ASSERT_EQUAL_HEX8(UINT8_C(0x02), output.send_mask);
    TEST_ASSERT_EQUAL_UINT32(2U, output.command.generation);
}

void test_coordinator_tick_uploads_at_exact_boundary_and_sink_refusal_keeps_item(void)
{
    backend_coordinator_t coordinator;
    backend_coordinator_init(&coordinator);
    upload_sink_fixture_t sink = {.accept = true};
    backend_coordinator_set_upload_sink(
        &coordinator, upload_sink_capture, &sink);
    backend_detection_observation_t first =
        coordinator_observation("first", 0U, -60);

    backend_coordinator_ingest_result_t ingested =
        backend_coordinator_ingest_detection(&coordinator, 0U, &first, 1000);
    TEST_ASSERT_TRUE(ingested.consumed);
    TEST_ASSERT_TRUE(ingested.accepted_for_upload);
    TEST_ASSERT_TRUE(ingested.update_local_threat);
    TEST_ASSERT_FALSE(ingested.retained_for_retry);
    TEST_ASSERT_EQUAL_UINT32(
        0U, backend_coordinator_tick_detections(&coordinator, 1499));
    TEST_ASSERT_EQUAL_UINT32(0U, sink.accepted_count);

    sink.accept = false;
    TEST_ASSERT_EQUAL_UINT32(
        1U, backend_coordinator_tick_detections(&coordinator, 1500));
    TEST_ASSERT_EQUAL_UINT32(1U, sink.attempts);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.accepted_count);
    TEST_ASSERT_EQUAL_MEMORY(&first, &sink.last_attempt, sizeof(first));
    TEST_ASSERT_TRUE(backend_coordinator_flow_paused(&coordinator));
}

void test_coordinator_retains_exact_backpressured_item_and_resumes_in_order(void)
{
    backend_coordinator_t coordinator;
    backend_coordinator_init(&coordinator);
    upload_sink_fixture_t sink = {.accept = true};
    backend_coordinator_set_upload_sink(
        &coordinator, upload_sink_capture, &sink);
    backend_detection_observation_t first =
        coordinator_observation("first", 0U, -60);
    backend_detection_observation_t second =
        coordinator_observation("second", 1U, -50);
    backend_detection_observation_t third =
        coordinator_observation("third", 0U, -40);

    TEST_ASSERT_TRUE(backend_coordinator_ingest_detection(
        &coordinator, 0U, &first, 1000).accepted_for_upload);
    sink.accept = false;
    TEST_ASSERT_EQUAL_UINT32(
        1U, backend_coordinator_tick_detections(&coordinator, 1500));

    backend_coordinator_ingest_result_t retained_second =
        backend_coordinator_ingest_detection(
            &coordinator, 1U, &second, 1600);
    TEST_ASSERT_TRUE(retained_second.consumed);
    TEST_ASSERT_TRUE(retained_second.retained_for_retry);
    TEST_ASSERT_FALSE(retained_second.accepted_for_upload);
    TEST_ASSERT_FALSE(retained_second.update_local_threat);

    backend_coordinator_ingest_result_t retained_third =
        backend_coordinator_ingest_detection(
            &coordinator, 0U, &third, 1700);
    TEST_ASSERT_TRUE(retained_third.consumed);
    TEST_ASSERT_TRUE(retained_third.retained_for_retry);
    backend_detection_observation_t retained = {0};
    TEST_ASSERT_TRUE(backend_coordinator_retained_detection(
        &coordinator, 1U, &retained));
    TEST_ASSERT_EQUAL_MEMORY(&second, &retained, sizeof(second));

    backend_detection_observation_t rejected =
        coordinator_observation("not-consumed", 1U, -30);
    sink.accept = true;
    backend_coordinator_ingest_result_t duplicate_slot =
        backend_coordinator_ingest_detection(
            &coordinator, 1U, &rejected, 1800);
    TEST_ASSERT_FALSE(duplicate_slot.consumed);
    TEST_ASSERT_TRUE(duplicate_slot.flow_paused);
    TEST_ASSERT_EQUAL_UINT32(1U, sink.accepted_count);
    TEST_ASSERT_EQUAL_MEMORY(&first, &sink.accepted[0], sizeof(first));
    TEST_ASSERT_TRUE(backend_coordinator_retained_detection(
        &coordinator, 1U, &retained));
    TEST_ASSERT_EQUAL_MEMORY(&second, &retained, sizeof(second));

    uint8_t retried_slot = UINT8_MAX;
    backend_detection_observation_t threat_copy = {0};
    TEST_ASSERT_TRUE(backend_coordinator_retry_one(
        &coordinator, &retried_slot, &threat_copy));
    TEST_ASSERT_EQUAL_UINT8(1U, retried_slot);
    TEST_ASSERT_EQUAL_MEMORY(&second, &threat_copy, sizeof(second));
    TEST_ASSERT_EQUAL_UINT32(1U, sink.accepted_count);
    TEST_ASSERT_EQUAL_MEMORY(&first, &sink.accepted[0], sizeof(first));
    TEST_ASSERT_TRUE(backend_coordinator_flow_paused(&coordinator));

    TEST_ASSERT_TRUE(backend_coordinator_retry_one(
        &coordinator, &retried_slot, &threat_copy));
    TEST_ASSERT_EQUAL_UINT8(0U, retried_slot);
    TEST_ASSERT_EQUAL_MEMORY(&third, &threat_copy, sizeof(third));
    TEST_ASSERT_FALSE(backend_coordinator_flow_paused(&coordinator));

    TEST_ASSERT_EQUAL_UINT32(
        1U, backend_coordinator_tick_detections(&coordinator, 2100));
    TEST_ASSERT_EQUAL_UINT32(2U, sink.accepted_count);
    TEST_ASSERT_EQUAL_MEMORY(&second, &sink.accepted[1], sizeof(second));
    TEST_ASSERT_EQUAL_UINT32(
        1U, backend_coordinator_tick_detections(&coordinator, 2200));
    TEST_ASSERT_EQUAL_UINT32(3U, sink.accepted_count);
    TEST_ASSERT_EQUAL_MEMORY(&third, &sink.accepted[2], sizeof(third));
}

void test_coordinator_generation_exhaustion_fails_closed(void)
{
    backend_coordinator_t coordinator;
    backend_coordinator_init(&coordinator);
    backend_led_mirror_output_t output = {0};
    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_HEALTHY, UINT8_C(0x03), 1000, &output);
    coordinator.generation = UINT32_MAX;

    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_DRONE, UINT8_C(0x03), 1001, &output);
    TEST_ASSERT_EQUAL_HEX8(0U, output.send_mask);
    TEST_ASSERT_TRUE(backend_coordinator_generation_exhausted(&coordinator));
    TEST_ASSERT_EQUAL(BACKEND_LED_HEALTHY, coordinator.state);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, coordinator.generation);

    backend_coordinator_update_led(
        &coordinator, BACKEND_LED_HEALTHY, UINT8_C(0x03), 3000, &output);
    TEST_ASSERT_EQUAL_HEX8(0U, output.send_mask);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_drone_ssid_and_remote_id_keep_their_distinct_live_windows);
    BACKEND_RUN_TEST(
        test_remote_id_outlives_stale_ssid_and_age_uses_newest_live_evidence);
    BACKEND_RUN_TEST(test_duplicate_identity_refreshes_one_live_entity);
    BACKEND_RUN_TEST(test_far_valid_drone_and_meta_are_not_display_filtered);
    BACKEND_RUN_TEST(
        test_overlapping_drone_and_meta_evidence_uses_badge_drone_precedence);
    BACKEND_RUN_TEST(
        test_meta_requires_badge_derived_identity_evidence_and_expires_at_90s);
    BACKEND_RUN_TEST(
        test_duplicate_drone_does_not_inflate_count_and_backward_clock_age_is_safe);
    BACKEND_RUN_TEST(test_non_threat_detection_never_changes_liveness);
    BACKEND_RUN_TEST(
        test_health_maps_scanner_and_network_failures_then_applies_threat_priority);
    BACKEND_RUN_TEST(
        test_wifi_or_backend_outage_is_degraded_without_stopping_local_threats);
    BACKEND_RUN_TEST(
        test_coordinator_mirrors_on_change_new_connection_and_exact_two_second_refresh);
    BACKEND_RUN_TEST(
        test_coordinator_tick_uploads_at_exact_boundary_and_sink_refusal_keeps_item);
    BACKEND_RUN_TEST(
        test_coordinator_retains_exact_backpressured_item_and_resumes_in_order);
    BACKEND_RUN_TEST(test_coordinator_generation_exhaustion_fails_closed);
    return UNITY_END();
}
