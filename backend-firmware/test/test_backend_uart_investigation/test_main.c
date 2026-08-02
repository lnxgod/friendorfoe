#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_scanner_control_codec.h"
#include "backend_uart_rx.h"
#include "backend_uart_tx.h"
#include "../support/backend_test_main.h"

static const char COMMAND_ID[] = "0123456789abcdef0123456789abcdef";

typedef struct {
    backend_scanner_control_t controls[16];
    size_t control_count;
    ble_investigation_request_t investigation;
    size_t investigation_count;
    int64_t investigation_now_ms;
    char cancel_id[33];
    size_t cancel_count;
    int64_t cancel_now_ms;
    bool flow_paused;
    bool cancel_seen_while_paused;
    bool arm_binary_on_ota_begin;
    bool binary_mode;
    bool accept_dispatch;
    uint8_t tx_bytes[BACKEND_UART_TX_LINE_CAPACITY];
    size_t tx_length;
    size_t tx_calls;
} capture_t;

void setUp(void) {}
void tearDown(void) {}

static bool capture_control(
    void *context,
    const backend_scanner_control_t *control,
    int64_t now_ms)
{
    capture_t *capture = context;
    (void)now_ms;
    if (capture->control_count <
        sizeof(capture->controls) / sizeof(capture->controls[0])) {
        capture->controls[capture->control_count] = *control;
    }
    ++capture->control_count;
    if (control->type == BACKEND_SCANNER_CONTROL_FLOW) {
        capture->flow_paused = control->payload.flow.paused;
    }
    if (control->type == BACKEND_SCANNER_CONTROL_OTA_BEGIN &&
        capture->arm_binary_on_ota_begin && capture->accept_dispatch) {
        capture->binary_mode = true;
    }
    return capture->accept_dispatch;
}

static bool capture_investigation(
    void *context,
    const ble_investigation_request_t *request,
    int64_t now_ms)
{
    capture_t *capture = context;
    capture->investigation = *request;
    capture->investigation_now_ms = now_ms;
    ++capture->investigation_count;
    return capture->accept_dispatch;
}

static bool capture_cancel(void *context,
                           const char *command_id,
                           int64_t now_ms)
{
    capture_t *capture = context;
    snprintf(capture->cancel_id, sizeof(capture->cancel_id), "%s", command_id);
    capture->cancel_now_ms = now_ms;
    capture->cancel_seen_while_paused = capture->flow_paused;
    ++capture->cancel_count;
    return capture->accept_dispatch;
}

static bool capture_binary_active(void *context)
{
    const capture_t *capture = context;
    return capture->binary_mode;
}

static bool capture_write(void *context,
                          const uint8_t *bytes,
                          size_t length)
{
    capture_t *capture = context;
    ++capture->tx_calls;
    if (length > sizeof(capture->tx_bytes)) {
        return false;
    }
    memcpy(capture->tx_bytes, bytes, length);
    capture->tx_length = length;
    return capture->accept_dispatch;
}

static backend_uart_rx_callbacks_t callbacks(void)
{
    backend_uart_rx_callbacks_t value = {
        .control = capture_control,
        .investigate = capture_investigation,
        .cancel = capture_cancel,
        .binary_active = capture_binary_active,
    };
    return value;
}

static void initialize_rx(backend_uart_rx_t *rx, capture_t *capture)
{
    backend_uart_rx_callbacks_t handlers = callbacks();
    TEST_ASSERT_TRUE(backend_uart_rx_init(rx, &handlers, capture));
}

static backend_uart_rx_result_t consume_text(
    backend_uart_rx_t *rx,
    const char *text,
    int64_t now_ms)
{
    return backend_uart_rx_consume(
        rx, (const uint8_t *)text, strlen(text), now_ms);
}

static ble_investigation_chunk_t chunk(
    ble_investigation_chunk_kind_t kind)
{
    ble_investigation_chunk_t value = {.kind = kind};
    strcpy(value.request_id, COMMAND_ID);
    return value;
}

