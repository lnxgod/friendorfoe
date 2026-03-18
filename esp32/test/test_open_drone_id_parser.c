/**
 * Friend or Foe — Unit Tests for OpenDroneID Parser
 *
 * Tests ASTM F3411-22a message parsing: Basic ID, Location, System,
 * Operator ID, Self-ID, Message Pack, accuracy codes, and state-to-detection
 * conversion.
 *
 * Build: PlatformIO native test environment (env:test)
 */

#include "unity.h"
#include "open_drone_id_parser.h"
#include "constants.h"
#include "detection_types.h"

#include <string.h>
#include <math.h>

/* ── Helpers ───────────────────────────────────────────────────────────── */

/**
 * Create a zeroed 25-byte ODID message buffer with the given type in the
 * high nibble of byte 0.
 */
static void make_msg(uint8_t *buf, uint8_t msg_type)
{
    memset(buf, 0, ODID_MSG_SIZE);
    buf[0] = (uint8_t)((msg_type & 0x0F) << 4);
}

/** Write a little-endian int32 at the given offset. */
static void write_int32_le(uint8_t *buf, size_t offset, int32_t val)
{
    buf[offset + 0] = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((val >> 24) & 0xFF);
}

/** Write a little-endian uint16 at the given offset. */
static void write_uint16_le(uint8_t *buf, size_t offset, uint16_t val)
{
    buf[offset + 0] = (uint8_t)(val & 0xFF);
    buf[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
}

/* ── Test: Basic ID (Type 0) ───────────────────────────────────────────── */

void test_parse_basic_id(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    uint8_t msg[ODID_MSG_SIZE];
    make_msg(msg, ODID_MSG_TYPE_BASIC_ID);

    /* Byte 1: id_type=1 (high nibble), ua_type=2 (low nibble) */
    msg[1] = (1 << 4) | 2;

    /* Bytes 2-21: serial "TEST1234567890" padded with zeroes to 20 bytes */
    const char *serial = "TEST1234567890";
    memcpy(&msg[2], serial, strlen(serial));

    odid_parse_message(msg, ODID_MSG_SIZE, &state, 0);

    TEST_ASSERT_EQUAL_STRING("TEST1234567890", state.drone_id);
    TEST_ASSERT_EQUAL_UINT8(2, state.ua_type);
    TEST_ASSERT_EQUAL_UINT8(1, state.id_type);
}

/* ── Test: Location (Type 1) ───────────────────────────────────────────── */

void test_parse_location(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    uint8_t msg[ODID_MSG_SIZE];
    make_msg(msg, ODID_MSG_TYPE_LOCATION);

    /* Heading: 180 degrees = byte value 90 (90 * 2 = 180) */
    msg[2] = 90;

    /* Speed: 10 m/s = byte value 40 (40 * 0.25 = 10.0) */
    msg[3] = 40;

    /* Vertical speed: 2 m/s = byte value 4 (4 * 0.5 = 2.0) */
    msg[4] = 4;

    /* Latitude: 37.7749 degrees = 377749000 as int32 (x 1e-7) */
    int32_t lat_raw = 377749000;
    write_int32_le(msg, 5, lat_raw);

    /* Longitude: -122.4194 degrees = -1224194000 as int32 (x 1e-7) */
    int32_t lon_raw = -1224194000;
    write_int32_le(msg, 9, lon_raw);

    /* Pressure altitude: 100m = (100 + 1000) / 0.5 = 2200 as uint16 */
    uint16_t alt_raw = 2200;
    write_uint16_le(msg, 13, alt_raw);

    /* Geodetic altitude: 105m = (105 + 1000) / 0.5 = 2210 as uint16 */
    write_uint16_le(msg, 15, 2210);

    /* Height AGL: 50m = (50 + 1000) / 0.5 = 2100 as uint16 */
    write_uint16_le(msg, 17, 2100);

    /* Accuracy byte 19: h_accuracy=9 (high nibble), v_accuracy=10 (low nibble) */
    msg[19] = (9 << 4) | 10;

    /* Timestamp (bytes 21-22): 1234 tenths of seconds */
    write_uint16_le(msg, 21, 1234);

    odid_parse_message(msg, ODID_MSG_SIZE, &state, 0);

    TEST_ASSERT_TRUE(state.has_location);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.7749, state.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.4194, state.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 100.0, state.altitude_m);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 180.0f, state.heading_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, state.speed_mps);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, state.vertical_speed_mps);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 105.0, state.geodetic_alt_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 50.0, state.height_agl_m);
    TEST_ASSERT_EQUAL_UINT8(9, state.h_accuracy_code);
    TEST_ASSERT_EQUAL_UINT8(10, state.v_accuracy_code);
    TEST_ASSERT_EQUAL_UINT16(1234, state.location_timestamp);
}

