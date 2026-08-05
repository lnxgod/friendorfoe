#include "backend_detection_assert.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#define BACKEND_TEST_FLOAT_TOLERANCE 1.0e-6f
#define BACKEND_TEST_DOUBLE_TOLERANCE 1.0e-9

static bool float_equal(float left, float right)
{
    return fabsf(left - right) <= BACKEND_TEST_FLOAT_TOLERANCE;
}

static bool double_equal(double left, double right)
{
    return fabs(left - right) <= BACKEND_TEST_DOUBLE_TOLERANCE;
}

bool backend_detection_equal(const drone_detection_t *expected,
                             const drone_detection_t *actual)
{
    if (!expected || !actual) {
        return expected == actual;
    }
    if (strcmp(expected->drone_id, actual->drone_id) != 0 ||
        expected->source != actual->source ||
        !float_equal(expected->confidence, actual->confidence) ||
        !double_equal(expected->latitude, actual->latitude) ||
        !double_equal(expected->longitude, actual->longitude) ||
        !double_equal(expected->altitude_m, actual->altitude_m) ||
        !float_equal(expected->heading_deg, actual->heading_deg) ||
        !float_equal(expected->speed_mps, actual->speed_mps) ||
        !float_equal(expected->vertical_speed_mps,
                     actual->vertical_speed_mps) ||
        expected->rssi != actual->rssi ||
        !double_equal(expected->estimated_distance_m,
                      actual->estimated_distance_m) ||
        strcmp(expected->manufacturer, actual->manufacturer) != 0 ||
        strcmp(expected->model, actual->model) != 0 ||
        !double_equal(expected->operator_lat, actual->operator_lat) ||
        !double_equal(expected->operator_lon, actual->operator_lon) ||
        strcmp(expected->operator_id, actual->operator_id) != 0 ||
        expected->ua_type != actual->ua_type ||
        expected->id_type != actual->id_type ||
        expected->self_id_desc_type != actual->self_id_desc_type ||
        strcmp(expected->self_id_text, actual->self_id_text) != 0 ||
        !double_equal(expected->height_agl_m, actual->height_agl_m) ||
        !double_equal(expected->geodetic_alt_m, actual->geodetic_alt_m) ||
        !float_equal(expected->h_accuracy_m, actual->h_accuracy_m) ||
        !float_equal(expected->v_accuracy_m, actual->v_accuracy_m) ||
        expected->area_count != actual->area_count ||
        expected->area_radius != actual->area_radius ||
        !double_equal(expected->area_ceiling, actual->area_ceiling) ||
        !double_equal(expected->area_floor, actual->area_floor) ||
        expected->classification_type != actual->classification_type ||
        strcmp(expected->ssid, actual->ssid) != 0 ||
        strcmp(expected->bssid, actual->bssid) != 0 ||
        expected->freq_mhz != actual->freq_mhz ||
        expected->channel_width_mhz != actual->channel_width_mhz ||
        expected->ble_company_id != actual->ble_company_id ||
        expected->ble_apple_type != actual->ble_apple_type ||
        expected->ble_svc_uuid_count != actual->ble_svc_uuid_count ||
        expected->ble_svc_uuid_128_count != actual->ble_svc_uuid_128_count ||
        strcmp(expected->ble_svc_uuids_raw,
               actual->ble_svc_uuids_raw) != 0 ||
        expected->ble_ad_type_count != actual->ble_ad_type_count ||
        expected->ble_payload_len != actual->ble_payload_len ||
        expected->ble_addr_type != actual->ble_addr_type ||
        expected->ble_ja3_hash != actual->ble_ja3_hash ||
        strcmp(expected->ble_name, actual->ble_name) != 0 ||
        strcmp(expected->class_reason, actual->class_reason) != 0 ||
        expected->ble_apple_activity != actual->ble_apple_activity ||
        expected->ble_apple_flags != actual->ble_apple_flags ||
        expected->ble_raw_mfr_len != actual->ble_raw_mfr_len ||
        expected->ble_adv_interval_us != actual->ble_adv_interval_us ||
        expected->first_seen_ms != actual->first_seen_ms ||
        expected->last_updated_ms != actual->last_updated_ms ||
        !float_equal(expected->fused_confidence,
                     actual->fused_confidence) ||
        strcmp(expected->probed_ssids, actual->probed_ssids) != 0 ||
        expected->probe_ie_hash != actual->probe_ie_hash ||
        expected->wifi_generation != actual->wifi_generation ||
        expected->wifi_auth_mode != actual->wifi_auth_mode ||
        expected->scanner_slot != actual->scanner_slot ||
        expected->scanner_slots_seen != actual->scanner_slots_seen ||
        expected->ble_threat_kind != actual->ble_threat_kind ||
        expected->ble_prompt_family_mask !=
            actual->ble_prompt_family_mask ||
        expected->ble_unique_macs != actual->ble_unique_macs ||
        expected->ble_observation_count != actual->ble_observation_count ||
        expected->ble_serial_service_uuid !=
            actual->ble_serial_service_uuid ||
        expected->ble_threat_evidence_mask !=
            actual->ble_threat_evidence_mask) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (expected->ble_service_uuids[i] !=
            actual->ble_service_uuids[i]) {
            return false;
        }
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        for (size_t byte = 0; byte < 16; ++byte) {
            if (expected->ble_service_uuids_128[slot][byte] !=
                actual->ble_service_uuids_128[slot][byte]) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        if (expected->ble_apple_auth[i] != actual->ble_apple_auth[i]) {
            return false;
        }
    }
    for (size_t i = 0; i < 20; ++i) {
        if (expected->ble_raw_mfr[i] != actual->ble_raw_mfr[i]) {
            return false;
        }
    }
    return true;
}

