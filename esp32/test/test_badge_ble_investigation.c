#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "badge_ble_investigation_state.h"

static void copy_text(char *out, size_t out_len, const char *text)
{
    snprintf(out, out_len, "%s", text ? text : "");
}

static void make_request(ble_investigation_request_t *request,
                         const char *request_id,
                         ble_investigation_mode_t mode,
                         const char *target_mac)
{
    memset(request, 0, sizeof(*request));
    copy_text(request->request_id, sizeof(request->request_id), request_id);
    request->mode = mode;
    copy_text(request->target_mac, sizeof(request->target_mac), target_mac);
    request->timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS;
}

static void make_begin(ble_investigation_chunk_t *chunk,
                       const ble_investigation_request_t *request)
{
    memset(chunk, 0, sizeof(*chunk));
    chunk->kind = BLE_INV_CHUNK_BEGIN;
    copy_text(chunk->request_id, sizeof(chunk->request_id), request->request_id);
    chunk->mode = request->mode;
    copy_text(chunk->target_mac, sizeof(chunk->target_mac), request->target_mac);
}

static void start_request(badge_ble_investigation_state_t *state,
                          ble_investigation_request_t *request)
{
    int scanner_slot = -1;
    badge_ble_investigation_state_init(state);
    make_request(request, "req-1", BLE_INV_MODE_GATT, "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_start(
        state, request, true, &scanner_slot));
    TEST_ASSERT_EQUAL_INT(0, scanner_slot);
}

static void accept_begin(badge_ble_investigation_state_t *state,
                         const ble_investigation_request_t *request)
{
    ble_investigation_chunk_t begin;
    make_begin(&begin, request);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(state, &begin));
}

void test_badge_investigation_routes_only_to_ble_scanner_slot(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    int scanner_slot = 99;
    badge_ble_investigation_state_init(&state);
    make_request(&request, "route-1", BLE_INV_MODE_GATT,
                 "AA:BB:CC:DD:EE:FF");

    TEST_ASSERT_TRUE(badge_ble_investigation_state_start(
        &state, &request, true, &scanner_slot));

    TEST_ASSERT_EQUAL_INT(0, scanner_slot);
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL(BLE_INV_QUEUED, state.result.state);
    TEST_ASSERT_EQUAL_STRING("route-1", state.result.request_id);
}

void test_badge_investigation_rejects_when_ble_scanner_is_unavailable(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    int scanner_slot = 99;
    badge_ble_investigation_state_init(&state);
    make_request(&request, "offline-1", BLE_INV_MODE_GATT,
                 "AA:BB:CC:DD:EE:FF");

    TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
        &state, &request, false, &scanner_slot));

    TEST_ASSERT_EQUAL_INT(-1, scanner_slot);
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_EQUAL(BLE_INV_IDLE, state.result.state);
    TEST_ASSERT_EQUAL_STRING("", state.result.request_id);
}

void test_badge_investigation_assembles_scanner_chunks_by_request_id(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t chunk;
    ble_investigation_result_t result;
    start_request(&state, &request);
    accept_begin(&state, &request);

    memset(&chunk, 0, sizeof(chunk));
    chunk.kind = BLE_INV_CHUNK_PROGRESS;
    chunk.state = BLE_INV_DISCOVERING;
    copy_text(chunk.request_id, sizeof(chunk.request_id), request.request_id);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 0;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "FFE0");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    chunk.kind = BLE_INV_CHUNK_CHARACTERISTIC;
    chunk.index = 0;
    copy_text(chunk.service_uuid, sizeof(chunk.service_uuid), "FFE0");
    copy_text(chunk.uuid, sizeof(chunk.uuid), "FFE1");
    chunk.properties = BLE_INV_PROP_READ | BLE_INV_PROP_WRITE;
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    chunk.kind = BLE_INV_CHUNK_READ;
    chunk.index = 0;
    copy_text(chunk.uuid, sizeof(chunk.uuid), "FFE1");
    copy_text(chunk.value_hex, sizeof(chunk.value_hex), "414243");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.summary, sizeof(chunk.summary), "UART service found");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    badge_ble_investigation_state_get(&state, &result);
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_EQUAL(BLE_INV_COMPLETE, result.state);
    TEST_ASSERT_EQUAL_UINT8(1, result.service_count);
    TEST_ASSERT_EQUAL_UINT8(1, result.characteristic_count);
    TEST_ASSERT_EQUAL_UINT8(1, result.read_count);
    TEST_ASSERT_EQUAL_STRING("FFE0", result.services[0]);
    TEST_ASSERT_EQUAL_STRING("FFE1", result.characteristics[0].uuid);
    TEST_ASSERT_EQUAL_STRING("414243", result.reads[0].value_hex);
    TEST_ASSERT_EQUAL_STRING("UART service found", result.summary);
}

void test_badge_investigation_transport_loss_keeps_local_operation_active(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t progress = {0};
    start_request(&state, &request);
    accept_begin(&state, &request);

    badge_ble_investigation_state_transport_lost(&state);

    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL_STRING("req-1", state.result.request_id);
    progress.kind = BLE_INV_CHUNK_PROGRESS;
    progress.state = BLE_INV_CONNECTING;
    copy_text(progress.request_id, sizeof(progress.request_id), "req-1");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &progress));
    TEST_ASSERT_EQUAL(BLE_INV_CONNECTING, state.result.state);
}

