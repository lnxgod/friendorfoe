#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "ble_investigation_protocol.h"
#include "ble_investigation_types.h"
#include "uart_protocol.h"

static void copy_text(char *out, size_t out_len, const char *text)
{
    snprintf(out, out_len, "%s", text);
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
    for (int index = 0; index <= BLE_INV_MAX_SERVICES; ++index) {
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "service-%d", index);
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_SERVICES, result.service_count);

    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    copy_text(chunk.service_uuid, sizeof(chunk.service_uuid), "service-0");
    chunk.properties = BLE_INV_PROP_READ;
    for (int index = 0; index <= BLE_INV_MAX_CHARS; ++index) {
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "char-%d", index);
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_CHARS, result.characteristic_count);

    chunk.kind = BLE_INV_CHUNK_READ;
    for (int index = 0; index <= BLE_INV_MAX_READS; ++index) {
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "read-%d", index);
        copy_text(chunk.value_hex, sizeof(chunk.value_hex), "00");
        TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunk));
    }
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_READS, result.read_count);
    TEST_ASSERT_TRUE(result.truncated);
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
