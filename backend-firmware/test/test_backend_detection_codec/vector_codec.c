#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_detection_assert.h"
#include "backend_detection_codec.h"
#include "backend_json_reader.h"

static drone_detection_t fixture_full_detection(void)
{
    drone_detection_t value = {0};
    strcpy(value.drone_id, "RID-\"full\\record");
    value.source = DETECTION_SRC_WIFI_PROBE_REQUEST;
    value.confidence = 0.875f;
    value.latitude = 37.7749;
    value.longitude = -122.4194;
    value.altitude_m = 123.75;
    value.heading_deg = 271.25f;
    value.speed_mps = 14.5f;
    value.vertical_speed_mps = -1.25f;
    value.rssi = -47;
    value.estimated_distance_m = 8.125;
    strcpy(value.manufacturer, "Acme \"Air\"");
    strcpy(value.model, "M\\odel\nOne");
    value.operator_lat = 37.75;
    value.operator_lon = -122.40;
    strcpy(value.operator_id, "OP\\\"42");
    value.ua_type = 2;
    value.id_type = 1;
    value.self_id_desc_type = 3;
    strcpy(value.self_id_text, "inspection\nflight");
    value.height_agl_m = 42.25;
    value.geodetic_alt_m = 130.5;
    value.h_accuracy_m = 1.75f;
    value.v_accuracy_m = 2.5f;
    value.area_count = 17;
    value.area_radius = 250;
    value.area_ceiling = 160.25;
    value.area_floor = 15.5;
    value.classification_type = 3;
    strcpy(value.ssid, "Field,Net");
    strcpy(value.bssid, "02:12:34:56:78:9A");
    value.freq_mhz = 5180;
    value.channel_width_mhz = 80;
    value.ble_company_id = 0x004c;
    value.ble_apple_type = 0x10;
    value.ble_service_uuids[0] = 0x180f;
    value.ble_service_uuids[1] = 0xfd5f;
    value.ble_service_uuids[2] = 0xfeaa;
    value.ble_service_uuids[3] = 0xffe0;
    value.ble_svc_uuid_count = 4;
    for (size_t slot = 0; slot < 2; ++slot) {
        for (size_t byte = 0; byte < 16; ++byte) {
            value.ble_service_uuids_128[slot][byte] =
                (uint8_t)(slot * 0x40U + byte);
        }
    }
    value.ble_svc_uuid_128_count = 2;
    strcpy(value.ble_svc_uuids_raw,
           "180f,fd5f,feaa,ffe0,"
           "0f0e0d0c-0b0a-0908-0706-050403020100,"
           "4f4e4d4c-4b4a-4948-4746-454443424140");
    value.ble_ad_type_count = 6;
    value.ble_payload_len = 31;
    value.ble_addr_type = 0;
    value.ble_ja3_hash = UINT32_C(0x0123abcd);
    strcpy(value.ble_name, "Ray-\"Ban\\Meta");
    strcpy(value.class_reason, "camera\nservice");
    value.ble_apple_auth[0] = 0x00;
    value.ble_apple_auth[1] = 0xa5;
    value.ble_apple_auth[2] = 0xff;
    value.ble_apple_activity = 0;
    value.ble_apple_flags = 0;
    for (size_t i = 0; i < 20; ++i) {
        value.ble_raw_mfr[i] = (uint8_t)(i * 13U);
    }
    value.ble_raw_mfr[1] = 0xff;
    value.ble_raw_mfr_len = 20;
    value.ble_adv_interval_us = INT64_C(125500);
    value.first_seen_ms = INT64_C(1785600000001);
    value.last_updated_ms = INT64_C(1785600000999);
    value.fused_confidence = 0.9375f;
    strcpy(value.probed_ssids, "FieldNet,,Guest\\Net,Cafe\"WiFi");
    value.probe_ie_hash = UINT32_C(0x00abcdef);
    value.wifi_generation = 0;
    value.wifi_auth_mode = 0;
    value.scanner_slot = BACKEND_SCANNER_SLOT_BLE;
    value.scanner_slots_seen = 3;
    value.ble_threat_kind = BLE_THREAT_KIND_PAIRING_SPAM;
    value.ble_prompt_family_mask = 0x13;
    value.ble_unique_macs = 19;
    value.ble_observation_count = 27;
    value.ble_serial_service_uuid = 0xffe0;
    value.ble_threat_evidence_mask = 0x07;
    return value;
}

