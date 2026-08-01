#include "unity.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "ble_investigation_protocol.h"
#include "ble_investigation_types.h"
#include "uart_protocol.h"

static void copy_text(char *out, size_t out_len, const char *text)
{
    snprintf(out, out_len, "%s", text);
}

static void assert_result_rejected_without_change(
    ble_investigation_result_t *result,
    const ble_investigation_chunk_t *chunk)
{
    ble_investigation_result_t expected = *result;

    TEST_ASSERT_FALSE(ble_investigation_result_accept(result, chunk));
    TEST_ASSERT_EQUAL_MEMORY(&expected, result, sizeof(expected));
}

static void assert_chunk_json(const ble_investigation_chunk_t *chunk,
                              const char *message_type)
{
    char json[UART_JSON_MAX_SIZE];
    size_t len = ble_investigation_chunk_to_json(chunk, json, sizeof(json));

    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_LESS_THAN_UINT32(UART_JSON_MAX_SIZE, len);
    TEST_ASSERT_EQUAL_UINT32(strlen(json), len);
    TEST_ASSERT_EQUAL_CHAR('{', json[0]);
    TEST_ASSERT_EQUAL_CHAR('}', json[len - 1]);
    TEST_ASSERT_NULL(strchr(json, '\n'));
    TEST_ASSERT_NULL(strchr(json, '\r'));

    char expected_type[64];
    snprintf(expected_type, sizeof(expected_type), "\"type\":\"%s\"", message_type);
    TEST_ASSERT_NOT_NULL(strstr(json, expected_type));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"request_id\":\"req-1\""));
}

void test_ble_investigation_result_defaults_to_idle(void)
{
    ble_investigation_result_t result;
    memset(&result, 0xA5, sizeof(result));

    ble_investigation_result_init(&result);

    TEST_ASSERT_EQUAL(BLE_INV_IDLE, result.state);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_GATT, result.mode);
    TEST_ASSERT_EQUAL_UINT8(0, result.service_count);
    TEST_ASSERT_EQUAL_UINT8(0, result.characteristic_count);
    TEST_ASSERT_EQUAL_UINT8(0, result.read_count);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_STRING("", result.request_id);

    const char *expected_states[] = {
        "idle", "queued", "scanning", "connecting", "discovering",
        "reading", "complete", "failed", "cancelled",
    };
    for (int state = BLE_INV_IDLE; state <= BLE_INV_CANCELLED; ++state) {
        TEST_ASSERT_EQUAL_STRING(
            expected_states[state],
            ble_investigation_state_name((ble_investigation_state_t)state));
    }
    TEST_ASSERT_EQUAL_STRING("gatt", ble_investigation_mode_name(BLE_INV_MODE_GATT));
    TEST_ASSERT_EQUAL_STRING(
        "passive_capture",
        ble_investigation_mode_name(BLE_INV_MODE_PASSIVE_CAPTURE));
}