void test_rx_gatt_command_split_across_reads_maps_mac_to_investigator_request(void)
{
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    static const char line[] =
        "{\"type\":\"investigate\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"gatt\","
        "\"timeout_ms\":12000}\n";
    const size_t split = 19U;

    backend_uart_rx_result_t first = backend_uart_rx_consume(
        &rx, (const uint8_t *)line, split, 999);
    TEST_ASSERT_EQUAL_UINT(0, first.accepted_frames);
    TEST_ASSERT_TRUE(backend_uart_rx_has_partial(&rx));
    backend_uart_rx_result_t second = backend_uart_rx_consume(
        &rx, (const uint8_t *)line + split,
        sizeof(line) - 1U - split, 1000);

    TEST_ASSERT_EQUAL_UINT(1, second.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(0, second.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(1, capture.investigation_count);
    TEST_ASSERT_EQUAL_INT64(1000, capture.investigation_now_ms);
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, capture.investigation.request_id);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_GATT, capture.investigation.mode);
    TEST_ASSERT_EQUAL_STRING(
        "AA:BB:CC:DD:EE:FF", capture.investigation.target_mac);
    TEST_ASSERT_EQUAL_UINT32(12000, capture.investigation.timeout_ms);
    TEST_ASSERT_FALSE(backend_uart_rx_has_partial(&rx));

    ble_investigation_chunk_t begin = chunk(BLE_INV_CHUNK_BEGIN);
    begin.mode = capture.investigation.mode;
    strcpy(begin.request_id, capture.investigation.request_id);
    strcpy(begin.target_mac, capture.investigation.target_mac);
    char round_trip[BACKEND_UART_TX_LINE_CAPACITY];
    TEST_ASSERT_GREATER_THAN_UINT(
        0, backend_uart_tx_encode_investigation(
               &begin, round_trip, sizeof(round_trip)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ble_inv_begin\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mode\":\"gatt\",\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}\n",
        round_trip);
}

void test_passive_command_maps_null_mac_and_cancel_wire_has_only_two_fields(void)
{
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    backend_uart_rx_result_t passive = consume_text(
        &rx,
        "{\"type\":\"investigate\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mac\":null,\"mode\":\"passive_capture\","
        "\"timeout_ms\":12000}\n",
        2000);
    TEST_ASSERT_EQUAL_UINT(1, passive.accepted_frames);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_PASSIVE_CAPTURE,
                      capture.investigation.mode);
    TEST_ASSERT_EQUAL_STRING("", capture.investigation.target_mac);
    ble_investigation_chunk_t begin = chunk(BLE_INV_CHUNK_BEGIN);
    begin.mode = capture.investigation.mode;
    strcpy(begin.request_id, capture.investigation.request_id);
    char passive_round_trip[BACKEND_UART_TX_LINE_CAPACITY];
    TEST_ASSERT_GREATER_THAN_UINT(
        0, backend_uart_tx_encode_investigation(
               &begin, passive_round_trip, sizeof(passive_round_trip)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ble_inv_begin\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mode\":\"passive_capture\",\"target_mac\":null}\n",
        passive_round_trip);

    backend_scanner_control_t cancel = {
        .type = BACKEND_SCANNER_CONTROL_CANCEL,
    };
    strcpy(cancel.payload.cancel.command_id, COMMAND_ID);
    char encoded[160];
    size_t length = backend_scanner_control_encode(
        &cancel, encoded, sizeof(encoded));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"cancel\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\"}",
        encoded);
    TEST_ASSERT_NULL(strstr(encoded, "target"));
    TEST_ASSERT_NULL(strstr(encoded, "mac"));
    TEST_ASSERT_NULL(strstr(encoded, "mode"));
    TEST_ASSERT_NULL(strstr(encoded, "timeout"));
    encoded[length++] = '\n';
    encoded[length] = '\0';
    backend_uart_rx_result_t result = consume_text(&rx, encoded, 2100);
    TEST_ASSERT_EQUAL_UINT(1, result.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, capture.cancel_count);
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, capture.cancel_id);
    TEST_ASSERT_EQUAL_INT64(2100, capture.cancel_now_ms);
}

