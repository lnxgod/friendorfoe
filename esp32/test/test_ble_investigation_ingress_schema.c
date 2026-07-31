#include "unity.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_investigation_ingress_schema.h"
#include "ble_investigation_protocol.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    ble_investigation_chunk_kind_t kind;
    fof_ble_inv_ingress_schema_id_t expected_schema;
} chunk_fixture_t;

static const chunk_fixture_t CHUNK_FIXTURES[] = {
    {BLE_INV_CHUNK_BEGIN, FOF_BLE_INV_INGRESS_BEGIN},
    {BLE_INV_CHUNK_PROGRESS, FOF_BLE_INV_INGRESS_PROGRESS},
    {BLE_INV_CHUNK_SERVICE, FOF_BLE_INV_INGRESS_SERVICE},
    {BLE_INV_CHUNK_CHARACTERISTIC, FOF_BLE_INV_INGRESS_CHARACTERISTIC},
    {BLE_INV_CHUNK_READ, FOF_BLE_INV_INGRESS_READ},
    {BLE_INV_CHUNK_END, FOF_BLE_INV_INGRESS_END},
};

static void fixture_chunk(
    ble_investigation_chunk_kind_t kind,
    ble_investigation_chunk_t *chunk)
{
    memset(chunk, 0, sizeof(*chunk));
    chunk->kind = kind;
    memcpy(chunk->request_id, "req-1", sizeof("req-1"));
    chunk->mode = BLE_INV_MODE_GATT;
    memcpy(
        chunk->target_mac, "AA:BB:CC:DD:EE:FF",
        sizeof("AA:BB:CC:DD:EE:FF"));
    chunk->state = BLE_INV_SCANNING;
    chunk->index = 0;
    memcpy(
        chunk->service_uuid, "180D",
        sizeof("180D"));
    memcpy(
        chunk->uuid, "00002A37-0000-1000-8000-00805F9B34FB",
        sizeof("00002A37-0000-1000-8000-00805F9B34FB"));
    chunk->properties = BLE_INV_PROP_READ | BLE_INV_PROP_NOTIFY;
    memcpy(chunk->value_hex, "00FF", sizeof("00FF"));
    memcpy(chunk->summary, "complete", sizeof("complete"));
    chunk->authentication_required = true;
    chunk->truncated = false;

    if (kind == BLE_INV_CHUNK_END) {
        chunk->state = BLE_INV_COMPLETE;
    }
}

static fof_ble_inv_ingress_result_t validate_wire(
    const char *wire,
    int scanner_slot,
    fof_ble_inv_ingress_schema_id_t *schema_out)
{
    return fof_ble_investigation_ingress_validate(
        (const uint8_t *)wire, strlen(wire), scanner_slot, schema_out);
}

static void assert_rejected(const char *wire)
{
    fof_ble_inv_ingress_schema_id_t schema =
        (fof_ble_inv_ingress_schema_id_t)0x5a;
    fof_ble_inv_ingress_result_t result = validate_wire(
        wire, FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema);
    TEST_ASSERT_NOT_EQUAL(FOF_BLE_INV_INGRESS_OK, result);
    TEST_ASSERT_EQUAL(FOF_BLE_INV_INGRESS_NONE, schema);
}

void test_ble_inv_ingress_accepts_every_real_producer_frame(void)
{
    char wire[1024];
    for (size_t i = 0U; i < ARRAY_SIZE(CHUNK_FIXTURES); ++i) {
        ble_investigation_chunk_t chunk;
        fixture_chunk(CHUNK_FIXTURES[i].kind, &chunk);
        size_t wire_len = ble_investigation_chunk_to_json(
            &chunk, wire, sizeof(wire));
        TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, wire_len, wire);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(strlen(wire), wire_len, wire);

        fof_ble_inv_ingress_schema_id_t schema =
            FOF_BLE_INV_INGRESS_NONE;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_BLE_INV_INGRESS_OK,
            fof_ble_investigation_ingress_validate(
                (const uint8_t *)wire, wire_len,
                FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema),
            wire);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            CHUNK_FIXTURES[i].expected_schema, schema, wire);
    }
}