/* ── Test: System (Type 4) ─────────────────────────────────────────────── */

void test_parse_system(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    uint8_t msg[ODID_MSG_SIZE];
    make_msg(msg, ODID_MSG_TYPE_SYSTEM);

    /* Operator latitude: 37.0 degrees = 370000000 as int32 */
    write_int32_le(msg, 2, 370000000);

    /* Operator longitude: -122.0 degrees = -1220000000 as int32 */
    write_int32_le(msg, 6, -1220000000);

    /* Area count (bytes 10-11) */
    write_uint16_le(msg, 10, 5);

    /* Area radius (bytes 12-13): x 10m, so 50 = 500m */
    write_uint16_le(msg, 12, 50);

    /* Classification type (byte 18) */
    msg[18] = 3;

    odid_parse_message(msg, ODID_MSG_SIZE, &state, 0);

    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.0, state.operator_lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.0, state.operator_lon);
    TEST_ASSERT_EQUAL_UINT16(5, state.area_count);
    TEST_ASSERT_EQUAL_UINT16(50, state.area_radius);
    TEST_ASSERT_EQUAL_UINT8(3, state.classification_type);
}

/* ── Test: Operator ID (Type 5) ────────────────────────────────────────── */

void test_parse_operator_id(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    uint8_t msg[ODID_MSG_SIZE];
    make_msg(msg, ODID_MSG_TYPE_OPERATOR_ID);

    /* Byte 1: operator ID type */
    msg[1] = 0;

    /* Bytes 2-21: "FAA-REG-12345" padded to 20 bytes */
    const char *op_id = "FAA-REG-12345";
    memcpy(&msg[2], op_id, strlen(op_id));

    odid_parse_message(msg, ODID_MSG_SIZE, &state, 0);

    TEST_ASSERT_EQUAL_STRING("FAA-REG-12345", state.operator_id);
}

/* ── Test: Self-ID (Type 3) ────────────────────────────────────────────── */

void test_parse_self_id(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    uint8_t msg[ODID_MSG_SIZE];
    make_msg(msg, ODID_MSG_TYPE_SELF_ID);

    /* Byte 1: description type */
    msg[1] = 1;

    /* Bytes 2-24: "Test flight mission" (19 chars, padded to 23 bytes) */
    const char *text = "Test flight mission";
    memcpy(&msg[2], text, strlen(text));

    odid_parse_message(msg, ODID_MSG_SIZE, &state, 0);

    TEST_ASSERT_EQUAL_STRING("Test flight mission", state.self_id_text);
    TEST_ASSERT_EQUAL_UINT8(1, state.self_id_desc_type);
}

/* ── Test: Message Pack (Type 0xF) ─────────────────────────────────────── */

void test_parse_message_pack(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    /*
     * Message pack: header (2 bytes) + 2 x 25-byte messages = 52 bytes.
     * Byte 0: type=0xF in high nibble
     * Byte 1: message count = 2
     * Bytes 2-26: Basic ID message
     * Bytes 27-51: Location message
     */
    uint8_t pack[2 + 2 * ODID_MSG_SIZE];
    memset(pack, 0, sizeof(pack));
    pack[0] = 0xF0; /* Message Pack type */
    pack[1] = 2;    /* 2 messages */

    /* First message: Basic ID with serial "PACK-TEST" */
    uint8_t *msg1 = &pack[2];
    msg1[0] = (ODID_MSG_TYPE_BASIC_ID << 4);
    msg1[1] = (1 << 4) | 1; /* id_type=1, ua_type=1 */
    memcpy(&msg1[2], "PACK-TEST", 9);

    /* Second message: Location with San Francisco coordinates */
    uint8_t *msg2 = &pack[2 + ODID_MSG_SIZE];
    msg2[0] = (ODID_MSG_TYPE_LOCATION << 4);
    msg2[2] = 45;  /* heading: 45*2 = 90 degrees */
    msg2[3] = 20;  /* speed: 20*0.25 = 5.0 m/s */
    write_int32_le(msg2, 5, 377749000);    /* lat 37.7749 */
    write_int32_le(msg2, 9, -1224194000);  /* lon -122.4194 */
    write_uint16_le(msg2, 13, 2200);       /* alt 100m */

    odid_parse_message(pack, sizeof(pack), &state, 0);

    /* Both messages should have been parsed */
    TEST_ASSERT_EQUAL_STRING("PACK-TEST", state.drone_id);
    TEST_ASSERT_TRUE(state.has_location);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.7749, state.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.4194, state.longitude);
}

/* ── Test: Accuracy code to meters table ───────────────────────────────── */

