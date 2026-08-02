#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_detection_assert.h"
#include "backend_identity.h"
#include "backend_json_reader.h"
#include "backend_upload_batch.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
#define EXPECTED_PRODUCT_FAMILY "s3_fullsize"
#define EXPECTED_LED_CAPABILITY "rgb_led"
#else
#define EXPECTED_PRODUCT_FAMILY "badge_lite"
#define EXPECTED_LED_CAPABILITY "yellow_led"
#endif

static const char *const EXPECTED_UPLINK_CAPABILITIES[] = {
    "display_none",
    EXPECTED_LED_CAPABILITY,
    "scanner_uart",
    "http_uplink",
    "config_ap",
    "remote_ota",
    "uart_relay_ota",
};

static const char *const EXPECTED_SCANNER_CAPABILITIES[] = {
    "display_none",
    EXPECTED_LED_CAPABILITY,
    "ble_wifi_sensing",
    "uart_control",
    "uart_ota",
    "remote_ota_via_uplink",
};

static backend_scanner_status_t fixture_scanner(
    uint32_t sequence, uint32_t boot_id, const char *mac,
    backend_scan_profile_t profile)
{
    backend_scanner_status_t status = {
        .schema = BACKEND_SCANNER_STATUS_SCHEMA,
        .sequence = sequence,
        .boot_id = boot_id,
        .profile = profile,
        .role_generation = 4U,
        .role_acked = true,
        .command_ingress = true,
        .ble_healthy = profile == BACKEND_SCAN_PROFILE_BLE_PRIMARY,
        .wifi_healthy = profile == BACKEND_SCAN_PROFILE_WIFI_PRIMARY,
        .uptime_ms = UINT64_C(9000),
    };
    strcpy(status.mac, mac);
    strcpy(status.target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(status.project, FOF_BACKEND_SCANNER_PROJECT);
    strcpy(status.hardware, FOF_BACKEND_HARDWARE);
    strcpy(status.version, "0.1.0-scanner-skew");
    strcpy(status.ota_state, "idle");
    strcpy(status.rollback_state, "valid");
    return status;
}

static backend_batch_context_t fixture_batch_context(void)
{
    backend_batch_context_t context = {
        .capability_count = 7U,
        .has_device_location = true,
        .device_lat = 36.1699,
        .device_lon = -115.1398,
        .device_alt = 610.5,
        .scanner_present = {true, true},
        .clock_valid = true,
        .epoch_ms = INT64_C(1785600000999),
        .wifi_rssi = -53,
        .ap_active = true,
        .config_generation = 9U,
        .command_success_count = 17U,
        .command_failure_count = 2U,
        .uptime_ms = UINT64_C(9876543210),
        .led_state = BACKEND_LED_DRONE_META,
        .upload_queue = {
            .depth_batches = 7U,
            .capacity_batches = BACKEND_UPLOAD_FIFO_CAPACITY,
            .overflow_dropped_batches = 2U,
            .quarantined_batches = 1U,
        },
        .upload = {
            .ok = 11U,
            .failed = 3U,
            .retry_count = 4U,
            .has_last_success_age = true,
            .last_success_age_s = 8U,
        },
        .sequence = 41U,
    };
    strcpy(context.device_id, "uplink_CB77A4");
    strcpy(context.product_family, EXPECTED_PRODUCT_FAMILY);
    strcpy(context.firmware_line, "backend");
    strcpy(context.component, "uplink");
    strcpy(context.firmware_version, FOF_VERSION_BACKEND);
    strcpy(context.firmware_target, FOF_BACKEND_UPLINK_TARGET);
    strcpy(context.app_project, FOF_BACKEND_UPLINK_PROJECT);
    strcpy(context.hardware_type, FOF_BACKEND_HARDWARE);
    strcpy(context.hardware_mac, "A4:CF:12:CB:77:A4");
    strcpy(context.node_name, "Roof backend sensor");
    for (size_t index = 0U;
         index < sizeof(EXPECTED_UPLINK_CAPABILITIES) /
                     sizeof(EXPECTED_UPLINK_CAPABILITIES[0]);
         ++index) {
        strcpy(context.capabilities[index], EXPECTED_UPLINK_CAPABILITIES[index]);
    }
    strcpy(context.wifi_ssid, "FoF Lab");
    context.scanners[0] = fixture_scanner(
        12U, UINT32_C(0x12345678), "AA:BB:CC:DD:EE:01",
        BACKEND_SCAN_PROFILE_BLE_PRIMARY);
    context.scanners[1] = fixture_scanner(
        19U, UINT32_C(0x87654321), "AA:BB:CC:DD:EE:02",
        BACKEND_SCAN_PROFILE_HYBRID_FAILOVER);
    context.scanners[1].ble_healthy = false;
    context.scanners[1].wifi_healthy = true;
    return context;
}

static drone_detection_t fixture_full_detection(void)
{
    drone_detection_t value = {0};
    strcpy(value.drone_id, "RID-\"full\\record");
    value.source = DETECTION_SRC_WIFI_PROBE_REQUEST;
    value.confidence = 0.875f;
    value.fused_confidence = 0.9375f;
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
    value.ua_type = 2U;
    value.id_type = 1U;
    value.self_id_desc_type = 3U;
    strcpy(value.self_id_text, "inspection\nflight");
    value.height_agl_m = 42.25;
    value.geodetic_alt_m = 130.5;
    value.h_accuracy_m = 1.75f;
    value.v_accuracy_m = 2.5f;
    value.area_count = 17U;
    value.area_radius = 250U;
    value.area_ceiling = 160.25;
    value.area_floor = 15.5;
    value.classification_type = 3U;
    strcpy(value.ssid, "Field,Net");
    strcpy(value.bssid, "02:12:34:56:78:9A");
    value.freq_mhz = 2437;
    value.channel_width_mhz = 80;
    value.wifi_auth_mode = 0U;
    value.wifi_generation = 0U;
    strcpy(value.probed_ssids, "FieldNet,,Guest\\Net,Cafe\"WiFi");
    value.probe_ie_hash = UINT32_C(0x00abcdef);
    value.ble_company_id = 0x004cU;
    value.ble_apple_type = 0x10U;
    value.ble_service_uuids[0] = 0x180fU;
    value.ble_service_uuids[1] = 0xfd5fU;
    value.ble_svc_uuid_count = 2U;
    for (size_t byte = 0; byte < 16U; ++byte) {
        value.ble_service_uuids_128[0][byte] = (uint8_t)byte;
    }
    value.ble_svc_uuid_128_count = 1U;
    strcpy(value.ble_svc_uuids_raw,
           "180f,fd5f,0f0e0d0c-0b0a-0908-0706-050403020100");
    value.ble_ad_type_count = 6U;
    value.ble_payload_len = 31U;
    value.ble_addr_type = 0U;
    value.ble_ja3_hash = UINT32_C(0x0123abcd);
    strcpy(value.ble_name, "Ray-\"Ban\\Meta");
    strcpy(value.class_reason, "camera\nservice");
    value.ble_apple_auth[1] = 0xa5U;
    value.ble_apple_auth[2] = 0xffU;
    value.ble_apple_activity = 0U;
    value.ble_apple_flags = 0U;
    for (size_t i = 0; i < 20U; ++i) {
        value.ble_raw_mfr[i] = (uint8_t)(i * 13U);
    }
    value.ble_raw_mfr_len = 20U;
    value.ble_adv_interval_us = INT64_C(125500);
    value.first_seen_ms = INT64_C(1785600000001);
    value.last_updated_ms = INT64_C(1785600000999);
    value.scanner_slot = BACKEND_SCANNER_SLOT_BLE;
    value.scanner_slots_seen = 3U;
    value.ble_threat_kind = BLE_THREAT_KIND_PAIRING_SPAM;
    value.ble_prompt_family_mask = 0x13U;
    value.ble_unique_macs = 19U;
    value.ble_observation_count = 27U;
    value.ble_serial_service_uuid = 0xffe0U;
    value.ble_threat_evidence_mask = 0x07U;
    return value;
}

static size_t require_key(const char *json,
                          const backend_json_token_t *tokens,
                          size_t token_count,
                          size_t object_index,
                          const char *key)
{
    size_t value_index = 0U;
    TEST_ASSERT_TRUE_MESSAGE(backend_json_object_find(
        json, tokens, token_count, object_index, key, &value_index), key);
    return value_index;
}

static void assert_key_absent(const char *json,
                              const backend_json_token_t *tokens,
                              size_t token_count,
                              size_t object_index,
                              const char *key)
{
    size_t ignored = 0U;
    TEST_ASSERT_FALSE_MESSAGE(backend_json_object_find(
        json, tokens, token_count, object_index, key, &ignored), key);
}

static void assert_string_key(const char *json,
                              const backend_json_token_t *tokens,
                              size_t token_count,
                              size_t object_index,
                              const char *key,
                              const char *expected)
{
    char actual[192];
    size_t index = require_key(json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_copy_string(
        json, &tokens[index], actual, sizeof(actual)));
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

static void assert_i64_key(const char *json,
                           const backend_json_token_t *tokens,
                           size_t token_count,
                           size_t object_index,
                           const char *key,
                           int64_t expected)
{
    int64_t actual = 0;
    size_t index = require_key(json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_get_i64(json, &tokens[index], &actual));
    TEST_ASSERT_EQUAL_INT64(expected, actual);
}

static void assert_bool_key(const char *json,
                            const backend_json_token_t *tokens,
                            size_t token_count,
                            size_t object_index,
                            const char *key,
                            bool expected)
{
    bool actual = !expected;
    size_t index = require_key(json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_get_bool(json, &tokens[index], &actual));
    TEST_ASSERT_EQUAL(expected, actual);
}

static void assert_double_key(const char *json,
                              const backend_json_token_t *tokens,
                              size_t token_count,
                              size_t object_index,
                              const char *key,
                              double expected)
{
    double actual = 0.0;
    size_t index = require_key(json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_get_double(json, &tokens[index], &actual));
    TEST_ASSERT_DOUBLE_WITHIN(1.0e-9, expected, actual);
}

static bool extract_array_object(const char *json, const char *array_key,
                                 size_t wanted, char *output,
                                 size_t output_capacity, size_t *out_length)
{
    char marker[64];
    int written = snprintf(marker, sizeof(marker), "\"%s\":[", array_key);
    if (written <= 0 || (size_t)written >= sizeof(marker)) {
        return false;
    }
    const char *cursor = strstr(json, marker);
    if (!cursor) {
        return false;
    }
    cursor += (size_t)written;
    size_t found = 0U;
    while (*cursor && *cursor != ']') {
        while (*cursor == ',' || *cursor == ' ' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor != '{') {
            return false;
        }
        const char *start = cursor;
        unsigned depth = 0U;
        bool in_string = false;
        bool escaped = false;
        do {
            char ch = *cursor++;
            if (ch == '\0') {
                return false;
            }
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
            } else if (ch == '"') {
                in_string = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
            }
        } while (depth > 0U);
        if (found++ == wanted) {
            size_t length = (size_t)(cursor - start);
            if (length + 1U > output_capacity) {
                return false;
            }
            memcpy(output, start, length);
            output[length] = '\0';
            *out_length = length;
            return true;
        }
    }
    return false;
}

static size_t parse_object(const char *json, size_t length,
                           backend_json_token_t *tokens, size_t capacity)
{
    size_t token_count = 0U;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OK, backend_json_parse(
        json, length, tokens, capacity, &token_count));
    TEST_ASSERT_GREATER_THAN(0U, token_count);
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[0].kind);
    return token_count;
}