void test_ble_investigation_protocol_emits_bounded_begin_service_char_read_end(void)
{
    ble_investigation_request_t request = {0};
    copy_text(request.request_id, sizeof(request.request_id), "req-1");
    request.mode = BLE_INV_MODE_PASSIVE_CAPTURE;
    request.timeout_ms = 12000;
    char json[UART_JSON_MAX_SIZE];
    size_t len = ble_investigation_request_to_json(&request, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_LESS_THAN_UINT32(UART_JSON_MAX_SIZE, len);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"type\":\"ble_investigate\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"mode\":\"passive_capture\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"timeout_ms\":12000"));

    ble_investigation_chunk_t chunk = {0};
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");

    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_GATT;
    copy_text(chunk.target_mac, sizeof(chunk.target_mac), "AA:BB:CC:DD:EE:FF");
    assert_chunk_json(&chunk, MSG_TYPE_BLE_INV_BEGIN);

    chunk.kind = BLE_INV_CHUNK_PROGRESS;
    chunk.state = BLE_INV_DISCOVERING;
    assert_chunk_json(&chunk, MSG_TYPE_BLE_INV_PROGRESS);

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 0;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "FFE0");
    assert_chunk_json(&chunk, MSG_TYPE_BLE_INV_SERVICE);

    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    copy_text(chunk.service_uuid, sizeof(chunk.service_uuid), "FFE0");
    copy_text(chunk.uuid, sizeof(chunk.uuid), "FFE1");
    chunk.properties = BLE_INV_PROP_READ | BLE_INV_PROP_WRITE;
    len = ble_investigation_chunk_to_json(&chunk, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"properties\":[\"read\",\"write\"]"));
    assert_chunk_json(&chunk, MSG_TYPE_BLE_INV_CHAR);

    chunk.kind = BLE_INV_CHUNK_READ;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "FFE1");
    copy_text(chunk.value_hex, sizeof(chunk.value_hex), "4142");
    assert_chunk_json(&chunk, MSG_TYPE_BLE_INV_READ);

    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.summary, sizeof(chunk.summary), "UART \"service\"\nfound");
    copy_text(chunk.error, sizeof(chunk.error), "");
    assert_chunk_json(&chunk, MSG_TYPE_BLE_INV_END);
    len = ble_investigation_chunk_to_json(&chunk, json, sizeof(json));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"summary\":\"UART \\\"service\\\"\\nfound\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"error\":null"));

    memset(chunk.summary, '"', sizeof(chunk.summary) - 1);
    chunk.summary[sizeof(chunk.summary) - 1] = '\0';
    memset(chunk.error, '\\', sizeof(chunk.error) - 1);
    chunk.error[sizeof(chunk.error) - 1] = '\0';
    len = ble_investigation_chunk_to_json(&chunk, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_LESS_THAN_UINT32(UART_JSON_MAX_SIZE, len);
    TEST_ASSERT_EQUAL_CHAR('}', json[len - 1]);
}

void test_ble_investigation_protocol_caps_service_and_characteristic_counts(void)
{
    ble_investigation_result_t result;
    ble_investigation_result_init(&result);

    ble_investigation_chunk_t chunk = {0};
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_GATT;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    for (int index = 0; index < BLE_INV_MAX_SERVICES; ++index) {
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "service-%d", index);
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    char services[sizeof(result.services)];
    memcpy(services, result.services, sizeof(services));
    chunk.index = BLE_INV_MAX_SERVICES;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "service-overflow");
    TEST_ASSERT_FALSE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_SERVICES, result.service_count);
    TEST_ASSERT_TRUE(result.truncated);
    TEST_ASSERT_EQUAL_MEMORY(services, result.services, sizeof(services));
    assert_result_rejected_without_change(&result, &chunk);
    chunk.index = INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);

    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    copy_text(chunk.service_uuid, sizeof(chunk.service_uuid), "service-0");
    chunk.properties = BLE_INV_PROP_READ;
    for (int index = 0; index < BLE_INV_MAX_CHARS; ++index) {
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "char-%d", index);
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    ble_investigation_characteristic_t characteristics[BLE_INV_MAX_CHARS];
    memcpy(characteristics, result.characteristics, sizeof(characteristics));
    chunk.index = BLE_INV_MAX_CHARS;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "char-overflow");
    TEST_ASSERT_FALSE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_CHARS, result.characteristic_count);
    TEST_ASSERT_EQUAL_MEMORY(
        characteristics,
        result.characteristics,
        sizeof(characteristics));
    assert_result_rejected_without_change(&result, &chunk);
    chunk.index = INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);

    chunk.kind = BLE_INV_CHUNK_READ;
    for (int index = 0; index < BLE_INV_MAX_READS; ++index) {
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "read-%d", index);
        copy_text(chunk.value_hex, sizeof(chunk.value_hex), "00");
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    ble_investigation_read_t reads[BLE_INV_MAX_READS];
    memcpy(reads, result.reads, sizeof(reads));
    chunk.index = BLE_INV_MAX_READS;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "read-overflow");
    TEST_ASSERT_FALSE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_READS, result.read_count);
    TEST_ASSERT_TRUE(result.truncated);
    TEST_ASSERT_EQUAL_MEMORY(reads, result.reads, sizeof(reads));
    assert_result_rejected_without_change(&result, &chunk);
    chunk.index = INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);

    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.summary, sizeof(chunk.summary), "done");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL(BLE_INV_COMPLETE, result.state);
}

