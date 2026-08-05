/**
 * Friend or Foe — Unit Tests for DJI DroneID Parser
 *
 * Tests parsing of DJI-proprietary vendor-specific Information Elements
 * from WiFi beacon frames. DJI DroneID encodes GPS telemetry, altitude,
 * speed, heading, serial prefix, and operator home point.
 *
 * Build: PlatformIO native test environment (env:test)
 */

#include "unity.h"
#include "dji_drone_id_parser.h"
#include "constants.h"

#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────── */

/** Write a little-endian int32 at the given offset. */
static void write_int32_le(uint8_t *buf, size_t offset, int32_t val)
{
    buf[offset + 0] = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((val >> 24) & 0xFF);
}

/** Write a little-endian int16 at the given offset. */
static void write_int16_le(uint8_t *buf, size_t offset, int16_t val)
{
    buf[offset + 0] = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
}

/** Write a little-endian uint16 at the given offset. */
static void write_uint16_le(uint8_t *buf, size_t offset, uint16_t val)
{
    buf[offset + 0] = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
}

/**
 * Build a valid DJI DroneID payload (OUI + data).
 *
 * Layout (after OUI 0x26 0x37 0x12):
 *   Byte 0:      Version
 *   Bytes 1-4:   Serial prefix (4 ASCII chars)
 *   Bytes 5-8:   Drone longitude (int32, degrees x 1e7)
 *   Bytes 9-12:  Drone latitude (int32, degrees x 1e7)
 *   Bytes 13-14: Altitude (int16, meters relative to takeoff)
 *   Bytes 15-16: Height above ground (int16, decimeters)
 *   Bytes 17-18: Speed (uint16, cm/s)
 *   Bytes 19-20: Heading (int16, degrees x 100)
 *   Bytes 21-24: Home longitude (int32, degrees x 1e7)
 *   Bytes 25-28: Home latitude (int32, degrees x 1e7)
 */
static size_t build_dji_payload(uint8_t *buf, size_t buf_size,
                                double drone_lat, double drone_lon,
                                int16_t alt_m, int16_t height_dm,
                                uint16_t speed_cms, int16_t heading_cdeg,
                                double home_lat, double home_lon,
                                const char *serial)
{
    /* Minimum size: 3 (OUI) + 29 (DJI_MIN_PAYLOAD_LENGTH) = 32 */
    const size_t total = 3 + 29;
    if (buf_size < total) return 0;

    memset(buf, 0, buf_size);

    /* OUI */
    buf[0] = DJI_OUI_0;
    buf[1] = DJI_OUI_1;
    buf[2] = DJI_OUI_2;

    /* Data starts at offset 3 (after OUI) */
    uint8_t *data = &buf[3];

    /* Version */
    data[0] = 1;

    /* Serial prefix (bytes 1-4) */
    if (serial) {
        size_t slen = strlen(serial);
        if (slen > 4) slen = 4;
        memcpy(&data[1], serial, slen);
    }

    /* Drone position (degrees x 1e7 as int32) */
    write_int32_le(data, 5, (int32_t)(drone_lon * 1e7));
    write_int32_le(data, 9, (int32_t)(drone_lat * 1e7));

    /* Altitude (int16, meters) */
    write_int16_le(data, 13, alt_m);

    /* Height above ground (int16, decimeters) */
    write_int16_le(data, 15, height_dm);

    /* Speed (uint16, cm/s) */
    write_uint16_le(data, 17, speed_cms);

    /* Heading (int16, degrees x 100) */
    write_int16_le(data, 19, heading_cdeg);

    /* Home point (degrees x 1e7 as int32) */
    write_int32_le(data, 21, (int32_t)(home_lon * 1e7));
    write_int32_le(data, 25, (int32_t)(home_lat * 1e7));

    return total;
}

/* ── Test: Valid DJI payload parsing ───────────────────────────────────── */

void test_parse_valid_payload(void)
{
    uint8_t payload[64];
    size_t len = build_dji_payload(
        payload, sizeof(payload),
        37.7749,     /* drone lat */
        -122.4194,   /* drone lon */
        100,         /* altitude: 100m */
        500,         /* height: 50.0m (500 decimeters) */
        1000,        /* speed: 10.0 m/s (1000 cm/s) */
        18000,       /* heading: 180.00 degrees (18000 centidegrees) */
        37.0,        /* home lat */
        -122.0,      /* home lon */
        "TEST"       /* serial prefix */
    );

    dji_drone_id_data_t out;
    bool ok = dji_parse_vendor_ie(payload, len, &out);

    TEST_ASSERT_TRUE(ok);

    /* Drone position */
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 37.7749, out.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -122.4194, out.longitude);

    /* Altitude */
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 100.0, out.altitude_m);

    /* Height above ground: 500 decimeters = 50.0m */
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 50.0, out.height_above_ground_m);

    /* Speed: 1000 cm/s = 10.0 m/s */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, out.speed_mps);

    /* Heading: 18000 centidegrees = 180.0 degrees */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, out.heading_deg);

    /* Home point */
    TEST_ASSERT_TRUE(out.has_home);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 37.0, out.home_lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -122.0, out.home_lon);

    /* Serial prefix */
    TEST_ASSERT_EQUAL_STRING("TEST", out.serial_prefix);

    /* Version */
    TEST_ASSERT_EQUAL_UINT8(1, out.version);
}

/* ── Test: Zero coordinates should fail ────────────────────────────────── */

void test_parse_zero_coords(void)
{
    uint8_t payload[64];
    size_t len = build_dji_payload(
        payload, sizeof(payload),
        0.0, 0.0,    /* zero coordinates — GPS not acquired */
        100, 500, 1000, 18000,
        0.0, 0.0,
        "TEST"
    );

    dji_drone_id_data_t out;
    bool ok = dji_parse_vendor_ie(payload, len, &out);

    TEST_ASSERT_FALSE(ok);
}

/* ── Test: Short payload should fail ───────────────────────────────────── */

void test_parse_short_payload(void)
{
    /* Only 10 bytes — way too short */
    uint8_t payload[10] = { DJI_OUI_0, DJI_OUI_1, DJI_OUI_2, 1, 0 };

    dji_drone_id_data_t out;
    bool ok = dji_parse_vendor_ie(payload, sizeof(payload), &out);

    TEST_ASSERT_FALSE(ok);
}

/* ── Test: Wrong OUI should fail ───────────────────────────────────────── */

void test_parse_wrong_oui(void)
{
    uint8_t payload[64];
    size_t len = build_dji_payload(
        payload, sizeof(payload),
        37.7749, -122.4194,
        100, 500, 1000, 18000,
        37.0, -122.0,
        "TEST"
    );

    /* Corrupt the OUI */
    payload[0] = 0xFF;
    payload[1] = 0xFF;
    payload[2] = 0xFF;

    dji_drone_id_data_t out;
    bool ok = dji_parse_vendor_ie(payload, len, &out);

    TEST_ASSERT_FALSE(ok);
}