static uint32_t independent_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ?
                UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

static void assert_string_array(const char *json,
                                const backend_json_token_t *tokens,
                                size_t count, size_t object,
                                const char *key,
                                const char *const *expected,
                                size_t expected_count)
{
    size_t array = require_key(json, tokens, count, object, key);
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[array].kind);
    TEST_ASSERT_EQUAL_UINT16(expected_count, tokens[array].child_count);
    for (size_t index = 0U; index < expected_count; ++index) {
        char actual[41];
        TEST_ASSERT_TRUE(backend_json_copy_string(
            json, &tokens[array + index + 1U], actual, sizeof(actual)));
        TEST_ASSERT_EQUAL_STRING(expected[index], actual);
    }
}

void test_empty_batch_has_exact_identity_health_and_scanner_bridge(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    TEST_ASSERT_TRUE(builder.active);
    TEST_ASSERT_FALSE(builder.failed);

    backend_upload_batch_t batch;
    memset(&batch, 0xa5, sizeof(batch));
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));
    TEST_ASSERT_EQUAL_UINT32(41U, batch.sequence);
    TEST_ASSERT_EQUAL_UINT16(0U, batch.item_count);
    TEST_ASSERT_EQUAL_UINT32(
        independent_crc32(batch.json, batch.json_len), batch.json_crc32);
    TEST_ASSERT_EQUAL_CHAR('}', batch.json[batch.json_len - 1U]);
    TEST_ASSERT_EQUAL_CHAR('\0', batch.json[batch.json_len]);

    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t count = parse_object(
        batch.json, batch.json_len, tokens, BACKEND_JSON_MAX_TOKENS);
    TEST_ASSERT_EQUAL_UINT16(46U, tokens[0].child_count);
    static const char *const top_level_keys[] = {
        "device_id", "product_family", "firmware_line", "component",
        "firmware_version", "firmware_target", "app_project", "hardware_type",
        "hardware_mac", "node_name", "capabilities",
        "device_lat", "device_lon", "device_alt", "timestamp", "scanners",
        "wifi_ssid", "wifi_rssi", "led_state", "upload_queue", "upload",
        "health", "detections",
    };
    for (size_t i = 0U;
         i < sizeof(top_level_keys) / sizeof(top_level_keys[0]); ++i) {
        (void)require_key(batch.json, tokens, count, 0U, top_level_keys[i]);
    }
    assert_string_key(batch.json, tokens, count, 0U,
                      "device_id", "uplink_CB77A4");
    assert_string_key(batch.json, tokens, count, 0U,
                      "product_family", EXPECTED_PRODUCT_FAMILY);
    assert_string_key(batch.json, tokens, count, 0U,
                      "firmware_line", "backend");
    assert_string_key(batch.json, tokens, count, 0U,
                      "component", "uplink");
    assert_string_key(batch.json, tokens, count, 0U,
                      "firmware_version", FOF_VERSION_BACKEND);
    assert_string_key(batch.json, tokens, count, 0U,
                      "firmware_target", FOF_BACKEND_UPLINK_TARGET);
    assert_string_key(batch.json, tokens, count, 0U,
                      "app_project", FOF_BACKEND_UPLINK_PROJECT);
    assert_string_key(batch.json, tokens, count, 0U,
                      "hardware_type", FOF_BACKEND_HARDWARE);
    assert_string_key(batch.json, tokens, count, 0U,
                      "hardware_mac", "A4:CF:12:CB:77:A4");
    assert_string_key(batch.json, tokens, count, 0U,
                      "node_name", "Roof backend sensor");
    assert_string_key(batch.json, tokens, count, 0U,
                      "wifi_ssid", "FoF Lab");
    assert_i64_key(batch.json, tokens, count, 0U, "wifi_rssi", -53);
    assert_string_key(batch.json, tokens, count, 0U,
                      "led_state", "drone_meta");
    assert_double_key(batch.json, tokens, count, 0U,
                      "device_lat", 36.1699);
    assert_double_key(batch.json, tokens, count, 0U,
                      "device_lon", -115.1398);
    assert_double_key(batch.json, tokens, count, 0U,
                      "device_alt", 610.5);
    assert_i64_key(batch.json, tokens, count, 0U,
                   "timestamp", INT64_C(1785600000));
    assert_key_absent(batch.json, tokens, count, 0U, "sequence");
    assert_key_absent(batch.json, tokens, count, 0U, "firmware_name");
    assert_key_absent(batch.json, tokens, count, 0U, "hardware_id");
    assert_key_absent(batch.json, tokens, count, 0U, "ap_active");
    assert_key_absent(batch.json, tokens, count, 0U, "config_generation");

    assert_string_array(batch.json, tokens, count, 0U, "capabilities",
                        EXPECTED_UPLINK_CAPABILITIES,
                        sizeof(EXPECTED_UPLINK_CAPABILITIES) /
                            sizeof(EXPECTED_UPLINK_CAPABILITIES[0]));

    size_t scanners = require_key(batch.json, tokens, count, 0U, "scanners");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[scanners].kind);
    TEST_ASSERT_EQUAL_UINT16(2U, tokens[scanners].child_count);
    size_t detections = require_key(
        batch.json, tokens, count, 0U, "detections");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[detections].kind);
    TEST_ASSERT_EQUAL_UINT16(0U, tokens[detections].child_count);

    size_t queue = require_key(
        batch.json, tokens, count, 0U, "upload_queue");
    TEST_ASSERT_EQUAL_UINT16(8U, tokens[queue].child_count);
    assert_i64_key(batch.json, tokens, count, queue, "depth_batches", 7);
    assert_i64_key(batch.json, tokens, count, queue, "capacity_batches", 512);
    assert_i64_key(batch.json, tokens, count, queue,
                   "overflow_dropped_batches", 2);
    assert_i64_key(batch.json, tokens, count, queue,
                   "quarantined_batches", 1);
    size_t upload = require_key(batch.json, tokens, count, 0U, "upload");
    TEST_ASSERT_EQUAL_UINT16(8U, tokens[upload].child_count);
    assert_i64_key(batch.json, tokens, count, upload, "ok", 11);
    assert_i64_key(batch.json, tokens, count, upload, "failed", 3);
    assert_i64_key(batch.json, tokens, count, upload, "retry_count", 4);
    assert_i64_key(batch.json, tokens, count, upload,
                   "last_success_age_s", 8);

    size_t health = require_key(batch.json, tokens, count, 0U, "health");
    TEST_ASSERT_EQUAL_UINT16(14U, tokens[health].child_count);
    assert_bool_key(batch.json, tokens, count, health, "clock_valid", true);
    assert_i64_key(batch.json, tokens, count, health, "epoch_ms",
                   INT64_C(1785600000999));
    assert_bool_key(batch.json, tokens, count, health, "ap_active", true);
    assert_i64_key(batch.json, tokens, count, health,
                   "config_generation", 9);
    assert_i64_key(batch.json, tokens, count, health,
                   "command_success_count", 17);
    assert_i64_key(batch.json, tokens, count, health,
                   "command_failure_count", 2);
    assert_i64_key(batch.json, tokens, count, health,
                   "uptime_ms", INT64_C(9876543210));

    char scanner_json[2048];
    size_t scanner_length = 0U;
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "scanners", 0U, scanner_json, sizeof(scanner_json),
        &scanner_length));
    backend_json_token_t scanner_tokens[96];
    size_t scanner_count = parse_object(
        scanner_json, scanner_length, scanner_tokens, 96U);
    TEST_ASSERT_EQUAL_UINT16(44U, scanner_tokens[0].child_count);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "uart", "ble");
    assert_i64_key(scanner_json, scanner_tokens, scanner_count, 0U,
                   "slot", 0);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "product_family", EXPECTED_PRODUCT_FAMILY);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "firmware_line", "backend");
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "component", "scanner");
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "firmware_target", FOF_BACKEND_SCANNER_TARGET);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "app_project", FOF_BACKEND_SCANNER_PROJECT);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "hardware_type", FOF_BACKEND_HARDWARE);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "firmware_version", "0.1.0-scanner-skew");
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "mac", "AA:BB:CC:DD:EE:01");
    assert_i64_key(scanner_json, scanner_tokens, scanner_count, 0U,
                   "boot_id", UINT32_C(0x12345678));
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "profile", "ble_primary");
    assert_i64_key(scanner_json, scanner_tokens, scanner_count, 0U,
                   "status_sequence", 12);
    assert_i64_key(scanner_json, scanner_tokens, scanner_count, 0U,
                   "role_generation", 4);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "role_acked", true);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "command_ingress", true);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "radio_healthy", true);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "ble_healthy", true);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "wifi_healthy", false);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "ota_state", "idle");
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "rollback_state", "valid");
    assert_string_array(scanner_json, scanner_tokens, scanner_count, 0U,
                        "capabilities", EXPECTED_SCANNER_CAPABILITIES,
                        sizeof(EXPECTED_SCANNER_CAPABILITIES) /
                            sizeof(EXPECTED_SCANNER_CAPABILITIES[0]));
    static const char *const forbidden[] = {
        "target", "project", "hardware", "version", "sequence",
        "uart_slot", "flow_paused", "rx_errors", "tx_drops", "uptime_ms",
    };
    for (size_t i = 0U; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        assert_key_absent(scanner_json, scanner_tokens, scanner_count,
                          0U, forbidden[i]);
    }

    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "scanners", 1U, scanner_json, sizeof(scanner_json),
        &scanner_length));
    scanner_count = parse_object(
        scanner_json, scanner_length, scanner_tokens, 96U);
    TEST_ASSERT_EQUAL_UINT16(44U, scanner_tokens[0].child_count);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "uart", "wifi");
    assert_i64_key(scanner_json, scanner_tokens, scanner_count, 0U,
                   "slot", 1);
    assert_string_key(scanner_json, scanner_tokens, scanner_count, 0U,
                      "profile", "hybrid_failover");
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "radio_healthy", false);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "ble_healthy", false);
    assert_bool_key(scanner_json, scanner_tokens, scanner_count, 0U,
                    "wifi_healthy", true);
}