void test_badge_investigation_ble_chunk_cursor_is_bounded(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t chunk = {0};
    ble_investigation_chunk_t selected;
    start_request(&state, &request);
    accept_begin(&state, &request);

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 0;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    copy_text(chunk.uuid, sizeof(chunk.uuid), "1800");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));
    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.summary, sizeof(chunk.summary), "done");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    TEST_ASSERT_EQUAL_UINT8(3, state.chunk_count);
    for (int seq = 0; seq < state.chunk_count; ++seq) {
        TEST_ASSERT_TRUE(badge_ble_investigation_state_select_chunk(
            &state, "req-1", seq));
        TEST_ASSERT_TRUE(badge_ble_investigation_state_get_selected_chunk(
            &state, &selected));
        TEST_ASSERT_EQUAL(seq, state.selected_chunk);
    }
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, selected.kind);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(
        BADGE_BLE_INVESTIGATION_MAX_CHUNKS, state.chunk_count);
}

void test_badge_investigation_rejects_mismatched_and_out_of_order_chunks(void)
{
    badge_ble_investigation_state_t state;
    badge_ble_investigation_state_t expected;
    ble_investigation_request_t request;
    ble_investigation_chunk_t chunk = {0};
    start_request(&state, &request);
    accept_begin(&state, &request);

    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "other-request");
    expected = state;
    TEST_ASSERT_FALSE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &state, sizeof(state));
    TEST_ASSERT_TRUE(state.active);

    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 1;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    expected = state;
    TEST_ASSERT_FALSE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &state, sizeof(state));
}

void test_badge_investigation_rejects_duplicate_begin_and_end(void)
{
    badge_ble_investigation_state_t state;
    badge_ble_investigation_state_t expected;
    ble_investigation_request_t request;
    ble_investigation_chunk_t chunk;
    start_request(&state, &request);
    make_begin(&chunk, &request);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));

    expected = state;
    TEST_ASSERT_FALSE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &state, sizeof(state));

    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.summary, sizeof(chunk.summary), "done");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));
    expected = state;
    TEST_ASSERT_FALSE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &state, sizeof(state));
}

void test_badge_investigation_cursor_rejects_mismatch_and_out_of_range(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t selected;
    start_request(&state, &request);
    accept_begin(&state, &request);

    TEST_ASSERT_TRUE(badge_ble_investigation_state_select_chunk(
        &state, "req-1", 0));
    TEST_ASSERT_FALSE(badge_ble_investigation_state_select_chunk(
        &state, "wrong", 0));
    TEST_ASSERT_FALSE(badge_ble_investigation_state_select_chunk(
        &state, "req-1", -1));
    TEST_ASSERT_FALSE(badge_ble_investigation_state_select_chunk(
        &state, "req-1", state.chunk_count));
    TEST_ASSERT_EQUAL_INT(0, state.selected_chunk);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_selected_chunk(
        &state, &selected));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, selected.kind);
}

void test_badge_investigation_compact_status_is_bounded_and_has_no_full_arrays(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t end = {0};
    char json[BADGE_BLE_INVESTIGATION_STATUS_JSON_MAX];
    start_request(&state, &request);
    accept_begin(&state, &request);

    end.kind = BLE_INV_CHUNK_END;
    end.state = BLE_INV_FAILED;
    end.authentication_required = true;
    end.truncated = true;
    copy_text(end.request_id, sizeof(end.request_id), "req-1");
    copy_text(end.summary, sizeof(end.summary), "quote \" and newline\nsummary");
    copy_text(end.error, sizeof(end.error), "denied\r\n\"unsafe\"");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &end));

    size_t len = badge_ble_investigation_state_status_json(
        &state, json, sizeof(json));

    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(json), len);
    TEST_ASSERT_EQUAL_UINT32(strlen(json), len);
    TEST_ASSERT_NULL(strchr(json, '\n'));
    TEST_ASSERT_NULL(strchr(json, '\r'));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"request_id\":\"req-1\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"state\":\"failed\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"service_count\":0"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"characteristic_count\":0"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"authentication_required\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"truncated\":true"));
    TEST_ASSERT_NULL(strstr(json, "\"services\":"));
    TEST_ASSERT_NULL(strstr(json, "\"characteristics\":"));
    TEST_ASSERT_NULL(strstr(json, "\"reads\":"));

    char too_small[16] = "not-cleared";
    TEST_ASSERT_EQUAL_UINT32(0, badge_ble_investigation_state_status_json(
        &state, too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_CHAR('\0', too_small[0]);
}

void test_badge_investigation_chunk_read_requires_encrypted_authorized_connection(void)
{
    TEST_ASSERT_FALSE(badge_ble_investigation_chunk_read_authorized(false, false));
    TEST_ASSERT_FALSE(badge_ble_investigation_chunk_read_authorized(true, false));
    TEST_ASSERT_FALSE(badge_ble_investigation_chunk_read_authorized(false, true));
    TEST_ASSERT_TRUE(badge_ble_investigation_chunk_read_authorized(true, true));
}

void test_badge_investigation_same_request_restart_does_not_corrupt_active_state(void)
{
    badge_ble_investigation_state_t state;
    badge_ble_investigation_state_t expected;
    ble_investigation_request_t request;
    int scanner_slot = -1;
    start_request(&state, &request);
    accept_begin(&state, &request);
    expected = state;

    TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
        &state, &request, true, &scanner_slot));

    TEST_ASSERT_EQUAL_MEMORY(&expected, &state, sizeof(state));
}
