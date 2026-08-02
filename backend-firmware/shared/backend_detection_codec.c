#include "backend_detection_codec.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "backend_json_reader.h"
#include "backend_json_writer.h"
#include "backend_uart_protocol.h"

static bool has_terminator(const char *value, size_t capacity)
{
    return value && memchr(value, '\0', capacity) != NULL;
}

static bool valid_epoch_or_zero(int64_t value)
{
    return value == 0 || value >= BACKEND_DETECTION_EPOCH_MIN_MS;
}

static bool valid_probe_tokens(const drone_detection_t *value)
{
    if (value->source != DETECTION_SRC_WIFI_PROBE_REQUEST) {
        return true;
    }
    const char *cursor = value->probed_ssids;
    bool found = false;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length > 32U) {
            return false;
        }
        found = found || length > 0U;
        if (!comma) {
            break;
        }
        cursor = comma + 1;
    }
    return found || value->ssid[0] != '\0';
}

static bool valid_detection(const drone_detection_t *value)
{
    if (!value || !has_terminator(value->drone_id,
                                  sizeof(value->drone_id)) ||
        value->drone_id[0] == '\0' ||
        !has_terminator(value->manufacturer,
                        sizeof(value->manufacturer)) ||
        !has_terminator(value->model, sizeof(value->model)) ||
        !has_terminator(value->operator_id, sizeof(value->operator_id)) ||
        !has_terminator(value->self_id_text,
                        sizeof(value->self_id_text)) ||
        !has_terminator(value->ssid, sizeof(value->ssid)) ||
        !has_terminator(value->bssid, sizeof(value->bssid)) ||
        !has_terminator(value->ble_svc_uuids_raw,
                        sizeof(value->ble_svc_uuids_raw)) ||
        !has_terminator(value->ble_name, sizeof(value->ble_name)) ||
        !has_terminator(value->class_reason,
                        sizeof(value->class_reason)) ||
        !has_terminator(value->probed_ssids,
                        sizeof(value->probed_ssids))) {
        return false;
    }
    if (value->source > DETECTION_SRC_WIFI_AP_INVENTORY ||
        !isfinite(value->confidence) || value->confidence < 0.0f ||
        value->confidence > 1.0f ||
        !isfinite(value->fused_confidence) ||
        value->fused_confidence < 0.0f ||
        value->fused_confidence > 1.0f ||
        !isfinite(value->latitude) || value->latitude < -90.0 ||
        value->latitude > 90.0 ||
        !isfinite(value->longitude) || value->longitude < -180.0 ||
        value->longitude > 180.0 ||
        !isfinite(value->altitude_m) ||
        !isfinite(value->heading_deg) || value->heading_deg < 0.0f ||
        value->heading_deg > 360.0f ||
        !isfinite(value->speed_mps) ||
        !isfinite(value->vertical_speed_mps) ||
        !isfinite(value->estimated_distance_m) ||
        value->estimated_distance_m < 0.0 ||
        !isfinite(value->operator_lat) || value->operator_lat < -90.0 ||
        value->operator_lat > 90.0 ||
        !isfinite(value->operator_lon) || value->operator_lon < -180.0 ||
        value->operator_lon > 180.0 ||
        !isfinite(value->height_agl_m) ||
        !isfinite(value->geodetic_alt_m) ||
        !isfinite(value->h_accuracy_m) || value->h_accuracy_m < 0.0f ||
        !isfinite(value->v_accuracy_m) || value->v_accuracy_m < 0.0f ||
        !isfinite(value->area_ceiling) ||
        !isfinite(value->area_floor) || value->freq_mhz < 0 ||
        value->channel_width_mhz < 0 ||
        value->ble_svc_uuid_count > 4U ||
        value->ble_svc_uuid_128_count > 2U ||
        value->ble_addr_type > 3U || value->ble_raw_mfr_len > 20U ||
        value->ble_adv_interval_us < 0 ||
        !valid_epoch_or_zero(value->first_seen_ms) ||
        !valid_epoch_or_zero(value->last_updated_ms) ||
        value->wifi_generation > 6U ||
        (value->wifi_auth_mode != UINT8_MAX &&
         value->wifi_auth_mode > 10U) ||
        value->ble_threat_kind > BLE_THREAT_KIND_SERIAL_SKIMMER ||
        !valid_probe_tokens(value)) {
        return false;
    }
    return true;
}