void test_detection_parity_uses_canonical_keys_and_copies_input(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t observation = {
        .detection = fixture_full_detection(),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1785600000123),
    };
    drone_detection_t expected = observation.detection;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &observation, 1001));
    memset(&observation, 0, sizeof(observation));
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));

    char detection_json[4097];
    size_t detection_length = 0U;
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 0U, detection_json,
        sizeof(detection_json), &detection_length));
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t count = parse_object(
        detection_json, detection_length, tokens, BACKEND_JSON_MAX_TOKENS);
    backend_assert_detection_json_equal(
        &expected, detection_json, tokens, count, 0U);
    assert_i64_key(detection_json, tokens, count, 0U,
                   "timestamp", INT64_C(1785600000123));
    static const char *const wrong_keys[] = {
        "frequency_mhz", "wifi_auth_mode", "probe_ie_hash",
        "ble_ja3_hash", "ble_adv_interval_us", "ble_service_uuids",
    };
    for (size_t i = 0U; i < sizeof(wrong_keys) / sizeof(wrong_keys[0]); ++i) {
        assert_key_absent(detection_json, tokens, count, 0U, wrong_keys[i]);
    }
}

static drone_detection_t fixture_minimal_detection(
    const char *id, uint8_t source)
{
    drone_detection_t value = {0};
    strcpy(value.drone_id, id);
    value.source = source;
    value.confidence = 0.5f;
    value.wifi_auth_mode = UINT8_MAX;
    return value;
}

