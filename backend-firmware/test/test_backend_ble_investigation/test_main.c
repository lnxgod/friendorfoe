#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_ble_investigation.h"
#include "backend_scanner_topology.h"
#include "../support/backend_test_main.h"

static const char COMMAND_ID[] = "0123456789abcdef0123456789abcdef";
static const char OTHER_COMMAND_ID[] = "fedcba9876543210fedcba9876543210";

void setUp(void) {}
void tearDown(void) {}

static ble_investigation_request_t gatt_request(const char *command_id)
{
    ble_investigation_request_t request = {
        .mode = BLE_INV_MODE_GATT,
        .timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS,
    };
    strcpy(request.request_id, command_id);
    strcpy(request.target_mac, "aa:bb:cc:dd:ee:ff");
    return request;
}

static ble_investigation_request_t passive_request(const char *command_id)
{
    ble_investigation_request_t request = {
        .mode = BLE_INV_MODE_PASSIVE_CAPTURE,
        .timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS,
    };
    strcpy(request.request_id, command_id);
    return request;
}

static ble_investigation_chunk_t chunk_for(
    ble_investigation_chunk_kind_t kind)
{
    ble_investigation_chunk_t chunk = {.kind = kind};
    strcpy(chunk.request_id, COMMAND_ID);
    return chunk;
}

static void accept_begin(backend_ble_investigation_state_t *state)
{
    ble_investigation_chunk_t chunk = chunk_for(BLE_INV_CHUNK_BEGIN);
    chunk.mode = BLE_INV_MODE_GATT;
    strcpy(chunk.target_mac, "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        state, BACKEND_SCANNER_SLOT_WIFI, &chunk));
}

static void accept_all_progress(backend_ble_investigation_state_t *state)
{
    for (int value = BLE_INV_QUEUED; value <= BLE_INV_READING; ++value) {
        ble_investigation_chunk_t chunk =
            chunk_for(BLE_INV_CHUNK_PROGRESS);
        chunk.state = (ble_investigation_state_t)value;
        TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
            state, BACKEND_SCANNER_SLOT_WIFI, &chunk));
    }
}

static void accept_all_services(backend_ble_investigation_state_t *state)
{
    for (int index = 0; index < BLE_INV_MAX_SERVICES; ++index) {
        ble_investigation_chunk_t chunk =
            chunk_for(BLE_INV_CHUNK_SERVICE);
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "18%02x", index);
        TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
            state, BACKEND_SCANNER_SLOT_WIFI, &chunk));
    }
}

static void accept_all_characteristics(
    backend_ble_investigation_state_t *state)
{
    for (int index = 0; index < BLE_INV_MAX_CHARS; ++index) {
        ble_investigation_chunk_t chunk =
            chunk_for(BLE_INV_CHUNK_CHARACTERISTIC);
        chunk.index = index;
        strcpy(chunk.service_uuid, "180f");
        snprintf(chunk.uuid, sizeof(chunk.uuid), "2a%02x", index);
        chunk.properties = BLE_INV_PROP_READ | BLE_INV_PROP_NOTIFY;
        TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
            state, BACKEND_SCANNER_SLOT_WIFI, &chunk));
    }
}

static void accept_all_reads(backend_ble_investigation_state_t *state)
{
    for (int index = 0; index < BLE_INV_MAX_READS; ++index) {
        ble_investigation_chunk_t chunk = chunk_for(BLE_INV_CHUNK_READ);
        chunk.index = index;
        snprintf(chunk.uuid, sizeof(chunk.uuid), "2a%02x", index);
        strcpy(chunk.value_hex, "001122aabbcc");
        TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
            state, BACKEND_SCANNER_SLOT_WIFI, &chunk));
    }
}

static backend_ble_investigation_state_t full_nonterminal_fixture(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);
    accept_all_progress(&state);
    accept_all_services(&state);
    accept_all_characteristics(&state);
    accept_all_reads(&state);
    return state;
}

void test_start_uses_current_ble_owner_and_duplicate_does_not_restart(void)
{
    backend_scanner_plan_t plan = {
        .desired = {
            BACKEND_SCAN_PROFILE_WIFI_PRIMARY,
            BACKEND_SCAN_PROFILE_HYBRID_FAILOVER,
        },
        .eligible_mask = UINT8_C(0x03),
        .converged_mask = UINT8_C(0x03),
    };
    int owner = backend_scanner_ble_owner(&plan);
    TEST_ASSERT_EQUAL_INT(BACKEND_SCANNER_SLOT_WIFI, owner);

    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, (backend_scanner_slot_t)owner, 1000));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_SLOT_WIFI, state.scanner_slot);
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_start_count);

    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1100));
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_start_count);

    ble_investigation_request_t conflicting = gatt_request(OTHER_COMMAND_ID);
    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, OTHER_COMMAND_ID, &conflicting,
        BACKEND_SCANNER_SLOT_BLE, 1200));
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, state.command_id);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_SLOT_WIFI, state.scanner_slot);
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_start_count);
}

