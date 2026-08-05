#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_ble_investigation.h"
#include "backend_uart_investigation.h"
#include "ble_investigation_protocol.h"
#include "../support/backend_test_main.h"

#define REQUEST_ID "0123456789abcdef0123456789abcdef"
#define UUID128 "12345678-1234-5678-9abc-def012345678"
#define ALL_PROPERTIES ((uint16_t)( \
    BLE_INV_PROP_BROADCAST | BLE_INV_PROP_READ | \
    BLE_INV_PROP_WRITE_WITHOUT_RESPONSE | BLE_INV_PROP_WRITE | \
    BLE_INV_PROP_NOTIFY | BLE_INV_PROP_INDICATE | \
    BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES | \
    BLE_INV_PROP_EXTENDED_PROPERTIES))

void setUp(void) {}
void tearDown(void) {}

static backend_uart_investigation_decode_result_t decode_text(
    const char *text,
    ble_investigation_chunk_t *out)
{
    return backend_uart_investigation_decode(
        (const uint8_t *)text, strlen(text), out);
}

static void assert_rejected(const char *text)
{
    ble_investigation_chunk_t out;
    memset(&out, 0xA5, sizeof(out));
    const backend_uart_investigation_decode_result_t result =
        decode_text(text, &out);
    TEST_ASSERT_NOT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_OK, result);
    TEST_ASSERT_EQUAL_CHAR('\0', out.request_id[0]);
}

static ble_investigation_chunk_t make_chunk(
    ble_investigation_chunk_kind_t kind)
{
    ble_investigation_chunk_t value;
    memset(&value, 0, sizeof(value));
    value.kind = kind;
    strcpy(value.request_id, REQUEST_ID);
    return value;
}

static void assert_chunks_equal(
    const ble_investigation_chunk_t *expected,
    const ble_investigation_chunk_t *actual)
{
    TEST_ASSERT_EQUAL_INT(expected->kind, actual->kind);
    TEST_ASSERT_EQUAL_STRING(expected->request_id, actual->request_id);
    TEST_ASSERT_EQUAL_INT(expected->index, actual->index);
    TEST_ASSERT_EQUAL_INT(expected->state, actual->state);
    TEST_ASSERT_EQUAL_INT(expected->mode, actual->mode);
    TEST_ASSERT_EQUAL_STRING(expected->target_mac, actual->target_mac);
    TEST_ASSERT_EQUAL_STRING(expected->service_uuid, actual->service_uuid);
    TEST_ASSERT_EQUAL_STRING(expected->uuid, actual->uuid);
    TEST_ASSERT_EQUAL_UINT16(expected->properties, actual->properties);
    TEST_ASSERT_EQUAL_STRING(expected->value_hex, actual->value_hex);
    TEST_ASSERT_EQUAL_STRING(expected->summary, actual->summary);
    TEST_ASSERT_EQUAL_STRING(expected->error, actual->error);
    TEST_ASSERT_EQUAL(expected->authentication_required,
                      actual->authentication_required);
    TEST_ASSERT_EQUAL(expected->truncated, actual->truncated);
}

static void assert_round_trip(const ble_investigation_chunk_t *expected)
{
    char wire[BACKEND_UART_INVESTIGATION_MAX_JSON + 2U];
    const size_t length = ble_investigation_chunk_to_json(
        expected, wire, sizeof(wire));
    TEST_ASSERT_GREATER_THAN_UINT(0U, length);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(wire) - 1U, length);
    wire[length] = '\n';
    wire[length + 1U] = '\0';

    ble_investigation_chunk_t actual;
    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_OK,
        backend_uart_investigation_decode(
            (const uint8_t *)wire, length + 1U, &actual));
    assert_chunks_equal(expected, &actual);
}

void test_decodes_gatt_begin_without_newline_and_preserves_exact_request_id(void)
{
    static const char LINE[] =
        "{\"type\":\"ble_inv_begin\","
        "\"request_id\":\"" REQUEST_ID "\","
        "\"mode\":\"gatt\",\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}";
    ble_investigation_chunk_t actual;

    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_OK,
        decode_text(LINE, &actual));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, actual.kind);
    TEST_ASSERT_EQUAL_STRING(REQUEST_ID, actual.request_id);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_GATT, actual.mode);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", actual.target_mac);
}

