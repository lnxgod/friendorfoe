#include "backend_upload_batch.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "backend_identity.h"
#include "backend_json_writer.h"

#define BACKEND_UPLOAD_IDLE_FLUSH_MS INT64_C(80)

static bool terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool utf8_sequence_valid(const uint8_t *value, size_t remaining,
                                size_t *sequence_length)
{
    const uint8_t first = value[0];
    if (first < 0x80U) {
        *sequence_length = 1U;
        return true;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
        if (remaining < 2U || (value[1] & 0xc0U) != 0x80U) {
            return false;
        }
        *sequence_length = 2U;
        return true;
    }
    if (first >= 0xe0U && first <= 0xefU) {
        if (remaining < 3U || (value[1] & 0xc0U) != 0x80U ||
            (value[2] & 0xc0U) != 0x80U ||
            (first == 0xe0U && value[1] < 0xa0U) ||
            (first == 0xedU && value[1] >= 0xa0U)) {
            return false;
        }
        *sequence_length = 3U;
        return true;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
        if (remaining < 4U || (value[1] & 0xc0U) != 0x80U ||
            (value[2] & 0xc0U) != 0x80U ||
            (value[3] & 0xc0U) != 0x80U ||
            (first == 0xf0U && value[1] < 0x90U) ||
            (first == 0xf4U && value[1] >= 0x90U)) {
            return false;
        }
        *sequence_length = 4U;
        return true;
    }
    return false;
}

static bool valid_text(const char *value, size_t capacity)
{
    if (!terminated(value, capacity)) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)value;
    size_t remaining = strlen(value);
    while (remaining > 0U) {
        size_t sequence_length = 0U;
        if (!utf8_sequence_valid(bytes, remaining, &sequence_length)) {
            return false;
        }
        bytes += sequence_length;
        remaining -= sequence_length;
    }
    return true;
}

