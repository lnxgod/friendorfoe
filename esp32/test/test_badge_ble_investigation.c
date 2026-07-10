#include "unity.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "badge_ble_investigation_state.h"

void test_badge_investigation_operations_have_bounded_stack_contract(void)
{
    TEST_ASSERT_GREATER_THAN_UINT32(
        BADGE_BLE_INVESTIGATION_OPERATION_STACK_MAX,
        sizeof(badge_ble_investigation_state_t));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(
        2048, BADGE_BLE_INVESTIGATION_OPERATION_STACK_MAX);
}

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

static void start_request_at(badge_ble_investigation_state_t *state,
                             ble_investigation_request_t *request,
                             int64_t now_ms)
{
    int scanner_slot = -1;
    badge_ble_investigation_state_init(state);
    make_request(request, "expiry-1", BLE_INV_MODE_GATT,
                 "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_start_at(
        state, request, true, now_ms, &scanner_slot));
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

void test_badge_investigation_expiry_before_deadline_is_noop(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    badge_ble_investigation_expiry_t expiry;
    start_request_at(&state, &request, 1000);

    TEST_ASSERT_EQUAL_INT64(
        1000 + BLE_INV_DEFAULT_TIMEOUT_MS +
            BADGE_BLE_INVESTIGATION_TRANSPORT_GRACE_MS,
        state.deadline_ms);
    TEST_ASSERT_FALSE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms - 1, &expiry));
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_FALSE(state.end_received);
    TEST_ASSERT_EQUAL_UINT8(0, state.chunk_count);
    TEST_ASSERT_EQUAL_UINT8(0, expiry.chunk_count);
}

void test_badge_investigation_expiry_at_deadline_synthesizes_timeout(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    badge_ble_investigation_expiry_t expiry;
    ble_investigation_chunk_t terminal;
    start_request_at(&state, &request, 5000);
    accept_begin(&state, &request);

    TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms, &expiry));
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_TRUE(state.end_received);
    TEST_ASSERT_EQUAL(BLE_INV_FAILED, state.result.state);
    TEST_ASSERT_EQUAL_STRING("timeout", state.result.summary);
    TEST_ASSERT_EQUAL_STRING("timeout", state.result.error);
    TEST_ASSERT_EQUAL_UINT8(1, expiry.chunk_count);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, request.request_id, expiry.first_seq, &terminal));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, terminal.kind);
    TEST_ASSERT_EQUAL(BLE_INV_FAILED, terminal.state);
    TEST_ASSERT_EQUAL_STRING("timeout", terminal.summary);
    TEST_ASSERT_EQUAL_STRING("timeout", terminal.error);
}

void test_badge_investigation_expiry_before_begin_stores_valid_begin_then_end(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    badge_ble_investigation_expiry_t expiry;
    ble_investigation_chunk_t begin;
    ble_investigation_chunk_t terminal;
    char json[UART_JSON_MAX_SIZE];
    start_request_at(&state, &request, 9000);

    TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms + 1, &expiry));
    TEST_ASSERT_EQUAL_UINT8(2, expiry.chunk_count);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, request.request_id, expiry.first_seq, &begin));
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, request.request_id, expiry.first_seq + 1, &terminal));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, begin.kind);
    TEST_ASSERT_EQUAL_STRING(request.request_id, begin.request_id);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_GATT, begin.mode);
    TEST_ASSERT_EQUAL_STRING(request.target_mac, begin.target_mac);
    TEST_ASSERT_GREATER_THAN_UINT32(0,
        ble_investigation_chunk_to_json(&begin, json, sizeof(json)));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, terminal.kind);
    TEST_ASSERT_EQUAL(BLE_INV_FAILED, terminal.state);
    TEST_ASSERT_GREATER_THAN_UINT32(0,
        ble_investigation_chunk_to_json(&terminal, json, sizeof(json)));
}

void test_badge_investigation_expiry_rejects_late_chunks_and_allows_restart(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_request_t next_request;
    badge_ble_investigation_expiry_t expiry;
    ble_investigation_chunk_t late_progress = {0};
    int scanner_slot = -1;
    start_request_at(&state, &request, 12000);
    accept_begin(&state, &request);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms + 50, &expiry));

    late_progress.kind = BLE_INV_CHUNK_PROGRESS;
    late_progress.state = BLE_INV_READING;
    copy_text(late_progress.request_id, sizeof(late_progress.request_id),
              request.request_id);
    TEST_ASSERT_FALSE(badge_ble_investigation_state_accept(
        &state, &late_progress));

    make_request(&next_request, "expiry-2", BLE_INV_MODE_GATT,
                 "11:22:33:44:55:66");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_start_at(
        &state, &next_request, true, state.deadline_ms + 100,
        &scanner_slot));
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_EQUAL_STRING("expiry-2", state.result.request_id);
}