void test_detection_codec_round_trips_full_record(void)
{
    drone_detection_t input = fixture_full_detection();
    backend_scanner_stamp_t stamp = {
        .sequence = 44,
        .time_valid = true,
        .observed_epoch_ms = INT64_C(1785600000123),
    };
    char line[4096] = {0};
    size_t length = backend_detection_uart_encode(
        &input, &stamp, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0, length);
    TEST_ASSERT_LESS_THAN(4096, length);
    TEST_ASSERT_EQUAL_CHAR('\0', line[length]);

    drone_detection_t output = {0};
    backend_scanner_stamp_t decoded = {0};
    TEST_ASSERT_EQUAL(BACKEND_DECODE_OK, backend_detection_uart_decode(
        line, length, BACKEND_SCANNER_SLOT_WIFI, &output, &decoded));
    drone_detection_t expected = input;
    expected.scanner_slot = BACKEND_SCANNER_SLOT_WIFI;
    expected.scanner_slots_seen = 1U << BACKEND_SCANNER_SLOT_WIFI;
    backend_assert_detection_equal(&expected, &output);
    TEST_ASSERT_EQUAL_UINT32(44, decoded.sequence);
    TEST_ASSERT_TRUE(decoded.time_valid);
    TEST_ASSERT_EQUAL_INT64(INT64_C(1785600000123),
                            decoded.observed_epoch_ms);
}