static bool append_prefix(backend_json_writer_t *writer, const char *key)
{
    return backend_json_append_format(writer, ",\"%s\":", key);
}

static bool append_string_field(backend_json_writer_t *writer,
                                const char *key,
                                const char *value)
{
    return append_prefix(writer, key) &&
           backend_json_append_escaped(writer, value);
}

static bool append_i64_field(backend_json_writer_t *writer,
                             const char *key,
                             int64_t value)
{
    return append_prefix(writer, key) &&
           backend_json_append_format(writer, "%" PRId64, value);
}

static bool append_u64_field(backend_json_writer_t *writer,
                             const char *key,
                             uint64_t value)
{
    return append_prefix(writer, key) &&
           backend_json_append_format(writer, "%" PRIu64, value);
}

static bool append_float_field(backend_json_writer_t *writer,
                               const char *key,
                               float value)
{
    return append_prefix(writer, key) &&
           backend_json_append_format(writer, "%.9g", (double)value);
}

static bool append_double_field(backend_json_writer_t *writer,
                                const char *key,
                                double value)
{
    return append_prefix(writer, key) &&
           backend_json_append_format(writer, "%.17g", value);
}

static void bytes_to_hex(const uint8_t *bytes, size_t count, char *output)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        output[i * 2U] = HEX[bytes[i] >> 4];
        output[i * 2U + 1U] = HEX[bytes[i] & 0x0fU];
    }
    output[count * 2U] = '\0';
}

int backend_detection_wifi_channel(int32_t freq_mhz)
{
    if (freq_mhz >= 2412 && freq_mhz <= 2472 &&
        (freq_mhz - 2407) % 5 == 0) {
        return (freq_mhz - 2407) / 5;
    }
    if (freq_mhz == 2484) {
        return 14;
    }
    if (freq_mhz >= 5005 && freq_mhz <= 5895 && freq_mhz % 5 == 0) {
        return (freq_mhz - 5000) / 5;
    }
    return 0;
}