void test_rx_accepts_only_the_complete_control_allowlist(void)
{
    static const char *const ALLOWED[] = {
        "{\"type\":\"role\",\"boot_id\":77,\"generation\":4,"
        "\"profile\":\"ble_primary\"}\n",
        "{\"type\":\"time\",\"generation\":5,\"valid\":true,"
        "\"epoch_ms\":1785600000123,\"source\":\"sntp\"}\n",
        "{\"type\":\"flow\",\"generation\":6,\"paused\":false}\n",
        "{\"type\":\"led_state\",\"state\":\"drone\","
        "\"generation\":7,\"ttl_ms\":2000}\n",
        "{\"type\":\"health_request\",\"sequence\":8}\n",
        "{\"type\":\"recovery\",\"boot_id\":77,\"generation\":9,"
        "\"action\":\"restart_radios\"}\n",
        "{\"type\":\"ota_begin\",\"session_id\":7,\"generation\":12,"
        "\"component_slot\":0,\"expected_mac\":\"AA:BB:CC:DD:EE:01\","
        "\"expected_boot_id\":305419896,"
        "\"expected_topology_generation\":4,"
        "\"target\":\"scanner-s3-combo-backend\","
        "\"project\":\"fof_backend_scanner\","
        "\"hardware\":\"seeed_xiao_esp32s3\","
        "\"version\":\"0.1.1-backend\",\"image_size\":1048576,"
        "\"crc32\":305419896,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef\","
        "\"allow_same_version\":false,\"dry_run\":false}\n",
        "{\"type\":\"ota_end\",\"session_id\":7,\"generation\":13,"
        "\"reason\":\"complete\"}\n",
        "{\"type\":\"ota_abort\",\"session_id\":8,\"generation\":14,"
        "\"reason\":\"operator_cancel\"}\n",
    };
    static const backend_scanner_control_kind_t EXPECTED[] = {
        BACKEND_SCANNER_CONTROL_ROLE,
        BACKEND_SCANNER_CONTROL_TIME,
        BACKEND_SCANNER_CONTROL_FLOW,
        BACKEND_SCANNER_CONTROL_LED_STATE,
        BACKEND_SCANNER_CONTROL_HEALTH_REQUEST,
        BACKEND_SCANNER_CONTROL_RECOVERY,
        BACKEND_SCANNER_CONTROL_OTA_BEGIN,
        BACKEND_SCANNER_CONTROL_OTA_END,
        BACKEND_SCANNER_CONTROL_OTA_ABORT,
    };
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);

    for (size_t index = 0U; index < sizeof(ALLOWED) / sizeof(ALLOWED[0]);
         ++index) {
        backend_uart_rx_result_t result =
            consume_text(&rx, ALLOWED[index], (int64_t)(3000U + index));
        TEST_ASSERT_EQUAL_UINT(1, result.accepted_frames);
        TEST_ASSERT_EQUAL_UINT(0, result.rejected_frames);
        TEST_ASSERT_EQUAL(EXPECTED[index], capture.controls[index].type);
    }
    TEST_ASSERT_EQUAL_UINT(
        sizeof(ALLOWED) / sizeof(ALLOWED[0]), capture.control_count);
}

void test_rx_rejects_wrong_extra_duplicate_malformed_and_semantic_lines(void)
{
    static const char *const REJECTED[] = {
        "{\"type\":\"detection\",\"drone_id\":\"x\"}\n",
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"extra\":1}\n",
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n",
        "{\"type\":\"cancel\",\"command_id\":\"short\"}\n",
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789ABCDEF0123456789ABCDEF\"}\n",
        "{\"type\":\"investigate\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"mac\":null,"
        "\"mode\":\"gatt\",\"timeout_ms\":12000}\n",
        "{\"type\":\"investigate\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"mode\":\"passive_capture\",\"timeout_ms\":12000}\n",
        "{\"type\":\"investigate\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\","
        "\"mac\":\"aa:bb:cc:dd:ee:ff\",\"mode\":\"gatt\","
        "\"timeout_ms\":12000}\n",
        "{\"type\":\"investigate\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"gatt\","
        "\"timeout_ms\":12001}\n",
        "{\"type\":\"cancel\",\"command_id\":}\n",
    };
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);

    for (size_t index = 0U;
         index < sizeof(REJECTED) / sizeof(REJECTED[0]); ++index) {
        backend_uart_rx_result_t result =
            consume_text(&rx, REJECTED[index], (int64_t)(4000U + index));
        TEST_ASSERT_EQUAL_UINT(0, result.accepted_frames);
        TEST_ASSERT_EQUAL_UINT(1, result.rejected_frames);
    }
    TEST_ASSERT_EQUAL_UINT(0, capture.control_count);
    TEST_ASSERT_EQUAL_UINT(0, capture.investigation_count);
    TEST_ASSERT_EQUAL_UINT(0, capture.cancel_count);
}

