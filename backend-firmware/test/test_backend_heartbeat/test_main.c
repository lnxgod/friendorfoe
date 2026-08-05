#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_http_policy.h"
#include "backend_identity.h"
#include "backend_ingest_ack.h"
#include "backend_json_reader.h"
#include "backend_upload_batch.h"
#include "backend_upload_fifo.h"
#include "backend_uploader.h"
#include "../support/backend_test_main.h"

void setUp(void) {}
void tearDown(void) {}

static backend_batch_context_t heartbeat_context(void)
{
    backend_batch_context_t context = {
        .boot_id = UINT32_C(0x10203040),
        .topology_generation = 7U,
        .capability_count = 2U,
        .scanner_present = {true, false},
        .clock_valid = true,
        .epoch_ms = INT64_C(1785600000123),
        .wifi_rssi = -61,
        .ap_active = true,
        .config_generation = 17U,
        .command_success_count = 8U,
        .command_failure_count = 2U,
        .uptime_ms = UINT64_C(7654321),
        .led_state = BACKEND_LED_NETWORK_DEGRADED,
        .upload_queue = {
            .depth_batches = 4U,
            .capacity_batches = BACKEND_UPLOAD_FIFO_CAPACITY,
            .overflow_dropped_batches = 3U,
            .quarantined_batches = 1U,
        },
        .upload = {
            .ok = 12U,
            .failed = 5U,
            .retry_count = 9U,
            .has_last_success_age = true,
            .last_success_age_s = 7U,
        },
        .sequence = 77U,
    };
    strcpy(context.device_id, "uplink_CB77A4");
    strcpy(context.product_family, FOF_BACKEND_PRODUCT_FAMILY);
    strcpy(context.firmware_line, FOF_BACKEND_FIRMWARE_LINE);
    strcpy(context.component, "uplink");
    strcpy(context.firmware_version, "0.1.0-backend");
    strcpy(context.firmware_target, FOF_BACKEND_UPLINK_TARGET);
    strcpy(context.app_project, FOF_BACKEND_UPLINK_PROJECT);
    strcpy(context.hardware_type, FOF_BACKEND_HARDWARE);
    strcpy(context.hardware_mac, "A4:CF:12:CB:77:A4");
    strcpy(context.node_name, "West hall sensor");
    strcpy(context.capabilities[0], "dual_scanner");
    strcpy(context.capabilities[1], "backend_upload");
    strcpy(context.wifi_ssid, "FoF Ops");

    backend_scanner_status_t *scanner = &context.scanners[0];
    scanner->schema = BACKEND_SCANNER_STATUS_SCHEMA;
    scanner->sequence = 29U;
    scanner->boot_id = UINT32_C(0x1234ABCD);
    strcpy(scanner->mac, "AA:BB:CC:DD:EE:01");
    strcpy(scanner->target, FOF_BACKEND_SCANNER_TARGET);
    strcpy(scanner->project, FOF_BACKEND_SCANNER_PROJECT);
    strcpy(scanner->hardware, FOF_BACKEND_HARDWARE);
    strcpy(scanner->version, "0.1.0-backend");
    scanner->profile = BACKEND_SCAN_PROFILE_BLE_PRIMARY;
    scanner->role_generation = 6U;
    scanner->role_acked = true;
    scanner->command_ingress = true;
    scanner->ble_healthy = true;
    scanner->wifi_healthy = false;
    strcpy(scanner->ota_state, "idle");
    strcpy(scanner->rollback_state, "valid");
    return context;
}

static backend_upload_batch_t build_empty_heartbeat(void)
{
    const backend_batch_context_t context = heartbeat_context();
    backend_upload_builder_t builder;
    backend_upload_builder_init(&builder, &context, 1000);
    TEST_ASSERT_TRUE(builder.active);
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));
    TEST_ASSERT_EQUAL_UINT16(0U, batch.item_count);
    return batch;
}

static size_t require_key(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key)
{
    size_t value_index = 0U;
    TEST_ASSERT_TRUE_MESSAGE(backend_json_object_find(
        json,
        tokens,
        token_count,
        object_index,
        key,
        &value_index), key);
    return value_index;
}

