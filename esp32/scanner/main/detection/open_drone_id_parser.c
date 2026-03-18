/**
 * Friend or Foe — OpenDroneID Parser (ESP32-S3)
 *
 * Ported from Android OpenDroneIdParser.kt.
 * Parses ASTM F3411-22a messages and accumulates partial state per device.
 */

#include "open_drone_id_parser.h"
#include "constants.h"

#include <string.h>
#include <math.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "odid_parser";

/* ── Little-endian read helpers ─────────────────────────────────────────── */

static int32_t read_int32_le(const uint8_t *data, size_t offset, size_t len)
{
    if (offset + 4 > len) return 0;
    return (int32_t)(
        ((uint32_t)data[offset])           |
        ((uint32_t)data[offset + 1] << 8)  |
        ((uint32_t)data[offset + 2] << 16) |
        ((uint32_t)data[offset + 3] << 24)
    );
}

static uint16_t read_uint16_le(const uint8_t *data, size_t offset, size_t len)
{
    if (offset + 2 > len) return 0;
    return (uint16_t)(
        ((uint16_t)data[offset]) |
        ((uint16_t)data[offset + 1] << 8)
    );
}

/* ── Trim trailing nulls and spaces from a fixed-width ASCII field ────── */

static size_t trim_ascii(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    size_t copy_len = (src_len < dst_size - 1) ? src_len : dst_size - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';

    /* Trim trailing nulls and spaces */
    while (copy_len > 0 && (dst[copy_len - 1] == '\0' || dst[copy_len - 1] == ' ')) {
        copy_len--;
        dst[copy_len] = '\0';
    }
    return copy_len;
}

/* ── Message parsers ────────────────────────────────────────────────────── */

/**
 * Parse Basic ID message (Type 0).
 *
 * Format (25 bytes):
 *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
 *   Byte 1: [ID type (4 bits)] [UA type (4 bits)]
 *   Bytes 2-21: UAS ID (20 bytes, null-padded ASCII for serial number)
 *   Bytes 22-24: Reserved
 */
static void parse_basic_id(const uint8_t *data, size_t len, odid_state_t *state)
{
    if (len < 22) return;

    uint8_t id_type_byte = data[1];
    uint8_t ua_type = id_type_byte & 0x0F;
    uint8_t id_type = (id_type_byte >> 4) & 0x0F;

    char serial[21];
    size_t serial_len = trim_ascii(&data[2], 20, serial, sizeof(serial));

    if (serial_len > 0) {
        strncpy(state->drone_id, serial, sizeof(state->drone_id) - 1);
        state->drone_id[sizeof(state->drone_id) - 1] = '\0';
    }
    state->ua_type = ua_type;
    state->id_type = id_type;
}

/**
 * Parse Location/Vector message (Type 1).
 *
 * Format (25 bytes):
 *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
 *   Byte 1: Status flags
 *   Byte 2: Direction (heading / 2, 0-179 => 0-358 degrees)
 *   Byte 3: Speed (horizontal, in 0.25 m/s increments)
 *   Byte 4: Vertical speed (int8, 0.5 m/s increments, 63 = unknown)
 *   Bytes 5-8: Latitude (int32, x 1e-7 degrees)
 *   Bytes 9-12: Longitude (int32, x 1e-7 degrees)
 *   Bytes 13-14: Pressure altitude (uint16, x 0.5 - 1000 meters)
 *   Bytes 15-16: Geodetic altitude (uint16, x 0.5 - 1000 meters)
 *   Bytes 17-18: Height AGL (uint16, x 0.5 - 1000 meters)
 *   Byte 19: Horizontal/vertical accuracy
 *   Byte 20: Baro alt accuracy / speed accuracy
 *   Bytes 21-22: Timestamp (uint16, tenths of seconds since hour)
 *   Byte 23: Timestamp accuracy
 *   Byte 24: Reserved
 */