void test_start_rejects_noncanonical_ids_requests_slots_and_timeouts(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);

    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, "0123456789ABCDEF0123456789ABCDEF", &request,
        BACKEND_SCANNER_SLOT_WIFI, 1000));
    strcpy(request.request_id, OTHER_COMMAND_ID);
    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    request = gatt_request(COMMAND_ID);
    request.timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS + 1U;
    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    request = gatt_request(COMMAND_ID);
    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, (backend_scanner_slot_t)99, 1000));
    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, -1));
    TEST_ASSERT_EQUAL_UINT32(0, state.radio_start_count);
}

void test_begin_is_translated_once_to_exact_api_envelope_and_replayed_stably(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);

    backend_command_result_t first = {0};
    backend_command_result_t retry = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &first));
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &retry));
    TEST_ASSERT_EQUAL_UINT32(0, first.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_begin", first.type);
    TEST_ASSERT_EQUAL_STRING(first.json, retry.json);
    TEST_ASSERT_EQUAL_UINT32(first.sequence, retry.sequence);
    TEST_ASSERT_EQUAL_STRING(
        "{\"sequence\":0,\"type\":\"ble_inv_begin\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mode\":\"gatt\",\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}",
        first.json);
    TEST_ASSERT_EQUAL_UINT(strlen(first.json), first.json_length);
}

void test_only_matching_result_ack_advances_the_stable_head(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);

    backend_command_result_t pending = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &pending));
    TEST_ASSERT_FALSE(backend_ble_investigation_mark_acked(
        &state, OTHER_COMMAND_ID, pending.sequence));
    TEST_ASSERT_FALSE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, pending.sequence + 1U));
    TEST_ASSERT_EQUAL_UINT8(1, state.queue_count);
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, pending.sequence));
    TEST_ASSERT_EQUAL_UINT8(0, state.queue_count);
    TEST_ASSERT_FALSE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, pending.sequence));
}

void test_each_scanner_chunk_gets_the_exact_backend_result_shape(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);

    ble_investigation_chunk_t progress =
        chunk_for(BLE_INV_CHUNK_PROGRESS);
    progress.state = BLE_INV_SCANNING;
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &progress));

    ble_investigation_chunk_t service = chunk_for(BLE_INV_CHUNK_SERVICE);
    service.index = 0;
    strcpy(service.uuid, "180F");
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &service));

    ble_investigation_chunk_t characteristic =
        chunk_for(BLE_INV_CHUNK_CHARACTERISTIC);
    characteristic.index = 0;
    strcpy(characteristic.service_uuid, "180F");
    strcpy(characteristic.uuid, "2A19");
    characteristic.properties = BLE_INV_PROP_READ | BLE_INV_PROP_NOTIFY;
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &characteristic));

    ble_investigation_chunk_t read = chunk_for(BLE_INV_CHUNK_READ);
    read.index = 0;
    strcpy(read.uuid, "2A19");
    strcpy(read.value_hex, "A0ff");
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &read));

    ble_investigation_chunk_t end = chunk_for(BLE_INV_CHUNK_END);
    end.state = BLE_INV_FAILED;
    strcpy(end.summary, "auth needed");
    strcpy(end.error, "denied");
    end.authentication_required = true;
    end.truncated = true;
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &end));

    static const char *const EXPECTED[] = {
        "{\"sequence\":0,\"type\":\"ble_inv_begin\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mode\":\"gatt\",\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}",
        "{\"sequence\":1,\"type\":\"ble_inv_progress\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"state\":\"scanning\"}",
        "{\"sequence\":2,\"type\":\"ble_inv_service\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"index\":0,\"uuid\":\"180F\"}",
        "{\"sequence\":3,\"type\":\"ble_inv_char\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"index\":0,\"service_uuid\":\"180F\",\"uuid\":\"2A19\","
        "\"properties\":[\"read\",\"notify\"]}",
        "{\"sequence\":4,\"type\":\"ble_inv_read\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"index\":0,\"uuid\":\"2A19\",\"value_hex\":\"A0ff\"}",
        "{\"sequence\":5,\"type\":\"ble_inv_end\","
        "\"request_id\":\"0123456789abcdef0123456789abcdef\","
        "\"state\":\"failed\",\"summary\":\"auth needed\","
        "\"error\":\"denied\",\"authentication_required\":true,"
        "\"truncated\":true}",
    };
    for (uint32_t sequence = 0U;
         sequence < sizeof(EXPECTED) / sizeof(EXPECTED[0]); ++sequence) {
        backend_command_result_t result = {0};
        TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &result));
        TEST_ASSERT_EQUAL_STRING(EXPECTED[sequence], result.json);
        TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
            &state, COMMAND_ID, sequence));
    }
}