void test_handwritten_wifi_and_ble_zero_absence_rules_survive(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);

    backend_detection_observation_t wifi = {
        .detection = fixture_minimal_detection(
            "WIFI:backend", DETECTION_SRC_WIFI_PROBE_REQUEST),
        .timestamp_valid = false,
        .timestamp_epoch_ms = 9999,
    };
    wifi.detection.freq_mhz = 2437;
    wifi.detection.wifi_auth_mode = 0U;
    wifi.detection.wifi_generation = 0U;
    strcpy(wifi.detection.ssid, "Fallback");
    strcpy(wifi.detection.probed_ssids, "Alpha,,Beta");

    backend_detection_observation_t ble = {
        .detection = fixture_minimal_detection(
            "BLE:backend", DETECTION_SRC_BLE_FINGERPRINT),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1699999999999),
    };
    ble.detection.ble_company_id = 0x004cU;
    ble.detection.ble_addr_type = 0U;
    ble.detection.ble_ja3_hash = UINT32_C(0x0123abcd);
    ble.detection.ble_apple_auth[1] = 0xffU;
    ble.detection.ble_apple_activity = 0U;
    ble.detection.ble_apple_flags = 0U;
    ble.detection.ble_raw_mfr[0] = 0x4cU;
    ble.detection.ble_raw_mfr[1] = 0x00U;
    ble.detection.ble_raw_mfr[2] = 0x10U;
    ble.detection.ble_raw_mfr_len = 3U;
    ble.detection.ble_adv_interval_us = INT64_C(125500);
    ble.detection.ble_service_uuids[0] = 0xfd5fU;
    ble.detection.ble_svc_uuid_count = 1U;
    for (size_t byte = 0U; byte < 16U; ++byte) {
        ble.detection.ble_service_uuids_128[0][byte] = (uint8_t)byte;
    }
    ble.detection.ble_svc_uuid_128_count = 1U;

    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &wifi, 1001));
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &ble, 1002));
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));

    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    char item[4097];
    size_t item_length = 0U;
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 0U, item, sizeof(item), &item_length));
    size_t count = parse_object(item, item_length, tokens,
                                BACKEND_JSON_MAX_TOKENS);
    backend_assert_detection_json_equal(
        &wifi.detection, item, tokens, count, 0U);
    assert_i64_key(item, tokens, count, 0U, "freq_mhz", 2437);
    assert_i64_key(item, tokens, count, 0U, "channel", 6);
    assert_i64_key(item, tokens, count, 0U, "auth_m", 0);
    assert_i64_key(item, tokens, count, 0U, "wifi_generation", 0);
    assert_key_absent(item, tokens, count, 0U, "timestamp");
    assert_key_absent(item, tokens, count, 0U, "wifi_auth_mode");
    assert_key_absent(item, tokens, count, 0U, "probe_ie_hash");

    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 1U, item, sizeof(item), &item_length));
    count = parse_object(item, item_length, tokens, BACKEND_JSON_MAX_TOKENS);
    backend_assert_detection_json_equal(
        &ble.detection, item, tokens, count, 0U);
    assert_key_absent(item, tokens, count, 0U, "timestamp");
    assert_i64_key(item, tokens, count, 0U, "ble_activity", 0);
    assert_i64_key(item, tokens, count, 0U, "ble_apple_flags", 0);
    assert_string_key(item, tokens, count, 0U, "ble_raw_mfr", "4c0010");
    assert_string_key(item, tokens, count, 0U, "ble_svc_uuids",
                      "fd5f,0f0e0d0c-0b0a-0908-0706-050403020100");
    assert_key_absent(item, tokens, count, 0U, "ble_adv_interval_us");
    assert_key_absent(item, tokens, count, 0U, "ble_service_uuids");
    assert_key_absent(item, tokens, count, 0U, "ble_ja3_hash");
}