static void parse_location(const uint8_t *data, size_t len, odid_state_t *state)
{
    if (len < ODID_MSG_SIZE) return;

    /* Heading: byte 2 * 2 degrees */
    uint8_t direction_raw = data[2];
    float heading = (float)direction_raw * 2.0f;

    /* Speed: byte 3 * 0.25 m/s */
    uint8_t speed_raw = data[3];
    float speed_mps = (float)speed_raw * ODID_SPEED_SCALE;

    /* Latitude: int32 at offset 5, scaled by 1e-7 */
    int32_t lat_raw = read_int32_le(data, 5, len);
    double latitude = (double)lat_raw * ODID_LAT_LON_SCALE;

    /* Longitude: int32 at offset 9, scaled by 1e-7 */
    int32_t lon_raw = read_int32_le(data, 9, len);
    double longitude = (double)lon_raw * ODID_LAT_LON_SCALE;

    /* Pressure altitude: uint16 at offset 13, * 0.5 - 1000 */
    uint16_t alt_raw = read_uint16_le(data, 13, len);
    double altitude_m = (double)alt_raw * ODID_ALT_SCALE + ODID_ALT_OFFSET;

    /* Validate coordinates */
    if (latitude == 0.0 && longitude == 0.0) return;
    if (latitude < -90.0 || latitude > 90.0) return;
    if (longitude < -180.0 || longitude > 180.0) return;

    state->has_location = true;
    state->latitude = latitude;
    state->longitude = longitude;
    state->altitude_m = altitude_m;
    state->heading_deg = (heading <= 360.0f) ? heading : NAN;
    state->speed_mps = speed_mps;

    /* Vertical speed (byte 4): int8, 0.5 m/s increments, 63 = unknown */
    if (len >= 5) {
        int8_t vs_raw = (int8_t)data[4];
        if (vs_raw != ODID_VSPEED_UNKNOWN) {
            state->vertical_speed_mps = (float)vs_raw * 0.5f;
        }
    }

    /* Geodetic altitude (bytes 15-16): uint16, 0.5m - 1000m, 0xFFFF = unknown */
    if (len >= 17) {
        uint16_t geo_alt_raw = read_uint16_le(data, 15, len);
        if (geo_alt_raw != 0xFFFF) {
            state->geodetic_alt_m = (double)geo_alt_raw * ODID_ALT_SCALE + ODID_ALT_OFFSET;
        }
    }

    /* Height AGL (bytes 17-18): uint16, 0.5m - 1000m, 0xFFFF = unknown */
    if (len >= 19) {
        uint16_t height_raw = read_uint16_le(data, 17, len);
        if (height_raw != 0xFFFF) {
            state->height_agl_m = (double)height_raw * ODID_ALT_SCALE + ODID_ALT_OFFSET;
        }
    }

    /* Accuracy (byte 19): high nibble = horizontal, low nibble = vertical */
    if (len >= 20) {
        uint8_t acc_byte = data[19];
        state->h_accuracy_code = (acc_byte >> 4) & 0x0F;
        state->v_accuracy_code = acc_byte & 0x0F;
    }

    /* Timestamp (bytes 21-22): uint16, tenths of seconds since hour, 0xFFFF = unknown */
    if (len >= 23) {
        uint16_t ts_raw = read_uint16_le(data, 21, len);
        if (ts_raw != 0xFFFF) {
            state->location_timestamp = ts_raw;
        }
    }
}

/**
 * Parse System message (Type 4) — contains operator location and area info.
 *
 * Format:
 *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
 *   Byte 1: Operator location type
 *   Bytes 2-5: Operator latitude (int32 * 1e-7)
 *   Bytes 6-9: Operator longitude (int32 * 1e-7)
 *   Bytes 10-11: Area count (uint16)
 *   Bytes 12-13: Area radius (uint16, x 10m)
 *   Bytes 14-15: Area ceiling (uint16, x 0.5 - 1000)
 *   Bytes 16-17: Area floor (uint16, x 0.5 - 1000)
 *   Byte 18: Classification type
 */