void test_ble_inv_ingress_accepts_nullable_begin_and_end_variants(void)
{
    static const char *const accepted[] = {
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"req\","
        "\"mode\":\"passive_capture\",\"target_mac\":null}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"failed\",\"summary\":\"not found\",\"error\":\"timeout\","
        "\"authentication_required\":false,\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"cancelled\",\"summary\":\"cancelled\",\"error\":null,"
        "\"authentication_required\":false,\"truncated\":true}",
    };
    static const fof_ble_inv_ingress_schema_id_t expected[] = {
        FOF_BLE_INV_INGRESS_BEGIN,
        FOF_BLE_INV_INGRESS_END,
        FOF_BLE_INV_INGRESS_END,
    };

    for (size_t i = 0U; i < ARRAY_SIZE(accepted); ++i) {
        fof_ble_inv_ingress_schema_id_t schema =
            FOF_BLE_INV_INGRESS_NONE;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            FOF_BLE_INV_INGRESS_OK,
            validate_wire(
                accepted[i], FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema),
            accepted[i]);
        TEST_ASSERT_EQUAL_INT(expected[i], schema);
    }
}

void test_ble_inv_ingress_accepts_zero_length_read_value(void)
{
    ble_investigation_chunk_t chunk;
    fixture_chunk(BLE_INV_CHUNK_READ, &chunk);
    chunk.value_hex[0] = '\0';

    char wire[1024];
    size_t wire_len = ble_investigation_chunk_to_json(
        &chunk, wire, sizeof(wire));
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, wire_len, wire);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ble_inv_read\",\"request_id\":\"req-1\","
        "\"index\":0,\"uuid\":\"00002A37-0000-1000-8000-00805F9B34FB\","
        "\"value_hex\":\"\"}",
        wire);

    fof_ble_inv_ingress_schema_id_t schema =
        FOF_BLE_INV_INGRESS_NONE;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        FOF_BLE_INV_INGRESS_OK,
        fof_ble_investigation_ingress_validate(
            (const uint8_t *)wire, wire_len,
            FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema),
        wire);
    TEST_ASSERT_EQUAL_INT(FOF_BLE_INV_INGRESS_READ, schema);
}

void test_ble_inv_ingress_enforces_terminal_text_capacity(void)
{
    char summary[BLE_INV_SUMMARY_LEN + 1U];
    char error[BLE_INV_ERROR_LEN + 1U];
    char escaped_summary[(BLE_INV_SUMMARY_LEN * 2U) + 1U];
    char wire[512];
    memset(summary, 'S', sizeof(summary));
    memset(error, 'E', sizeof(error));

    summary[BLE_INV_SUMMARY_LEN - 1U] = '\0';
    error[BLE_INV_ERROR_LEN - 1U] = '\0';
    int wire_len = snprintf(
        wire, sizeof(wire),
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"complete\",\"summary\":\"%s\",\"error\":\"%s\","
        "\"authentication_required\":false,\"truncated\":false}",
        summary, error);
    TEST_ASSERT_GREATER_THAN_INT(0, wire_len);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(wire), wire_len);

    fof_ble_inv_ingress_schema_id_t schema =
        FOF_BLE_INV_INGRESS_NONE;
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_OK,
        fof_ble_investigation_ingress_validate(
            (const uint8_t *)wire, (size_t)wire_len,
            FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema));
    TEST_ASSERT_EQUAL_INT(FOF_BLE_INV_INGRESS_END, schema);

    for (size_t i = 0U; i < BLE_INV_SUMMARY_LEN - 1U; ++i) {
        escaped_summary[i * 2U] = '\\';
        escaped_summary[(i * 2U) + 1U] = '"';
    }
    escaped_summary[(BLE_INV_SUMMARY_LEN - 1U) * 2U] = '\0';
    wire_len = snprintf(
        wire, sizeof(wire),
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"complete\",\"summary\":\"%s\",\"error\":null,"
        "\"authentication_required\":false,\"truncated\":false}",
        escaped_summary);
    TEST_ASSERT_GREATER_THAN_INT(0, wire_len);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(wire), wire_len);
    schema = FOF_BLE_INV_INGRESS_NONE;
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_OK,
        fof_ble_investigation_ingress_validate(
            (const uint8_t *)wire, (size_t)wire_len,
            FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema));
    TEST_ASSERT_EQUAL_INT(FOF_BLE_INV_INGRESS_END, schema);

    summary[BLE_INV_SUMMARY_LEN - 1U] = 'S';
    summary[BLE_INV_SUMMARY_LEN] = '\0';
    wire_len = snprintf(
        wire, sizeof(wire),
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"complete\",\"summary\":\"%s\",\"error\":null,"
        "\"authentication_required\":false,\"truncated\":false}",
        summary);
    TEST_ASSERT_GREATER_THAN_INT(0, wire_len);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(wire), wire_len);
    assert_rejected(wire);

    error[BLE_INV_ERROR_LEN - 1U] = 'E';
    error[BLE_INV_ERROR_LEN] = '\0';
    wire_len = snprintf(
        wire, sizeof(wire),
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"complete\",\"summary\":\"done\",\"error\":\"%s\","
        "\"authentication_required\":false,\"truncated\":false}",
        error);
    TEST_ASSERT_GREATER_THAN_INT(0, wire_len);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(wire), wire_len);
    assert_rejected(wire);
}

