#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_config.h"
#include "../support/backend_test_main.h"

static void copy_logical_string(char *out, size_t capacity, const char *value)
{
    const size_t length = strlen(value);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(capacity, length + 1U);
    memcpy(out, value, length + 1U);
}

static backend_config_record_t valid_config_fixture(void)
{
    backend_config_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    record.generation = 7;
    record.network_count = 1;
    strcpy(record.networks[0].ssid, "FieldNet");
    strcpy(record.networks[0].password, "secret-value");
    strcpy(record.backend_url, "http://10.0.0.2:8000");
    strcpy(record.device_id, "uplink_CB77A4");
    strcpy(record.display_name, "Lite Front Yard");
    strcpy(record.ap_password, "friendorfoe");
    record.auto_update_enabled = false;
    record.has_location = true;
    record.latitude = 37.7749;
    record.longitude = -122.4194;
    record.altitude_m = 16.5f;
    return record;
}

static void assert_logical_record_equal(
    const backend_config_record_t *expected,
    const backend_config_record_t *actual)
{
    TEST_ASSERT_EQUAL_UINT16(expected->schema_version, actual->schema_version);
    TEST_ASSERT_EQUAL_UINT32(expected->generation, actual->generation);
    TEST_ASSERT_EQUAL_UINT8(expected->network_count, actual->network_count);
    for (uint8_t index = 0; index < expected->network_count; ++index) {
        TEST_ASSERT_EQUAL_STRING(
            expected->networks[index].ssid, actual->networks[index].ssid);
        TEST_ASSERT_EQUAL_STRING(
            expected->networks[index].password,
            actual->networks[index].password);
    }
    TEST_ASSERT_EQUAL_STRING(expected->backend_url, actual->backend_url);
    TEST_ASSERT_EQUAL_STRING(expected->device_id, actual->device_id);
    TEST_ASSERT_EQUAL_STRING(expected->display_name, actual->display_name);
    TEST_ASSERT_EQUAL_STRING(expected->ap_password, actual->ap_password);
    TEST_ASSERT_EQUAL(expected->auto_update_enabled,
                      actual->auto_update_enabled);
    TEST_ASSERT_EQUAL(expected->has_location, actual->has_location);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, expected->latitude, actual->latitude);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, expected->longitude, actual->longitude);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, expected->altitude_m, actual->altitude_m);
}

void test_config_accepts_four_networks_and_preserves_device_id(void)
{
    backend_config_record_t record = valid_config_fixture();
    record.network_count = 4;
    for (uint8_t index = 1; index < record.network_count; ++index) {
        snprintf(record.networks[index].ssid,
                 sizeof(record.networks[index].ssid), "FieldNet-%u", index);
        snprintf(record.networks[index].password,
                 sizeof(record.networks[index].password), "password-%u", index);
    }
    strcpy(record.device_id, "uplink_CB77A4");

    TEST_ASSERT_EQUAL(BACKEND_CONFIG_VALID, backend_config_validate(&record));
    backend_config_blob_t blob = {0};
    TEST_ASSERT_TRUE(backend_config_encode_canonical(&record, &blob));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(BACKEND_CONFIG_BLOB_MAX, blob.length);

    backend_config_record_t decoded = {0};
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_VALID,
        backend_config_decode_canonical(blob.bytes, blob.length, &decoded));
    assert_logical_record_equal(&record, &decoded);
    TEST_ASSERT_EQUAL_STRING("uplink_CB77A4", decoded.device_id);
}

void test_canonical_encoding_has_exact_order_endianness_and_crc(void)
{
    backend_config_record_t record;
    memset(&record, 0, sizeof(record));
    record.schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
    record.generation = 7;
    record.network_count = 1;
    strcpy(record.networks[0].ssid, "A");
    strcpy(record.networks[0].password, "B");
    strcpy(record.backend_url, "http://h");
    strcpy(record.device_id, "uplink_CB77A4");
    strcpy(record.display_name, "Lite");
    strcpy(record.ap_password, "12345678");
    record.auto_update_enabled = true;
    record.has_location = true;
    record.latitude = 1.0;
    record.longitude = -2.0;
    record.altitude_m = 3.5f;

    static const uint8_t expected[] =
        "\x42\x43\x46\x47\x01\x00\x46\x00\x07\x00\x00\x00"
        "\x01\x01\x00\x41\x01\x00\x42\x08\x00\x68\x74\x74\x70"
        "\x3a\x2f\x2f\x68\x0d\x00\x75\x70\x6c\x69\x6e\x6b\x5f"
        "\x43\x42\x37\x37\x41\x34\x04\x00\x4c\x69\x74\x65\x08"
        "\x00\x31\x32\x33\x34\x35\x36\x37\x38\x01\x01\x00\x00"
        "\x00\x00\x00\x00\xf0\x3f\x00\x00\x00\x00\x00\x00\x00"
        "\xc0\x00\x00\x60\x40\xf6\x4f\x7b\xcd";

    backend_config_blob_t blob = {0};
    TEST_ASSERT_TRUE(backend_config_encode_canonical(&record, &blob));
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected) - 1U, blob.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, blob.bytes, sizeof(expected) - 1U);
}