void test_raw_uuid_list_accepts_six_and_emits_lowercase_canonical_text(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t observation = {
        .detection = fixture_minimal_detection(
            "BLE:uuid-raw", DETECTION_SRC_BLE_FINGERPRINT),
    };
    strcpy(observation.detection.ble_svc_uuids_raw,
           "FD5F,180F,FEAA,FFE0,"
           "12345678-1234-5678-9ABC-DEF012345678,ABCD");
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &observation, 1001));
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));

    char item[4097];
    size_t item_length = 0U;
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 0U, item, sizeof(item), &item_length));
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t count = parse_object(
        item, item_length, tokens, BACKEND_JSON_MAX_TOKENS);
    assert_string_key(item, tokens, count, 0U, "ble_svc_uuids",
        "fd5f,180f,feaa,ffe0,"
        "12345678-1234-5678-9abc-def012345678,abcd");
}

void test_malformed_or_seven_token_raw_uuid_uses_bounded_array_fallback(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t observation = {
        .detection = fixture_minimal_detection(
            "BLE:uuid-seven", DETECTION_SRC_BLE_FINGERPRINT),
    };
    strcpy(observation.detection.ble_svc_uuids_raw,
           "1800,1801,1802,1803,1804,1805,1806");
    observation.detection.ble_service_uuids[0] = 0x180fU;
    observation.detection.ble_service_uuids[1] = 0xfd5fU;
    observation.detection.ble_svc_uuid_count = 2U;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &observation, 1001));

    observation.detection = fixture_minimal_detection(
        "BLE:uuid-malformed", DETECTION_SRC_BLE_FINGERPRINT);
    strcpy(observation.detection.ble_svc_uuids_raw, "not-a-uuid");
    observation.detection.ble_service_uuids[0] = 0xfeaaU;
    observation.detection.ble_svc_uuid_count = 1U;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &observation, 1002));
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));

    char item[4097];
    size_t item_length = 0U;
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 0U, item, sizeof(item), &item_length));
    size_t count = parse_object(
        item, item_length, tokens, BACKEND_JSON_MAX_TOKENS);
    assert_string_key(item, tokens, count, 0U,
                      "ble_svc_uuids", "180f,fd5f");
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 1U, item, sizeof(item), &item_length));
    count = parse_object(
        item, item_length, tokens, BACKEND_JSON_MAX_TOKENS);
    assert_string_key(item, tokens, count, 0U,
                      "ble_svc_uuids", "feaa");
}