void test_full_63_event_stream_holds_head_then_drains_in_sequence(void)
{
    backend_ble_investigation_state_t state = full_nonterminal_fixture();
    backend_command_result_t held = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &held));

    ble_investigation_chunk_t end = chunk_for(BLE_INV_CHUNK_END);
    end.state = BLE_INV_COMPLETE;
    strcpy(end.summary, "complete");
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &end));
    TEST_ASSERT_EQUAL_UINT8(63, state.queue_count);
    TEST_ASSERT_TRUE(state.terminal_queued);
    TEST_ASSERT_FALSE(state.radio_active);
    TEST_ASSERT_TRUE(state.active);

    backend_command_result_t replay = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &replay));
    TEST_ASSERT_EQUAL_STRING(held.json, replay.json);

    for (uint32_t sequence = 0; sequence < 63U; ++sequence) {
        backend_command_result_t result = {0};
        TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &result));
        TEST_ASSERT_EQUAL_UINT32(sequence, result.sequence);
        TEST_ASSERT_TRUE(result.json_length <= BACKEND_COMMAND_RESULT_MAX_JSON);
        if (sequence == 62U) {
            TEST_ASSERT_EQUAL_STRING("ble_inv_end", result.type);
            TEST_ASSERT_EQUAL_STRING("complete", result.state);
        }
        TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
            &state, COMMAND_ID, sequence));
    }
    TEST_ASSERT_FALSE(backend_ble_investigation_next_result(&state, &replay));
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_EQUAL_UINT8(0, state.queue_count);
}

void test_out_of_limit_chunk_uses_reserved_terminal_without_overwriting_evidence(void)
{
    backend_ble_investigation_state_t state = full_nonterminal_fixture();
    TEST_ASSERT_EQUAL_UINT8(62, state.queue_count);
    backend_command_result_t held = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &held));

    ble_investigation_chunk_t overflow = chunk_for(BLE_INV_CHUNK_SERVICE);
    overflow.index = BLE_INV_MAX_SERVICES;
    strcpy(overflow.uuid, "180f");
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &overflow));
    TEST_ASSERT_EQUAL_UINT8(63, state.queue_count);
    TEST_ASSERT_EQUAL_UINT8(BLE_INV_MAX_SERVICES, state.service_count);
    TEST_ASSERT_TRUE(state.terminal_queued);

    backend_command_result_t replay = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &replay));
    TEST_ASSERT_EQUAL_STRING(held.json, replay.json);
    TEST_ASSERT_EQUAL_UINT32(held.sequence, replay.sequence);

    for (uint32_t sequence = 0; sequence < 62U; ++sequence) {
        TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
            &state, COMMAND_ID, sequence));
    }
    backend_command_result_t terminal = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &terminal));
    TEST_ASSERT_EQUAL_UINT32(62, terminal.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_end", terminal.type);
    TEST_ASSERT_EQUAL_STRING("failed", terminal.state);
    TEST_ASSERT_NOT_NULL(strstr(terminal.json, "\"error\":\"result_overflow\""));
}

void test_repeated_or_regressing_progress_closes_with_overflow_failure(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);

    ble_investigation_chunk_t progress =
        chunk_for(BLE_INV_CHUNK_PROGRESS);
    progress.state = BLE_INV_DISCOVERING;
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &progress));
    progress.state = BLE_INV_SCANNING;
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &progress));
    TEST_ASSERT_TRUE(state.terminal_queued);
    TEST_ASSERT_FALSE(state.radio_active);

    backend_ble_investigation_init(&state);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);
    accept_all_progress(&state);
    progress.state = BLE_INV_READING;
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &progress));
    TEST_ASSERT_TRUE(state.terminal_queued);
}