static void assert_string_key(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key,
    const char *expected)
{
    char actual[80];
    const size_t value_index = require_key(
        json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_copy_string(
        json, &tokens[value_index], actual, sizeof(actual)));
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

static void assert_i64_key(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key,
    int64_t expected)
{
    int64_t actual = 0;
    const size_t value_index = require_key(
        json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_get_i64(
        json, &tokens[value_index], &actual));
    TEST_ASSERT_EQUAL_INT64(expected, actual);
}

static void assert_bool_key(
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key,
    bool expected)
{
    bool actual = !expected;
    const size_t value_index = require_key(
        json, tokens, token_count, object_index, key);
    TEST_ASSERT_TRUE(backend_json_get_bool(
        json, &tokens[value_index], &actual));
    TEST_ASSERT_EQUAL(expected, actual);
}

void test_empty_heartbeat_contains_exact_operational_fields(void)
{
    const backend_upload_batch_t batch = build_empty_heartbeat();
    TEST_ASSERT_EQUAL_UINT32(77U, batch.sequence);
    TEST_ASSERT_EQUAL_UINT32(
        backend_identity_crc32(batch.json, batch.json_len),
        batch.json_crc32);

    backend_json_token_t tokens[BACKEND_JSON_MAX_TOKENS];
    size_t token_count = 0U;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OK, backend_json_parse(
        batch.json,
        batch.json_len,
        tokens,
        BACKEND_JSON_MAX_TOKENS,
        &token_count));
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[0].kind);
    TEST_ASSERT_EQUAL_UINT16(44U, tokens[0].child_count);

    assert_string_key(batch.json, tokens, token_count, 0U,
                      "device_id", "uplink_CB77A4");
    assert_i64_key(batch.json, tokens, token_count, 0U,
                   "boot_id", UINT32_C(0x10203040));
    assert_i64_key(batch.json, tokens, token_count, 0U,
                   "topology_generation", 7);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "product_family", FOF_BACKEND_PRODUCT_FAMILY);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "firmware_line", FOF_BACKEND_FIRMWARE_LINE);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "component", "uplink");
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "firmware_version", "0.1.0-backend");
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "firmware_target", FOF_BACKEND_UPLINK_TARGET);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "app_project", FOF_BACKEND_UPLINK_PROJECT);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "hardware_type", FOF_BACKEND_HARDWARE);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "hardware_mac", "A4:CF:12:CB:77:A4");
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "node_name", "West hall sensor");
    const size_t capabilities = require_key(
        batch.json, tokens, token_count, 0U, "capabilities");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[capabilities].kind);
    TEST_ASSERT_EQUAL_UINT16(2U, tokens[capabilities].child_count);
    char capability[41];
    TEST_ASSERT_TRUE(backend_json_copy_string(
        batch.json,
        &tokens[capabilities + 1U],
        capability,
        sizeof(capability)));
    TEST_ASSERT_EQUAL_STRING("dual_scanner", capability);
    TEST_ASSERT_TRUE(backend_json_copy_string(
        batch.json,
        &tokens[capabilities + 2U],
        capability,
        sizeof(capability)));
    TEST_ASSERT_EQUAL_STRING("backend_upload", capability);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "wifi_ssid", "FoF Ops");
    assert_i64_key(batch.json, tokens, token_count, 0U,
                   "wifi_rssi", -61);
    assert_string_key(batch.json, tokens, token_count, 0U,
                      "led_state", "network_degraded");
    assert_i64_key(batch.json, tokens, token_count, 0U,
                   "timestamp", INT64_C(1785600000));

    const size_t scanners = require_key(
        batch.json, tokens, token_count, 0U, "scanners");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[scanners].kind);
    TEST_ASSERT_EQUAL_UINT16(1U, tokens[scanners].child_count);
    const size_t scanner = scanners + 1U;
    TEST_ASSERT_EQUAL(BACKEND_JSON_OBJECT, tokens[scanner].kind);
    TEST_ASSERT_EQUAL_UINT16(44U, tokens[scanner].child_count);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "uart", "ble");
    assert_i64_key(batch.json, tokens, token_count, scanner, "slot", 0);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "product_family", FOF_BACKEND_PRODUCT_FAMILY);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "firmware_line", FOF_BACKEND_FIRMWARE_LINE);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "component", "scanner");
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "firmware_target", FOF_BACKEND_SCANNER_TARGET);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "app_project", FOF_BACKEND_SCANNER_PROJECT);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "hardware_type", FOF_BACKEND_HARDWARE);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "firmware_version", "0.1.0-backend");
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "mac", "AA:BB:CC:DD:EE:01");
    assert_i64_key(batch.json, tokens, token_count, scanner,
                   "boot_id", UINT32_C(0x1234ABCD));
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "profile", "ble_primary");
    assert_i64_key(batch.json, tokens, token_count, scanner,
                   "status_sequence", 29);
    assert_i64_key(batch.json, tokens, token_count, scanner,
                   "role_generation", 6);
    assert_bool_key(batch.json, tokens, token_count, scanner,
                    "role_acked", true);
    assert_bool_key(batch.json, tokens, token_count, scanner,
                    "command_ingress", true);
    assert_bool_key(batch.json, tokens, token_count, scanner,
                    "radio_healthy", true);
    assert_bool_key(batch.json, tokens, token_count, scanner,
                    "ble_healthy", true);
    assert_bool_key(batch.json, tokens, token_count, scanner,
                    "wifi_healthy", false);
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "ota_state", "idle");
    assert_string_key(batch.json, tokens, token_count, scanner,
                      "rollback_state", "valid");
    const size_t scanner_capabilities = require_key(
        batch.json, tokens, token_count, scanner, "capabilities");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY,
                      tokens[scanner_capabilities].kind);
    TEST_ASSERT_EQUAL_UINT16(6U,
                             tokens[scanner_capabilities].child_count);

    const size_t queue = require_key(
        batch.json, tokens, token_count, 0U, "upload_queue");
    TEST_ASSERT_EQUAL_UINT16(8U, tokens[queue].child_count);
    assert_i64_key(batch.json, tokens, token_count, queue,
                   "depth_batches", 4);
    assert_i64_key(batch.json, tokens, token_count, queue,
                   "capacity_batches", BACKEND_UPLOAD_FIFO_CAPACITY);
    assert_i64_key(batch.json, tokens, token_count, queue,
                   "overflow_dropped_batches", 3);
    assert_i64_key(batch.json, tokens, token_count, queue,
                   "quarantined_batches", 1);

    const size_t upload = require_key(
        batch.json, tokens, token_count, 0U, "upload");
    TEST_ASSERT_EQUAL_UINT16(8U, tokens[upload].child_count);
    assert_i64_key(batch.json, tokens, token_count, upload, "ok", 12);
    assert_i64_key(batch.json, tokens, token_count, upload, "failed", 5);
    assert_i64_key(batch.json, tokens, token_count, upload,
                   "retry_count", 9);
    assert_i64_key(batch.json, tokens, token_count, upload,
                   "last_success_age_s", 7);

    const size_t health = require_key(
        batch.json, tokens, token_count, 0U, "health");
    TEST_ASSERT_EQUAL_UINT16(14U, tokens[health].child_count);
    assert_bool_key(batch.json, tokens, token_count, health,
                    "clock_valid", true);
    assert_i64_key(batch.json, tokens, token_count, health,
                   "epoch_ms", INT64_C(1785600000123));
    assert_bool_key(batch.json, tokens, token_count, health,
                    "ap_active", true);
    assert_i64_key(batch.json, tokens, token_count, health,
                   "config_generation", 17);
    assert_i64_key(batch.json, tokens, token_count, health,
                   "command_success_count", 8);
    assert_i64_key(batch.json, tokens, token_count, health,
                   "command_failure_count", 2);
    assert_i64_key(batch.json, tokens, token_count, health,
                   "uptime_ms", INT64_C(7654321));

    const size_t detections = require_key(
        batch.json, tokens, token_count, 0U, "detections");
    TEST_ASSERT_EQUAL(BACKEND_JSON_ARRAY, tokens[detections].kind);
    TEST_ASSERT_EQUAL_UINT16(0U, tokens[detections].child_count);
}