void test_detection_codec_never_returns_partial_json(void)
{
    drone_detection_t input = fixture_full_detection();
    char too_small[80];
    memset(too_small, 'X', sizeof(too_small));
    TEST_ASSERT_EQUAL_UINT(0, backend_detection_uart_encode(
        &input, NULL, too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_CHAR('\0', too_small[0]);
}

static void assert_rejects_wire_slot_and_invalid_schema_values(void)
{
    static const char wire_slot[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":0,"
        "\"conf\":0.5,\"scanner_slot\":0}";
    static const char unknown_source[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":9,"
        "\"conf\":0.5}";
    static const char invalid_confidence[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":0,"
        "\"conf\":1.01}";
    static const char missing_identity[] =
        "{\"type\":\"detection\",\"src\":0,\"conf\":0.5}";
    static const char duplicate_source[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":0,"
        "\"src\":1,\"conf\":0.5}";
    drone_detection_t output = {0};
    backend_scanner_stamp_t stamp = {0};

    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(wire_slot, sizeof(wire_slot) - 1,
            BACKEND_SCANNER_SLOT_BLE, &output, &stamp));
    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(unknown_source,
            sizeof(unknown_source) - 1, BACKEND_SCANNER_SLOT_BLE,
            &output, &stamp));
    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(invalid_confidence,
            sizeof(invalid_confidence) - 1, BACKEND_SCANNER_SLOT_BLE,
            &output, &stamp));
    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(missing_identity,
            sizeof(missing_identity) - 1, BACKEND_SCANNER_SLOT_BLE,
            &output, &stamp));
    TEST_ASSERT_EQUAL(BACKEND_DECODE_MALFORMED,
        backend_detection_uart_decode(duplicate_source,
            sizeof(duplicate_source) - 1, BACKEND_SCANNER_SLOT_BLE,
            &output, &stamp));
}

static void assert_rejects_uptime_as_epoch_and_bad_hex(void)
{
    static const char uptime[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":0,"
        "\"conf\":0.5,\"seq\":1,\"tv\":true,\"ts\":9000}";
    static const char bad_hex[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":0,"
        "\"conf\":0.5,\"ble_ja3\":\"1234ZZZZ\"}";
    drone_detection_t output = {0};
    backend_scanner_stamp_t stamp = {0};

    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(uptime, sizeof(uptime) - 1,
            BACKEND_SCANNER_SLOT_BLE, &output, &stamp));
    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(bad_hex, sizeof(bad_hex) - 1,
            BACKEND_SCANNER_SLOT_BLE, &output, &stamp));
}

static void assert_accepts_4095_and_rejects_4096_bytes(void)
{
    static const char base[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":0,"
        "\"conf\":0.5}";
    char line[4096];
    const size_t base_length = sizeof(base) - 1;
    memcpy(line, base, base_length);
    memset(line + base_length, ' ', 4095U - base_length);
    line[4095] = 'X';
    drone_detection_t output = {0};
    backend_scanner_stamp_t stamp = {0};

    TEST_ASSERT_EQUAL(BACKEND_DECODE_OK,
        backend_detection_uart_decode(line, 4095,
            BACKEND_SCANNER_SLOT_BLE, &output, &stamp));
    TEST_ASSERT_EQUAL(BACKEND_DECODE_TOO_LARGE,
        backend_detection_uart_decode(line, 4096,
            BACKEND_SCANNER_SLOT_BLE, &output, &stamp));
}

static void assert_rejects_overlong_strings_without_modifying_output(void)
{
    char json[256];
    char id[65];
    memset(id, 'A', sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    int written = snprintf(json, sizeof(json),
        "{\"type\":\"detection\",\"drone_id\":\"%s\",\"src\":0,"
        "\"conf\":0.5}", id);
    TEST_ASSERT_GREATER_THAN(0, written);
    drone_detection_t output;
    backend_scanner_stamp_t stamp;
    memset(&output, 0xa5, sizeof(output));
    memset(&stamp, 0xa5, sizeof(stamp));

    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(json, (size_t)written,
            BACKEND_SCANNER_SLOT_BLE, &output, &stamp));
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((const uint8_t *)&output)[0]);
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((const uint8_t *)&stamp)[0]);

    static const char overlong_probe[] =
        "{\"type\":\"detection\",\"drone_id\":\"x\",\"src\":5,"
        "\"conf\":0.5,\"ssid\":\"fallback\","
        "\"probed\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    TEST_ASSERT_EQUAL(BACKEND_DECODE_SCHEMA_MISMATCH,
        backend_detection_uart_decode(
            overlong_probe, sizeof(overlong_probe) - 1,
            BACKEND_SCANNER_SLOT_WIFI, &output, &stamp));
}

void test_detection_codec_rejects_hostile_and_boundary_inputs(void)
{
    assert_rejects_wire_slot_and_invalid_schema_values();
    assert_rejects_uptime_as_epoch_and_bad_hex();
    assert_accepts_4095_and_rejects_4096_bytes();
    assert_rejects_overlong_strings_without_modifying_output();
}