void backend_assert_detection_equal(const drone_detection_t *expected,
                                    const drone_detection_t *actual)
{
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_TRUE_MESSAGE(
        backend_detection_equal(expected, actual),
        "drone_detection_t differs in the independent field enumerator");
}

static bool source_is_wifi(uint8_t source)
{
    return source == DETECTION_SRC_WIFI_SSID ||
           source == DETECTION_SRC_WIFI_DJI_IE ||
           source == DETECTION_SRC_WIFI_BEACON ||
           source == DETECTION_SRC_WIFI_OUI ||
           source == DETECTION_SRC_WIFI_PROBE_REQUEST ||
           source == DETECTION_SRC_WIFI_ASSOC ||
           source == DETECTION_SRC_WIFI_AP_INVENTORY;
}

static bool source_is_ble(uint8_t source)
{
    return source == DETECTION_SRC_BLE_RID ||
           source == DETECTION_SRC_BLE_FINGERPRINT;
}

/* Intentionally independent from the production codec. */
static int expected_wifi_channel(int32_t frequency_mhz)
{
    if (frequency_mhz >= 2412 && frequency_mhz <= 2472 &&
        (frequency_mhz - 2412) % 5 == 0) {
        return 1 + (frequency_mhz - 2412) / 5;
    }
    if (frequency_mhz == 2484) {
        return 14;
    }
    if (frequency_mhz >= 5005 && frequency_mhz <= 5895 &&
        frequency_mhz % 5 == 0) {
        return (frequency_mhz - 5000) / 5;
    }
    return 0;
}

static const char *source_name(uint8_t source)
{
    static const char *const names[] = {
        "ble_rid", "wifi_ssid", "wifi_dji_ie", "wifi_beacon_rid",
        "wifi_oui", "wifi_probe_request", "ble_fingerprint",
        "wifi_assoc", "wifi_ap_inventory",
    };
    return source <= DETECTION_SRC_WIFI_AP_INVENTORY ? names[source] : NULL;
}

static size_t require_key(const char *json,
                          const backend_json_token_t *tokens,
                          size_t token_count,
                          size_t object_index,
                          const char *key)
{
    size_t value_index = 0;
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
    size_t ignored = 0;
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
    size_t index = require_key(
        json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE_MESSAGE(backend_json_copy_string(
        json, &tokens[index], actual, sizeof(actual)), key);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, actual, key);
}