static backend_uploader_outcome_t submit_heartbeat_ack(
    const backend_upload_batch_t *batch,
    const char *ack_json,
    size_t ack_length,
    uint32_t request_sequence,
    uint32_t request_crc32)
{
    backend_upload_batch_t storage[1];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 1U);
    bool dropped = true;
    TEST_ASSERT_TRUE(backend_upload_fifo_push(
        &fifo, batch, &dropped));
    TEST_ASSERT_FALSE(dropped);

    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    TEST_ASSERT_TRUE(backend_uploader_note_enqueued(
        &state, fifo.count, false, 0U, 0U));
    TEST_ASSERT_TRUE(backend_uploader_begin_head(
        &state, backend_upload_fifo_peek(&fifo), fifo.count, 1000));

    const bool exact_ack = backend_ingest_ack_validate(
        ack_json,
        ack_length,
        "uplink_CB77A4",
        batch->item_count);
    const backend_http_disposition_t disposition = backend_http_classify(
        true, 200, exact_ack);
    backend_uploader_queue_result_t queue_result =
        BACKEND_UPLOADER_QUEUE_UNCHANGED;
    const backend_upload_batch_t *head = backend_upload_fifo_peek(&fifo);
    if (disposition == BACKEND_HTTP_ACK && head &&
        head->sequence == request_sequence &&
        head->json_crc32 == request_crc32 &&
        backend_upload_fifo_pop_acked(&fifo, request_sequence)) {
        queue_result = BACKEND_UPLOADER_QUEUE_POPPED;
    }
    return backend_uploader_note_response(
        &state,
        request_sequence,
        request_crc32,
        disposition,
        200,
        queue_result,
        fifo.count,
        0U,
        1100);
}

