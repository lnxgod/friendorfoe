#include "backend_config.h"

#include <math.h>
#include <string.h>

#define BACKEND_CONFIG_HEADER_SIZE 12U
#define BACKEND_CONFIG_CRC_SIZE 4U
#define BACKEND_CONFIG_AP_PASSWORD_MIN 8U
#define BACKEND_CONFIG_AP_PASSWORD_MAX 63U

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    size_t position;
    bool valid;
} backend_config_writer_t;

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t position;
    bool valid;
} backend_config_reader_t;

static bool bounded_string_length(
    const char *value, size_t capacity, size_t *out_length)
{
    if (!value || capacity == 0) {
        return false;
    }
    const char *terminator = memchr(value, '\0', capacity);
    if (!terminator) {
        return false;
    }
    if (out_length) {
        *out_length = (size_t)(terminator - value);
    }
    return true;
}

static bool backend_url_is_valid(const char *url, size_t length)
{
    static const char prefix[] = "http://";
    const size_t prefix_length = sizeof(prefix) - 1U;
    if (length <= prefix_length ||
        memcmp(url, prefix, prefix_length) != 0) {
        return false;
    }

    for (size_t index = prefix_length; index < length; ++index) {
        const unsigned char character = (unsigned char)url[index];
        if (character <= 0x20U || character == 0x7FU) {
            return false;
        }
    }

    size_t authority_end = prefix_length;
    while (authority_end < length && url[authority_end] != '/' &&
           url[authority_end] != '?' && url[authority_end] != '#') {
        authority_end++;
    }
    if (authority_end == prefix_length) {
        return false;
    }

    size_t host_start = prefix_length;
    for (size_t index = prefix_length; index < authority_end; ++index) {
        if (url[index] == '@') {
            host_start = index + 1U;
        }
    }
    if (host_start >= authority_end) {
        return false;
    }

    if (url[host_start] == '[') {
        size_t close_bracket = host_start + 1U;
        while (close_bracket < authority_end &&
               url[close_bracket] != ']') {
            close_bracket++;
        }
        if (close_bracket == host_start + 1U ||
            close_bracket == authority_end) {
            return false;
        }
        if (close_bracket + 1U == authority_end) {
            return true;
        }
        return url[close_bracket + 1U] == ':' &&
               close_bracket + 2U < authority_end;
    }

    size_t host_end = host_start;
    while (host_end < authority_end && url[host_end] != ':') {
        host_end++;
    }
    if (host_end == host_start) {
        return false;
    }
    return host_end == authority_end || host_end + 1U < authority_end;
}

backend_config_result_t backend_config_validate(
    const backend_config_record_t *record)
{
    if (!record || record->schema_version != BACKEND_CONFIG_SCHEMA_VERSION ||
        record->generation == 0 ||
        record->network_count > BACKEND_CONFIG_MAX_NETWORKS) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }

    for (uint8_t index = 0; index < record->network_count; ++index) {
        size_t ssid_length = 0;
        if (!bounded_string_length(record->networks[index].ssid,
                                   sizeof(record->networks[index].ssid),
                                   &ssid_length) ||
            ssid_length == 0 ||
            !bounded_string_length(record->networks[index].password,
                                   sizeof(record->networks[index].password),
                                   NULL)) {
            return BACKEND_CONFIG_INVALID_FIELD;
        }
    }

    size_t backend_url_length = 0;
    size_t device_id_length = 0;
    size_t ap_password_length = 0;
    if (!bounded_string_length(record->backend_url,
                               sizeof(record->backend_url),
                               &backend_url_length) ||
        !backend_url_is_valid(record->backend_url, backend_url_length) ||
        !bounded_string_length(record->device_id, sizeof(record->device_id),
                               &device_id_length) ||
        device_id_length == 0 ||
        !bounded_string_length(record->display_name,
                               sizeof(record->display_name), NULL) ||
        !bounded_string_length(record->ap_password,
                               sizeof(record->ap_password),
                               &ap_password_length) ||
        ap_password_length < BACKEND_CONFIG_AP_PASSWORD_MIN ||
        ap_password_length > BACKEND_CONFIG_AP_PASSWORD_MAX) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }

    if (record->has_location &&
        (!isfinite(record->latitude) || record->latitude < -90.0 ||
         record->latitude > 90.0 || !isfinite(record->longitude) ||
         record->longitude < -180.0 || record->longitude > 180.0 ||
         !isfinite(record->altitude_m))) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }

    return BACKEND_CONFIG_VALID;
}

static void writer_bytes(
    backend_config_writer_t *writer, const void *bytes, size_t length)
{
    if (!writer || !writer->valid || (!bytes && length != 0) ||
        length > writer->capacity - writer->position) {
        if (writer) {
            writer->valid = false;
        }
        return;
    }
    if (length != 0) {
        memcpy(writer->bytes + writer->position, bytes, length);
    }
    writer->position += length;
}