void test_badge_investigation_dropped_end_reconnect_expires_without_transport_cancel(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    badge_ble_investigation_expiry_t expiry;
    ble_investigation_chunk_t progress = {0};
    ble_investigation_chunk_t terminal;
    start_request_at(&state, &request, 15000);
    accept_begin(&state, &request);
    progress.kind = BLE_INV_CHUNK_PROGRESS;
    progress.state = BLE_INV_READING;
    copy_text(progress.request_id, sizeof(progress.request_id),
              request.request_id);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &progress));

    badge_ble_investigation_state_transport_lost(&state);
    TEST_ASSERT_TRUE(state.active);
    TEST_ASSERT_FALSE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms - 1, &expiry));
    TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms, &expiry));
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, request.request_id, expiry.first_seq, &terminal));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, terminal.kind);
    TEST_ASSERT_EQUAL_STRING("timeout", terminal.error);
}

void test_badge_investigation_pending_timeout_survives_subsequent_start(void)
{
    static badge_ble_investigation_state_t state;
    static badge_ble_investigation_pending_queue_t pending;
    static char chunk_json[UART_JSON_MAX_SIZE];
    static char usb_frame[BADGE_BLE_INVESTIGATION_USB_FRAME_MAX];
    ble_investigation_request_t request;
    ble_investigation_request_t next_request;
    badge_ble_investigation_expiry_t expiry;
    ble_investigation_chunk_t queued;
    int scanner_slot = -1;
    badge_ble_investigation_pending_queue_init(&pending);
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_can_accept_expiry(
        &pending));
    start_request_at(&state, &request, 20000);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms, &expiry));
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_enqueue_expiry(
        &pending, &state, &expiry));
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_can_accept_expiry(
        &pending));

    make_request(&next_request, "after-timeout", BLE_INV_MODE_GATT,
                 "11:22:33:44:55:66");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_start_at(
        &state, &next_request, true, 40000, &scanner_slot));
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_peek(
        &pending, &queued));
    TEST_ASSERT_EQUAL_STRING("expiry-1", queued.request_id);
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, queued.kind);
    TEST_ASSERT_GREATER_THAN_UINT32(0, ble_investigation_chunk_to_json(
        &queued, chunk_json, sizeof(chunk_json)));
    TEST_ASSERT_GREATER_THAN_UINT32(0, badge_ble_investigation_usb_frame(
        chunk_json, usb_frame, sizeof(usb_frame)));
    TEST_ASSERT_EQUAL_STRING_LEN("FOF_INV:", usb_frame, 8);
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_consume(&pending));
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_peek(
        &pending, &queued));
    TEST_ASSERT_EQUAL_STRING("expiry-1", queued.request_id);
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, queued.kind);
    TEST_ASSERT_GREATER_THAN_UINT32(0, ble_investigation_chunk_to_json(
        &queued, chunk_json, sizeof(chunk_json)));
    TEST_ASSERT_NOT_NULL(strstr(chunk_json, "\"error\":\"timeout\""));
    TEST_ASSERT_GREATER_THAN_UINT32(0, badge_ble_investigation_usb_frame(
        chunk_json, usb_frame, sizeof(usb_frame)));
    TEST_ASSERT_LESS_THAN_UINT32(BADGE_BLE_INVESTIGATION_USB_FRAME_MAX,
                                 strlen(usb_frame));
}