void test_uuid_array_fallback_emits_all_four_16_and_two_128_in_order(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t observation = {
        .detection = fixture_minimal_detection(
            "BLE:uuid-arrays", DETECTION_SRC_BLE_FINGERPRINT),
    };
    static const uint16_t uuid16[4] = {
        0x180fU, 0xfd5fU, 0xfeaaU, 0xffe0U,
    };
    memcpy(observation.detection.ble_service_uuids,
           uuid16, sizeof(uuid16));
    observation.detection.ble_svc_uuid_count = 4U;
    for (size_t slot = 0U; slot < 2U; ++slot) {
        for (size_t byte = 0U; byte < 16U; ++byte) {
            observation.detection.ble_service_uuids_128[slot][byte] =
                (uint8_t)(slot * 0x40U + byte);
        }
    }
    observation.detection.ble_svc_uuid_128_count = 2U;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &observation, 1001));
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));

    char item[4097];
    size_t item_length = 0U;
    TEST_ASSERT_TRUE(extract_array_object(
        batch.json, "detections", 0U, item, sizeof(item), &item_length));
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t count = parse_object(
        item, item_length, tokens, BACKEND_JSON_MAX_TOKENS);
    assert_string_key(item, tokens, count, 0U, "ble_svc_uuids",
        "180f,fd5f,feaa,ffe0,"
        "0f0e0d0c-0b0a-0908-0706-050403020100,"
        "4f4e4d4c-4b4a-4948-4746-454443424140");
}

void test_invalid_clock_and_optional_fields_are_absent_and_context_is_copied(void)
{
    backend_batch_context_t context = fixture_batch_context();
    context.clock_valid = false;
    context.epoch_ms = 9999;
    context.has_device_location = false;
    context.upload.has_last_success_age = false;
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    strcpy(context.device_id, "uplink_MUTATED");
    strcpy(context.node_name, "mutated");
    context.sequence = 999U;

    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));
    TEST_ASSERT_EQUAL_UINT32(41U, batch.sequence);
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t count = parse_object(
        batch.json, batch.json_len, tokens, BACKEND_JSON_MAX_TOKENS);
    TEST_ASSERT_EQUAL_UINT16(38U, tokens[0].child_count);
    assert_string_key(batch.json, tokens, count, 0U,
                      "device_id", "uplink_CB77A4");
    assert_string_key(batch.json, tokens, count, 0U,
                      "node_name", "Roof backend sensor");
    assert_key_absent(batch.json, tokens, count, 0U, "timestamp");
    assert_key_absent(batch.json, tokens, count, 0U, "device_lat");
    assert_key_absent(batch.json, tokens, count, 0U, "device_lon");
    assert_key_absent(batch.json, tokens, count, 0U, "device_alt");
    size_t health = require_key(batch.json, tokens, count, 0U, "health");
    TEST_ASSERT_EQUAL_UINT16(12U, tokens[health].child_count);
    assert_bool_key(batch.json, tokens, count, health, "clock_valid", false);
    assert_key_absent(batch.json, tokens, count, health, "epoch_ms");
    size_t upload = require_key(batch.json, tokens, count, 0U, "upload");
    TEST_ASSERT_EQUAL_UINT16(6U, tokens[upload].child_count);
    assert_key_absent(batch.json, tokens, count, upload,
                      "last_success_age_s");
}