void test_rx_rejects_oversized_and_embedded_nul_lines_then_recovers(void)
{
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    uint8_t oversized[SCANNER_UART_LINE_MAX_PAYLOAD + 2U];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1U] = '\n';
    backend_uart_rx_result_t too_large = backend_uart_rx_consume(
        &rx, oversized, sizeof(oversized), 5000);
    TEST_ASSERT_EQUAL_UINT(0, too_large.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, too_large.rejected_frames);
    TEST_ASSERT_EQUAL(SCANNER_UART_LINE_REJECT_TOO_LONG,
                      too_large.last_line_reject);

    static const uint8_t embedded_nul[] = {
        '{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'c', 'a', 'n',
        'c', 'e', 'l', '"', ',', '"', 'c', 'o', 'm', 'm', 'a', 'n',
        'd', '_', 'i', 'd', '"', ':', '"', '0', '\0', '1', '"', '}',
        '\n',
    };
    backend_uart_rx_result_t nul = backend_uart_rx_consume(
        &rx, embedded_nul, sizeof(embedded_nul), 5100);
    TEST_ASSERT_EQUAL_UINT(0, nul.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, nul.rejected_frames);

    backend_uart_rx_result_t recovered = consume_text(
        &rx,
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n",
        5200);
    TEST_ASSERT_EQUAL_UINT(1, recovered.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, capture.cancel_count);

}