static void writer_u8(backend_config_writer_t *writer, uint8_t value)
{
    writer_bytes(writer, &value, sizeof(value));
}

static void writer_u16_le(backend_config_writer_t *writer, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value & UINT16_C(0x00FF)),
        (uint8_t)((value >> 8U) & UINT16_C(0x00FF)),
    };
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u32_le(backend_config_writer_t *writer, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value & UINT32_C(0x000000FF)),
        (uint8_t)((value >> 8U) & UINT32_C(0x000000FF)),
        (uint8_t)((value >> 16U) & UINT32_C(0x000000FF)),
        (uint8_t)((value >> 24U) & UINT32_C(0x000000FF)),
    };
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u64_le(backend_config_writer_t *writer, uint64_t value)
{
    uint8_t bytes[8];
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)((value >> (index * 8U)) & UINT64_C(0xFF));
    }
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_string(
    backend_config_writer_t *writer, const char *value, size_t capacity)
{
    size_t length = 0;
    if (!bounded_string_length(value, capacity, &length) ||
        length > UINT16_MAX) {
        writer->valid = false;
        return;
    }
    writer_u16_le(writer, (uint16_t)length);
    writer_bytes(writer, value, length);
}

static uint32_t canonical_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & UINT32_C(1)));
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

bool backend_config_encode_canonical(
    const backend_config_record_t *record, backend_config_blob_t *out)
{
    if (!out || backend_config_validate(record) != BACKEND_CONFIG_VALID) {
        return false;
    }

    uint8_t payload[BACKEND_CONFIG_PAYLOAD_MAX];
    backend_config_writer_t payload_writer = {
        .bytes = payload,
        .capacity = sizeof(payload),
        .position = 0,
        .valid = true,
    };
    writer_u8(&payload_writer, record->network_count);
    for (uint8_t index = 0; index < record->network_count; ++index) {
        writer_string(&payload_writer, record->networks[index].ssid,
                      sizeof(record->networks[index].ssid));
        writer_string(&payload_writer, record->networks[index].password,
                      sizeof(record->networks[index].password));
    }
    writer_string(&payload_writer, record->backend_url,
                  sizeof(record->backend_url));
    writer_string(&payload_writer, record->device_id,
                  sizeof(record->device_id));
    writer_string(&payload_writer, record->display_name,
                  sizeof(record->display_name));
    writer_string(&payload_writer, record->ap_password,
                  sizeof(record->ap_password));
    writer_u8(&payload_writer, record->auto_update_enabled ? 1U : 0U);
    writer_u8(&payload_writer, record->has_location ? 1U : 0U);

    double latitude = record->has_location ? record->latitude : 0.0;
    double longitude = record->has_location ? record->longitude : 0.0;
    float altitude_m = record->has_location ? record->altitude_m : 0.0f;
    uint64_t latitude_bits = 0;
    uint64_t longitude_bits = 0;
    uint32_t altitude_bits = 0;
    memcpy(&latitude_bits, &latitude, sizeof(latitude_bits));
    memcpy(&longitude_bits, &longitude, sizeof(longitude_bits));
    memcpy(&altitude_bits, &altitude_m, sizeof(altitude_bits));
    writer_u64_le(&payload_writer, latitude_bits);
    writer_u64_le(&payload_writer, longitude_bits);
    writer_u32_le(&payload_writer, altitude_bits);

    if (!payload_writer.valid ||
        payload_writer.position > BACKEND_CONFIG_PAYLOAD_MAX ||
        payload_writer.position > UINT16_MAX) {
        return false;
    }

    backend_config_blob_t encoded;
    memset(&encoded, 0, sizeof(encoded));
    backend_config_writer_t blob_writer = {
        .bytes = encoded.bytes,
        .capacity = sizeof(encoded.bytes),
        .position = 0,
        .valid = true,
    };
    writer_u32_le(&blob_writer, BACKEND_CONFIG_MAGIC);
    writer_u16_le(&blob_writer, BACKEND_CONFIG_SCHEMA_VERSION);
    writer_u16_le(&blob_writer, (uint16_t)payload_writer.position);
    writer_u32_le(&blob_writer, record->generation);
    writer_bytes(&blob_writer, payload, payload_writer.position);
    const uint32_t crc = canonical_crc32(
        encoded.bytes, blob_writer.position);
    writer_u32_le(&blob_writer, crc);
    if (!blob_writer.valid ||
        blob_writer.position > BACKEND_CONFIG_BLOB_MAX) {
        return false;
    }
    encoded.length = blob_writer.position;
    *out = encoded;
    return true;
}