void test_batch_boundary_is_atomic_and_sequence_advances_only_on_finish(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t item = {
        .detection = fixture_full_detection(),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1785600000123),
    };
    backend_encode_result_t result = BACKEND_ENCODE_INVALID;
    unsigned accepted = 0U;
    do {
        backend_upload_builder_t before = builder;
        result = backend_upload_builder_add(&builder, &item, 1001 + accepted);
        if (result == BACKEND_ENCODE_OK) {
            ++accepted;
        } else {
            TEST_ASSERT_EQUAL_MEMORY(&before, &builder, sizeof(builder));
        }
        TEST_ASSERT_LESS_THAN(64U, accepted);
    } while (result == BACKEND_ENCODE_OK);
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_NEEDS_FLUSH, result);
    TEST_ASSERT_GREATER_THAN(0U, accepted);
    TEST_ASSERT_EQUAL_UINT32(41U, builder.context.sequence);

    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));
    TEST_ASSERT_EQUAL_UINT32(41U, batch.sequence);
    TEST_ASSERT_EQUAL_UINT16(accepted, batch.item_count);
    TEST_ASSERT_TRUE(batch.json_len <= BACKEND_UPLOAD_MAX_JSON);
    TEST_ASSERT_EQUAL_CHAR('}', batch.json[batch.json_len - 1U]);
    TEST_ASSERT_EQUAL_CHAR('\0', batch.json[batch.json_len]);
    TEST_ASSERT_EQUAL_UINT32(42U, builder.context.sequence);
    TEST_ASSERT_EQUAL_UINT16(0U, builder.item_count);
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &item, 2000));

    char immutable_json[BACKEND_UPLOAD_MAX_JSON + 1U];
    memcpy(immutable_json, batch.json, batch.json_len + 1U);
    memset(&builder, 0, sizeof(builder));
    TEST_ASSERT_EQUAL_MEMORY(
        immutable_json, batch.json, batch.json_len + 1U);
}

void test_finish_failure_preserves_batch_sequence_and_unpublished_output(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t item = {
        .detection = fixture_minimal_detection(
            "RID:transaction", DETECTION_SRC_BLE_RID),
    };
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &item, 1001));

    builder.context.scanner_present[0] = false;
    builder.context.scanner_present[1] = false;
    builder.context.capability_count = 16U;
    for (size_t capability = 0U;
         capability < builder.context.capability_count; ++capability) {
        memset(builder.context.capabilities[capability], 1,
               sizeof(builder.context.capabilities[capability]) - 1U);
        builder.context.capabilities[capability]
            [sizeof(builder.context.capabilities[capability]) - 1U] = '\0';
    }
    memset(builder.context.capabilities[0], 1,
           sizeof(builder.context.capabilities[0]));
    const uint32_t sequence = builder.context.sequence;
    const uint16_t item_count = builder.item_count;
    const size_t json_len = builder.json_len;
    char partial[BACKEND_UPLOAD_MAX_JSON + 1U];
    memcpy(partial, builder.json, json_len + 1U);
    backend_upload_batch_t output;
    memset(&output, 0xa5, sizeof(output));

    TEST_ASSERT_FALSE(backend_upload_builder_finish(&builder, &output));
    TEST_ASSERT_EQUAL_UINT32(sequence, builder.context.sequence);
    TEST_ASSERT_EQUAL_UINT16(item_count, builder.item_count);
    TEST_ASSERT_EQUAL_UINT(json_len, builder.json_len);
    TEST_ASSERT_EQUAL_MEMORY(partial, builder.json, json_len + 1U);
    TEST_ASSERT_TRUE(builder.active);
    TEST_ASSERT_FALSE(builder.failed);
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((const uint8_t *)&output)[0]);
}

void test_near_limit_populated_batch_parses_as_one_complete_document(void)
{
    backend_batch_context_t context = fixture_batch_context();
    context.scanner_present[0] = false;
    context.scanner_present[1] = false;
    context.capability_count = 11U;
    for (size_t capability = 0U; capability < context.capability_count;
         ++capability) {
        memset(context.capabilities[capability], 1,
               sizeof(context.capabilities[capability]) - 1U);
        context.capabilities[capability]
            [sizeof(context.capabilities[capability]) - 1U] = '\0';
    }
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    TEST_ASSERT_TRUE(builder.active);
    backend_detection_observation_t item = {
        .detection = fixture_minimal_detection(
            "RID:near-limit", DETECTION_SRC_BLE_RID),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1785600000123),
    };
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &item, 1001));
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4000U, batch.json_len);
    TEST_ASSERT_TRUE(batch.json_len <= BACKEND_UPLOAD_MAX_JSON);
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0U;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OK, backend_json_parse(
        batch.json, batch.json_len, tokens, BACKEND_JSON_MAX_TOKENS,
        &token_count));
    TEST_ASSERT_GREATER_THAN(0U, token_count);
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[0].kind);
    size_t detections = require_key(
        batch.json, tokens, token_count, 0U, "detections");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[detections].kind);
    TEST_ASSERT_EQUAL_UINT16(1U, tokens[detections].child_count);
    TEST_ASSERT_EQUAL_UINT16(batch.json_len, tokens[0].end);
}

void test_tick_flushes_at_exact_80_ms_and_invalid_add_is_non_destructive(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t item = {
        .detection = fixture_minimal_detection(
            "RID:tick", DETECTION_SRC_BLE_RID),
    };
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &item, 1001));
    backend_upload_batch_t untouched;
    memset(&untouched, 0xa5, sizeof(untouched));
    TEST_ASSERT_FALSE(backend_upload_builder_tick(&builder, 1080, &untouched));
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((uint8_t *)&untouched)[0]);
    TEST_ASSERT_TRUE(backend_upload_builder_tick(&builder, 1081, &untouched));
    TEST_ASSERT_EQUAL_UINT16(1U, untouched.item_count);

    backend_upload_builder_init(&builder, &context, 2000);
    item.detection.drone_id[0] = '\0';
    backend_upload_builder_t before = builder;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_INVALID,
        backend_upload_builder_add(&builder, &item, 2001));
    TEST_ASSERT_EQUAL_MEMORY(&before, &builder, sizeof(builder));
    item.detection = fixture_minimal_detection(
        "RID:utf8", DETECTION_SRC_BLE_RID);
    item.detection.drone_id[0] = (char)0xc3;
    item.detection.drone_id[1] = '(';
    item.detection.drone_id[2] = '\0';
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_INVALID,
        backend_upload_builder_add(&builder, &item, 2002));
    TEST_ASSERT_EQUAL_MEMORY(&before, &builder, sizeof(builder));
    TEST_ASSERT_FALSE(backend_upload_builder_tick(&builder, 5000, &untouched));
}