void test_only_exact_matching_heartbeat_ack_is_ap_success_eligible(void)
{
    const backend_upload_batch_t batch = build_empty_heartbeat();
    static const char exact[] =
        "{\"status\":\"ok\",\"accepted\":0,\"processed\":0,"
        "\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    static const char wrong_count[] =
        "{\"status\":\"ok\",\"accepted\":1,\"processed\":1,"
        "\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    static const char unknown[] =
        "{\"status\":\"ok\",\"accepted\":0,\"processed\":0,"
        "\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\",\"extra\":0}";
    static const char wrong_device[] =
        "{\"status\":\"ok\",\"accepted\":0,\"processed\":0,"
        "\"deduplicated\":0,\"filtered\":0,"
        "\"device_id\":\"uplink_OTHER\"}";

    backend_uploader_outcome_t outcome = submit_heartbeat_ack(
        &batch,
        exact,
        sizeof(exact) - 1U,
        batch.sequence,
        batch.json_crc32);
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_ACKED, outcome);
    TEST_ASSERT_TRUE(outcome == BACKEND_UPLOADER_ACKED);

    outcome = submit_heartbeat_ack(
        &batch,
        wrong_count,
        sizeof(wrong_count) - 1U,
        batch.sequence,
        batch.json_crc32);
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_RETRY, outcome);
    TEST_ASSERT_FALSE(outcome == BACKEND_UPLOADER_ACKED);

    outcome = submit_heartbeat_ack(
        &batch,
        unknown,
        sizeof(unknown) - 1U,
        batch.sequence,
        batch.json_crc32);
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_RETRY, outcome);
    TEST_ASSERT_FALSE(outcome == BACKEND_UPLOADER_ACKED);

    outcome = submit_heartbeat_ack(
        &batch,
        wrong_device,
        sizeof(wrong_device) - 1U,
        batch.sequence,
        batch.json_crc32);
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_RETRY, outcome);
    TEST_ASSERT_FALSE(outcome == BACKEND_UPLOADER_ACKED);

    outcome = submit_heartbeat_ack(
        &batch,
        exact,
        sizeof(exact) - 1U,
        batch.sequence,
        batch.json_crc32 ^ UINT32_C(1));
    TEST_ASSERT_EQUAL(BACKEND_UPLOADER_IGNORED, outcome);
    TEST_ASSERT_FALSE(outcome == BACKEND_UPLOADER_ACKED);
}

void test_empty_heartbeat_is_due_only_at_60000_ms(void)
{
    backend_heartbeat_state_t heartbeat = {0};
    backend_heartbeat_init(&heartbeat, 1000);
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 60999));
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 61000));
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 61001));

    backend_heartbeat_mark_queued(&heartbeat, 61000);
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 120999));
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 121000));
}

void test_heartbeat_cadence_uses_actual_queue_time_and_never_moves_backward(void)
{
    backend_heartbeat_state_t heartbeat = {0};
    backend_heartbeat_init(&heartbeat, 5000);

    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 4999));
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 5000));
    backend_heartbeat_mark_queued(&heartbeat, 4000);
    TEST_ASSERT_EQUAL_INT64(5000, heartbeat.last_queued_ms);
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 65000));

    backend_heartbeat_mark_queued(&heartbeat, 65007);
    TEST_ASSERT_EQUAL_INT64(65007, heartbeat.last_queued_ms);
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 125006));
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 125007));
}

void test_uninitialized_or_invalid_heartbeat_state_is_never_due(void)
{
    backend_heartbeat_state_t heartbeat = {0};
    TEST_ASSERT_FALSE(backend_heartbeat_due(NULL, 60000));
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 60000));
    backend_heartbeat_mark_queued(&heartbeat, 60000);
    TEST_ASSERT_FALSE(heartbeat.initialized);

    backend_heartbeat_init(NULL, 0);
    backend_heartbeat_init(&heartbeat, -1);
    TEST_ASSERT_FALSE(heartbeat.initialized);
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, INT64_MAX));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_empty_heartbeat_contains_exact_operational_fields);
    BACKEND_RUN_TEST(
        test_only_exact_matching_heartbeat_ack_is_ap_success_eligible);
    BACKEND_RUN_TEST(test_empty_heartbeat_is_due_only_at_60000_ms);
    BACKEND_RUN_TEST(
        test_heartbeat_cadence_uses_actual_queue_time_and_never_moves_backward);
    BACKEND_RUN_TEST(
        test_uninitialized_or_invalid_heartbeat_state_is_never_due);
    return UNITY_END();
}