static void parse_system(const uint8_t *data, size_t len, odid_state_t *state)
{
    if (len < 10) return;

    int32_t op_lat_raw = read_int32_le(data, 2, len);
    int32_t op_lon_raw = read_int32_le(data, 6, len);

    double op_lat = (double)op_lat_raw * ODID_LAT_LON_SCALE;
    double op_lon = (double)op_lon_raw * ODID_LAT_LON_SCALE;

    /* Only store if not both zero and within valid range */
    if (op_lat != 0.0 || op_lon != 0.0) {
        if (op_lat >= -90.0 && op_lat <= 90.0 && op_lon >= -180.0 && op_lon <= 180.0) {
            state->operator_lat = op_lat;
            state->operator_lon = op_lon;
        }
    }

    /* Area count (bytes 10-11) */
    if (len >= 12) {
        state->area_count = read_uint16_le(data, 10, len);
    }

    /* Area radius (bytes 12-13): x 10m */
    if (len >= 14) {
        state->area_radius = read_uint16_le(data, 12, len);
    }

    /* Area ceiling (bytes 14-15): x 0.5 - 1000 */
    if (len >= 16) {
        uint16_t ceiling_raw = read_uint16_le(data, 14, len);
        state->area_ceiling = (double)ceiling_raw * ODID_ALT_SCALE + ODID_ALT_OFFSET;
    }

    /* Area floor (bytes 16-17): x 0.5 - 1000 */
    if (len >= 18) {
        uint16_t floor_raw = read_uint16_le(data, 16, len);
        state->area_floor = (double)floor_raw * ODID_ALT_SCALE + ODID_ALT_OFFSET;
    }

    /* Classification type (byte 18) */
    if (len >= 19) {
        state->classification_type = data[18];
    }
}

/**
 * Parse Operator ID message (Type 5).
 *
 * Format:
 *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
 *   Byte 1: Operator ID type
 *   Bytes 2-21: Operator ID (20 bytes ASCII, null-padded)
 */
static void parse_operator_id(const uint8_t *data, size_t len, odid_state_t *state)
{
    if (len < 22) return;

    char operator_id[21];
    size_t id_len = trim_ascii(&data[2], 20, operator_id, sizeof(operator_id));

    if (id_len > 0) {
        strncpy(state->operator_id, operator_id, sizeof(state->operator_id) - 1);
        state->operator_id[sizeof(state->operator_id) - 1] = '\0';
    }
}

/**
 * Parse Self-ID message (Type 3).
 *
 * Format (25 bytes):
 *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
 *   Byte 1: Description type
 *   Bytes 2-24: 23-char ASCII text (null-padded)
 */
static void parse_self_id(const uint8_t *data, size_t len, odid_state_t *state)
{
    if (len < 25) return;

    state->self_id_desc_type = data[1];

    char text[24];
    size_t text_len = trim_ascii(&data[2], 23, text, sizeof(text));

    if (text_len > 0) {
        strncpy(state->self_id_text, text, sizeof(state->self_id_text) - 1);
        state->self_id_text[sizeof(state->self_id_text) - 1] = '\0';
    }
}

/**
 * Parse Message Pack (Type 0xF).
 *
 * Format:
 *   Byte 0: [msg type (4 bits)] [protocol version (4 bits)]
 *   Byte 1: Message count
 *   Bytes 2+: N x 25-byte messages
 */