void test_round_trips_every_scanner_chunk_kind_with_exact_fields(void)
{
    ble_investigation_chunk_t value = make_chunk(BLE_INV_CHUNK_BEGIN);
    value.mode = BLE_INV_MODE_GATT;
    strcpy(value.target_mac, "AA:BB:CC:DD:EE:FF");
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_PROGRESS);
    value.state = BLE_INV_READING;
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_SERVICE);
    value.index = BLE_INV_MAX_SERVICES - 1;
    strcpy(value.uuid, UUID128);
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_CHARACTERISTIC);
    value.index = BLE_INV_MAX_CHARS - 1;
    strcpy(value.service_uuid, UUID128);
    strcpy(value.uuid, "2A19");
    value.properties = ALL_PROPERTIES;
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_READ);
    value.index = BLE_INV_MAX_READS - 1;
    strcpy(value.uuid, "1234ABCD");
    strcpy(value.value_hex, "00aAbBff");
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_END);
    value.state = BLE_INV_FAILED;
    strcpy(value.summary, "scan \"done\"\nwith detail");
    strcpy(value.error, "auth\\required");
    value.authentication_required = true;
    value.truncated = true;
    assert_round_trip(&value);
}

void test_round_trips_passive_null_target_empty_properties_and_null_error(void)
{
    ble_investigation_chunk_t value = make_chunk(BLE_INV_CHUNK_BEGIN);
    value.mode = BLE_INV_MODE_PASSIVE_CAPTURE;
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_CHARACTERISTIC);
    strcpy(value.service_uuid, "180F");
    strcpy(value.uuid, "2A19");
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_READ);
    strcpy(value.uuid, "2A19");
    assert_round_trip(&value);

    value = make_chunk(BLE_INV_CHUNK_END);
    value.state = BLE_INV_COMPLETE;
    strcpy(value.summary, "complete");
    assert_round_trip(&value);
}

void test_decoded_scanner_chunk_restores_api_request_id_through_uplink_core(void)
{
    ble_investigation_request_t request = {
        .mode = BLE_INV_MODE_GATT,
        .timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS,
    };
    strcpy(request.request_id, REQUEST_ID);
    strcpy(request.target_mac, "AA:BB:CC:DD:EE:FF");
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, REQUEST_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));

    ble_investigation_chunk_t scanner = make_chunk(BLE_INV_CHUNK_BEGIN);
    scanner.mode = BLE_INV_MODE_GATT;
    strcpy(scanner.target_mac, request.target_mac);
    char wire[BACKEND_UART_INVESTIGATION_MAX_JSON + 1U];
    const size_t length = ble_investigation_chunk_to_json(
        &scanner, wire, sizeof(wire));
    TEST_ASSERT_GREATER_THAN_UINT(0U, length);

    ble_investigation_chunk_t decoded;
    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_OK,
        backend_uart_investigation_decode(
            (const uint8_t *)wire, length, &decoded));
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &decoded));

    backend_command_result_t api_result;
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(
        &state, &api_result));
    TEST_ASSERT_EQUAL_UINT32(0U, api_result.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_begin", api_result.type);
    TEST_ASSERT_NOT_NULL(strstr(
        api_result.json,
        "\"request_id\":\"" REQUEST_ID "\""));
    TEST_ASSERT_NOT_NULL(strstr(
        api_result.json,
        "\"target_mac\":\"AA:BB:CC:DD:EE:FF\""));
}

void test_rejects_malformed_embedded_nul_and_non_object_inputs(void)
{
    static const uint8_t EMBEDDED_NUL[] =
        "{\"type\":\"ble_inv_progress\",\0"
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}";
    ble_investigation_chunk_t out;

    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_MALFORMED,
        backend_uart_investigation_decode(NULL, 1U, &out));
    TEST_ASSERT_EQUAL_CHAR('\0', out.request_id[0]);
    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_MALFORMED,
        backend_uart_investigation_decode(
            (const uint8_t *)"", 0U, &out));
    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_MALFORMED,
        backend_uart_investigation_decode(
            EMBEDDED_NUL, sizeof(EMBEDDED_NUL) - 1U, &out));
    assert_rejected("{");
    assert_rejected("[]");
    assert_rejected("null");
}

void test_accepts_only_one_object_and_at_most_one_trailing_lf(void)
{
    static const char BASE[] =
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}";
    ble_investigation_chunk_t out;
    char with_lf[sizeof(BASE) + 1U];
    snprintf(with_lf, sizeof(with_lf), "%s\n", BASE);
    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_OK,
        decode_text(with_lf, &out));

    assert_rejected(
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}"
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}");
    assert_rejected(
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}x");
    assert_rejected(
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}\n\n");
    assert_rejected(
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}\r\n");
    assert_rejected(
        " {\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}");
    assert_rejected(
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"} ");
}