static bool hex_char(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool valid_mac(const char *value)
{
    if (value == NULL || strlen(value) != 17U) {
        return false;
    }
    for (size_t i = 0U; i < 17U; ++i) {
        if ((i + 1U) % 3U == 0U) {
            if (value[i] != ':') {
                return false;
            }
        } else if (!hex_char(value[i])) {
            return false;
        }
    }
    return true;
}

static const char *profile_name(backend_scan_profile_t profile)
{
    switch (profile) {
    case BACKEND_SCAN_PROFILE_QUIESCENT:
        return "quiescent";
    case BACKEND_SCAN_PROFILE_BLE_PRIMARY:
        return "ble_primary";
    case BACKEND_SCAN_PROFILE_WIFI_PRIMARY:
        return "wifi_primary";
    case BACKEND_SCAN_PROFILE_HYBRID_FAILOVER:
        return "hybrid_failover";
    default:
        return NULL;
    }
}

static const char *led_state_name(backend_led_state_t state)
{
    switch (state) {
    case BACKEND_LED_HEALTHY:
        return "healthy";
    case BACKEND_LED_NETWORK_DEGRADED:
        return "network_degraded";
    case BACKEND_LED_DRONE:
        return "drone";
    case BACKEND_LED_META:
        return "meta";
    case BACKEND_LED_DRONE_META:
        return "drone_meta";
    case BACKEND_LED_FATAL:
        return "fatal";
    case BACKEND_LED_UART_LOST:
        return "uart_lost";
    default:
        return NULL;
    }
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

static bool valid_raw_uuid_list(const char *value);

static bool valid_probe_tokens(const drone_detection_t *value)
{
    if (value->source != DETECTION_SRC_WIFI_PROBE_REQUEST) {
        return true;
    }
    const char *cursor = value->probed_ssids;
    bool found = false;
    while (*cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length > 32U) {
            return false;
        }
        found = found || length > 0U;
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }
    return found || value->ssid[0] != '\0';
}

static bool valid_detection(const drone_detection_t *value)
{
    if (value == NULL ||
        !valid_text(value->drone_id, sizeof(value->drone_id)) ||
        value->drone_id[0] == '\0' ||
        !valid_text(value->manufacturer, sizeof(value->manufacturer)) ||
        !valid_text(value->model, sizeof(value->model)) ||
        !valid_text(value->operator_id, sizeof(value->operator_id)) ||
        !valid_text(value->self_id_text, sizeof(value->self_id_text)) ||
        !valid_text(value->ssid, sizeof(value->ssid)) ||
        !valid_text(value->bssid, sizeof(value->bssid)) ||
        !valid_text(value->ble_svc_uuids_raw,
                    sizeof(value->ble_svc_uuids_raw)) ||
        !valid_text(value->ble_name, sizeof(value->ble_name)) ||
        !valid_text(value->class_reason, sizeof(value->class_reason)) ||
        !valid_text(value->probed_ssids, sizeof(value->probed_ssids)) ||
        source_name(value->source) == NULL ||
        !isfinite(value->confidence) || value->confidence < 0.0f ||
        value->confidence > 1.0f ||
        !isfinite(value->fused_confidence) ||
        value->fused_confidence < 0.0f ||
        value->fused_confidence > 1.0f ||
        !isfinite(value->latitude) || value->latitude < -90.0 ||
        value->latitude > 90.0 ||
        !isfinite(value->longitude) || value->longitude < -180.0 ||
        value->longitude > 180.0 || !isfinite(value->altitude_m) ||
        !isfinite(value->heading_deg) || value->heading_deg < 0.0f ||
        value->heading_deg > 360.0f || !isfinite(value->speed_mps) ||
        !isfinite(value->vertical_speed_mps) ||
        !isfinite(value->estimated_distance_m) ||
        value->estimated_distance_m < 0.0 ||
        !isfinite(value->operator_lat) || value->operator_lat < -90.0 ||
        value->operator_lat > 90.0 || !isfinite(value->operator_lon) ||
        value->operator_lon < -180.0 || value->operator_lon > 180.0 ||
        !isfinite(value->height_agl_m) ||
        !isfinite(value->geodetic_alt_m) ||
        !isfinite(value->h_accuracy_m) || value->h_accuracy_m < 0.0f ||
        !isfinite(value->v_accuracy_m) || value->v_accuracy_m < 0.0f ||
        !isfinite(value->area_ceiling) || !isfinite(value->area_floor) ||
        value->freq_mhz < 0 || value->channel_width_mhz < 0 ||
        value->ble_svc_uuid_count > 4U ||
        value->ble_svc_uuid_128_count > 2U ||
        value->ble_addr_type > 3U || value->ble_raw_mfr_len > 20U ||
        value->ble_adv_interval_us < 0 ||
        (value->first_seen_ms != 0 &&
         value->first_seen_ms < BACKEND_DETECTION_EPOCH_MIN_MS) ||
        (value->last_updated_ms != 0 &&
         value->last_updated_ms < BACKEND_DETECTION_EPOCH_MIN_MS) ||
        value->wifi_generation > 6U ||
        (value->wifi_auth_mode != UINT8_MAX &&
         value->wifi_auth_mode > 10U) ||
        value->scanner_slot > BACKEND_SCANNER_SLOT_WIFI ||
        value->scanner_slots_seen > 3U ||
        value->ble_threat_kind > BLE_THREAT_KIND_SERIAL_SKIMMER ||
        !valid_probe_tokens(value)) {
        return false;
    }
    return true;
}

static bool valid_scanner(const backend_scanner_status_t *status)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);
    return status != NULL &&
           status->schema == BACKEND_SCANNER_STATUS_SCHEMA &&
           status->sequence != 0U && status->boot_id != 0U &&
           valid_text(status->mac, sizeof(status->mac)) &&
           valid_mac(status->mac) &&
           valid_text(status->target, sizeof(status->target)) &&
           valid_text(status->project, sizeof(status->project)) &&
           valid_text(status->hardware, sizeof(status->hardware)) &&
           valid_text(status->version, sizeof(status->version)) &&
           status->version[0] != '\0' &&
           valid_text(status->ota_state, sizeof(status->ota_state)) &&
           status->ota_state[0] != '\0' &&
           valid_text(status->rollback_state,
                      sizeof(status->rollback_state)) &&
           status->rollback_state[0] != '\0' &&
           profile_name(status->profile) != NULL &&
           backend_identity_matches(identity, status->target,
                                    status->project, status->hardware);
}