void test_init_and_add_reject_negative_or_regressing_monotonic_time(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, -1);
    TEST_ASSERT_FALSE(builder.active);
    TEST_ASSERT_TRUE(builder.failed);
    TEST_ASSERT_EQUAL_UINT(0U, builder.json_len);

    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t item = {
        .detection = fixture_minimal_detection(
            "RID:backward", DETECTION_SRC_BLE_RID),
    };
    backend_upload_builder_t before = builder;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_INVALID,
        backend_upload_builder_add(&builder, &item, 999));
    TEST_ASSERT_EQUAL_MEMORY(&before, &builder, sizeof(builder));
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &item, 1000));
    before = builder;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_INVALID,
        backend_upload_builder_add(&builder, &item, -1));
    TEST_ASSERT_EQUAL_MEMORY(&before, &builder, sizeof(builder));
}

void test_tick_rejects_invalid_time_and_flushes_safely_at_int64_max(void)
{
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_t builder;
    backend_detection_observation_t item = {
        .detection = fixture_minimal_detection(
            "RID:max-time", DETECTION_SRC_BLE_RID),
    };
    backend_upload_builder_init(
        &builder, &context, INT64_MAX - INT64_C(81));
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(
            &builder, &item, INT64_MAX - INT64_C(80)));
    backend_upload_batch_t output;
    memset(&output, 0xa5, sizeof(output));
    TEST_ASSERT_FALSE(backend_upload_builder_tick(
        &builder, INT64_MAX - INT64_C(1), &output));
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((const uint8_t *)&output)[0]);
    TEST_ASSERT_TRUE(backend_upload_builder_tick(
        &builder, INT64_MAX, &output));
    TEST_ASSERT_EQUAL_UINT16(1U, output.item_count);

    backend_upload_builder_init(&builder, &context, 0);
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_OK,
        backend_upload_builder_add(&builder, &item, 0));
    backend_upload_builder_t before = builder;
    builder.last_item_ms = -1;
    memset(&output, 0xa5, sizeof(output));
    TEST_ASSERT_FALSE(backend_upload_builder_tick(
        &builder, INT64_MAX, &output));
    TEST_ASSERT_EQUAL_INT64(-1, builder.last_item_ms);
    TEST_ASSERT_EQUAL_UINT(before.json_len, builder.json_len);
    TEST_ASSERT_EQUAL_MEMORY(before.json, builder.json, before.json_len + 1U);
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((const uint8_t *)&output)[0]);
}

void test_serialization_rejects_above_cap_without_partial_json(void)
{
    backend_batch_context_t context = fixture_batch_context();
    context.scanner_present[0] = false;
    context.scanner_present[1] = false;
    context.capability_count = 13U;
    for (size_t capability = 0U; capability < context.capability_count;
         ++capability) {
        memset(context.capabilities[capability], 1,
               sizeof(context.capabilities[capability]) - 1U);
        context.capabilities[capability]
            [sizeof(context.capabilities[capability]) - 1U] = '\0';
    }
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    TEST_ASSERT_TRUE(builder.active);
    backend_detection_observation_t item = {
        .detection = fixture_full_detection(),
    };
    backend_upload_builder_t before = builder;
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_ITEM_TOO_LARGE,
        backend_upload_builder_add(&builder, &item, 1001));
    TEST_ASSERT_EQUAL_MEMORY(&before, &builder, sizeof(builder));

    context = fixture_batch_context();
    context.capability_count = 17U;
    backend_upload_builder_init(&builder, &context, 1000);
    TEST_ASSERT_FALSE(builder.active);
    TEST_ASSERT_TRUE(builder.failed);
    backend_upload_batch_t output;
    memset(&output, 0xa5, sizeof(output));
    TEST_ASSERT_FALSE(backend_upload_builder_finish(&builder, &output));
    TEST_ASSERT_EQUAL_HEX8(0xa5, ((uint8_t *)&output)[0]);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_empty_batch_has_exact_identity_health_and_scanner_bridge);
    BACKEND_RUN_TEST(
        test_detection_parity_uses_canonical_keys_and_copies_input);
    BACKEND_RUN_TEST(
        test_handwritten_wifi_and_ble_zero_absence_rules_survive);
    BACKEND_RUN_TEST(
        test_raw_uuid_list_accepts_six_and_emits_lowercase_canonical_text);
    BACKEND_RUN_TEST(
        test_malformed_or_seven_token_raw_uuid_uses_bounded_array_fallback);
    BACKEND_RUN_TEST(
        test_uuid_array_fallback_emits_all_four_16_and_two_128_in_order);
    BACKEND_RUN_TEST(
        test_invalid_clock_and_optional_fields_are_absent_and_context_is_copied);
    BACKEND_RUN_TEST(
        test_batch_boundary_is_atomic_and_sequence_advances_only_on_finish);
    BACKEND_RUN_TEST(
        test_finish_failure_preserves_batch_sequence_and_unpublished_output);
    BACKEND_RUN_TEST(
        test_near_limit_populated_batch_parses_as_one_complete_document);
    BACKEND_RUN_TEST(
        test_tick_flushes_at_exact_80_ms_and_invalid_add_is_non_destructive);
    BACKEND_RUN_TEST(
        test_init_and_add_reject_negative_or_regressing_monotonic_time);
    BACKEND_RUN_TEST(
        test_tick_rejects_invalid_time_and_flushes_safely_at_int64_max);
    BACKEND_RUN_TEST(
        test_serialization_rejects_above_cap_without_partial_json);
    return UNITY_END();
}