void test_rejects_oversized_payload_with_or_without_trailing_lf(void)
{
    uint8_t wire[BACKEND_UART_INVESTIGATION_MAX_JSON + 2U];
    memset(wire, 'x', sizeof(wire));
    ble_investigation_chunk_t out;

    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_TOO_LARGE,
        backend_uart_investigation_decode(
            wire, BACKEND_UART_INVESTIGATION_MAX_JSON + 1U, &out));
    wire[sizeof(wire) - 1U] = '\n';
    TEST_ASSERT_EQUAL(
        BACKEND_UART_INVESTIGATION_DECODE_TOO_LARGE,
        backend_uart_investigation_decode(wire, sizeof(wire), &out));
}

void test_rejects_unknown_type_duplicate_extra_and_missing_keys(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_unknown\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"queued\"}",
        "{\"type\":\"ble_inv_progress\",\"type\":\"ble_inv_progress\","
            "\"request_id\":\"" REQUEST_ID "\",\"state\":\"queued\"}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"queued\",\"extra\":0}",
        "{\"type\":\"ble_inv_progress\",\"state\":\"queued\"}",
        "{\"type\":7,\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"queued\"}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }
}

void test_rejects_noncanonical_request_ids_for_every_record(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_progress\",\"request_id\":"
            "\"0123456789ABCDEF0123456789ABCDEF\",\"state\":\"queued\"}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":"
            "\"0123456789abcdef0123456789abcde\",\"state\":\"queued\"}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":"
            "\"0123456789abcdef0123456789abcdeg\",\"state\":\"queued\"}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":null,"
            "\"state\":\"queued\"}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }
}

void test_rejects_invalid_begin_mode_mac_and_nullability_combinations(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"" REQUEST_ID
            "\",\"mode\":\"unknown\",\"target_mac\":null}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"" REQUEST_ID
            "\",\"mode\":\"gatt\",\"target_mac\":null}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"" REQUEST_ID
            "\",\"mode\":\"passive_capture\","
            "\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"" REQUEST_ID
            "\",\"mode\":\"gatt\","
            "\"target_mac\":\"aa:bb:cc:dd:ee:ff\"}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"" REQUEST_ID
            "\",\"mode\":\"gatt\","
            "\"target_mac\":\"AA-BB-CC-DD-EE-FF\"}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"" REQUEST_ID
            "\",\"mode\":null,\"target_mac\":null}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }
}

void test_rejects_unknown_or_semantically_wrong_progress_states(void)
{
    static const char *const STATES[] = {
        "idle", "complete", "failed", "cancelled", "unknown",
    };
    char json[192];
    for (size_t index = 0U;
         index < sizeof(STATES) / sizeof(STATES[0]); ++index) {
        snprintf(
            json, sizeof(json),
            "{\"type\":\"ble_inv_progress\",\"request_id\":\"%s\","
            "\"state\":\"%s\"}", REQUEST_ID, STATES[index]);
        assert_rejected(json);
    }
    assert_rejected(
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"" REQUEST_ID
        "\",\"state\":null}");
}

void test_rejects_out_of_range_service_indices_and_invalid_uuids(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_service\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":-1,\"uuid\":\"180F\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":16,\"uuid\":\"180F\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":1.5,\"uuid\":\"180F\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"uuid\":\"180\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"uuid\":\"ZZZZ\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,"
            "\"uuid\":\"12345678-1234-5678-9abcXdef012345678\"}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }
}

void test_rejects_invalid_characteristic_properties_uuids_and_indices(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":32,\"service_uuid\":\"180F\","
            "\"uuid\":\"2A19\",\"properties\":[]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"service_uuid\":\"bad\","
            "\"uuid\":\"2A19\",\"properties\":[]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"service_uuid\":\"180F\","
            "\"uuid\":\"bad\",\"properties\":[]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"service_uuid\":\"180F\","
            "\"uuid\":\"2A19\",\"properties\":7}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"service_uuid\":\"180F\","
            "\"uuid\":\"2A19\",\"properties\":[\"unknown\"]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"service_uuid\":\"180F\","
            "\"uuid\":\"2A19\",\"properties\":[\"read\",\"read\"]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"service_uuid\":\"180F\","
            "\"uuid\":\"2A19\",\"properties\":[\"read\",1]}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }
}