size_t backend_detection_uart_encode(
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *stamp,
    char *output,
    size_t capacity)
{
    if (output && capacity > 0U) {
        output[0] = '\0';
    }
    if (!output || capacity == 0U || !valid_detection(detection) ||
        (stamp && stamp->time_valid &&
         stamp->observed_epoch_ms < BACKEND_DETECTION_EPOCH_MIN_MS)) {
        return 0U;
    }
    size_t bounded_capacity = capacity;
    if (bounded_capacity > BACKEND_DETECTION_UART_MAX_LINE + 1U) {
        bounded_capacity = BACKEND_DETECTION_UART_MAX_LINE + 1U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, bounded_capacity);
    backend_json_append(&writer, "{\"type\":");
    backend_json_append_escaped(&writer, MSG_TYPE_DETECTION);

    append_string_field(&writer, JSON_KEY_DRONE_ID, detection->drone_id);
    append_u64_field(&writer, JSON_KEY_SOURCE, detection->source);
    append_float_field(&writer, JSON_KEY_CONFIDENCE, detection->confidence);
    append_double_field(&writer, JSON_KEY_LATITUDE, detection->latitude);
    append_double_field(&writer, JSON_KEY_LONGITUDE, detection->longitude);
    append_double_field(&writer, JSON_KEY_ALTITUDE, detection->altitude_m);
    append_float_field(&writer, JSON_KEY_HEADING, detection->heading_deg);
    append_float_field(&writer, JSON_KEY_SPEED, detection->speed_mps);
    append_float_field(&writer, JSON_KEY_VSPEED,
                       detection->vertical_speed_mps);
    append_i64_field(&writer, JSON_KEY_RSSI, detection->rssi);
    append_double_field(&writer, JSON_KEY_DISTANCE,
                        detection->estimated_distance_m);
    append_string_field(&writer, JSON_KEY_MANUFACTURER,
                        detection->manufacturer);
    append_string_field(&writer, JSON_KEY_MODEL, detection->model);
    append_double_field(&writer, JSON_KEY_OPERATOR_LAT,
                        detection->operator_lat);
    append_double_field(&writer, JSON_KEY_OPERATOR_LON,
                        detection->operator_lon);
    append_string_field(&writer, JSON_KEY_OPERATOR_ID,
                        detection->operator_id);
    append_u64_field(&writer, JSON_KEY_UA_TYPE, detection->ua_type);
    append_u64_field(&writer, JSON_KEY_ID_TYPE, detection->id_type);
    append_u64_field(&writer, "self_dt", detection->self_id_desc_type);
    append_string_field(&writer, JSON_KEY_SELF_ID, detection->self_id_text);
    append_double_field(&writer, JSON_KEY_HEIGHT_AGL,
                        detection->height_agl_m);
    append_double_field(&writer, JSON_KEY_GEODETIC_ALT,
                        detection->geodetic_alt_m);
    append_float_field(&writer, JSON_KEY_H_ACCURACY,
                       detection->h_accuracy_m);
    append_float_field(&writer, JSON_KEY_V_ACCURACY,
                       detection->v_accuracy_m);
    append_u64_field(&writer, "area_n", detection->area_count);
    append_u64_field(&writer, "area_r", detection->area_radius);
    append_double_field(&writer, "area_c", detection->area_ceiling);
    append_double_field(&writer, "area_f", detection->area_floor);
    append_u64_field(&writer, "class_t", detection->classification_type);
    append_string_field(&writer, JSON_KEY_SSID, detection->ssid);
    append_string_field(&writer, JSON_KEY_BSSID, detection->bssid);
    append_i64_field(&writer, JSON_KEY_FREQ, detection->freq_mhz);
    append_i64_field(&writer, JSON_KEY_CHANNEL_WIDTH,
                     detection->channel_width_mhz);
    append_u64_field(&writer, JSON_KEY_WIFI_AUTH_MODE,
                     detection->wifi_auth_mode);
    append_u64_field(&writer, "wifi_gen", detection->wifi_generation);
    append_string_field(&writer, JSON_KEY_PROBED_SSIDS,
                        detection->probed_ssids);
    char hash_hex[9];
    snprintf(hash_hex, sizeof(hash_hex), "%08" PRIx32,
             detection->probe_ie_hash);
    append_string_field(&writer, "ie_hash", hash_hex);

    append_u64_field(&writer, JSON_KEY_BLE_COMPANY_ID,
                     detection->ble_company_id);
    append_u64_field(&writer, JSON_KEY_BLE_APPLE_TYPE,
                     detection->ble_apple_type);
    append_prefix(&writer, "ble_u16");
    backend_json_append(&writer, "[");
    for (size_t i = 0; i < 4U; ++i) {
        backend_json_append_format(&writer, "%s%u", i == 0U ? "" : ",",
                                   detection->ble_service_uuids[i]);
    }
    backend_json_append(&writer, "]");
    append_u64_field(&writer, "ble_u16_n", detection->ble_svc_uuid_count);
    append_prefix(&writer, "ble_u128");
    backend_json_append(&writer, "[");
    for (size_t i = 0; i < 2U; ++i) {
        char hex[33];
        bytes_to_hex(detection->ble_service_uuids_128[i], 16U, hex);
        if (i > 0U) {
            backend_json_append(&writer, ",");
        }
        backend_json_append_escaped(&writer, hex);
    }
    backend_json_append(&writer, "]");
    append_u64_field(&writer, "ble_u128_n",
                     detection->ble_svc_uuid_128_count);
    append_string_field(&writer, JSON_KEY_BLE_SVC_UUIDS,
                        detection->ble_svc_uuids_raw);
    append_u64_field(&writer, JSON_KEY_BLE_AD_TYPES,
                     detection->ble_ad_type_count);
    append_u64_field(&writer, JSON_KEY_BLE_PAYLOAD_LEN,
                     detection->ble_payload_len);
    append_u64_field(&writer, JSON_KEY_BLE_ADDR_TYPE,
                     detection->ble_addr_type);
    snprintf(hash_hex, sizeof(hash_hex), "%08" PRIx32,
             detection->ble_ja3_hash);
    append_string_field(&writer, JSON_KEY_BLE_JA3, hash_hex);
    append_string_field(&writer, JSON_KEY_BLE_NAME, detection->ble_name);
    append_string_field(&writer, JSON_KEY_CLASS_REASON,
                        detection->class_reason);
    char auth_hex[7];
    bytes_to_hex(detection->ble_apple_auth, 3U, auth_hex);
    append_string_field(&writer, JSON_KEY_BLE_APPLE_AUTH, auth_hex);
    append_u64_field(&writer, JSON_KEY_BLE_ACTIVITY,
                     detection->ble_apple_activity);
    append_u64_field(&writer, JSON_KEY_BLE_APPLE_FLAGS,
                     detection->ble_apple_flags);
    char raw_mfr_hex[41];
    bytes_to_hex(detection->ble_raw_mfr, 20U, raw_mfr_hex);
    append_string_field(&writer, JSON_KEY_BLE_RAW_MFR, raw_mfr_hex);
    append_u64_field(&writer, "ble_mfr_n", detection->ble_raw_mfr_len);
    append_i64_field(&writer, JSON_KEY_BLE_ADV_INTERVAL,
                     detection->ble_adv_interval_us);
    append_i64_field(&writer, JSON_KEY_FIRST_SEEN,
                     detection->first_seen_ms);
    append_i64_field(&writer, JSON_KEY_LAST_UPDATED,
                     detection->last_updated_ms);
    append_float_field(&writer, JSON_KEY_FUSED_CONFIDENCE,
                       detection->fused_confidence);
    append_u64_field(&writer, JSON_KEY_BLE_THREAT_KIND,
                     detection->ble_threat_kind);
    append_u64_field(&writer, JSON_KEY_BLE_PROMPT_FAMILIES,
                     detection->ble_prompt_family_mask);
    append_u64_field(&writer, JSON_KEY_BLE_UNIQUE_MACS,
                     detection->ble_unique_macs);
    append_u64_field(&writer, JSON_KEY_BLE_OBSERVATIONS,
                     detection->ble_observation_count);
    append_u64_field(&writer, JSON_KEY_BLE_SERIAL_UUID,
                     detection->ble_serial_service_uuid);
    append_u64_field(&writer, JSON_KEY_BLE_THREAT_EVIDENCE,
                     detection->ble_threat_evidence_mask);

    if (stamp) {
        append_u64_field(&writer, JSON_KEY_SEQ, stamp->sequence);
        append_prefix(&writer, "tv");
        backend_json_append(&writer, stamp->time_valid ? "true" : "false");
        if (stamp->time_valid) {
            append_i64_field(&writer, JSON_KEY_TIMESTAMP,
                             stamp->observed_epoch_ms);
        }
    }
    backend_json_append(&writer, "}");
    return backend_json_writer_finish(&writer);
}