static bool valid_context(const backend_batch_context_t *context)
{
    if (context == NULL || context->sequence == 0U ||
        context->capability_count > 16U ||
        !valid_text(context->device_id, sizeof(context->device_id)) ||
        context->device_id[0] == '\0' ||
        !valid_text(context->firmware_version,
                    sizeof(context->firmware_version)) ||
        context->firmware_version[0] == '\0' ||
        !valid_text(context->firmware_target,
                    sizeof(context->firmware_target)) ||
        !valid_text(context->app_project, sizeof(context->app_project)) ||
        !valid_text(context->hardware_type,
                    sizeof(context->hardware_type)) ||
        !valid_text(context->hardware_mac,
                    sizeof(context->hardware_mac)) ||
        !valid_mac(context->hardware_mac) ||
        !valid_text(context->node_name, sizeof(context->node_name)) ||
        !valid_text(context->wifi_ssid, sizeof(context->wifi_ssid)) ||
        led_state_name(context->led_state) == NULL ||
        context->upload_queue.capacity_batches == 0U ||
        context->upload_queue.capacity_batches >
            BACKEND_UPLOAD_FIFO_CAPACITY ||
        context->upload_queue.depth_batches >
            context->upload_queue.capacity_batches ||
        (context->clock_valid &&
         context->epoch_ms < BACKEND_DETECTION_EPOCH_MIN_MS)) {
        return false;
    }
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    if (!backend_identity_matches(identity, context->firmware_target,
                                  context->app_project,
                                  context->hardware_type)) {
        return false;
    }
    if (context->has_device_location &&
        (!isfinite(context->device_lat) || context->device_lat < -90.0 ||
         context->device_lat > 90.0 || !isfinite(context->device_lon) ||
         context->device_lon < -180.0 || context->device_lon > 180.0 ||
         !isfinite(context->device_alt))) {
        return false;
    }
    for (size_t i = 0U; i < context->capability_count; ++i) {
        if (!valid_text(context->capabilities[i],
                        sizeof(context->capabilities[i])) ||
            context->capabilities[i][0] == '\0') {
            return false;
        }
    }
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (context->scanner_present[slot] &&
            !valid_scanner(&context->scanners[slot])) {
            return false;
        }
    }
    return true;
}

static bool append_key(backend_json_writer_t *writer, const char *key)
{
    return backend_json_append_format(writer, ",\"%s\":", key);
}

static bool append_string(backend_json_writer_t *writer,
                          const char *key, const char *value)
{
    return append_key(writer, key) &&
           backend_json_append_escaped(writer, value);
}

static bool append_i64(backend_json_writer_t *writer,
                       const char *key, int64_t value)
{
    return append_key(writer, key) &&
           backend_json_append_format(writer, "%" PRId64, value);
}

static bool append_u64(backend_json_writer_t *writer,
                       const char *key, uint64_t value)
{
    return append_key(writer, key) &&
           backend_json_append_format(writer, "%" PRIu64, value);
}

static bool append_double(backend_json_writer_t *writer,
                          const char *key, double value)
{
    return append_key(writer, key) &&
           backend_json_append_format(writer, "%.17g", value);
}

static bool append_float(backend_json_writer_t *writer,
                         const char *key, float value)
{
    return append_key(writer, key) &&
           backend_json_append_format(writer, "%.9g", (double)value);
}

static bool append_bool(backend_json_writer_t *writer,
                        const char *key, bool value)
{
    return append_key(writer, key) &&
           backend_json_append(writer, value ? "true" : "false");
}

static void bytes_to_hex(const uint8_t *bytes, size_t count, char *output)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0U; i < count; ++i) {
        output[i * 2U] = hex[bytes[i] >> 4U];
        output[i * 2U + 1U] = hex[bytes[i] & 0x0fU];
    }
    output[count * 2U] = '\0';
}