void test_detection_assert_helper_detects_each_field_group(void)
{
    drone_detection_t base = fixture_full_detection();
    drone_detection_t changed = base;

#define ASSERT_CHANGED(statement) do { \
    changed = base; \
    statement; \
    TEST_ASSERT_FALSE(backend_detection_equal(&base, &changed)); \
} while (0)
    ASSERT_CHANGED(changed.drone_id[0] ^= 1);
    ASSERT_CHANGED(changed.source ^= 1);
    ASSERT_CHANGED(changed.confidence += 0.1f);
    ASSERT_CHANGED(changed.latitude += 1.0);
    ASSERT_CHANGED(changed.longitude += 1.0);
    ASSERT_CHANGED(changed.altitude_m += 1.0);
    ASSERT_CHANGED(changed.heading_deg += 1.0f);
    ASSERT_CHANGED(changed.speed_mps += 1.0f);
    ASSERT_CHANGED(changed.vertical_speed_mps += 1.0f);
    ASSERT_CHANGED(changed.rssi += 1);
    ASSERT_CHANGED(changed.estimated_distance_m += 1.0);
    ASSERT_CHANGED(changed.manufacturer[0] ^= 1);
    ASSERT_CHANGED(changed.model[0] ^= 1);
    ASSERT_CHANGED(changed.operator_lat += 1.0);
    ASSERT_CHANGED(changed.operator_lon += 1.0);
    ASSERT_CHANGED(changed.operator_id[0] ^= 1);
    ASSERT_CHANGED(changed.ua_type += 1);
    ASSERT_CHANGED(changed.id_type += 1);
    ASSERT_CHANGED(changed.self_id_desc_type += 1);
    ASSERT_CHANGED(changed.self_id_text[0] ^= 1);
    ASSERT_CHANGED(changed.height_agl_m += 1.0);
    ASSERT_CHANGED(changed.geodetic_alt_m += 1.0);
    ASSERT_CHANGED(changed.h_accuracy_m += 1.0f);
    ASSERT_CHANGED(changed.v_accuracy_m += 1.0f);
    ASSERT_CHANGED(changed.area_count += 1);
    ASSERT_CHANGED(changed.area_radius += 1);
    ASSERT_CHANGED(changed.area_ceiling += 1.0);
    ASSERT_CHANGED(changed.area_floor += 1.0);
    ASSERT_CHANGED(changed.classification_type += 1);
    ASSERT_CHANGED(changed.ssid[0] ^= 1);
    ASSERT_CHANGED(changed.bssid[0] ^= 1);
    ASSERT_CHANGED(changed.freq_mhz += 5);
    ASSERT_CHANGED(changed.channel_width_mhz += 1);
    ASSERT_CHANGED(changed.ble_company_id += 1);
    ASSERT_CHANGED(changed.ble_apple_type += 1);
    ASSERT_CHANGED(changed.ble_service_uuids[0] ^= 1);
    ASSERT_CHANGED(changed.ble_service_uuids[3] ^= 1);
    ASSERT_CHANGED(changed.ble_svc_uuid_count -= 1);
    ASSERT_CHANGED(changed.ble_service_uuids_128[0][0] ^= 1);
    ASSERT_CHANGED(changed.ble_service_uuids_128[1][15] ^= 1);
    ASSERT_CHANGED(changed.ble_svc_uuid_128_count -= 1);
    ASSERT_CHANGED(changed.ble_svc_uuids_raw[0] ^= 1);
    ASSERT_CHANGED(changed.ble_ad_type_count += 1);
    ASSERT_CHANGED(changed.ble_payload_len += 1);
    ASSERT_CHANGED(changed.ble_addr_type += 1);
    ASSERT_CHANGED(changed.ble_ja3_hash ^= 1);
    ASSERT_CHANGED(changed.ble_name[0] ^= 1);
    ASSERT_CHANGED(changed.class_reason[0] ^= 1);
    ASSERT_CHANGED(changed.ble_apple_auth[0] ^= 1);
    ASSERT_CHANGED(changed.ble_apple_auth[2] ^= 1);
    ASSERT_CHANGED(changed.ble_apple_activity += 1);
    ASSERT_CHANGED(changed.ble_apple_flags += 1);
    ASSERT_CHANGED(changed.ble_raw_mfr[0] ^= 1);
    ASSERT_CHANGED(changed.ble_raw_mfr[19] ^= 1);
    ASSERT_CHANGED(changed.ble_raw_mfr_len -= 1);
    ASSERT_CHANGED(changed.ble_adv_interval_us += 1);
    ASSERT_CHANGED(changed.first_seen_ms += 1);
    ASSERT_CHANGED(changed.last_updated_ms += 1);
    ASSERT_CHANGED(changed.fused_confidence -= 0.1f);
    ASSERT_CHANGED(changed.probed_ssids[0] ^= 1);
    ASSERT_CHANGED(changed.probe_ie_hash ^= 1);
    ASSERT_CHANGED(changed.wifi_generation += 1);
    ASSERT_CHANGED(changed.wifi_auth_mode += 1);
    ASSERT_CHANGED(changed.scanner_slot ^= 1);
    ASSERT_CHANGED(changed.scanner_slots_seen ^= 1);
    ASSERT_CHANGED(changed.ble_threat_kind += 1);
    ASSERT_CHANGED(changed.ble_prompt_family_mask ^= 1);
    ASSERT_CHANGED(changed.ble_unique_macs += 1);
    ASSERT_CHANGED(changed.ble_observation_count += 1);
    ASSERT_CHANGED(changed.ble_serial_service_uuid ^= 1);
    ASSERT_CHANGED(changed.ble_threat_evidence_mask ^= 1);