void test_unterminated_or_oversized_chunk_closes_without_array_overwrite(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);

    ble_investigation_chunk_t read = chunk_for(BLE_INV_CHUNK_READ);
    read.index = 0;
    strcpy(read.uuid, "2a19");
    memset(read.value_hex, 'a', sizeof(read.value_hex));
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &read));
    TEST_ASSERT_EQUAL_UINT8(0, state.read_count);
    TEST_ASSERT_EQUAL_UINT8(2, state.queue_count);
    TEST_ASSERT_TRUE(state.terminal_queued);
}

void test_bounded_chunk_whose_escaped_json_exceeds_512_closes_as_overflow(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);

    ble_investigation_chunk_t end = chunk_for(BLE_INV_CHUNK_END);
    end.state = BLE_INV_COMPLETE;
    memset(end.summary, 1, sizeof(end.summary) - 1U);
    end.summary[sizeof(end.summary) - 1U] = '\0';
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &end));
    TEST_ASSERT_EQUAL_UINT8(2, state.queue_count);
    TEST_ASSERT_TRUE(state.terminal_queued);
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, 0));
    backend_command_result_t overflow = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &overflow));
    TEST_ASSERT_EQUAL_UINT32(1, overflow.sequence);
    TEST_ASSERT_TRUE(overflow.json_length <= BACKEND_COMMAND_RESULT_MAX_JSON);
    TEST_ASSERT_NOT_NULL(strstr(
        overflow.json, "\"error\":\"result_overflow\""));
}

void test_wrong_scanner_or_request_id_is_ignored_without_closing_command(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    ble_investigation_chunk_t begin = chunk_for(BLE_INV_CHUNK_BEGIN);
    begin.mode = BLE_INV_MODE_GATT;
    strcpy(begin.target_mac, "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_BLE, &begin));
    strcpy(begin.request_id, OTHER_COMMAND_ID);
    TEST_ASSERT_FALSE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &begin));
    TEST_ASSERT_EQUAL_UINT8(0, state.queue_count);
    TEST_ASSERT_FALSE(state.terminal_queued);
    TEST_ASSERT_TRUE(state.radio_active);
}

void test_timeout_closes_at_exact_deadline_and_not_on_backward_clock(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    request.timeout_ms = 4000;
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    accept_begin(&state);
    TEST_ASSERT_FALSE(backend_ble_investigation_check_timeout(&state, 999));
    TEST_ASSERT_FALSE(backend_ble_investigation_check_timeout(&state, 4999));
    TEST_ASSERT_TRUE(backend_ble_investigation_check_timeout(&state, 5000));
    TEST_ASSERT_TRUE(state.terminal_queued);
    TEST_ASSERT_FALSE(backend_ble_investigation_check_timeout(&state, 5001));
}

void test_first_seen_cancel_emits_begin_then_cancelled_without_radio_start(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = passive_request(COMMAND_ID);
    TEST_ASSERT_FALSE(backend_ble_investigation_cancel_first_seen(
        &state, COMMAND_ID, &request, -1));
    TEST_ASSERT_TRUE(backend_ble_investigation_cancel_first_seen(
        &state, COMMAND_ID, &request, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, state.radio_start_count);
    TEST_ASSERT_FALSE(state.radio_active);
    TEST_ASSERT_TRUE(state.terminal_queued);

    backend_command_result_t begin = {0};
    backend_command_result_t end = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &begin));
    TEST_ASSERT_EQUAL_UINT32(0, begin.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_begin", begin.type);
    TEST_ASSERT_NOT_NULL(strstr(begin.json, "\"target_mac\":null"));
    TEST_ASSERT_NULL(strstr(begin.json, "target\""));
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, begin.sequence));
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &end));
    TEST_ASSERT_EQUAL_UINT32(1, end.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_end", end.type);
    TEST_ASSERT_EQUAL_STRING("cancelled", end.state);
    TEST_ASSERT_NOT_NULL(strstr(end.json, "\"error\":null"));
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, end.sequence));
    TEST_ASSERT_FALSE(state.active);
}

void test_active_cancel_marks_original_owner_without_inventing_result(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    TEST_ASSERT_TRUE(backend_ble_investigation_request_cancel(
        &state, COMMAND_ID));
    backend_scanner_slot_t slot = BACKEND_SCANNER_SLOT_BLE;
    TEST_ASSERT_TRUE(backend_ble_investigation_cancel_pending(&state, &slot));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_SLOT_WIFI, slot);
    TEST_ASSERT_TRUE(backend_ble_investigation_request_cancel(
        &state, COMMAND_ID));
    TEST_ASSERT_FALSE(backend_ble_investigation_request_cancel(
        &state, OTHER_COMMAND_ID));
    TEST_ASSERT_EQUAL_UINT8(0, state.queue_count);
    TEST_ASSERT_EQUAL_UINT32(1, state.radio_start_count);
}