void test_ble_inv_ingress_requires_exact_scanner_slot(void)
{
    static const char *const wire =
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\","
        "\"state\":\"scanning\"}";
    fof_ble_inv_ingress_schema_id_t schema =
        (fof_ble_inv_ingress_schema_id_t)0x5a;

    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_WRONG_SCANNER_SLOT,
        validate_wire(wire, 1, &schema));
    TEST_ASSERT_EQUAL(FOF_BLE_INV_INGRESS_NONE, schema);
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_WRONG_SCANNER_SLOT,
        validate_wire(wire, -1, &schema));
    TEST_ASSERT_EQUAL(FOF_BLE_INV_INGRESS_NONE, schema);
}

void test_ble_inv_ingress_rejects_prefix_near_matches_and_selector_corruption(
    void)
{
    static const char *const rejected[] = {
        "{\"type\":\"ble_inv_\"}",
        "{\"type\":\"ble_inv_unknown\",\"request_id\":\"req\"}",
        "{\"type\":\"ble_inv_beginning\",\"request_id\":\"req\"}",
        "{\"type\":\"ble_inv_progress\",\"type\":\"ble_inv_end\","
        "\"request_id\":\"req\",\"state\":\"scanning\"}",
        "{\"type\":\"ble_inv_end\",\"type\":\"ble_inv_progress\","
        "\"request_id\":\"req\",\"state\":\"scanning\"}",
        "{\"t\\u0079pe\":\"ble_inv_progress\",\"request_id\":\"req\","
        "\"state\":\"scanning\"}",
        "{\"type\":\"ble_inv_progr\\u0065ss\",\"request_id\":\"req\","
        "\"state\":\"scanning\"}",
        "{\"type\":1,\"request_id\":\"req\",\"state\":\"scanning\"}",
        "{\"request_id\":\"req\",\"state\":\"scanning\"}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        assert_rejected(rejected[i]);
    }
}