#undef ASSERT_CHANGED
}

void test_detection_codec_matches_independent_http_mapping(void)
{
    drone_detection_t expected = fixture_full_detection();
    static const char json[] =
        "{"
        "\"drone_id\":\"RID-\\\"full\\\\record\","
        "\"source\":\"wifi_probe_request\",\"confidence\":0.875,"
        "\"fused_confidence\":0.9375,"
        "\"latitude\":37.7749,\"longitude\":-122.4194,"
        "\"altitude_m\":123.75,\"heading_deg\":271.25,"
        "\"speed_mps\":14.5,\"vertical_speed_mps\":-1.25,"
        "\"rssi\":-47,\"estimated_distance_m\":8.125,"
        "\"manufacturer\":\"Acme \\\"Air\\\"\","
        "\"model\":\"M\\\\odel\\nOne\","
        "\"operator_lat\":37.75,\"operator_lon\":-122.4,"
        "\"operator_id\":\"OP\\\\\\\"42\","
        "\"ua_type\":2,\"id_type\":1,\"self_id_desc_type\":3,"
        "\"self_id_text\":\"inspection\\nflight\","
        "\"height_agl_m\":42.25,\"geodetic_alt_m\":130.5,"
        "\"h_accuracy_m\":1.75,\"v_accuracy_m\":2.5,"
        "\"area_count\":17,\"area_radius\":250,"
        "\"area_ceiling\":160.25,\"area_floor\":15.5,"
        "\"classification_type\":3,\"ssid\":\"Field,Net\","
        "\"bssid\":\"02:12:34:56:78:9A\","
        "\"freq_mhz\":5180,\"channel\":36,\"channel_width_mhz\":80,"
        "\"auth_m\":0,\"wifi_generation\":0,"
        "\"probed_ssids\":[\"FieldNet\",\"Guest\\\\Net\",\"Cafe\\\"WiFi\"],"
        "\"ie_hash\":\"00abcdef\",\"ble_company_id\":76,"
        "\"ble_apple_type\":16,\"ble_ad_type_count\":6,"
        "\"ble_payload_len\":31,\"ble_ja3\":\"0123abcd\","
        "\"ble_name\":\"Ray-\\\"Ban\\\\Meta\","
        "\"class_reason\":\"camera\\nservice\","
        "\"ble_apple_auth\":\"00a5ff\",\"ble_activity\":0,"
        "\"ble_apple_flags\":0,"
        "\"ble_raw_mfr\":\"00ff1a2734414e5b6875828f9ca9b6c3d0ddeaf7\","
        "\"ble_adv_interval\":125.5,"
        "\"ble_svc_uuids\":\"180f,fd5f,feaa,ffe0,"
        "0f0e0d0c-0b0a-0908-0706-050403020100,"
        "4f4e4d4c-4b4a-4948-4746-454443424140\","
        "\"first_seen_ms\":1785600000001,"
        "\"last_updated_ms\":1785600000999,"
        "\"scanner_slot\":0,\"scanner_slots_seen\":3,"
        "\"ble_threat_kind\":1,\"ble_prompt_family_mask\":19,"
        "\"ble_unique_macs\":19,\"ble_observation_count\":27,"
        "\"ble_serial_service_uuid\":65504,"
        "\"ble_threat_evidence_mask\":7}"
    ;
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OK, backend_json_parse(
        json, sizeof(json) - 1, tokens, BACKEND_JSON_MAX_TOKENS,
        &token_count));
    backend_assert_detection_json_equal(
        &expected, json, tokens, token_count, 0);

    /* Force the independent oracle down the array fallback path. The same
     * hand-written literal proves all four 16-bit UUIDs precede both stored-
     * little-endian 128-bit UUIDs in canonical reversed byte order. */
    expected.ble_svc_uuids_raw[0] = '\0';
    backend_assert_detection_json_equal(
        &expected, json, tokens, token_count, 0);
}