void test_active_cancel_before_scanner_begin_still_queues_ordered_begin_and_end(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, COMMAND_ID, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    TEST_ASSERT_TRUE(backend_ble_investigation_request_cancel(
        &state, COMMAND_ID));

    ble_investigation_chunk_t end = chunk_for(BLE_INV_CHUNK_END);
    end.state = BLE_INV_CANCELLED;
    strcpy(end.summary, "cancelled");
    TEST_ASSERT_TRUE(backend_ble_investigation_accept_chunk(
        &state, BACKEND_SCANNER_SLOT_WIFI, &end));
    TEST_ASSERT_EQUAL_UINT8(2, state.queue_count);
    backend_command_result_t result = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &result));
    TEST_ASSERT_EQUAL_UINT32(0, result.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_begin", result.type);
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, 0));
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &result));
    TEST_ASSERT_EQUAL_UINT32(1, result.sequence);
    TEST_ASSERT_EQUAL_STRING("cancelled", result.state);
}

static void assert_resume(uint32_t next_sequence, bool cancel_pending)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_resume_after_reboot(
        &state, COMMAND_ID, &request, next_sequence, cancel_pending));
    TEST_ASSERT_EQUAL_UINT32(0, state.radio_start_count);
    TEST_ASSERT_FALSE(state.radio_active);
    TEST_ASSERT_TRUE(state.terminal_queued);
    TEST_ASSERT_EQUAL_UINT8(1, state.queue_count);

    backend_command_result_t result = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &result));
    TEST_ASSERT_EQUAL_UINT32(next_sequence, result.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_end", result.type);
    TEST_ASSERT_EQUAL_STRING(cancel_pending ? "cancelled" : "failed", result.state);
    if (cancel_pending) {
        TEST_ASSERT_NOT_NULL(strstr(result.json, "\"error\":null"));
        TEST_ASSERT_NULL(strstr(result.json, "device_restarted"));
    } else {
        TEST_ASSERT_NOT_NULL(strstr(
            result.json, "\"error\":\"device_restarted\""));
    }
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, COMMAND_ID, next_sequence));
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_FALSE(backend_ble_investigation_next_result(&state, &result));
}

void test_reboot_resume_at_one_and_37_emits_only_the_exact_terminal_sequence(void)
{
    assert_resume(1, false);
    assert_resume(37, false);
    assert_resume(1, true);
    assert_resume(37, true);

    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request = gatt_request(COMMAND_ID);
    TEST_ASSERT_FALSE(backend_ble_investigation_resume_after_reboot(
        &state, COMMAND_ID, &request, 0, false));
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(
        test_start_uses_current_ble_owner_and_duplicate_does_not_restart);
    BACKEND_RUN_TEST(
        test_start_rejects_noncanonical_ids_requests_slots_and_timeouts);
    BACKEND_RUN_TEST(
        test_begin_is_translated_once_to_exact_api_envelope_and_replayed_stably);
    BACKEND_RUN_TEST(test_only_matching_result_ack_advances_the_stable_head);
    BACKEND_RUN_TEST(
        test_each_scanner_chunk_gets_the_exact_backend_result_shape);
    BACKEND_RUN_TEST(
        test_full_63_event_stream_holds_head_then_drains_in_sequence);
    BACKEND_RUN_TEST(
        test_out_of_limit_chunk_uses_reserved_terminal_without_overwriting_evidence);
    BACKEND_RUN_TEST(
        test_repeated_or_regressing_progress_closes_with_overflow_failure);
    BACKEND_RUN_TEST(
        test_unterminated_or_oversized_chunk_closes_without_array_overwrite);
    BACKEND_RUN_TEST(
        test_bounded_chunk_whose_escaped_json_exceeds_512_closes_as_overflow);
    BACKEND_RUN_TEST(
        test_wrong_scanner_or_request_id_is_ignored_without_closing_command);
    BACKEND_RUN_TEST(
        test_timeout_closes_at_exact_deadline_and_not_on_backward_clock);
    BACKEND_RUN_TEST(
        test_first_seen_cancel_emits_begin_then_cancelled_without_radio_start);
    BACKEND_RUN_TEST(
        test_active_cancel_marks_original_owner_without_inventing_result);
    BACKEND_RUN_TEST(
        test_active_cancel_before_scanner_begin_still_queues_ordered_begin_and_end);
    BACKEND_RUN_TEST(
        test_reboot_resume_at_one_and_37_emits_only_the_exact_terminal_sequence);
    return UNITY_END();
}