static bool find_value(const char *json,
                       const backend_json_token_t *tokens,
                       size_t token_count,
                       const char *key,
                       size_t *out_index)
{
    return backend_json_object_find(
        json, tokens, token_count, 0U, key, out_index);
}

static bool copy_required_string(const char *json,
                                 const backend_json_token_t *tokens,
                                 size_t token_count,
                                 const char *key,
                                 char *output,
                                 size_t capacity)
{
    size_t index = 0;
    return find_value(json, tokens, token_count, key, &index) &&
           backend_json_copy_string(json, &tokens[index], output, capacity);
}

static bool copy_optional_string(const char *json,
                                 const backend_json_token_t *tokens,
                                 size_t token_count,
                                 const char *key,
                                 char *output,
                                 size_t capacity)
{
    size_t index = 0;
    if (!find_value(json, tokens, token_count, key, &index)) {
        return true;
    }
    return backend_json_copy_string(json, &tokens[index], output, capacity);
}

static bool get_required_u64(const char *json,
                             const backend_json_token_t *tokens,
                             size_t token_count,
                             const char *key,
                             uint64_t *out)
{
    size_t index = 0;
    return find_value(json, tokens, token_count, key, &index) &&
           backend_json_get_u64(json, &tokens[index], out);
}

static bool get_optional_u64(const char *json,
                             const backend_json_token_t *tokens,
                             size_t token_count,
                             const char *key,
                             uint64_t *out,
                             bool *present)
{
    size_t index = 0;
    *present = find_value(json, tokens, token_count, key, &index);
    return !*present || backend_json_get_u64(json, &tokens[index], out);
}

static bool get_optional_i64(const char *json,
                             const backend_json_token_t *tokens,
                             size_t token_count,
                             const char *key,
                             int64_t *out,
                             bool *present)
{
    size_t index = 0;
    *present = find_value(json, tokens, token_count, key, &index);
    return !*present || backend_json_get_i64(json, &tokens[index], out);
}

static bool get_required_double(const char *json,
                                const backend_json_token_t *tokens,
                                size_t token_count,
                                const char *key,
                                double *out)
{
    size_t index = 0;
    return find_value(json, tokens, token_count, key, &index) &&
           backend_json_get_double(json, &tokens[index], out);
}