static void assert_i64_key(const char *json,
                           const backend_json_token_t *tokens,
                           size_t token_count,
                           size_t object_index,
                           const char *key,
                           int64_t expected)
{
    int64_t actual = 0;
    size_t index = require_key(
        json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE_MESSAGE(backend_json_get_i64(
        json, &tokens[index], &actual), key);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(expected, actual, key);
}

static void assert_double_key(const char *json,
                              const backend_json_token_t *tokens,
                              size_t token_count,
                              size_t object_index,
                              const char *key,
                              double expected)
{
    double actual = 0.0;
    size_t index = require_key(
        json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE_MESSAGE(backend_json_get_double(
        json, &tokens[index], &actual), key);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(
        BACKEND_TEST_DOUBLE_TOLERANCE, expected, actual, key);
}

static void assert_optional_string(const char *json,
                                   const backend_json_token_t *tokens,
                                   size_t token_count,
                                   size_t object_index,
                                   const char *key,
                                   const char *expected)
{
    if (expected[0] == '\0') {
        assert_key_absent(json, tokens, token_count, object_index, key);
    } else {
        assert_string_key(
            json, tokens, token_count, object_index, key, expected);
    }
}

static void format_hex(char *output, size_t capacity,
                       const uint8_t *bytes, size_t count)
{
    TEST_ASSERT_GREATER_OR_EQUAL(count * 2U + 1U, capacity);
    for (size_t i = 0; i < count; ++i) {
        int result = snprintf(output + i * 2U, capacity - i * 2U,
                              "%02x", bytes[i]);
        TEST_ASSERT_EQUAL_INT(2, result);
    }
    output[count * 2U] = '\0';
}

static bool is_hex_char(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool valid_raw_uuid_list(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    const char *token = value;
    while (*token) {
        const char *end = strchr(token, ',');
        size_t length = end ? (size_t)(end - token) : strlen(token);
        bool valid = length == 4U || length == 36U;
        for (size_t i = 0; valid && i < length; ++i) {
            bool hyphen = length == 36U &&
                (i == 8U || i == 13U || i == 18U || i == 23U);
            if (hyphen ? token[i] != '-' : !is_hex_char(token[i])) {
                valid = false;
            }
        }
        if (!valid) {
            return false;
        }
        if (!end) {
            return true;
        }
        token = end + 1;
        if (*token == '\0') {
            return false;
        }
    }
    return true;
}

static void expected_uuid_list(const drone_detection_t *expected,
                               char *output, size_t capacity)
{
    if (valid_raw_uuid_list(expected->ble_svc_uuids_raw)) {
        snprintf(output, capacity, "%s", expected->ble_svc_uuids_raw);
        return;
    }
    output[0] = '\0';
    size_t offset = 0;
    size_t count16 = expected->ble_svc_uuid_count <= 4U
        ? expected->ble_svc_uuid_count : 4U;
    for (size_t i = 0; i < count16; ++i) {
        int written = snprintf(output + offset, capacity - offset,
            "%s%04x", offset == 0 ? "" : ",",
            expected->ble_service_uuids[i]);
        TEST_ASSERT_GREATER_THAN(0, written);
        offset += (size_t)written;
    }
    size_t count128 = expected->ble_svc_uuid_128_count <= 2U
        ? expected->ble_svc_uuid_128_count : 2U;
    for (size_t i = 0; i < count128; ++i) {
        const uint8_t *u = expected->ble_service_uuids_128[i];
        int written = snprintf(output + offset, capacity - offset,
            "%s%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x",
            offset == 0 ? "" : ",",
            u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
            u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
        TEST_ASSERT_GREATER_THAN(0, written);
        offset += (size_t)written;
    }
}

static void assert_probed_ssids(const drone_detection_t *expected,
                                const char *json,
                                const backend_json_token_t *tokens,
                                size_t token_count,
                                size_t object_index)
{
    if (expected->source != DETECTION_SRC_WIFI_PROBE_REQUEST) {
        assert_key_absent(
            json, tokens, token_count, object_index, "probed_ssids");
        return;
    }
    size_t array_index = require_key(
        json, tokens, token_count, object_index, "probed_ssids");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[array_index].kind);

    const char *cursor = expected->probed_ssids;
    const char *fallback = expected->ssid;
    size_t actual_index = array_index + 1U;
    size_t matched = 0;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length > 0U) {
            while (actual_index < token_count &&
                   tokens[actual_index].parent != (int16_t)array_index) {
                ++actual_index;
            }
            TEST_ASSERT_LESS_THAN(token_count, actual_index);
            char actual[33];
            TEST_ASSERT_TRUE(backend_json_copy_string(
                json, &tokens[actual_index], actual, sizeof(actual)));
            TEST_ASSERT_EQUAL_UINT(length, strlen(actual));
            TEST_ASSERT_EQUAL_MEMORY(cursor, actual, length);
            ++actual_index;
            ++matched;
        }
        if (!comma) {
            break;
        }
        cursor = comma + 1;
    }
    if (matched == 0U && fallback[0] != '\0') {
        while (actual_index < token_count &&
               tokens[actual_index].parent != (int16_t)array_index) {
            ++actual_index;
        }
        TEST_ASSERT_LESS_THAN(token_count, actual_index);
        char actual[33];
        TEST_ASSERT_TRUE(backend_json_copy_string(
            json, &tokens[actual_index], actual, sizeof(actual)));
        TEST_ASSERT_EQUAL_STRING(fallback, actual);
        ++matched;
    }
    TEST_ASSERT_EQUAL_UINT(matched, tokens[array_index].child_count);
}

void backend_assert_detection_json_equal(
    const drone_detection_t *expected,
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index)
{
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(tokens);
    TEST_ASSERT_LESS_THAN(token_count, object_index);
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[object_index].kind);

    assert_string_key(json, tokens, token_count, object_index,
                      "drone_id", expected->drone_id);
    TEST_ASSERT_NOT_NULL(source_name(expected->source));
    assert_string_key(json, tokens, token_count, object_index,
                      "source", source_name(expected->source));
    assert_double_key(json, tokens, token_count, object_index,
                      "confidence", expected->confidence);
    if (isfinite(expected->fused_confidence) &&
        expected->fused_confidence > 0.0f) {
        assert_double_key(json, tokens, token_count, object_index,
            "fused_confidence", expected->fused_confidence);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "fused_confidence");
    }