void test_rejects_invalid_read_indices_uuids_and_value_hex_lengths(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_read\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":8,\"uuid\":\"2A19\",\"value_hex\":\"00\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":-1,\"uuid\":\"2A19\",\"value_hex\":\"00\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"uuid\":\"bad\",\"value_hex\":\"00\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"uuid\":\"2A19\",\"value_hex\":\"abc\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"uuid\":\"2A19\",\"value_hex\":\"0g\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"" REQUEST_ID
            "\",\"index\":0,\"uuid\":\"2A19\",\"value_hex\":null}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }

    char value_hex[131];
    memset(value_hex, 'a', sizeof(value_hex) - 1U);
    value_hex[sizeof(value_hex) - 1U] = '\0';
    char json[384];
    snprintf(
        json, sizeof(json),
        "{\"type\":\"ble_inv_read\",\"request_id\":\"%s\","
        "\"index\":0,\"uuid\":\"2A19\",\"value_hex\":\"%s\"}",
        REQUEST_ID, value_hex);
    assert_rejected(json);
}

void test_rejects_wrong_end_state_nullability_flags_and_text_lengths(void)
{
    static const char *const CASES[] = {
        "{\"type\":\"ble_inv_end\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"reading\",\"summary\":\"done\","
            "\"error\":null,\"authentication_required\":false,"
            "\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"unknown\",\"summary\":\"done\","
            "\"error\":null,\"authentication_required\":false,"
            "\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"complete\",\"summary\":null,"
            "\"error\":null,\"authentication_required\":false,"
            "\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"complete\",\"summary\":\"done\","
            "\"error\":7,\"authentication_required\":false,"
            "\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"complete\",\"summary\":\"done\","
            "\"error\":null,\"authentication_required\":0,"
            "\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"" REQUEST_ID
            "\",\"state\":\"complete\",\"summary\":\"done\","
            "\"error\":null,\"authentication_required\":false,"
            "\"truncated\":null}",
    };
    for (size_t index = 0U;
         index < sizeof(CASES) / sizeof(CASES[0]); ++index) {
        assert_rejected(CASES[index]);
    }

    char summary[BLE_INV_SUMMARY_LEN + 1U];
    char error[BLE_INV_ERROR_LEN + 1U];
    memset(summary, 's', BLE_INV_SUMMARY_LEN);
    summary[BLE_INV_SUMMARY_LEN] = '\0';
    memset(error, 'e', BLE_INV_ERROR_LEN);
    error[BLE_INV_ERROR_LEN] = '\0';
    char json[512];
    snprintf(
        json, sizeof(json),
        "{\"type\":\"ble_inv_end\",\"request_id\":\"%s\","
        "\"state\":\"complete\",\"summary\":\"%s\",\"error\":null,"
        "\"authentication_required\":false,\"truncated\":false}",
        REQUEST_ID, summary);
    assert_rejected(json);
    snprintf(
        json, sizeof(json),
        "{\"type\":\"ble_inv_end\",\"request_id\":\"%s\","
        "\"state\":\"failed\",\"summary\":\"done\",\"error\":\"%s\","
        "\"authentication_required\":true,\"truncated\":false}",
        REQUEST_ID, error);
    assert_rejected(json);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_decodes_gatt_begin_without_newline_and_preserves_exact_request_id);
    BACKEND_RUN_TEST(
        test_round_trips_every_scanner_chunk_kind_with_exact_fields);
    BACKEND_RUN_TEST(
        test_round_trips_passive_null_target_empty_properties_and_null_error);
    BACKEND_RUN_TEST(
        test_decoded_scanner_chunk_restores_api_request_id_through_uplink_core);
    BACKEND_RUN_TEST(
        test_rejects_malformed_embedded_nul_and_non_object_inputs);
    BACKEND_RUN_TEST(
        test_accepts_only_one_object_and_at_most_one_trailing_lf);
    BACKEND_RUN_TEST(
        test_rejects_oversized_payload_with_or_without_trailing_lf);
    BACKEND_RUN_TEST(
        test_rejects_unknown_type_duplicate_extra_and_missing_keys);
    BACKEND_RUN_TEST(
        test_rejects_noncanonical_request_ids_for_every_record);
    BACKEND_RUN_TEST(
        test_rejects_invalid_begin_mode_mac_and_nullability_combinations);
    BACKEND_RUN_TEST(
        test_rejects_unknown_or_semantically_wrong_progress_states);
    BACKEND_RUN_TEST(
        test_rejects_out_of_range_service_indices_and_invalid_uuids);
    BACKEND_RUN_TEST(
        test_rejects_invalid_characteristic_properties_uuids_and_indices);
    BACKEND_RUN_TEST(
        test_rejects_invalid_read_indices_uuids_and_value_hex_lengths);
    BACKEND_RUN_TEST(
        test_rejects_wrong_end_state_nullability_flags_and_text_lengths);
    return UNITY_END();
}