void test_ble_investigation_protocol_rejects_duplicate_first_post_cap_indexes(void)
{
    ble_investigation_result_t result;
    ble_investigation_chunk_t chunk = {0};
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_GATT;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");

    ble_investigation_result_init(&result);
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    chunk.kind = BLE_INV_CHUNK_SERVICE;
    for (int index = 0; index < BLE_INV_MAX_SERVICES; ++index) {
        chunk.index = index;
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    chunk.index = BLE_INV_MAX_SERVICES - 1;
    assert_result_rejected_without_change(&result, &chunk);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_SERVICES, result.service_count);

    ble_investigation_result_init(&result);
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    for (int index = 0; index < BLE_INV_MAX_CHARS; ++index) {
        chunk.index = index;
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    chunk.index = BLE_INV_MAX_CHARS - 1;
    assert_result_rejected_without_change(&result, &chunk);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_CHARS, result.characteristic_count);

    ble_investigation_result_init(&result);
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    chunk.kind = BLE_INV_CHUNK_READ;
    for (int index = 0; index < BLE_INV_MAX_READS; ++index) {
        chunk.index = index;
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    chunk.index = BLE_INV_MAX_READS - 1;
    assert_result_rejected_without_change(&result, &chunk);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_READS, result.read_count);
}

void test_ble_investigation_protocol_rejects_int_max_first_post_cap_indexes(void)
{
    ble_investigation_result_t result;
    ble_investigation_chunk_t chunk = {0};
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_GATT;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");

    ble_investigation_result_init(&result);
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    chunk.kind = BLE_INV_CHUNK_SERVICE;
    for (int index = 0; index < BLE_INV_MAX_SERVICES; ++index) {
        chunk.index = index;
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    chunk.index = INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_SERVICES, result.service_count);

    ble_investigation_result_init(&result);
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    for (int index = 0; index < BLE_INV_MAX_CHARS; ++index) {
        chunk.index = index;
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    chunk.index = INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_CHARS, result.characteristic_count);

    ble_investigation_result_init(&result);
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    chunk.kind = BLE_INV_CHUNK_READ;
    for (int index = 0; index < BLE_INV_MAX_READS; ++index) {
        chunk.index = index;
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    chunk.index = INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);
    TEST_ASSERT_FALSE(result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_READS, result.read_count);
}

void test_ble_investigation_protocol_rejects_mismatched_request_id(void)
{
    ble_investigation_result_t result;
    ble_investigation_result_init(&result);

    ble_investigation_chunk_t chunk = {0};
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 0;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "BAD0");
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-2");
    TEST_ASSERT_FALSE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL_UINT8(0, result.service_count);

    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    chunk.index = 1;
    TEST_ASSERT_FALSE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL_UINT8(0, result.service_count);

    chunk.index = 0;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "1800");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    TEST_ASSERT_EQUAL_UINT8(1, result.service_count);
    TEST_ASSERT_EQUAL_STRING("1800", result.services[0]);
}