void test_canonical_encoding_ignores_struct_padding_and_string_tails(void)
{
    backend_config_record_t left;
    backend_config_record_t right;
    memset(&left, 0xAA, sizeof(left));
    memset(&right, 0x55, sizeof(right));

    backend_config_record_t *records[] = {&left, &right};
    for (size_t index = 0; index < 2; ++index) {
        backend_config_record_t *record = records[index];
        record->schema_version = BACKEND_CONFIG_SCHEMA_VERSION;
        record->generation = 42;
        record->network_count = 2;
        copy_logical_string(record->networks[0].ssid,
                            sizeof(record->networks[0].ssid), "Alpha");
        copy_logical_string(record->networks[0].password,
                            sizeof(record->networks[0].password), "one");
        copy_logical_string(record->networks[1].ssid,
                            sizeof(record->networks[1].ssid), "Bravo");
        copy_logical_string(record->networks[1].password,
                            sizeof(record->networks[1].password), "two");
        copy_logical_string(record->backend_url, sizeof(record->backend_url),
                            "http://backend.local:8000");
        copy_logical_string(record->device_id, sizeof(record->device_id),
                            "uplink_CB77A4");
        copy_logical_string(record->display_name, sizeof(record->display_name),
                            "Lite");
        copy_logical_string(record->ap_password, sizeof(record->ap_password),
                            "friendorfoe");
        record->auto_update_enabled = false;
        record->has_location = true;
        record->latitude = 1.25;
        record->longitude = -2.5;
        record->altitude_m = 3.75f;
    }

    backend_config_blob_t left_blob = {0};
    backend_config_blob_t right_blob = {0};
    TEST_ASSERT_TRUE(backend_config_encode_canonical(&left, &left_blob));
    TEST_ASSERT_TRUE(backend_config_encode_canonical(&right, &right_blob));
    TEST_ASSERT_EQUAL_UINT32(left_blob.length, right_blob.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        left_blob.bytes, right_blob.bytes, left_blob.length);
}

void test_config_validation_rejects_field_boundaries(void)
{
    backend_config_record_t record = valid_config_fixture();

    record.schema_version = BACKEND_CONFIG_SCHEMA_VERSION + 1U;
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    record.generation = 0;
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    record.network_count = BACKEND_CONFIG_MAX_NETWORKS + 1U;
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));

    record = valid_config_fixture();
    memset(record.networks[0].ssid, 'S', sizeof(record.networks[0].ssid));
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    memset(record.networks[0].password, 'P',
           sizeof(record.networks[0].password));
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));

    record = valid_config_fixture();
    strcpy(record.backend_url, "https://backend.local");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    strcpy(record.backend_url, "http://:8000");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    strcpy(record.backend_url, "http://user@");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    strcpy(record.backend_url, "http://user@/path");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    strcpy(record.backend_url, "http://user@backend.local:8000/path");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_VALID,
                      backend_config_validate(&record));

    record = valid_config_fixture();
    strcpy(record.ap_password, "1234567");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    memset(record.ap_password, 'A', 64);
    record.ap_password[64] = '\0';
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));

    record = valid_config_fixture();
    record.latitude = -90.001;
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    record.longitude = 180.001;
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
    record = valid_config_fixture();
    record.altitude_m = INFINITY;
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_INVALID_FIELD,
                      backend_config_validate(&record));
}

static void assert_decode_result_preserves_output(
    const uint8_t *bytes,
    size_t length,
    backend_config_result_t expected_result)
{
    backend_config_record_t actual;
    backend_config_record_t sentinel;
    memset(&actual, 0x5A, sizeof(actual));
    memcpy(&sentinel, &actual, sizeof(sentinel));
    TEST_ASSERT_EQUAL(expected_result,
        backend_config_decode_canonical(bytes, length, &actual));
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &actual, sizeof(actual));
}

void test_decoder_rejects_header_payload_crc_and_length_mutations_atomically(void)
{
    backend_config_record_t record = valid_config_fixture();
    backend_config_blob_t blob = {0};
    TEST_ASSERT_TRUE(backend_config_encode_canonical(&record, &blob));

    backend_config_blob_t changed = blob;
    changed.bytes[0] ^= UINT8_C(0x01);
    assert_decode_result_preserves_output(
        changed.bytes, changed.length, BACKEND_CONFIG_INVALID_FIELD);

    changed = blob;
    changed.bytes[8] ^= UINT8_C(0x01);
    assert_decode_result_preserves_output(
        changed.bytes, changed.length, BACKEND_CONFIG_INVALID_CRC);

    changed = blob;
    changed.bytes[12] ^= UINT8_C(0x01);
    assert_decode_result_preserves_output(
        changed.bytes, changed.length, BACKEND_CONFIG_INVALID_CRC);

    changed = blob;
    changed.bytes[changed.length - 1U] ^= UINT8_C(0x01);
    assert_decode_result_preserves_output(
        changed.bytes, changed.length, BACKEND_CONFIG_INVALID_CRC);

    assert_decode_result_preserves_output(
        blob.bytes, blob.length - 1U, BACKEND_CONFIG_INVALID_LENGTH);
    changed = blob;
    changed.bytes[changed.length] = 0;
    assert_decode_result_preserves_output(
        changed.bytes, changed.length + 1U, BACKEND_CONFIG_INVALID_LENGTH);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_config_accepts_four_networks_and_preserves_device_id);
    BACKEND_RUN_TEST(test_canonical_encoding_has_exact_order_endianness_and_crc);
    BACKEND_RUN_TEST(test_canonical_encoding_ignores_struct_padding_and_string_tails);
    BACKEND_RUN_TEST(test_config_validation_rejects_field_boundaries);
    BACKEND_RUN_TEST(test_decoder_rejects_header_payload_crc_and_length_mutations_atomically);
    return UNITY_END();
}