void test_badge_investigation_pending_queue_rejects_full_without_overwrite(void)
{
    static badge_ble_investigation_state_t state;
    static badge_ble_investigation_pending_queue_t pending;
    ble_investigation_request_t request;
    badge_ble_investigation_expiry_t expiry;
    ble_investigation_chunk_t queued;
    badge_ble_investigation_pending_queue_init(&pending);

    for (int i = 0; i < 2; ++i) {
        start_request_at(&state, &request, 50000 + i * 20000);
        TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
            &state, state.deadline_ms, &expiry));
        TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_enqueue_expiry(
            &pending, &state, &expiry));
    }
    TEST_ASSERT_EQUAL_UINT8(BADGE_BLE_INVESTIGATION_PENDING_CHUNKS,
                            pending.count);
    TEST_ASSERT_FALSE(badge_ble_investigation_pending_queue_can_accept_expiry(
        &pending));
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_peek(
        &pending, &queued));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, queued.kind);

    start_request_at(&state, &request, 100000);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_expire(
        &state, state.deadline_ms, &expiry));
    TEST_ASSERT_FALSE(badge_ble_investigation_pending_queue_enqueue_expiry(
        &pending, &state, &expiry));
    TEST_ASSERT_EQUAL_UINT8(BADGE_BLE_INVESTIGATION_PENDING_CHUNKS,
                            pending.count);
    TEST_ASSERT_FALSE(badge_ble_investigation_pending_queue_can_accept_expiry(
        &pending));
    TEST_ASSERT_TRUE(badge_ble_investigation_pending_queue_peek(
        &pending, &queued));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, queued.kind);
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
        TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
            &state, "req-1", seq, &selected));
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

    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, "req-1", 0, &selected));
    TEST_ASSERT_FALSE(badge_ble_investigation_state_get_chunk(
        &state, "wrong", 0, &selected));
    TEST_ASSERT_FALSE(badge_ble_investigation_state_get_chunk(
        &state, "req-1", -1, &selected));
    TEST_ASSERT_FALSE(badge_ble_investigation_state_get_chunk(
        &state, "req-1", state.chunk_count, &selected));
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

void test_badge_investigation_request_validator_matches_scanner_grammar(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_request_t normalized;
    int scanner_slot = 9;
    badge_ble_investigation_state_init(&state);

    make_request(&request, "req-grammar", BLE_INV_MODE_GATT,
                 "aa:bb:cc:dd:ee:ff");
    TEST_ASSERT_TRUE(badge_ble_investigation_request_validate(
        &request, &normalized));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", normalized.target_mac);

    const char *invalid_ids[] = {"", "bad id", "bad\tid", "bad\nid"};
    for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++i) {
        make_request(&request, invalid_ids[i], BLE_INV_MODE_GATT,
                     "AA:BB:CC:DD:EE:FF");
        TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
            &state, &request, true, &scanner_slot));
        TEST_ASSERT_FALSE(state.active);
        TEST_ASSERT_EQUAL(BLE_INV_IDLE, state.result.state);
        TEST_ASSERT_EQUAL_STRING("", state.result.request_id);
    }

    make_request(&request, "bad-mode", (ble_investigation_mode_t)99,
                 "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
        &state, &request, true, &scanner_slot));
    make_request(&request, "bad-mac", BLE_INV_MODE_GATT,
                 "AA-BB-CC-DD-EE-FF");
    TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
        &state, &request, true, &scanner_slot));
    make_request(&request, "missing-mac", BLE_INV_MODE_GATT, "");
    TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
        &state, &request, true, &scanner_slot));
    make_request(&request, "passive-target", BLE_INV_MODE_PASSIVE_CAPTURE,
                 "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_FALSE(badge_ble_investigation_state_start(
        &state, &request, true, &scanner_slot));
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_EQUAL(BLE_INV_IDLE, state.result.state);
}

void test_badge_investigation_stateless_chunks_and_independent_selections(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t first;
    ble_investigation_chunk_t second;
    badge_ble_investigation_selection_t phone_a;
    badge_ble_investigation_selection_t phone_b;
    start_request(&state, &request);
    accept_begin(&state, &request);
    ble_investigation_chunk_t progress = {0};
    progress.kind = BLE_INV_CHUNK_PROGRESS;
    progress.state = BLE_INV_CONNECTING;
    copy_text(progress.request_id, sizeof(progress.request_id), "req-1");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &progress));

    badge_ble_investigation_selection_init(&phone_a);
    badge_ble_investigation_selection_init(&phone_b);
    TEST_ASSERT_TRUE(badge_ble_investigation_selection_set(
        &phone_a, &state, "req-1", 0));
    TEST_ASSERT_TRUE(badge_ble_investigation_selection_set(
        &phone_b, &state, "req-1", 1));
    TEST_ASSERT_TRUE(badge_ble_investigation_selection_get(
        &phone_a, &state, &first));
    TEST_ASSERT_TRUE(badge_ble_investigation_selection_get(
        &phone_b, &state, &second));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, first.kind);
    TEST_ASSERT_NOT_EQUAL(first.kind, second.kind);

    badge_ble_investigation_selection_clear(&phone_a);
    TEST_ASSERT_FALSE(badge_ble_investigation_selection_get(
        &phone_a, &state, &first));
    TEST_ASSERT_TRUE(badge_ble_investigation_selection_get(
        &phone_b, &state, &second));
}