static bool valid_raw_uuid_list(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    const char *token = value;
    size_t token_count = 0U;
    while (*token != '\0') {
        if (++token_count > 6U) {
            return false;
        }
        const char *end = strchr(token, ',');
        size_t length = end ? (size_t)(end - token) : strlen(token);
        bool valid = length == 4U || length == 36U;
        for (size_t i = 0U; valid && i < length; ++i) {
            bool hyphen = length == 36U &&
                (i == 8U || i == 13U || i == 18U || i == 23U);
            if (hyphen ? token[i] != '-' : !hex_char(token[i])) {
                valid = false;
            }
        }
        if (!valid) {
            return false;
        }
        if (end == NULL) {
            return true;
        }
        token = end + 1;
        if (*token == '\0') {
            return false;
        }
    }
    return true;
}

static bool format_uuid_list(const drone_detection_t *value,
                             char *output, size_t capacity)
{
    if (valid_raw_uuid_list(value->ble_svc_uuids_raw)) {
        size_t length = strlen(value->ble_svc_uuids_raw);
        if (length >= capacity) {
            return false;
        }
        for (size_t i = 0U; i < length; ++i) {
            char value_char = value->ble_svc_uuids_raw[i];
            output[i] = value_char >= 'A' && value_char <= 'F' ?
                (char)(value_char + ('a' - 'A')) : value_char;
        }
        output[length] = '\0';
        return true;
    }
    output[0] = '\0';
    size_t offset = 0U;
    for (size_t i = 0U; i < value->ble_svc_uuid_count; ++i) {
        int written = snprintf(output + offset, capacity - offset,
            "%s%04x", offset == 0U ? "" : ",",
            value->ble_service_uuids[i]);
        if (written < 0 || (size_t)written >= capacity - offset) {
            return false;
        }
        offset += (size_t)written;
    }
    for (size_t i = 0U; i < value->ble_svc_uuid_128_count; ++i) {
        const uint8_t *u = value->ble_service_uuids_128[i];
        int written = snprintf(output + offset, capacity - offset,
            "%s%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x",
            offset == 0U ? "" : ",",
            u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
            u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
        if (written < 0 || (size_t)written >= capacity - offset) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool append_probed_ssids(backend_json_writer_t *writer,
                                const drone_detection_t *value)
{
    if (!append_key(writer, "probed_ssids") ||
        !backend_json_append(writer, "[")) {
        return false;
    }
    const char *cursor = value->probed_ssids;
    bool wrote = false;
    while (*cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length > 0U) {
            char token[33];
            memcpy(token, cursor, length);
            token[length] = '\0';
            if ((wrote && !backend_json_append(writer, ",")) ||
                !backend_json_append_escaped(writer, token)) {
                return false;
            }
            wrote = true;
        }
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }
    if (!wrote && value->ssid[0] != '\0') {
        if (!backend_json_append_escaped(writer, value->ssid)) {
            return false;
        }
    }
    return backend_json_append(writer, "]");
}

static size_t encode_detection(const backend_detection_observation_t *item,
                               char *output, size_t capacity)
{
    const drone_detection_t *value = item ? &item->detection : NULL;
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (output == NULL || capacity == 0U || item == NULL ||
        !valid_detection(value)) {
        return 0U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append(&writer, "{\"drone_id\":");
    backend_json_append_escaped(&writer, value->drone_id);
    append_string(&writer, "source", source_name(value->source));
    append_float(&writer, "confidence", value->confidence);
    if (value->fused_confidence > 0.0f) {
        append_float(&writer, "fused_confidence", value->fused_confidence);
    }
    append_double(&writer, "latitude", value->latitude);
    append_double(&writer, "longitude", value->longitude);
    append_double(&writer, "altitude_m", value->altitude_m);
    append_float(&writer, "heading_deg", value->heading_deg);
    append_float(&writer, "speed_mps", value->speed_mps);
    append_float(&writer, "vertical_speed_mps", value->vertical_speed_mps);
    append_i64(&writer, "rssi", value->rssi);
    append_double(&writer, "estimated_distance_m",
                  value->estimated_distance_m);
    if (value->manufacturer[0] != '\0') {
        append_string(&writer, "manufacturer", value->manufacturer);
    }
    if (value->model[0] != '\0') {
        append_string(&writer, "model", value->model);
    }
    append_double(&writer, "operator_lat", value->operator_lat);
    append_double(&writer, "operator_lon", value->operator_lon);
    if (value->operator_id[0] != '\0') {
        append_string(&writer, "operator_id", value->operator_id);
    }
    append_u64(&writer, "ua_type", value->ua_type);
    append_u64(&writer, "id_type", value->id_type);
    append_u64(&writer, "self_id_desc_type", value->self_id_desc_type);
    if (value->self_id_text[0] != '\0') {
        append_string(&writer, "self_id_text", value->self_id_text);
    }
    append_double(&writer, "height_agl_m", value->height_agl_m);
    append_double(&writer, "geodetic_alt_m", value->geodetic_alt_m);
    append_float(&writer, "h_accuracy_m", value->h_accuracy_m);
    append_float(&writer, "v_accuracy_m", value->v_accuracy_m);
    append_u64(&writer, "area_count", value->area_count);
    append_u64(&writer, "area_radius", value->area_radius);
    append_double(&writer, "area_ceiling", value->area_ceiling);
    append_double(&writer, "area_floor", value->area_floor);
    append_u64(&writer, "classification_type", value->classification_type);
    if (value->ssid[0] != '\0') {
        append_string(&writer, "ssid", value->ssid);
    }
    if (value->bssid[0] != '\0') {
        append_string(&writer, "bssid", value->bssid);
    }
    if (value->freq_mhz > 0) {
        append_i64(&writer, "freq_mhz", value->freq_mhz);
        int channel = backend_detection_wifi_channel(value->freq_mhz);
        if (channel > 0) {
            append_i64(&writer, "channel", channel);
        }
    }
    if (value->channel_width_mhz > 0) {
        append_i64(&writer, "channel_width_mhz",
                   value->channel_width_mhz);
    }
    if (source_is_wifi(value->source)) {
        if (value->wifi_auth_mode != UINT8_MAX) {
            append_u64(&writer, "auth_m", value->wifi_auth_mode);
        }
        append_u64(&writer, "wifi_generation", value->wifi_generation);
    }
    if (value->source == DETECTION_SRC_WIFI_PROBE_REQUEST) {
        append_probed_ssids(&writer, value);
    }
    if (value->probe_ie_hash != 0U) {
        char hash[9];
        snprintf(hash, sizeof(hash), "%08" PRIx32, value->probe_ie_hash);
        append_string(&writer, "ie_hash", hash);
    }
    if (value->ble_company_id != 0U) {
        append_u64(&writer, "ble_company_id", value->ble_company_id);
    }
    if (value->ble_apple_type != 0U) {
        append_u64(&writer, "ble_apple_type", value->ble_apple_type);
    }
    if (value->ble_ad_type_count != 0U) {
        append_u64(&writer, "ble_ad_type_count", value->ble_ad_type_count);
    }
    if (value->ble_payload_len != 0U) {
        append_u64(&writer, "ble_payload_len", value->ble_payload_len);
    }
    if (source_is_ble(value->source)) {
        append_u64(&writer, "ble_addr_type", value->ble_addr_type);
    }
    if (value->ble_ja3_hash != 0U) {
        char hash[9];
        snprintf(hash, sizeof(hash), "%08" PRIx32, value->ble_ja3_hash);
        append_string(&writer, "ble_ja3", hash);
    }
    if (value->ble_name[0] != '\0') {
        append_string(&writer, "ble_name", value->ble_name);
    }
    if (value->class_reason[0] != '\0') {
        append_string(&writer, "class_reason", value->class_reason);
    }
    if (value->ble_apple_auth[0] != 0U ||
        value->ble_apple_auth[1] != 0U ||
        value->ble_apple_auth[2] != 0U) {
        char auth[7];
        bytes_to_hex(value->ble_apple_auth, 3U, auth);
        append_string(&writer, "ble_apple_auth", auth);
    }
    if (value->ble_company_id == 0x004cU || value->ble_apple_type != 0U ||
        value->ble_apple_auth[0] != 0U ||
        value->ble_apple_auth[1] != 0U ||
        value->ble_apple_auth[2] != 0U) {
        append_u64(&writer, "ble_activity", value->ble_apple_activity);
    }
    append_u64(&writer, "ble_apple_flags", value->ble_apple_flags);
    if (value->ble_raw_mfr_len > 0U) {
        char raw[41];
        bytes_to_hex(value->ble_raw_mfr, value->ble_raw_mfr_len, raw);
        append_string(&writer, "ble_raw_mfr", raw);
    }
    if (value->ble_adv_interval_us > 0) {
        append_double(&writer, "ble_adv_interval",
                      (double)value->ble_adv_interval_us / 1000.0);
    }
    char uuids[160];
    if (!format_uuid_list(value, uuids, sizeof(uuids))) {
        return 0U;
    }
    if (uuids[0] != '\0') {
        append_string(&writer, "ble_svc_uuids", uuids);
    }
    if (value->first_seen_ms >= BACKEND_DETECTION_EPOCH_MIN_MS) {
        append_i64(&writer, "first_seen_ms", value->first_seen_ms);
    }
    if (value->last_updated_ms >= BACKEND_DETECTION_EPOCH_MIN_MS) {
        append_i64(&writer, "last_updated_ms", value->last_updated_ms);
    }
    append_u64(&writer, "scanner_slot", value->scanner_slot);
    append_u64(&writer, "scanner_slots_seen", value->scanner_slots_seen);
    if (value->ble_threat_kind != BLE_THREAT_KIND_NONE) {
        append_u64(&writer, "ble_threat_kind", value->ble_threat_kind);
        append_u64(&writer, "ble_prompt_family_mask",
                   value->ble_prompt_family_mask);
        append_u64(&writer, "ble_unique_macs", value->ble_unique_macs);
        append_u64(&writer, "ble_observation_count",
                   value->ble_observation_count);
        append_u64(&writer, "ble_serial_service_uuid",
                   value->ble_serial_service_uuid);
        append_u64(&writer, "ble_threat_evidence_mask",
                   value->ble_threat_evidence_mask);
    }
    if (item->timestamp_valid &&
        item->timestamp_epoch_ms >= BACKEND_DETECTION_EPOCH_MIN_MS) {
        append_i64(&writer, "timestamp", item->timestamp_epoch_ms);
    }
    backend_json_append(&writer, "}");
    return backend_json_writer_finish(&writer);
}

static bool append_scanner(backend_json_writer_t *writer,
                           const backend_scanner_status_t *status,
                           size_t slot)
{
    const char *profile = profile_name(status->profile);
    bool radio_healthy = backend_scanner_required_radio_healthy(
        status->profile, status->ble_healthy, status->wifi_healthy);
    return backend_json_append(writer, "{\"uart\":") &&
           backend_json_append_escaped(writer, slot == 0U ? "ble" : "wifi") &&
           append_u64(writer, "slot", slot) &&
           append_string(writer, "firmware_target", status->target) &&
           append_string(writer, "app_project", status->project) &&
           append_string(writer, "hardware_type", status->hardware) &&
           append_string(writer, "firmware_version", status->version) &&
           append_string(writer, "mac", status->mac) &&
           append_u64(writer, "boot_id", status->boot_id) &&
           append_string(writer, "profile", profile) &&
           append_u64(writer, "status_sequence", status->sequence) &&
           append_u64(writer, "role_generation", status->role_generation) &&
           append_bool(writer, "role_acked", status->role_acked) &&
           append_bool(writer, "command_ingress", status->command_ingress) &&
           append_bool(writer, "radio_healthy", radio_healthy) &&
           append_bool(writer, "ble_healthy", status->ble_healthy) &&
           append_bool(writer, "wifi_healthy", status->wifi_healthy) &&
           append_string(writer, "ota_state", status->ota_state) &&
           append_string(writer, "rollback_state",
                         status->rollback_state) &&
           backend_json_append(writer, "}");
}

static size_t encode_prefix(const backend_batch_context_t *context,
                            char *output, size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (output == NULL || capacity == 0U || !valid_context(context)) {
        return 0U;
    }
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append(&writer, "{\"device_id\":");
    backend_json_append_escaped(&writer, context->device_id);
    append_string(&writer, "firmware_version", context->firmware_version);
    append_string(&writer, "firmware_target", context->firmware_target);
    append_string(&writer, "app_project", context->app_project);
    append_string(&writer, "hardware_type", context->hardware_type);
    append_string(&writer, "hardware_mac", context->hardware_mac);
    append_string(&writer, "node_name", context->node_name);
    append_key(&writer, "capabilities");
    backend_json_append(&writer, "[");
    for (size_t i = 0U; i < context->capability_count; ++i) {
        if (i > 0U) {
            backend_json_append(&writer, ",");
        }
        backend_json_append_escaped(&writer, context->capabilities[i]);
    }
    backend_json_append(&writer, "]");
    if (context->has_device_location) {
        append_double(&writer, "device_lat", context->device_lat);
        append_double(&writer, "device_lon", context->device_lon);
        append_double(&writer, "device_alt", context->device_alt);
    }
    if (context->clock_valid) {
        append_i64(&writer, "timestamp", context->epoch_ms / 1000);
    }
    append_key(&writer, "scanners");
    backend_json_append(&writer, "[");
    bool wrote_scanner = false;
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (!context->scanner_present[slot]) {
            continue;
        }
        if (wrote_scanner) {
            backend_json_append(&writer, ",");
        }
        append_scanner(&writer, &context->scanners[slot], slot);
        wrote_scanner = true;
    }
    backend_json_append(&writer, "]");
    append_string(&writer, "wifi_ssid", context->wifi_ssid);
    append_i64(&writer, "wifi_rssi", context->wifi_rssi);
    append_string(&writer, "led_state", led_state_name(context->led_state));
    append_key(&writer, "upload_queue");
    backend_json_append(&writer, "{\"depth_batches\":");
    backend_json_append_format(&writer, "%u",
                               context->upload_queue.depth_batches);
    append_u64(&writer, "capacity_batches",
               context->upload_queue.capacity_batches);
    append_u64(&writer, "overflow_dropped_batches",
               context->upload_queue.overflow_dropped_batches);
    append_u64(&writer, "quarantined_batches",
               context->upload_queue.quarantined_batches);
    backend_json_append(&writer, "}");
    append_key(&writer, "upload");
    backend_json_append(&writer, "{\"ok\":");
    backend_json_append_format(&writer, "%" PRIu32, context->upload.ok);
    append_u64(&writer, "failed", context->upload.failed);
    append_u64(&writer, "retry_count", context->upload.retry_count);
    if (context->upload.has_last_success_age) {
        append_u64(&writer, "last_success_age_s",
                   context->upload.last_success_age_s);
    }
    backend_json_append(&writer, "}");
    append_key(&writer, "health");
    backend_json_append(&writer, "{\"clock_valid\":");
    backend_json_append(&writer, context->clock_valid ? "true" : "false");
    if (context->clock_valid) {
        append_i64(&writer, "epoch_ms", context->epoch_ms);
    }
    append_bool(&writer, "ap_active", context->ap_active);
    append_u64(&writer, "config_generation", context->config_generation);
    append_u64(&writer, "command_success_count",
               context->command_success_count);
    append_u64(&writer, "command_failure_count",
               context->command_failure_count);
    append_u64(&writer, "uptime_ms", context->uptime_ms);
    backend_json_append(&writer, "}");
    append_key(&writer, "detections");
    backend_json_append(&writer, "[");
    size_t length = backend_json_writer_finish(&writer);
    if (length == 0U || length + 2U > BACKEND_UPLOAD_MAX_JSON) {
        output[0] = '\0';
        return 0U;
    }
    return length;
}

void backend_upload_builder_init(
    backend_upload_builder_t *builder,
    const backend_batch_context_t *context,
    int64_t now_ms)
{
    if (builder == NULL) {
        return;
    }
    memset(builder, 0, sizeof(*builder));
    if (now_ms < 0 || !valid_context(context)) {
        builder->failed = true;
        return;
    }
    builder->context = *context;
    builder->opened_ms = now_ms;
    builder->last_item_ms = now_ms;
    builder->active = true;
    builder->json_len = encode_prefix(
        &builder->context, builder->json, sizeof(builder->json));
    if (builder->json_len == 0U) {
        builder->active = false;
        builder->failed = true;
    }
}

backend_encode_result_t backend_upload_builder_add(
    backend_upload_builder_t *builder,
    const backend_detection_observation_t *observation,
    int64_t now_ms)
{
    if (builder == NULL || observation == NULL || !builder->active ||
        builder->failed || now_ms < 0 || now_ms < builder->last_item_ms ||
        !valid_detection(&observation->detection)) {
        return BACKEND_ENCODE_INVALID;
    }
    size_t separator = builder->item_count > 0U ? 1U : 0U;
    if (builder->json_len > BACKEND_UPLOAD_MAX_JSON - 2U ||
        separator > BACKEND_UPLOAD_MAX_JSON - 2U - builder->json_len) {
        return builder->item_count > 0U ? BACKEND_ENCODE_NEEDS_FLUSH :
                                         BACKEND_ENCODE_ITEM_TOO_LARGE;
    }
    size_t maximum_item_length = BACKEND_UPLOAD_MAX_JSON - 2U -
                                 builder->json_len - separator;
    size_t item_length = encode_detection(
        observation, builder->scratch, maximum_item_length + 1U);
    if (item_length == 0U) {
        memset(builder->scratch, 0, sizeof(builder->scratch));
        return builder->item_count > 0U ? BACKEND_ENCODE_NEEDS_FLUSH :
                                         BACKEND_ENCODE_ITEM_TOO_LARGE;
    }
    if (separator != 0U) {
        builder->json[builder->json_len++] = ',';
    }
    memcpy(builder->json + builder->json_len,
           builder->scratch, item_length);
    builder->json_len += item_length;
    builder->json[builder->json_len] = '\0';
    memset(builder->scratch, 0, item_length + 1U);
    ++builder->item_count;
    builder->last_item_ms = now_ms;
    return BACKEND_ENCODE_OK;
}

bool backend_upload_builder_finish(
    backend_upload_builder_t *builder,
    backend_upload_batch_t *out)
{
    if (builder == NULL || out == NULL || !builder->active ||
        builder->failed || builder->context.sequence == 0U ||
        builder->json_len + 2U > BACKEND_UPLOAD_MAX_JSON) {
        return false;
    }
    size_t next_prefix_length = encode_prefix(
        &builder->context, builder->scratch, sizeof(builder->scratch));
    if (next_prefix_length == 0U) {
        memset(builder->scratch, 0, sizeof(builder->scratch));
        return false;
    }

    builder->json[builder->json_len] = ']';
    builder->json[builder->json_len + 1U] = '}';
    builder->json[builder->json_len + 2U] = '\0';
    const uint16_t completed_length =
        (uint16_t)(builder->json_len + 2U);
    const uint32_t completed_crc = backend_identity_crc32(
        builder->json, completed_length);
    const uint32_t completed_sequence = builder->context.sequence;
    const uint16_t completed_item_count = builder->item_count;
    const uint32_t next_sequence = completed_sequence == UINT32_MAX ? 1U :
                                    completed_sequence + 1U;

    memset(out, 0, sizeof(*out));
    out->sequence = completed_sequence;
    out->item_count = completed_item_count;
    out->json_len = completed_length;
    out->json_crc32 = completed_crc;
    memcpy(out->json, builder->json, (size_t)completed_length + 1U);

    memcpy(builder->json, builder->scratch, next_prefix_length + 1U);
    memset(builder->scratch, 0, next_prefix_length + 1U);
    builder->json_len = next_prefix_length;
    builder->item_count = 0U;
    builder->opened_ms = builder->last_item_ms;
    builder->context.sequence = next_sequence;
    return true;
}

bool backend_upload_builder_tick(
    backend_upload_builder_t *builder,
    int64_t now_ms,
    backend_upload_batch_t *out)
{
    if (builder == NULL || out == NULL || !builder->active ||
        builder->failed || builder->item_count == 0U ||
        now_ms < 0 || builder->last_item_ms < 0 ||
        now_ms < builder->last_item_ms ||
        now_ms - builder->last_item_ms < BACKEND_UPLOAD_IDLE_FLUSH_MS) {
        return false;
    }
    return backend_upload_builder_finish(builder, out);
}