static void parse_message_pack(const uint8_t *data, size_t len, odid_state_t *state, int depth)
{
    if (depth >= 2) {
        ESP_LOGW(TAG, "Message Pack recursion depth exceeded, skipping");
        return;
    }
    if (len < 2) return;

    uint8_t message_count = data[1];
    const size_t messages_start = 2;

    for (int i = 0; i < message_count; i++) {
        size_t offset = messages_start + (size_t)i * ODID_MSG_SIZE;
        if (offset + ODID_MSG_SIZE > len) break;
        odid_parse_message(&data[offset], ODID_MSG_SIZE, state, depth + 1);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void odid_parse_message(const uint8_t *data, size_t len, odid_state_t *state, int depth)
{
    if (data == NULL || len == 0 || state == NULL) return;

    uint8_t message_type = (data[0] & 0xF0) >> 4;

    switch (message_type) {
    case ODID_MSG_TYPE_BASIC_ID:
        parse_basic_id(data, len, state);
        break;
    case ODID_MSG_TYPE_LOCATION:
        parse_location(data, len, state);
        break;
    case ODID_MSG_TYPE_SELF_ID:
        parse_self_id(data, len, state);
        break;
    case ODID_MSG_TYPE_SYSTEM:
        parse_system(data, len, state);
        break;
    case ODID_MSG_TYPE_OPERATOR_ID:
        parse_operator_id(data, len, state);
        break;
    case ODID_MSG_TYPE_MESSAGE_PACK:
        parse_message_pack(data, len, state, depth);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled OpenDroneID message type: %d", message_type);
        break;
    }
}

float odid_accuracy_code_to_meters(uint8_t code)
{
    /*
     * ASTM F3411-22a Table 4: Horizontal/Vertical position accuracy.
     * Returns -1.0f for unknown (code 0 or out of range).
     */
    static const float table[] = {
        [0]  = -1.0f,      /* Unknown */
        [1]  = 18520.0f,   /* >= 18.52 km */
        [2]  = 7408.0f,    /* < 7.408 km */
        [3]  = 3704.0f,    /* < 3.704 km */
        [4]  = 1852.0f,    /* < 1.852 km (1 NM) */
        [5]  = 926.0f,     /* < 926 m */
        [6]  = 555.6f,     /* < 555.6 m */
        [7]  = 185.2f,     /* < 185.2 m */
        [8]  = 92.6f,      /* < 92.6 m */
        [9]  = 30.0f,      /* < 30 m */
        [10] = 10.0f,      /* < 10 m */
        [11] = 3.0f,       /* < 3 m */
        [12] = 1.0f,       /* < 1 m */
    };

    if (code > 12) return -1.0f;
    return table[code];
}

bool odid_state_to_detection(const odid_state_t *state, const char *id_prefix,
                             uint8_t source, drone_detection_t *out)
{
    if (state == NULL || out == NULL) return false;

    /* We need at least a drone ID or device address */
    const char *id = (state->drone_id[0] != '\0') ? state->drone_id : state->device_address;
    if (id[0] == '\0') return false;

    memset(out, 0, sizeof(*out));

    /* Build prefixed ID: e.g. "rid_SERIAL123" */
    snprintf(out->drone_id, sizeof(out->drone_id), "%s%s", id_prefix, id);

    out->source = source;

    /* Confidence: higher if we have location data */
    bool has_position = state->has_location &&
                        (state->latitude != 0.0 || state->longitude != 0.0);
    out->confidence = has_position ? 0.9f : 0.6f;

    /* Position */
    out->latitude = state->latitude;
    out->longitude = state->longitude;
    out->altitude_m = state->altitude_m;
    out->heading_deg = state->heading_deg;
    out->speed_mps = state->speed_mps;
    out->vertical_speed_mps = state->vertical_speed_mps;

    /* Extended position */
    out->geodetic_alt_m = state->geodetic_alt_m;
    out->height_agl_m = state->height_agl_m;

    /* Accuracy */
    float h_acc = odid_accuracy_code_to_meters(state->h_accuracy_code);
    float v_acc = odid_accuracy_code_to_meters(state->v_accuracy_code);
    out->h_accuracy_m = (h_acc > 0.0f) ? h_acc : NAN;
    out->v_accuracy_m = (v_acc > 0.0f) ? v_acc : NAN;

    /* Operator info */
    out->operator_lat = state->operator_lat;
    out->operator_lon = state->operator_lon;
    strncpy(out->operator_id, state->operator_id, sizeof(out->operator_id) - 1);
    out->operator_id[sizeof(out->operator_id) - 1] = '\0';

    /* Self-ID */
    out->self_id_desc_type = state->self_id_desc_type;
    strncpy(out->self_id_text, state->self_id_text, sizeof(out->self_id_text) - 1);
    out->self_id_text[sizeof(out->self_id_text) - 1] = '\0';

    /* Classification */
    out->ua_type = state->ua_type;
    out->id_type = state->id_type;

    /* Area info */
    out->area_count = state->area_count;
    out->area_radius = state->area_radius;
    out->area_ceiling = state->area_ceiling;
    out->area_floor = state->area_floor;
    out->classification_type = state->classification_type;

    /* Signal */
    out->rssi = state->rssi;

    /* Timestamps */
    out->first_seen_ms = state->first_seen_ms;
    out->last_updated_ms = state->last_updated_ms;

    return true;
}

void odid_state_init(odid_state_t *state, const char *device_address, int64_t now_ms)
{
    if (state == NULL) return;

    memset(state, 0, sizeof(*state));

    if (device_address != NULL) {
        strncpy(state->device_address, device_address, sizeof(state->device_address) - 1);
        state->device_address[sizeof(state->device_address) - 1] = '\0';
    }

    state->first_seen_ms = now_ms;
    state->last_updated_ms = now_ms;

    /* Initialise floating-point fields that use NAN for "unknown" */
    state->heading_deg = NAN;
    state->vertical_speed_mps = NAN;
    state->geodetic_alt_m = NAN;
    state->height_agl_m = NAN;
}