void test_badge_investigation_start_fence_is_generation_safe(void)
{
    badge_ble_investigation_start_fence_t fence;
    badge_ble_investigation_start_fence_init(&fence);
    uint32_t revision = 0;
    uint32_t generation = badge_ble_investigation_start_fence_reserve(
        &fence, revision);
    TEST_ASSERT_NOT_EQUAL(0, generation);
    TEST_ASSERT_TRUE(badge_ble_investigation_start_fence_should_rollback(
        &fence, generation, revision, false));

    generation = badge_ble_investigation_start_fence_reserve(&fence, revision);
    TEST_ASSERT_NOT_EQUAL(0, generation);
    TEST_ASSERT_FALSE(badge_ble_investigation_start_fence_should_rollback(
        &fence, generation, revision + 1, false));
    TEST_ASSERT_FALSE(badge_ble_investigation_start_fence_should_rollback(
        &fence, generation + 1, revision, false));
    TEST_ASSERT_FALSE(badge_ble_investigation_start_fence_should_rollback(
        &fence, generation, revision, true));
}

void test_badge_investigation_usb_output_is_one_bounded_frame(void)
{
    char frame[BADGE_BLE_INVESTIGATION_USB_FRAME_MAX];
    const char *json =
        "{\"type\":\"ble_inv_service\",\"request_id\":\"req-1\","
        "\"index\":0,\"uuid\":\"FFE0\"}";
    size_t len = badge_ble_investigation_usb_frame(json, frame,
                                                   sizeof(frame));
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    TEST_ASSERT_EQUAL_STRING_LEN("FOF_INV:", frame, 8);
    TEST_ASSERT_EQUAL_CHAR('\n', frame[len - 1]);
    TEST_ASSERT_EQUAL_PTR(&frame[len - 1], strchr(frame, '\n'));
    TEST_ASSERT_NULL(strchr(frame, '\r'));
    TEST_ASSERT_EQUAL_UINT32(0, badge_ble_investigation_usb_frame(
        "{\"bad\":\"line\nfeed\"}", frame, sizeof(frame)));
}

void test_badge_investigation_dedupes_progress_and_reserves_end_capacity(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t chunk = {0};
    ble_investigation_chunk_t selected;
    start_request(&state, &request);
    accept_begin(&state, &request);

    chunk.kind = BLE_INV_CHUNK_PROGRESS;
    chunk.state = BLE_INV_CONNECTING;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));
    uint8_t count_after_first = state.chunk_count;
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_EQUAL_UINT8(count_after_first, state.chunk_count);

    state.chunk_count = BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1;
    memset(&chunk, 0, sizeof(chunk));
    chunk.kind = BLE_INV_CHUNK_SERVICE;
    chunk.index = 0;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    copy_text(chunk.uuid, sizeof(chunk.uuid), "1800");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_TRUE(state.result.truncated);
    TEST_ASSERT_EQUAL_UINT8(BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1,
                            state.chunk_count);

    memset(&chunk, 0, sizeof(chunk));
    chunk.kind = BLE_INV_CHUNK_END;
    chunk.state = BLE_INV_COMPLETE;
    copy_text(chunk.request_id, sizeof(chunk.request_id), "req-1");
    copy_text(chunk.summary, sizeof(chunk.summary), "done");
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &chunk));
    TEST_ASSERT_FALSE(state.active);
    TEST_ASSERT_EQUAL_UINT8(BADGE_BLE_INVESTIGATION_MAX_CHUNKS,
                            state.chunk_count);
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, "req-1", BADGE_BLE_INVESTIGATION_MAX_CHUNKS - 1,
        &selected));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, selected.kind);
    TEST_ASSERT_TRUE(selected.truncated);
}