void test_accuracy_code_to_meters(void)
{
    /* Code 0: Unknown -> -1.0 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, odid_accuracy_code_to_meters(0));

    /* Code 1-12: Known values */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 18520.0f, odid_accuracy_code_to_meters(1));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 7408.0f,  odid_accuracy_code_to_meters(2));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3704.0f,  odid_accuracy_code_to_meters(3));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1852.0f,  odid_accuracy_code_to_meters(4));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 926.0f,   odid_accuracy_code_to_meters(5));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 555.6f,   odid_accuracy_code_to_meters(6));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 185.2f,   odid_accuracy_code_to_meters(7));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 92.6f,    odid_accuracy_code_to_meters(8));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 30.0f,    odid_accuracy_code_to_meters(9));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f,    odid_accuracy_code_to_meters(10));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3.0f,     odid_accuracy_code_to_meters(11));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1.0f,     odid_accuracy_code_to_meters(12));

    /* Out of range: -1.0 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, odid_accuracy_code_to_meters(13));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, odid_accuracy_code_to_meters(255));
}

/* ── Test: Location with zero coordinates should not update state ──────── */

void test_invalid_location_zero(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);

    uint8_t msg[ODID_MSG_SIZE];
    make_msg(msg, ODID_MSG_TYPE_LOCATION);

    /* lat=0, lon=0 — intentionally left as zeroes in the buffer */
    msg[2] = 90;  /* heading */
    msg[3] = 40;  /* speed */

    odid_parse_message(msg, ODID_MSG_SIZE, &state, 0);

    /* Parser should reject zero coordinates */
    TEST_ASSERT_FALSE(state.has_location);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, state.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, state.longitude);
}

/* ── Test: State to detection conversion ───────────────────────────────── */

void test_state_to_detection(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 5000);

    /* Populate state with full data */
    strncpy(state.drone_id, "SN-12345678", sizeof(state.drone_id) - 1);
    state.ua_type = 2;
    state.id_type = 1;
    state.has_location = true;
    state.latitude = 37.7749;
    state.longitude = -122.4194;
    state.altitude_m = 100.0;
    state.heading_deg = 180.0f;
    state.speed_mps = 10.0f;
    state.vertical_speed_mps = 1.5f;
    state.geodetic_alt_m = 105.0;
    state.height_agl_m = 50.0;
    state.h_accuracy_code = 10; /* 10m */
    state.v_accuracy_code = 11; /* 3m */
    state.operator_lat = 37.0;
    state.operator_lon = -122.0;
    strncpy(state.operator_id, "FAA-OP-999", sizeof(state.operator_id) - 1);
    strncpy(state.self_id_text, "Survey flight", sizeof(state.self_id_text) - 1);
    state.rssi = -65;
    state.last_updated_ms = 6000;

    drone_detection_t det;
    bool ok = odid_state_to_detection(&state, "rid_", DETECTION_SRC_BLE_RID, &det);

    TEST_ASSERT_TRUE(ok);

    /* drone_id should be prefixed */
    TEST_ASSERT_EQUAL_STRING("rid_SN-12345678", det.drone_id);

    TEST_ASSERT_EQUAL_UINT8(DETECTION_SRC_BLE_RID, det.source);

    /* Confidence should be 0.9 since we have location */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, det.confidence);

    /* Position */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.7749, det.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.4194, det.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 100.0, det.altitude_m);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 180.0f, det.heading_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, det.speed_mps);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, det.vertical_speed_mps);

    /* Extended position */
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 105.0, det.geodetic_alt_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 50.0, det.height_agl_m);

    /* Accuracy (code 10 -> 10.0m, code 11 -> 3.0m) */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, det.h_accuracy_m);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3.0f, det.v_accuracy_m);

    /* Operator info */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 37.0, det.operator_lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -122.0, det.operator_lon);
    TEST_ASSERT_EQUAL_STRING("FAA-OP-999", det.operator_id);

    /* Self-ID */
    TEST_ASSERT_EQUAL_STRING("Survey flight", det.self_id_text);

    /* Classification */
    TEST_ASSERT_EQUAL_UINT8(2, det.ua_type);
    TEST_ASSERT_EQUAL_UINT8(1, det.id_type);

    /* Signal */
    TEST_ASSERT_EQUAL_INT8(-65, det.rssi);

    /* Timestamps */
    TEST_ASSERT_EQUAL_INT64(5000, det.first_seen_ms);
    TEST_ASSERT_EQUAL_INT64(6000, det.last_updated_ms);
}

/* ── Unity runner ──────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_basic_id);
    RUN_TEST(test_parse_location);
    RUN_TEST(test_parse_system);
    RUN_TEST(test_parse_operator_id);
    RUN_TEST(test_parse_self_id);
    RUN_TEST(test_parse_message_pack);
    RUN_TEST(test_accuracy_code_to_meters);
    RUN_TEST(test_invalid_location_zero);
    RUN_TEST(test_state_to_detection);

    return UNITY_END();
}