static bool get_optional_double(const char *json,
                                const backend_json_token_t *tokens,
                                size_t token_count,
                                const char *key,
                                double *out,
                                bool *present)
{
    size_t index = 0;
    *present = find_value(json, tokens, token_count, key, &index);
    return !*present || backend_json_get_double(json, &tokens[index], out);
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool hex_to_bytes(const char *hex,
                         size_t expected_digits,
                         uint8_t *output)
{
    if (!hex || strlen(hex) != expected_digits ||
        (expected_digits % 2U) != 0U) {
        return false;
    }
    for (size_t i = 0; i < expected_digits / 2U; ++i) {
        int high = hex_nibble(hex[i * 2U]);
        int low = hex_nibble(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool get_optional_hex32(const char *json,
                               const backend_json_token_t *tokens,
                               size_t token_count,
                               const char *key,
                               uint32_t *output)
{
    size_t index = 0;
    if (!find_value(json, tokens, token_count, key, &index)) {
        return true;
    }
    char hex[9];
    uint8_t bytes[4];
    if (!backend_json_copy_string(
            json, &tokens[index], hex, sizeof(hex)) ||
        !hex_to_bytes(hex, 8U, bytes)) {
        return false;
    }
    *output = ((uint32_t)bytes[0] << 24) |
              ((uint32_t)bytes[1] << 16) |
              ((uint32_t)bytes[2] << 8) |
              bytes[3];
    return true;
}

static bool decode_u16_array(const char *json,
                             const backend_json_token_t *tokens,
                             size_t token_count,
                             size_t array_index,
                             uint16_t output[4])
{
    if (tokens[array_index].kind != BACKEND_JSON_ARRAY ||
        tokens[array_index].child_count != 4U) {
        return false;
    }
    size_t found = 0;
    for (size_t i = array_index + 1U;
         i < token_count && found < 4U; ++i) {
        if (tokens[i].parent != (int16_t)array_index) {
            continue;
        }
        uint64_t value = 0;
        if (!backend_json_get_u64(json, &tokens[i], &value) ||
            value > UINT16_MAX) {
            return false;
        }
        output[found++] = (uint16_t)value;
    }
    return found == 4U;
}

static bool decode_u128_array(const char *json,
                              const backend_json_token_t *tokens,
                              size_t token_count,
                              size_t array_index,
                              uint8_t output[2][16])
{
    if (tokens[array_index].kind != BACKEND_JSON_ARRAY ||
        tokens[array_index].child_count != 2U) {
        return false;
    }
    size_t found = 0;
    for (size_t i = array_index + 1U;
         i < token_count && found < 2U; ++i) {
        if (tokens[i].parent != (int16_t)array_index) {
            continue;
        }
        char hex[33];
        if (!backend_json_copy_string(
                json, &tokens[i], hex, sizeof(hex)) ||
            !hex_to_bytes(hex, 32U, output[found])) {
            return false;
        }
        ++found;
    }
    return found == 2U;
}

backend_detection_decode_result_t backend_detection_uart_decode(
    const char *json,
    size_t length,
    backend_scanner_slot_t slot,
    drone_detection_t *out_detection,
    backend_scanner_stamp_t *out_stamp)
{
    if (length > BACKEND_DETECTION_UART_MAX_LINE) {
        return BACKEND_DECODE_TOO_LARGE;
    }
    if (!json || !out_detection || !out_stamp || length == 0U ||
        (slot != BACKEND_SCANNER_SLOT_BLE &&
         slot != BACKEND_SCANNER_SLOT_WIFI)) {
        return BACKEND_DECODE_MALFORMED;
    }
    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0;
    backend_json_result_t parsed = backend_json_parse(
        json, length, tokens, BACKEND_JSON_MAX_TOKENS, &token_count);
    if (parsed != BACKEND_JSON_OK || token_count == 0U ||
        tokens[0].kind != BACKEND_JSON_OBJECT) {
        return BACKEND_DECODE_MALFORMED;
    }

    drone_detection_t detection = {0};
    backend_scanner_stamp_t stamp = {0};
    detection.wifi_auth_mode = UINT8_MAX;
    char type[16];
    uint64_t unsigned_value = 0;
    double double_value = 0.0;
    if (!copy_required_string(json, tokens, token_count, JSON_KEY_TYPE,
                              type, sizeof(type)) ||
        strcmp(type, MSG_TYPE_DETECTION) != 0 ||
        !copy_required_string(json, tokens, token_count, JSON_KEY_DRONE_ID,
                              detection.drone_id,
                              sizeof(detection.drone_id)) ||
        detection.drone_id[0] == '\0' ||
        !get_required_u64(json, tokens, token_count, JSON_KEY_SOURCE,
                          &unsigned_value) ||
        unsigned_value > DETECTION_SRC_WIFI_AP_INVENTORY) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    detection.source = (uint8_t)unsigned_value;
    if (!get_required_double(json, tokens, token_count,
                             JSON_KEY_CONFIDENCE, &double_value) ||
        !isfinite(double_value) || double_value < 0.0 ||
        double_value > 1.0) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    detection.confidence = (float)double_value;

    size_t ignored = 0;
    if (find_value(json, tokens, token_count, "scanner_slot", &ignored) ||
        find_value(json, tokens, token_count, "scanner_slots_seen",
                   &ignored)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }

#define READ_STRING(key, member) do { \
    if (!copy_optional_string(json, tokens, token_count, key, \
            detection.member, sizeof(detection.member))) { \
        return BACKEND_DECODE_SCHEMA_MISMATCH; \
    } \
} while (0)
#define READ_DOUBLE(key, member) do { \
    bool present_ = false; \
    double value_ = 0.0; \
    if (!get_optional_double(json, tokens, token_count, key, \
            &value_, &present_) || (present_ && !isfinite(value_))) { \
        return BACKEND_DECODE_SCHEMA_MISMATCH; \
    } \
    if (present_) detection.member = value_; \
} while (0)
#define READ_FLOAT(key, member) do { \
    bool present_ = false; \
    double value_ = 0.0; \
    if (!get_optional_double(json, tokens, token_count, key, \
            &value_, &present_) || (present_ && !isfinite(value_))) { \
        return BACKEND_DECODE_SCHEMA_MISMATCH; \
    } \
    if (present_) detection.member = (float)value_; \
} while (0)
#define READ_I64_RANGE(key, member, minimum, maximum, cast_type) do { \
    bool present_ = false; \
    int64_t value_ = 0; \
    if (!get_optional_i64(json, tokens, token_count, key, \
            &value_, &present_) || \
        (present_ && (value_ < (int64_t)(minimum) || \
                      value_ > (int64_t)(maximum)))) { \
        return BACKEND_DECODE_SCHEMA_MISMATCH; \
    } \
    if (present_) detection.member = (cast_type)value_; \
} while (0)
#define READ_U64_RANGE(key, member, maximum, cast_type) do { \
    bool present_ = false; \
    uint64_t value_ = 0; \
    if (!get_optional_u64(json, tokens, token_count, key, \
            &value_, &present_) || (present_ && value_ > (uint64_t)(maximum))) { \
        return BACKEND_DECODE_SCHEMA_MISMATCH; \
    } \
    if (present_) detection.member = (cast_type)value_; \
} while (0)

    READ_DOUBLE(JSON_KEY_LATITUDE, latitude);
    READ_DOUBLE(JSON_KEY_LONGITUDE, longitude);
    READ_DOUBLE(JSON_KEY_ALTITUDE, altitude_m);
    READ_FLOAT(JSON_KEY_HEADING, heading_deg);
    READ_FLOAT(JSON_KEY_SPEED, speed_mps);
    READ_FLOAT(JSON_KEY_VSPEED, vertical_speed_mps);
    READ_I64_RANGE(JSON_KEY_RSSI, rssi, INT8_MIN, INT8_MAX, int8_t);
    READ_DOUBLE(JSON_KEY_DISTANCE, estimated_distance_m);
    READ_STRING(JSON_KEY_MANUFACTURER, manufacturer);
    READ_STRING(JSON_KEY_MODEL, model);
    READ_DOUBLE(JSON_KEY_OPERATOR_LAT, operator_lat);
    READ_DOUBLE(JSON_KEY_OPERATOR_LON, operator_lon);
    READ_STRING(JSON_KEY_OPERATOR_ID, operator_id);
    READ_U64_RANGE(JSON_KEY_UA_TYPE, ua_type, UINT8_MAX, uint8_t);
    READ_U64_RANGE(JSON_KEY_ID_TYPE, id_type, UINT8_MAX, uint8_t);
    READ_U64_RANGE("self_dt", self_id_desc_type, UINT8_MAX, uint8_t);
    READ_STRING(JSON_KEY_SELF_ID, self_id_text);
    READ_DOUBLE(JSON_KEY_HEIGHT_AGL, height_agl_m);
    READ_DOUBLE(JSON_KEY_GEODETIC_ALT, geodetic_alt_m);
    READ_FLOAT(JSON_KEY_H_ACCURACY, h_accuracy_m);
    READ_FLOAT(JSON_KEY_V_ACCURACY, v_accuracy_m);
    READ_U64_RANGE("area_n", area_count, UINT16_MAX, uint16_t);
    READ_U64_RANGE("area_r", area_radius, UINT16_MAX, uint16_t);
    READ_DOUBLE("area_c", area_ceiling);
    READ_DOUBLE("area_f", area_floor);
    READ_U64_RANGE("class_t", classification_type, UINT8_MAX, uint8_t);
    READ_STRING(JSON_KEY_SSID, ssid);
    READ_STRING(JSON_KEY_BSSID, bssid);
    READ_I64_RANGE(JSON_KEY_FREQ, freq_mhz, 0, INT32_MAX, int32_t);
    READ_I64_RANGE(JSON_KEY_CHANNEL_WIDTH, channel_width_mhz,
                   0, INT32_MAX, int32_t);
    READ_U64_RANGE(JSON_KEY_WIFI_AUTH_MODE, wifi_auth_mode,
                   UINT8_MAX, uint8_t);
    if (detection.wifi_auth_mode != UINT8_MAX &&
        detection.wifi_auth_mode > 10U) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    READ_U64_RANGE("wifi_gen", wifi_generation, 6, uint8_t);
    READ_STRING(JSON_KEY_PROBED_SSIDS, probed_ssids);
    if (!get_optional_hex32(json, tokens, token_count, "ie_hash",
                            &detection.probe_ie_hash)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }

    READ_U64_RANGE(JSON_KEY_BLE_COMPANY_ID, ble_company_id,
                   UINT16_MAX, uint16_t);
    READ_U64_RANGE(JSON_KEY_BLE_APPLE_TYPE, ble_apple_type,
                   UINT8_MAX, uint8_t);
    size_t array_index = 0;
    if (find_value(json, tokens, token_count, "ble_u16", &array_index) &&
        !decode_u16_array(json, tokens, token_count, array_index,
                          detection.ble_service_uuids)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    READ_U64_RANGE("ble_u16_n", ble_svc_uuid_count, 4, uint8_t);
    if (find_value(json, tokens, token_count, "ble_u128", &array_index) &&
        !decode_u128_array(json, tokens, token_count, array_index,
                           detection.ble_service_uuids_128)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    READ_U64_RANGE("ble_u128_n", ble_svc_uuid_128_count, 2, uint8_t);
    READ_STRING(JSON_KEY_BLE_SVC_UUIDS, ble_svc_uuids_raw);
    READ_U64_RANGE(JSON_KEY_BLE_AD_TYPES, ble_ad_type_count,
                   UINT8_MAX, uint8_t);
    READ_U64_RANGE(JSON_KEY_BLE_PAYLOAD_LEN, ble_payload_len,
                   UINT8_MAX, uint8_t);
    READ_U64_RANGE(JSON_KEY_BLE_ADDR_TYPE, ble_addr_type, 3, uint8_t);
    if (!get_optional_hex32(json, tokens, token_count, JSON_KEY_BLE_JA3,
                            &detection.ble_ja3_hash)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    READ_STRING(JSON_KEY_BLE_NAME, ble_name);
    READ_STRING(JSON_KEY_CLASS_REASON, class_reason);

    size_t string_index = 0;
    if (find_value(json, tokens, token_count, JSON_KEY_BLE_APPLE_AUTH,
                   &string_index)) {
        char auth[7];
        if (!backend_json_copy_string(json, &tokens[string_index], auth,
                                      sizeof(auth)) ||
            !hex_to_bytes(auth, 6U, detection.ble_apple_auth)) {
            return BACKEND_DECODE_SCHEMA_MISMATCH;
        }
    }
    READ_U64_RANGE(JSON_KEY_BLE_ACTIVITY, ble_apple_activity,
                   UINT8_MAX, uint8_t);
    READ_U64_RANGE(JSON_KEY_BLE_APPLE_FLAGS, ble_apple_flags,
                   UINT8_MAX, uint8_t);
    bool raw_mfr_present = find_value(
        json, tokens, token_count, JSON_KEY_BLE_RAW_MFR, &string_index);
    size_t raw_bytes = 0;
    if (raw_mfr_present) {
        char raw[41];
        if (!backend_json_copy_string(json, &tokens[string_index], raw,
                                      sizeof(raw))) {
            return BACKEND_DECODE_SCHEMA_MISMATCH;
        }
        size_t digits = strlen(raw);
        if (digits == 0U || digits > 40U || (digits % 2U) != 0U ||
            !hex_to_bytes(raw, digits, detection.ble_raw_mfr)) {
            return BACKEND_DECODE_SCHEMA_MISMATCH;
        }
        raw_bytes = digits / 2U;
        detection.ble_raw_mfr_len = (uint8_t)raw_bytes;
    }
    bool mfr_len_present = false;
    uint64_t mfr_len = 0;
    if (!get_optional_u64(json, tokens, token_count, "ble_mfr_n",
                          &mfr_len, &mfr_len_present) ||
        (mfr_len_present &&
         (mfr_len > 20U || !raw_mfr_present || mfr_len > raw_bytes))) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    if (mfr_len_present) {
        detection.ble_raw_mfr_len = (uint8_t)mfr_len;
    }
    READ_I64_RANGE(JSON_KEY_BLE_ADV_INTERVAL, ble_adv_interval_us,
                   0, INT64_MAX, int64_t);
    READ_I64_RANGE(JSON_KEY_FIRST_SEEN, first_seen_ms,
                   0, INT64_MAX, int64_t);
    READ_I64_RANGE(JSON_KEY_LAST_UPDATED, last_updated_ms,
                   0, INT64_MAX, int64_t);
    READ_FLOAT(JSON_KEY_FUSED_CONFIDENCE, fused_confidence);
    READ_U64_RANGE(JSON_KEY_BLE_THREAT_KIND, ble_threat_kind,
                   BLE_THREAT_KIND_SERIAL_SKIMMER, uint8_t);
    READ_U64_RANGE(JSON_KEY_BLE_PROMPT_FAMILIES, ble_prompt_family_mask,
                   UINT8_MAX, uint8_t);
    READ_U64_RANGE(JSON_KEY_BLE_UNIQUE_MACS, ble_unique_macs,
                   UINT16_MAX, uint16_t);
    READ_U64_RANGE(JSON_KEY_BLE_OBSERVATIONS, ble_observation_count,
                   UINT16_MAX, uint16_t);
    READ_U64_RANGE(JSON_KEY_BLE_SERIAL_UUID, ble_serial_service_uuid,
                   UINT16_MAX, uint16_t);
    READ_U64_RANGE(JSON_KEY_BLE_THREAT_EVIDENCE,
                   ble_threat_evidence_mask, UINT8_MAX, uint8_t);

    bool seq_present = false;
    if (!get_optional_u64(json, tokens, token_count, JSON_KEY_SEQ,
                          &unsigned_value, &seq_present) ||
        unsigned_value > UINT32_MAX) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    if (seq_present) {
        stamp.sequence = (uint32_t)unsigned_value;
    }
    bool tv_present = find_value(json, tokens, token_count, "tv",
                                 &string_index);
    if (tv_present && !backend_json_get_bool(
            json, &tokens[string_index], &stamp.time_valid)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    bool ts_present = false;
    if (!get_optional_i64(json, tokens, token_count, JSON_KEY_TIMESTAMP,
                          &stamp.observed_epoch_ms, &ts_present) ||
        (ts_present &&
         stamp.observed_epoch_ms < BACKEND_DETECTION_EPOCH_MIN_MS) ||
        (tv_present && stamp.time_valid != ts_present)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    if (!tv_present && ts_present) {
        stamp.time_valid = true;
    }

    detection.scanner_slot = (uint8_t)slot;
    detection.scanner_slots_seen = (uint8_t)(1U << slot);
    if (!valid_detection(&detection)) {
        return BACKEND_DECODE_SCHEMA_MISMATCH;
    }
    *out_detection = detection;
    *out_stamp = stamp;
    return BACKEND_DECODE_OK;

#undef READ_STRING
#undef READ_DOUBLE
#undef READ_FLOAT
#undef READ_I64_RANGE
#undef READ_U64_RANGE
}