void test_detection_frequency_channel_boundaries(void)
{
    static const struct {
        int32_t frequency;
        int expected_channel;
    } cases[] = {
        {2412, 1}, {2472, 13}, {2484, 14}, {5180, 36}, {2413, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        TEST_ASSERT_EQUAL_INT(cases[i].expected_channel,
                              backend_detection_wifi_channel(
                                  cases[i].frequency));
    }
    TEST_ASSERT_EQUAL_INT(0, backend_detection_wifi_channel(5896));

    drone_detection_t ble = fixture_full_detection();
    ble.source = DETECTION_SRC_BLE_FINGERPRINT;
    ble.ble_addr_type = 0;
    static const char json[] =
        "{\"drone_id\":\"RID-\\\"full\\\\record\","
        "\"source\":\"ble_fingerprint\",\"confidence\":0.875,"
        "\"fused_confidence\":0.9375,"
        "\"latitude\":37.7749,\"longitude\":-122.4194,"
        "\"altitude_m\":123.75,\"heading_deg\":271.25,"
        "\"speed_mps\":14.5,\"vertical_speed_mps\":-1.25,"
        "\"rssi\":-47,\"estimated_distance_m\":8.125,"
        "\"manufacturer\":\"Acme \\\"Air\\\"\","
        "\"model\":\"M\\\\odel\\nOne\",\"operator_lat\":37.75,"
        "\"operator_lon\":-122.4,\"operator_id\":\"OP\\\\\\\"42\","
        "\"ua_type\":2,\"id_type\":1,\"self_id_desc_type\":3,"
        "\"self_id_text\":\"inspection\\nflight\","
        "\"height_agl_m\":42.25,\"geodetic_alt_m\":130.5,"
        "\"h_accuracy_m\":1.75,\"v_accuracy_m\":2.5,"
        "\"area_count\":17,\"area_radius\":250,"
        "\"area_ceiling\":160.25,\"area_floor\":15.5,"
        "\"classification_type\":3,\"ssid\":\"Field,Net\","
        "\"bssid\":\"02:12:34:56:78:9A\",\"freq_mhz\":5180,"
        "\"channel\":36,\"channel_width_mhz\":80,"
        "\"ble_company_id\":76,\"ble_apple_type\":16,"
        "\"ble_ad_type_count\":6,\"ble_payload_len\":31,"
        "\"ble_addr_type\":0,\"ble_ja3\":\"0123abcd\","
        "\"ble_name\":\"Ray-\\\"Ban\\\\Meta\","
        "\"class_reason\":\"camera\\nservice\","
        "\"ble_apple_auth\":\"00a5ff\",\"ble_activity\":0,"
        "\"ble_apple_flags\":0,"
        "\"ble_raw_mfr\":\"00ff1a2734414e5b6875828f9ca9b6c3d0ddeaf7\","
        "\"ble_adv_interval\":125.5,"
        "\"ble_svc_uuids\":\"180f,fd5f,feaa,ffe0,"
        "0f0e0d0c-0b0a-0908-0706-050403020100,"
        "4f4e4d4c-4b4a-4948-4746-454443424140\","
        "\"ie_hash\":\"00abcdef\","
        "\"first_seen_ms\":1785600000001,\"last_updated_ms\":1785600000999,"
        "\"scanner_slot\":0,\"scanner_slots_seen\":3,"
        "\"ble_threat_kind\":1,\"ble_prompt_family_mask\":19,"
        "\"ble_unique_macs\":19,\"ble_observation_count\":27,"
        "\"ble_serial_service_uuid\":65504,\"ble_threat_evidence_mask\":7}"
    ;
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OK, backend_json_parse(
        json, sizeof(json) - 1, tokens, BACKEND_JSON_MAX_TOKENS,
        &token_count));
    backend_assert_detection_json_equal(
        &ble, json, tokens, token_count, 0);
}