void test_ble_investigation_result_is_one_shot_and_terminal(void)
{
    ble_investigation_result_t result;
    ble_investigation_result_init(&result);

    ble_investigation_chunk_t chunk = {0};
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_GATT;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    copy_text(chunk.target_mac, sizeof(chunk.target_mac), "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 0;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "1800");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));

    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_PASSIVE_CAPTURE;
    copy_text(chunk.target_mac, sizeof(chunk.target_mac), "11:22:33:44:55:66");
    assert_result_rejected_without_change(&result, &chunk);
    chunk.mode = (ble_investigation_mode_t)INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);

    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.summary, sizeof(chunk.summary), "done");
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));

    copy_text(chunk.summary, sizeof(chunk.summary), "repeated end");
    assert_result_rejected_without_change(&result, &chunk);
    chunk.kind = BLE_INV_CHUNK_PROGRESS;
    chunk.state = BLE_INV_READING;
    assert_result_rejected_without_change(&result, &chunk);
    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 1;
    assert_result_rejected_without_change(&result, &chunk);
    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    chunk.index = 0;
    assert_result_rejected_without_change(&result, &chunk);
    chunk.kind = BLE_INV_CHUNK_READ;
    assert_result_rejected_without_change(&result, &chunk);
    chunk.kind = BLE_INV_CHUNK_BEGIN;
    chunk.mode = BLE_INV_MODE_GATT;
    assert_result_rejected_without_change(&result, &chunk);
    chunk.mode = (ble_investigation_mode_t)INT_MAX;
    assert_result_rejected_without_change(&result, &chunk);
}

void test_ble_investigation_request_json_uses_target_and_default_timeout(void)
{
    ble_investigation_request_t request = {0};
    copy_text(request.request_id, sizeof(request.request_id), "req-1");
    request.mode = BLE_INV_MODE_GATT;
    copy_text(request.target_mac, sizeof(request.target_mac), "AA:BB:CC:DD:EE:FF");

    char json[UART_JSON_MAX_SIZE];
    size_t len = ble_investigation_request_to_json(&request, json, sizeof(json));

    TEST_ASSERT_EQUAL_UINT32(12000, BLE_INV_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"target\":\"AA:BB:CC:DD:EE:FF\""));
    TEST_ASSERT_NULL(strstr(json, "\"target_mac\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"timeout_ms\":12000"));
}

void test_ble_investigation_chunk_encoder_validates_state_ranges(void)
{
    ble_investigation_chunk_t chunk = {0};
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    char json[UART_JSON_MAX_SIZE];

    chunk.kind = BLE_INV_CHUNK_PROGRESS;
    for (int state = BLE_INV_IDLE; state <= BLE_INV_CANCELLED; ++state) {
        chunk.state = (ble_investigation_state_t)state;
        bool valid = state >= BLE_INV_QUEUED && state <= BLE_INV_READING;
        size_t len = ble_investigation_chunk_to_json(&chunk, json, sizeof(json));
        TEST_ASSERT_EQUAL_INT(valid ? 1 : 0, len > 0 ? 1 : 0);
    }

    chunk.kind = BLE_INV_CHUNK_END;
    for (int state = BLE_INV_IDLE; state <= BLE_INV_CANCELLED; ++state) {
        chunk.state = (ble_investigation_state_t)state;
        bool valid = state >= BLE_INV_COMPLETE && state <= BLE_INV_CANCELLED;
        size_t len = ble_investigation_chunk_to_json(&chunk, json, sizeof(json));
        TEST_ASSERT_EQUAL_INT(valid ? 1 : 0, len > 0 ? 1 : 0);
    }
}

void test_ble_investigation_chunk_encoder_rejects_unknown_property_bits(void)
{
    ble_investigation_chunk_t chunk = {0};
    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    chunk.index = 0;
    chunk.properties = BLE_INV_PROP_READ | ((uint16_t)0x0100);
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    copy_text(chunk.service_uuid, sizeof(chunk.service_uuid), "1800");
    copy_text(chunk.uuid, sizeof(chunk.uuid), "2A00");

    char json[UART_JSON_MAX_SIZE];
    TEST_ASSERT_EQUAL_UINT32(
        0,
        ble_investigation_chunk_to_json(&chunk, json, sizeof(json)));
}