void test_ble_inv_ingress_rejects_exact_schema_corruption(void)
{
    static const char *const rejected[] = {
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target_mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"extra\":true}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"req\","
        "\"index\":\"0\",\"uuid\":\"180d\"}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"req\",\"index\":0,"
        "\"service_uuid\":\"180d\",\"uuid\":\"2a37\","
        "\"properties\":{}}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"req\",\"index\":0,"
        "\"uuid\":\"2a37\",\"value_hex\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"complete\",\"summary\":\"done\",\"error\":null,"
        "\"authentication_required\":1,\"truncated\":false}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\","
        "\"state\":\"scanning\"} true",
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\","
        "\"state\":\"scan\\u0000ning\"}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\\u0031\","
        "\"state\":\"scanning\"}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"complete\",\"summary\":\"bad\\u0000summary\","
        "\"error\":null,\"authentication_required\":false,"
        "\"truncated\":false}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"failed\",\"summary\":\"failed\","
        "\"error\":\"bad\\u007ferror\","
        "\"authentication_required\":false,\"truncated\":false}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        assert_rejected(rejected[i]);
    }

    static const uint8_t embedded_nul[] = {
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"',
        'b', 'l', 'e', '_', 'i', 'n', 'v', '_', 'p', 'r', 'o', 'g',
        'r', 'e', 's', 's', '"', ',', '"', 'r', 'e', 'q', 'u', 'e',
        's', 't', '_', 'i', 'd', '"', ':', '"', 'r', 'e', 'q', '"',
        ',', '"', 's', 't', 'a', 't', 'e', '"', ':', '"', 's', 'c',
        'a', 'n', 'n', 'i', 'n', 'g', '"', '}', '\0',
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'b', 'l', 'e',
        '_', 'i', 'n', 'v', '_', 'e', 'n', 'd', '"', '}',
    };
    fof_ble_inv_ingress_schema_id_t schema =
        (fof_ble_inv_ingress_schema_id_t)0x5a;
    TEST_ASSERT_NOT_EQUAL(
        FOF_BLE_INV_INGRESS_OK,
        fof_ble_investigation_ingress_validate(
            embedded_nul, sizeof(embedded_nul),
            FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema));
    TEST_ASSERT_EQUAL(FOF_BLE_INV_INGRESS_NONE, schema);
}

void test_ble_inv_ingress_enforces_type_specific_value_policy(void)
{
    static const char *const rejected[] = {
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target_mac\":null}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"req\","
        "\"mode\":\"passive_capture\","
        "\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}",
        "{\"type\":\"ble_inv_begin\",\"request_id\":\"req\","
        "\"mode\":\"gatt\",\"target_mac\":\"aa:bb:cc:dd:ee:ff\"}",
        "{\"type\":\"ble_inv_progress\",\"request_id\":\"req\","
        "\"state\":\"complete\"}",
        "{\"type\":\"ble_inv_service\",\"request_id\":\"req\","
        "\"index\":16,\"uuid\":\"180D\"}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"req\",\"index\":32,"
        "\"service_uuid\":\"180D\",\"uuid\":\"2A37\","
        "\"properties\":[]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"req\",\"index\":0,"
        "\"service_uuid\":\"180D\",\"uuid\":\"2A37\","
        "\"properties\":[\"read\",\"read\"]}",
        "{\"type\":\"ble_inv_char\",\"request_id\":\"req\",\"index\":0,"
        "\"service_uuid\":\"180D\",\"uuid\":\"2A37\","
        "\"properties\":[\"execute\"]}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"req\",\"index\":8,"
        "\"uuid\":\"2A37\",\"value_hex\":\"00\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"req\",\"index\":0,"
        "\"uuid\":\"2A37\",\"value_hex\":\"0\"}",
        "{\"type\":\"ble_inv_read\",\"request_id\":\"req\",\"index\":0,"
        "\"uuid\":\"2A37\",\"value_hex\":\"GG\"}",
        "{\"type\":\"ble_inv_end\",\"request_id\":\"req\","
        "\"state\":\"reading\",\"summary\":\"done\",\"error\":null,"
        "\"authentication_required\":false,\"truncated\":false}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(rejected); ++i) {
        assert_rejected(rejected[i]);
    }
}

void test_ble_inv_ingress_is_order_independent_and_output_atomic(void)
{
    static const char *const permuted =
        "{\"state\":\"discovering\",\"request_id\":\"req\","
        "\"type\":\"ble_inv_progress\"}";
    fof_ble_inv_ingress_schema_id_t schema =
        FOF_BLE_INV_INGRESS_NONE;
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_OK,
        validate_wire(
            permuted, FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema));
    TEST_ASSERT_EQUAL(FOF_BLE_INV_INGRESS_PROGRESS, schema);

    schema = (fof_ble_inv_ingress_schema_id_t)0x5a;
    TEST_ASSERT_EQUAL_INT(
        FOF_BLE_INV_INGRESS_INVALID_ARGUMENT,
        fof_ble_investigation_ingress_validate(
            NULL, 0U, FOF_BLE_INVESTIGATION_SCANNER_SLOT, &schema));
    TEST_ASSERT_EQUAL(FOF_BLE_INV_INGRESS_NONE, schema);
}
