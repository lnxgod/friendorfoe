/**
 * Friend or Foe — OpenDroneID Message Encoder
 *
 * Encodes 25-byte ODID messages matching ASTM F3411-22a format.
 * Byte layout matches what the scanner's open_drone_id_parser.c expects.
 *
 * Byte 0 of every message: (msg_type << 4) | protocol_version
 */

#include "odid_encoder.h"
#include <string.h>
#include <math.h>

#define ODID_PROTOCOL_VERSION   0x02
#define ODID_MSG_SIZE           25

/* Message type codes (upper nibble of byte 0) */
#define MSG_TYPE_BASIC_ID       0
#define MSG_TYPE_LOCATION       1
#define MSG_TYPE_SYSTEM         4
#define MSG_TYPE_OPERATOR_ID    5

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void write_int32_le(uint8_t *buf, size_t offset, int32_t val)
{
    buf[offset]     = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static void write_uint16_le(uint8_t *buf, size_t offset, uint16_t val)
{
    buf[offset]     = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void odid_encode_basic_id(uint8_t *msg25, const char *serial,
                           uint8_t ua_type, uint8_t id_type)
{
    memset(msg25, 0, ODID_MSG_SIZE);

    /* Byte 0: msg_type=0 (Basic ID), protocol version 2 */
    msg25[0] = (MSG_TYPE_BASIC_ID << 4) | ODID_PROTOCOL_VERSION;

    /* Byte 1: [ID type (high nibble)] [UA type (low nibble)] */
    msg25[1] = ((id_type & 0x0F) << 4) | (ua_type & 0x0F);

    /* Bytes 2-21: UAS ID (20 bytes, null-padded ASCII) */
    if (serial) {
        size_t len = strlen(serial);
        if (len > 20) len = 20;
        memcpy(&msg25[2], serial, len);
    }
}

void odid_encode_location(uint8_t *msg25, double lat, double lon, double alt_m,
                           float heading_deg, float speed_mps, float vspeed_mps)
{
    memset(msg25, 0, ODID_MSG_SIZE);

    /* Byte 0: msg_type=1 (Location), protocol version 2 */
    msg25[0] = (MSG_TYPE_LOCATION << 4) | ODID_PROTOCOL_VERSION;

    /* Byte 1: Status = airborne (0x02 in low nibble) */
    msg25[1] = 0x02;

    /* Byte 2: Heading / 2 */
    float h = heading_deg;
    if (h < 0.0f) h += 360.0f;
    if (h >= 360.0f) h -= 360.0f;
    msg25[2] = (uint8_t)(h / 2.0f);

    /* Byte 3: Speed in 0.25 m/s increments */
    msg25[3] = (uint8_t)(speed_mps / 0.25f);

    /* Byte 4: Vertical speed in 0.5 m/s increments, 63 = unknown */
    if (isnan(vspeed_mps)) {
        msg25[4] = 63;
    } else {
        msg25[4] = (uint8_t)((int8_t)(vspeed_mps / 0.5f));
    }

    /* Bytes 5-8: Latitude as int32 * 1e7 */
    int32_t lat_raw = (int32_t)(lat * 1e7);
    write_int32_le(msg25, 5, lat_raw);

    /* Bytes 9-12: Longitude as int32 * 1e7 */
    int32_t lon_raw = (int32_t)(lon * 1e7);
    write_int32_le(msg25, 9, lon_raw);

    /* Bytes 13-14: Pressure altitude = (alt + 1000) / 0.5 */
    uint16_t alt_raw = (uint16_t)((alt_m + 1000.0) / 0.5);
    write_uint16_le(msg25, 13, alt_raw);

    /* Bytes 15-16: Geodetic altitude (same as pressure for sim) */
    write_uint16_le(msg25, 15, alt_raw);

    /* Bytes 17-18: Height AGL (same as alt_m for sim) */
    uint16_t height_raw = (uint16_t)((alt_m + 1000.0) / 0.5);
    write_uint16_le(msg25, 17, height_raw);

    /* Byte 19: Accuracy — h_acc=10 (<10m) high nibble, v_acc=9 (<30m) low nibble */
    msg25[19] = (10 << 4) | 9;

    /* Byte 20: Baro accuracy (high nibble) / speed accuracy (low nibble) */
    msg25[20] = (9 << 4) | 0;

    /* Bytes 21-22: Timestamp (tenths of seconds since hour, 0 for sim) */
    write_uint16_le(msg25, 21, 0);

    /* Byte 23: Timestamp accuracy */
    msg25[23] = 1;
}

void odid_encode_system(uint8_t *msg25, double op_lat, double op_lon,
                         uint16_t area_count, double area_radius_m)
{
    memset(msg25, 0, ODID_MSG_SIZE);

    /* Byte 0: msg_type=4 (System), protocol version 2 */
    msg25[0] = (MSG_TYPE_SYSTEM << 4) | ODID_PROTOCOL_VERSION;

    /* Byte 1: Operator location type = takeoff (0x01) */
    msg25[1] = 0x01;

    /* Bytes 2-5: Operator latitude as int32 * 1e7 */
    int32_t op_lat_raw = (int32_t)(op_lat * 1e7);
    write_int32_le(msg25, 2, op_lat_raw);

    /* Bytes 6-9: Operator longitude as int32 * 1e7 */
    int32_t op_lon_raw = (int32_t)(op_lon * 1e7);
    write_int32_le(msg25, 6, op_lon_raw);

    /* Bytes 10-11: Area count */
    write_uint16_le(msg25, 10, area_count);

    /* Bytes 12-13: Area radius in 10m units */
    uint16_t radius_raw = (uint16_t)(area_radius_m / 10.0);
    write_uint16_le(msg25, 12, radius_raw);

    /* Bytes 14-15: Area ceiling (0xFFFF = unknown) */
    write_uint16_le(msg25, 14, 0xFFFF);

    /* Bytes 16-17: Area floor (0xFFFF = unknown) */
    write_uint16_le(msg25, 16, 0xFFFF);
}

void odid_encode_operator_id(uint8_t *msg25, const char *operator_id)
{
    memset(msg25, 0, ODID_MSG_SIZE);

    /* Byte 0: msg_type=5 (Operator ID), protocol version 2 */
    msg25[0] = (MSG_TYPE_OPERATOR_ID << 4) | ODID_PROTOCOL_VERSION;

    /* Byte 1: Operator ID type = FAA (0x01) */
    msg25[1] = 0x01;

    /* Bytes 2-21: Operator ID (20 bytes, null-padded ASCII) */
    if (operator_id) {
        size_t len = strlen(operator_id);
        if (len > 20) len = 20;
        memcpy(&msg25[2], operator_id, len);
    }
}