static void reader_bytes(
    backend_config_reader_t *reader, void *out, size_t length)
{
    if (!reader || !reader->valid || (!out && length != 0) ||
        length > reader->length - reader->position) {
        if (reader) {
            reader->valid = false;
        }
        return;
    }
    if (length != 0) {
        memcpy(out, reader->bytes + reader->position, length);
    }
    reader->position += length;
}

static uint8_t reader_u8(backend_config_reader_t *reader)
{
    uint8_t value = 0;
    reader_bytes(reader, &value, sizeof(value));
    return value;
}

static uint16_t reader_u16_le(backend_config_reader_t *reader)
{
    uint8_t bytes[2] = {0};
    reader_bytes(reader, bytes, sizeof(bytes));
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t reader_u32_le(backend_config_reader_t *reader)
{
    uint8_t bytes[4] = {0};
    reader_bytes(reader, bytes, sizeof(bytes));
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static uint64_t reader_u64_le(backend_config_reader_t *reader)
{
    uint8_t bytes[8] = {0};
    reader_bytes(reader, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static bool reader_string(
    backend_config_reader_t *reader, char *out, size_t capacity)
{
    const uint16_t length = reader_u16_le(reader);
    if (!reader->valid || length >= capacity ||
        length > reader->length - reader->position) {
        reader->valid = false;
        return false;
    }
    if (length != 0 &&
        memchr(reader->bytes + reader->position, '\0', length) != NULL) {
        reader->valid = false;
        return false;
    }
    reader_bytes(reader, out, length);
    if (!reader->valid) {
        return false;
    }
    out[length] = '\0';
    return true;
}

backend_config_result_t backend_config_decode_canonical(
    const uint8_t *bytes, size_t length, backend_config_record_t *out)
{
    if (!bytes || !out || length < BACKEND_CONFIG_HEADER_SIZE +
                                     BACKEND_CONFIG_CRC_SIZE ||
        length > BACKEND_CONFIG_BLOB_MAX) {
        return BACKEND_CONFIG_INVALID_LENGTH;
    }

    backend_config_reader_t header_reader = {
        .bytes = bytes,
        .length = BACKEND_CONFIG_HEADER_SIZE,
        .position = 0,
        .valid = true,
    };
    const uint32_t magic = reader_u32_le(&header_reader);
    const uint16_t schema_version = reader_u16_le(&header_reader);
    const uint16_t payload_length = reader_u16_le(&header_reader);
    const uint32_t generation = reader_u32_le(&header_reader);
    if (!header_reader.valid || payload_length > BACKEND_CONFIG_PAYLOAD_MAX ||
        length != BACKEND_CONFIG_HEADER_SIZE + (size_t)payload_length +
                  BACKEND_CONFIG_CRC_SIZE) {
        return BACKEND_CONFIG_INVALID_LENGTH;
    }
    if (magic != BACKEND_CONFIG_MAGIC ||
        schema_version != BACKEND_CONFIG_SCHEMA_VERSION) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }

    backend_config_reader_t crc_reader = {
        .bytes = bytes + length - BACKEND_CONFIG_CRC_SIZE,
        .length = BACKEND_CONFIG_CRC_SIZE,
        .position = 0,
        .valid = true,
    };
    const uint32_t stored_crc = reader_u32_le(&crc_reader);
    const uint32_t actual_crc = canonical_crc32(
        bytes, length - BACKEND_CONFIG_CRC_SIZE);
    if (!crc_reader.valid || stored_crc != actual_crc) {
        return BACKEND_CONFIG_INVALID_CRC;
    }

    backend_config_record_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.schema_version = schema_version;
    decoded.generation = generation;
    backend_config_reader_t payload_reader = {
        .bytes = bytes + BACKEND_CONFIG_HEADER_SIZE,
        .length = payload_length,
        .position = 0,
        .valid = true,
    };
    decoded.network_count = reader_u8(&payload_reader);
    if (!payload_reader.valid ||
        decoded.network_count > BACKEND_CONFIG_MAX_NETWORKS) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }
    for (uint8_t index = 0; index < decoded.network_count; ++index) {
        if (!reader_string(&payload_reader, decoded.networks[index].ssid,
                           sizeof(decoded.networks[index].ssid)) ||
            !reader_string(&payload_reader,
                           decoded.networks[index].password,
                           sizeof(decoded.networks[index].password))) {
            return BACKEND_CONFIG_INVALID_LENGTH;
        }
    }
    if (!reader_string(&payload_reader, decoded.backend_url,
                       sizeof(decoded.backend_url)) ||
        !reader_string(&payload_reader, decoded.device_id,
                       sizeof(decoded.device_id)) ||
        !reader_string(&payload_reader, decoded.display_name,
                       sizeof(decoded.display_name)) ||
        !reader_string(&payload_reader, decoded.ap_password,
                       sizeof(decoded.ap_password))) {
        return BACKEND_CONFIG_INVALID_LENGTH;
    }

    const uint8_t auto_update_enabled = reader_u8(&payload_reader);
    const uint8_t has_location = reader_u8(&payload_reader);
    const uint64_t latitude_bits = reader_u64_le(&payload_reader);
    const uint64_t longitude_bits = reader_u64_le(&payload_reader);
    const uint32_t altitude_bits = reader_u32_le(&payload_reader);
    if (!payload_reader.valid || payload_reader.position != payload_length) {
        return BACKEND_CONFIG_INVALID_LENGTH;
    }
    if (auto_update_enabled > 1U || has_location > 1U) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }
    decoded.auto_update_enabled = auto_update_enabled != 0;
    decoded.has_location = has_location != 0;
    if (!decoded.has_location &&
        (latitude_bits != 0 || longitude_bits != 0 || altitude_bits != 0)) {
        return BACKEND_CONFIG_INVALID_FIELD;
    }
    memcpy(&decoded.latitude, &latitude_bits, sizeof(latitude_bits));
    memcpy(&decoded.longitude, &longitude_bits, sizeof(longitude_bits));
    memcpy(&decoded.altitude_m, &altitude_bits, sizeof(altitude_bits));

    const backend_config_result_t validation =
        backend_config_validate(&decoded);
    if (validation != BACKEND_CONFIG_VALID) {
        return validation;
    }
    *out = decoded;
    return BACKEND_CONFIG_VALID;
}

static bool copy_bounded_string(
    char *out, size_t out_capacity, const char *value, size_t value_capacity)
{
    size_t length = 0;
    if (!bounded_string_length(value, value_capacity, &length) ||
        length >= out_capacity) {
        return false;
    }
    memcpy(out, value, length + 1U);
    return true;
}

bool backend_config_migrate_legacy(
    const backend_legacy_config_t *legacy,
    uint32_t generation,
    backend_config_record_t *out)
{
    if (!legacy || !out || generation == 0) {
        return false;
    }

    size_t ssid_length = 0;
    size_t canonical_password_length = 0;
    size_t alias_password_length = 0;
    if (!bounded_string_length(legacy->wifi_ssid,
                               sizeof(legacy->wifi_ssid), &ssid_length) ||
        ssid_length == 0 ||
        !bounded_string_length(legacy->wifi_password,
                               sizeof(legacy->wifi_password),
                               &canonical_password_length) ||
        !bounded_string_length(legacy->wifi_pass,
                               sizeof(legacy->wifi_pass),
                               &alias_password_length)) {
        return false;
    }
    if (canonical_password_length != 0 && alias_password_length != 0 &&
        (canonical_password_length != alias_password_length ||
         memcmp(legacy->wifi_password, legacy->wifi_pass,
                canonical_password_length) != 0)) {
        return false;
    }
    const char *password = canonical_password_length != 0
        ? legacy->wifi_password : legacy->wifi_pass;
    const size_t password_capacity = canonical_password_length != 0
        ? sizeof(legacy->wifi_password) : sizeof(legacy->wifi_pass);

    backend_config_record_t migrated;
    memset(&migrated, 0, sizeof(migrated));
    migrated.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    migrated.generation = generation;
    migrated.network_count = 1;
    if (!copy_bounded_string(migrated.networks[0].ssid,
                             sizeof(migrated.networks[0].ssid),
                             legacy->wifi_ssid,
                             sizeof(legacy->wifi_ssid)) ||
        !copy_bounded_string(migrated.networks[0].password,
                             sizeof(migrated.networks[0].password),
                             password, password_capacity) ||
        !copy_bounded_string(migrated.backend_url,
                             sizeof(migrated.backend_url),
                             legacy->backend_url,
                             sizeof(legacy->backend_url)) ||
        !copy_bounded_string(migrated.device_id,
                             sizeof(migrated.device_id), legacy->device_id,
                             sizeof(legacy->device_id))) {
        return false;
    }
    size_t ap_password_length = 0;
    if (!bounded_string_length(legacy->ap_pass, sizeof(legacy->ap_pass),
                               &ap_password_length)) {
        return false;
    }
    if (ap_password_length == 0) {
        static const char default_ap_password[] = "friendorfoe";
        memcpy(migrated.ap_password, default_ap_password,
               sizeof(default_ap_password));
    } else if (!copy_bounded_string(migrated.ap_password,
                                    sizeof(migrated.ap_password),
                                    legacy->ap_pass,
                                    sizeof(legacy->ap_pass))) {
        return false;
    }
    migrated.auto_update_enabled = false;
    migrated.has_location = false;

    if (backend_config_validate(&migrated) != BACKEND_CONFIG_VALID) {
        return false;
    }
    *out = migrated;
    return true;
}