void test_badge_investigation_strict_chunk_field_validators(void)
{
    int index = -1;
    TEST_ASSERT_TRUE(badge_ble_investigation_index_from_number(0.0, &index));
    TEST_ASSERT_EQUAL_INT(0, index);
    TEST_ASSERT_TRUE(badge_ble_investigation_index_from_number(42.0, &index));
    TEST_ASSERT_FALSE(badge_ble_investigation_index_from_number(-1.0, &index));
    TEST_ASSERT_FALSE(badge_ble_investigation_index_from_number(1.5, &index));
    TEST_ASSERT_FALSE(badge_ble_investigation_index_from_number(NAN, &index));
    TEST_ASSERT_FALSE(badge_ble_investigation_index_from_number(INFINITY, &index));
    TEST_ASSERT_FALSE(badge_ble_investigation_index_from_number(
        (double)INT_MAX + 1.0, &index));

    TEST_ASSERT_TRUE(badge_ble_investigation_uuid_is_canonical("FFE0"));
    TEST_ASSERT_TRUE(badge_ble_investigation_uuid_is_canonical("12345678"));
    TEST_ASSERT_TRUE(badge_ble_investigation_uuid_is_canonical(
        "12345678-1234-ABCD-9876-1234567890AB"));
    TEST_ASSERT_FALSE(badge_ble_investigation_uuid_is_canonical("ffe0"));
    TEST_ASSERT_FALSE(badge_ble_investigation_uuid_is_canonical("FFE"));
    TEST_ASSERT_FALSE(badge_ble_investigation_uuid_is_canonical(
        "12345678-1234-ABCD-9876-1234567890AZ"));

    TEST_ASSERT_TRUE(badge_ble_investigation_value_hex_is_valid(""));
    TEST_ASSERT_TRUE(badge_ble_investigation_value_hex_is_valid("00aF"));
    TEST_ASSERT_FALSE(badge_ble_investigation_value_hex_is_valid("0"));
    TEST_ASSERT_FALSE(badge_ble_investigation_value_hex_is_valid("00XZ"));
}

void test_badge_investigation_worst_case_status_and_end_fit_uart_bound(void)
{
    badge_ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_chunk_t end = {0};
    ble_investigation_chunk_t selected;
    char status[UART_JSON_MAX_SIZE];
    char larger_status[UART_JSON_MAX_SIZE + 32];
    char chunk_json[UART_JSON_MAX_SIZE];

    memset(&request, 0, sizeof(request));
    for (size_t i = 0; i < sizeof(request.request_id) - 1; ++i) {
        request.request_id[i] = (i % 2 == 0) ? '"' : '\\';
    }
    request.mode = BLE_INV_MODE_GATT;
    copy_text(request.target_mac, sizeof(request.target_mac),
              "AA:BB:CC:DD:EE:FF");
    request.timeout_ms = BLE_INV_DEFAULT_TIMEOUT_MS;
    badge_ble_investigation_state_init(&state);
    int slot = -1;
    TEST_ASSERT_TRUE(badge_ble_investigation_state_start(
        &state, &request, true, &slot));
    accept_begin(&state, &request);

    end.kind = BLE_INV_CHUNK_END;
    end.state = BLE_INV_FAILED;
    copy_text(end.request_id, sizeof(end.request_id), request.request_id);
    for (size_t i = 0; i < sizeof(end.summary) - 1; ++i) {
        end.summary[i] = (i % 2 == 0) ? '"' : '\\';
    }
    for (size_t i = 0; i < sizeof(end.error) - 1; ++i) {
        end.error[i] = (i % 2 == 0) ? '\\' : '"';
    }
    end.authentication_required = true;
    end.truncated = true;
    TEST_ASSERT_TRUE(badge_ble_investigation_state_accept(&state, &end));

    TEST_ASSERT_GREATER_THAN_UINT32(0,
        badge_ble_investigation_state_status_json(&state, status,
                                                  sizeof(status)));
    TEST_ASSERT_GREATER_THAN_UINT32(0,
        badge_ble_investigation_state_status_json(&state, larger_status,
                                                  sizeof(larger_status)));
    TEST_ASSERT_TRUE(badge_ble_investigation_state_get_chunk(
        &state, request.request_id, state.chunk_count - 1, &selected));
    TEST_ASSERT_GREATER_THAN_UINT32(0,
        ble_investigation_chunk_to_json(&selected, chunk_json,
                                        sizeof(chunk_json)));
    TEST_ASSERT_NOT_NULL(strstr(status, "\"state\":\"failed\""));
    TEST_ASSERT_NOT_NULL(strstr(chunk_json, "\"type\":\"ble_inv_end\""));
}