void test_rx_rejects_negative_time_and_expires_partial_without_suffix_revival(void)
{
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    static const char line[] =
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n";

    backend_uart_rx_result_t invalid = consume_text(&rx, line, -1);
    TEST_ASSERT_TRUE(invalid.invalid_argument);
    TEST_ASSERT_EQUAL_UINT(0, capture.cancel_count);

    const size_t split = 30U;
    backend_uart_rx_result_t partial = backend_uart_rx_consume(
        &rx, (const uint8_t *)line, split, 5300);
    TEST_ASSERT_EQUAL_UINT(0, partial.accepted_frames);
    TEST_ASSERT_TRUE(backend_uart_rx_has_partial(&rx));
    backend_uart_rx_result_t expired = backend_uart_rx_expire_partial(&rx);
    TEST_ASSERT_EQUAL_UINT(1, expired.rejected_frames);
    TEST_ASSERT_EQUAL(SCANNER_UART_LINE_REJECT_STALE_PARTIAL,
                      expired.last_line_reject);

    backend_uart_rx_result_t suffix = backend_uart_rx_consume(
        &rx, (const uint8_t *)line + split,
        sizeof(line) - 1U - split, 5400);
    TEST_ASSERT_EQUAL_UINT(0, suffix.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(0, capture.cancel_count);
    backend_uart_rx_result_t recovered = consume_text(&rx, line, 5500);
    TEST_ASSERT_EQUAL_UINT(1, recovered.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, capture.cancel_count);

}

void test_flow_pause_never_blocks_investigate_cancel_or_other_command_ingress(void)
{
    capture_t capture = {.accept_dispatch = true};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    static const char lines[] =
        "{\"type\":\"flow\",\"generation\":6,\"paused\":true}\n"
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n"
        "{\"type\":\"investigate\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"mac\":null,"
        "\"mode\":\"passive_capture\",\"timeout_ms\":12000}\n"
        "{\"type\":\"health_request\",\"sequence\":8}\n";
    backend_uart_rx_result_t result = consume_text(&rx, lines, 6000);

    TEST_ASSERT_EQUAL_UINT(4, result.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(0, result.rejected_frames);
    TEST_ASSERT_TRUE(capture.flow_paused);
    TEST_ASSERT_TRUE(capture.cancel_seen_while_paused);
    TEST_ASSERT_EQUAL_UINT(1, capture.cancel_count);
    TEST_ASSERT_EQUAL_UINT(1, capture.investigation_count);
    TEST_ASSERT_EQUAL_UINT(2, capture.control_count);
}

void test_ota_begin_hands_same_read_binary_to_caller_at_json_boundary(void)
{
    backend_scanner_control_t ota_begin = {
        .type = BACKEND_SCANNER_CONTROL_OTA_BEGIN,
        .payload.ota_begin = {
            .session_id = 7,
            .generation = 12,
            .component_slot = 0,
            .expected_boot_id = 305419896,
            .expected_topology_generation = 4,
            .image_size = 1048576,
            .crc32 = 305419896,
            .allow_same_version = false,
            .dry_run = false,
        },
    };
    strcpy(ota_begin.payload.ota_begin.expected_mac, "AA:BB:CC:DD:EE:01");
    strcpy(ota_begin.payload.ota_begin.target, "scanner-s3-combo-backend");
    strcpy(ota_begin.payload.ota_begin.project, "fof_backend_scanner");
    strcpy(ota_begin.payload.ota_begin.hardware, "seeed_xiao_esp32s3");
    strcpy(ota_begin.payload.ota_begin.version, "0.1.1-backend");
    strcpy(ota_begin.payload.ota_begin.sha256,
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef");

    uint8_t input[1400];
    const size_t json_length = backend_scanner_control_encode(
        &ota_begin, (char *)input, sizeof(input));
    TEST_ASSERT_GREATER_THAN_UINT(0, json_length);
    input[json_length] = '\n';
    static const uint8_t BINARY[] = {0xf0, 0x00, 0xff, '\n', 0x7e};
    memcpy(input + json_length + 1U, BINARY, sizeof(BINARY));

    capture_t capture = {
        .arm_binary_on_ota_begin = true,
        .accept_dispatch = true,
    };
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    backend_uart_rx_result_t result = backend_uart_rx_consume(
        &rx, input, json_length + 1U + sizeof(BINARY), 6500);

    TEST_ASSERT_EQUAL_UINT(1, result.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(0, result.rejected_frames);
    TEST_ASSERT_TRUE(result.binary_handoff);
    TEST_ASSERT_EQUAL_UINT(json_length + 1U, result.consumed_bytes);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        BINARY, input + result.consumed_bytes, sizeof(BINARY));
    TEST_ASSERT_EQUAL_UINT(1, capture.control_count);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_OTA_BEGIN,
                      capture.controls[0].type);
    TEST_ASSERT_FALSE(backend_uart_rx_has_partial(&rx));

    backend_uart_rx_result_t already_binary = backend_uart_rx_consume(
        &rx, BINARY, sizeof(BINARY), 6501);
    TEST_ASSERT_TRUE(already_binary.binary_handoff);
    TEST_ASSERT_EQUAL_UINT(0, already_binary.consumed_bytes);

    capture.binary_mode = false;
    backend_uart_rx_result_t recovered = consume_text(
        &rx,
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n",
        6502);
    TEST_ASSERT_EQUAL_UINT(1, recovered.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, capture.cancel_count);

    static const char COMMAND_SHAPED_REMAINDER[] =
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n";
    uint8_t failed_input[1400];
    memcpy(failed_input, input, json_length + 1U);
    memcpy(failed_input + json_length + 1U, COMMAND_SHAPED_REMAINDER,
           sizeof(COMMAND_SHAPED_REMAINDER) - 1U);
    const size_t failed_length = json_length + 1U +
                                 sizeof(COMMAND_SHAPED_REMAINDER) - 1U;
    capture_t failed_capture = {
        .arm_binary_on_ota_begin = true,
        .accept_dispatch = false,
    };
    backend_uart_rx_t failed_rx;
    initialize_rx(&failed_rx, &failed_capture);
    backend_uart_rx_result_t failed = backend_uart_rx_consume(
        &failed_rx, failed_input, failed_length, 6503);
    TEST_ASSERT_EQUAL_UINT(1, failed.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(1, failed.dispatch_failures);
    TEST_ASSERT_EQUAL_UINT(0, failed.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(0, failed_capture.cancel_count);
    TEST_ASSERT_FALSE(failed.binary_handoff);
    TEST_ASSERT_EQUAL_UINT(sizeof(COMMAND_SHAPED_REMAINDER) - 1U,
                           failed.discarded_bytes);
    TEST_ASSERT_EQUAL_UINT(failed_length, failed.consumed_bytes);
    TEST_ASSERT_FALSE(backend_uart_rx_has_partial(&failed_rx));
}

void test_dispatch_failure_is_reported_without_reclassifying_valid_wire(void)
{
    capture_t capture = {.accept_dispatch = false};
    backend_uart_rx_t rx;
    initialize_rx(&rx, &capture);
    backend_uart_rx_result_t result = consume_text(
        &rx,
        "{\"type\":\"cancel\",\"command_id\":"
        "\"0123456789abcdef0123456789abcdef\"}\n",
        7000);
    TEST_ASSERT_EQUAL_UINT(1, result.accepted_frames);
    TEST_ASSERT_EQUAL_UINT(0, result.rejected_frames);
    TEST_ASSERT_EQUAL_UINT(1, result.dispatch_failures);
    TEST_ASSERT_EQUAL_UINT(1, capture.cancel_count);
}

static void assert_encoded(
    const ble_investigation_chunk_t *value,
    const char *expected)
{
    char line[BACKEND_UART_TX_LINE_CAPACITY];
    size_t length = backend_uart_tx_encode_investigation(
        value, line, sizeof(line));
    TEST_ASSERT_EQUAL_UINT(strlen(expected), length);
    TEST_ASSERT_EQUAL_STRING(expected, line);
    TEST_ASSERT_EQUAL_CHAR('\n', line[length - 1U]);
}

void test_tx_encodes_every_bounded_chunk_with_exact_id_and_fields(void)
{
    ble_investigation_chunk_t begin = chunk(BLE_INV_CHUNK_BEGIN);
    begin.mode = BLE_INV_MODE_GATT;
    strcpy(begin.target_mac, "AA:BB:CC:DD:EE:FF");
    assert_encoded(
        &begin,
        "{\"type\":\"ble_inv_begin\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mode\":\"gatt\",\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}\n");

    ble_investigation_chunk_t progress = chunk(BLE_INV_CHUNK_PROGRESS);
    progress.state = BLE_INV_DISCOVERING;
    assert_encoded(
        &progress,
        "{\"type\":\"ble_inv_progress\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"state\":\"discovering\"}\n");

    ble_investigation_chunk_t service = chunk(BLE_INV_CHUNK_SERVICE);
    service.index = 0;
    strcpy(service.uuid, "180F");
    assert_encoded(
        &service,
        "{\"type\":\"ble_inv_service\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"index\":0,\"uuid\":\"180F\"}\n");

    ble_investigation_chunk_t characteristic =
        chunk(BLE_INV_CHUNK_CHARACTERISTIC);
    characteristic.index = 0;
    strcpy(characteristic.service_uuid, "180F");
    strcpy(characteristic.uuid, "2A19");
    characteristic.properties = BLE_INV_PROP_READ | BLE_INV_PROP_NOTIFY;
    assert_encoded(
        &characteristic,
        "{\"type\":\"ble_inv_char\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"index\":0,\"service_uuid\":\"180F\",\"uuid\":\"2A19\","
        "\"properties\":[\"read\",\"notify\"]}\n");

    ble_investigation_chunk_t read = chunk(BLE_INV_CHUNK_READ);
    read.index = 0;
    strcpy(read.uuid, "2A19");
    strcpy(read.value_hex, "A0ff");
    assert_encoded(
        &read,
        "{\"type\":\"ble_inv_read\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"index\":0,\"uuid\":\"2A19\",\"value_hex\":\"A0ff\"}\n");

    ble_investigation_chunk_t end = chunk(BLE_INV_CHUNK_END);
    end.state = BLE_INV_FAILED;
    strcpy(end.summary, "auth needed");
    strcpy(end.error, "authentication_required");
    end.authentication_required = true;
    end.truncated = true;
    assert_encoded(
        &end,
        "{\"type\":\"ble_inv_end\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"state\":\"failed\",\"summary\":\"auth needed\","
        "\"error\":\"authentication_required\","
        "\"authentication_required\":true,\"truncated\":true}\n");
}

void test_tx_writer_callback_receives_one_complete_newline_delimited_chunk(void)
{
    capture_t capture = {.accept_dispatch = true};
    backend_uart_tx_t tx;
    TEST_ASSERT_TRUE(backend_uart_tx_init(&tx, capture_write, &capture));
    ble_investigation_chunk_t end = chunk(BLE_INV_CHUNK_END);
    end.state = BLE_INV_CANCELLED;
    strcpy(end.summary, "cancelled");

    TEST_ASSERT_TRUE(backend_uart_tx_send_investigation(&tx, &end));
    TEST_ASSERT_EQUAL_UINT(1, capture.tx_calls);
    TEST_ASSERT_EQUAL_UINT32(1, tx.sent_chunks);
    TEST_ASSERT_EQUAL_UINT32(0, tx.dropped_chunks);
    TEST_ASSERT_EQUAL_CHAR('\n', capture.tx_bytes[capture.tx_length - 1U]);
    TEST_ASSERT_EQUAL_UINT(capture.tx_length, tx.last_length);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ble_inv_end\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"state\":\"cancelled\",\"summary\":\"cancelled\","
        "\"error\":null,\"authentication_required\":false,"
        "\"truncated\":false}\n",
        (const char *)capture.tx_bytes);
}

void test_tx_rejects_wrong_ids_limits_malformed_values_and_oversized_encoding(void)
{
    char line[BACKEND_UART_TX_LINE_CAPACITY];
    ble_investigation_chunk_t value = chunk(BLE_INV_CHUNK_SERVICE);
    strcpy(value.request_id, "0123456789ABCDEF0123456789ABCDEF");
    value.index = 0;
    strcpy(value.uuid, "180F");
    TEST_ASSERT_EQUAL_UINT(0, backend_uart_tx_encode_investigation(
        &value, line, sizeof(line)));

    value = chunk(BLE_INV_CHUNK_SERVICE);
    value.index = BLE_INV_MAX_SERVICES;
    strcpy(value.uuid, "180F");
    TEST_ASSERT_EQUAL_UINT(0, backend_uart_tx_encode_investigation(
        &value, line, sizeof(line)));

    value = chunk(BLE_INV_CHUNK_READ);
    value.index = 0;
    strcpy(value.uuid, "2A19");
    strcpy(value.value_hex, "abc");
    TEST_ASSERT_EQUAL_UINT(0, backend_uart_tx_encode_investigation(
        &value, line, sizeof(line)));

    value = chunk(BLE_INV_CHUNK_END);
    value.state = BLE_INV_COMPLETE;
    memset(value.summary, 'x', sizeof(value.summary));
    TEST_ASSERT_EQUAL_UINT(0, backend_uart_tx_encode_investigation(
        &value, line, sizeof(line)));

    value = chunk(BLE_INV_CHUNK_PROGRESS);
    value.state = BLE_INV_SCANNING;
    TEST_ASSERT_EQUAL_UINT(0, backend_uart_tx_encode_investigation(
        &value, line, 8));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_rx_gatt_command_split_across_reads_maps_mac_to_investigator_request);
    BACKEND_RUN_TEST(
        test_passive_command_maps_null_mac_and_cancel_wire_has_only_two_fields);
    BACKEND_RUN_TEST(test_rx_accepts_only_the_complete_control_allowlist);
    BACKEND_RUN_TEST(
        test_rx_rejects_wrong_extra_duplicate_malformed_and_semantic_lines);
    BACKEND_RUN_TEST(
        test_rx_rejects_oversized_and_embedded_nul_lines_then_recovers);
    BACKEND_RUN_TEST(
        test_rx_rejects_negative_time_and_expires_partial_without_suffix_revival);
    BACKEND_RUN_TEST(
        test_flow_pause_never_blocks_investigate_cancel_or_other_command_ingress);
    BACKEND_RUN_TEST(
        test_ota_begin_hands_same_read_binary_to_caller_at_json_boundary);
    BACKEND_RUN_TEST(
        test_dispatch_failure_is_reported_without_reclassifying_valid_wire);
    BACKEND_RUN_TEST(
        test_tx_encodes_every_bounded_chunk_with_exact_id_and_fields);
    BACKEND_RUN_TEST(
        test_tx_writer_callback_receives_one_complete_newline_delimited_chunk);
    BACKEND_RUN_TEST(
        test_tx_rejects_wrong_ids_limits_malformed_values_and_oversized_encoding);
    return UNITY_END();
}