#define ASSERT_DOUBLE(member, key) \
    assert_double_key(json, tokens, token_count, object_index, \
                      key, expected->member)
#define ASSERT_I64(member, key) \
    assert_i64_key(json, tokens, token_count, object_index, \
                   key, expected->member)
    ASSERT_DOUBLE(latitude, "latitude");
    ASSERT_DOUBLE(longitude, "longitude");
    ASSERT_DOUBLE(altitude_m, "altitude_m");
    ASSERT_DOUBLE(heading_deg, "heading_deg");
    ASSERT_DOUBLE(speed_mps, "speed_mps");
    ASSERT_DOUBLE(vertical_speed_mps, "vertical_speed_mps");
    ASSERT_I64(rssi, "rssi");
    ASSERT_DOUBLE(estimated_distance_m, "estimated_distance_m");
    assert_optional_string(json, tokens, token_count, object_index,
                           "manufacturer", expected->manufacturer);
    assert_optional_string(json, tokens, token_count, object_index,
                           "model", expected->model);
    ASSERT_DOUBLE(operator_lat, "operator_lat");
    ASSERT_DOUBLE(operator_lon, "operator_lon");
    assert_optional_string(json, tokens, token_count, object_index,
                           "operator_id", expected->operator_id);
    ASSERT_I64(ua_type, "ua_type");
    ASSERT_I64(id_type, "id_type");
    ASSERT_I64(self_id_desc_type, "self_id_desc_type");
    assert_optional_string(json, tokens, token_count, object_index,
                           "self_id_text", expected->self_id_text);
    ASSERT_DOUBLE(height_agl_m, "height_agl_m");
    ASSERT_DOUBLE(geodetic_alt_m, "geodetic_alt_m");
    ASSERT_DOUBLE(h_accuracy_m, "h_accuracy_m");
    ASSERT_DOUBLE(v_accuracy_m, "v_accuracy_m");
    ASSERT_I64(area_count, "area_count");
    ASSERT_I64(area_radius, "area_radius");
    ASSERT_DOUBLE(area_ceiling, "area_ceiling");
    ASSERT_DOUBLE(area_floor, "area_floor");
    ASSERT_I64(classification_type, "classification_type");
    assert_optional_string(json, tokens, token_count, object_index,
                           "ssid", expected->ssid);
    assert_optional_string(json, tokens, token_count, object_index,
                           "bssid", expected->bssid);
    if (expected->freq_mhz > 0) {
        ASSERT_I64(freq_mhz, "freq_mhz");
        int channel = expected_wifi_channel(expected->freq_mhz);
        if (channel > 0) {
            assert_i64_key(json, tokens, token_count, object_index,
                           "channel", channel);
        } else {
            assert_key_absent(json, tokens, token_count, object_index,
                              "channel");
        }
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "freq_mhz");
        assert_key_absent(json, tokens, token_count, object_index,
                          "channel");
    }
    if (expected->channel_width_mhz > 0) {
        ASSERT_I64(channel_width_mhz, "channel_width_mhz");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "channel_width_mhz");
    }
    if (source_is_wifi(expected->source)) {
        if (expected->wifi_auth_mode == UINT8_MAX) {
            assert_key_absent(json, tokens, token_count, object_index,
                              "auth_m");
        } else {
            TEST_ASSERT_LESS_OR_EQUAL_UINT8(10, expected->wifi_auth_mode);
            ASSERT_I64(wifi_auth_mode, "auth_m");
        }
        ASSERT_I64(wifi_generation, "wifi_generation");
    } else {
        assert_key_absent(json, tokens, token_count, object_index, "auth_m");
        assert_key_absent(json, tokens, token_count, object_index,
                          "wifi_generation");
    }
    assert_probed_ssids(
        expected, json, tokens, token_count, object_index);
    if (expected->probe_ie_hash != 0U) {
        char hash[9];
        snprintf(hash, sizeof(hash), "%08x", expected->probe_ie_hash);
        assert_string_key(json, tokens, token_count, object_index,
                          "ie_hash", hash);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ie_hash");
    }

    if (expected->ble_company_id != 0U) {
        ASSERT_I64(ble_company_id, "ble_company_id");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_company_id");
    }
    if (expected->ble_apple_type != 0U) {
        ASSERT_I64(ble_apple_type, "ble_apple_type");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_apple_type");
    }
    if (expected->ble_ad_type_count != 0U) {
        ASSERT_I64(ble_ad_type_count, "ble_ad_type_count");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_ad_type_count");
    }
    if (expected->ble_payload_len != 0U) {
        ASSERT_I64(ble_payload_len, "ble_payload_len");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_payload_len");
    }
    if (source_is_ble(expected->source)) {
        ASSERT_I64(ble_addr_type, "ble_addr_type");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_addr_type");
    }
    if (expected->ble_ja3_hash != 0U) {
        char hash[9];
        snprintf(hash, sizeof(hash), "%08x", expected->ble_ja3_hash);
        assert_string_key(json, tokens, token_count, object_index,
                          "ble_ja3", hash);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_ja3");
    }
    assert_optional_string(json, tokens, token_count, object_index,
                           "ble_name", expected->ble_name);
    assert_optional_string(json, tokens, token_count, object_index,
                           "class_reason", expected->class_reason);
    if (expected->ble_apple_auth[0] != 0U ||
        expected->ble_apple_auth[1] != 0U ||
        expected->ble_apple_auth[2] != 0U) {
        char auth[7];
        format_hex(auth, sizeof(auth), expected->ble_apple_auth, 3);
        assert_string_key(json, tokens, token_count, object_index,
                          "ble_apple_auth", auth);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_apple_auth");
    }
    if (expected->ble_company_id == 0x004cU ||
        expected->ble_apple_type != 0U ||
        expected->ble_apple_auth[0] != 0U ||
        expected->ble_apple_auth[1] != 0U ||
        expected->ble_apple_auth[2] != 0U) {
        ASSERT_I64(ble_apple_activity, "ble_activity");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_activity");
    }
    ASSERT_I64(ble_apple_flags, "ble_apple_flags");
    if (expected->ble_raw_mfr_len > 0U) {
        char raw[41];
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(20, expected->ble_raw_mfr_len);
        format_hex(raw, sizeof(raw), expected->ble_raw_mfr,
                   expected->ble_raw_mfr_len);
        assert_string_key(json, tokens, token_count, object_index,
                          "ble_raw_mfr", raw);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_raw_mfr");
    }
    if (expected->ble_adv_interval_us > 0) {
        assert_double_key(json, tokens, token_count, object_index,
            "ble_adv_interval",
            (double)expected->ble_adv_interval_us / 1000.0);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_adv_interval");
    }
    char uuids[160];
    expected_uuid_list(expected, uuids, sizeof(uuids));
    if (uuids[0] != '\0') {
        assert_string_key(json, tokens, token_count, object_index,
                          "ble_svc_uuids", uuids);
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "ble_svc_uuids");
    }
    if (expected->first_seen_ms >= INT64_C(1700000000000)) {
        ASSERT_I64(first_seen_ms, "first_seen_ms");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "first_seen_ms");
    }
    if (expected->last_updated_ms >= INT64_C(1700000000000)) {
        ASSERT_I64(last_updated_ms, "last_updated_ms");
    } else {
        assert_key_absent(json, tokens, token_count, object_index,
                          "last_updated_ms");
    }
    ASSERT_I64(scanner_slot, "scanner_slot");
    ASSERT_I64(scanner_slots_seen, "scanner_slots_seen");

    if (expected->ble_threat_kind != BLE_THREAT_KIND_NONE) {
        ASSERT_I64(ble_threat_kind, "ble_threat_kind");
        ASSERT_I64(ble_prompt_family_mask, "ble_prompt_family_mask");
        ASSERT_I64(ble_unique_macs, "ble_unique_macs");
        ASSERT_I64(ble_observation_count, "ble_observation_count");
        ASSERT_I64(ble_serial_service_uuid, "ble_serial_service_uuid");
        ASSERT_I64(ble_threat_evidence_mask, "ble_threat_evidence_mask");
    } else {
        static const char *const keys[] = {
            "ble_threat_kind", "ble_prompt_family_mask",
            "ble_unique_macs", "ble_observation_count",
            "ble_serial_service_uuid", "ble_threat_evidence_mask",
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            assert_key_absent(
                json, tokens, token_count, object_index, keys[i]);
        }
    }
#undef ASSERT_DOUBLE
#undef ASSERT_I64
}
