#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "ble_investigator.h"
#include "ble_investigation_protocol.h"

static ble_investigation_request_t make_request(ble_investigation_mode_t mode,
                                                 const char *request_id,
                                                 uint32_t timeout_ms)
{
    ble_investigation_request_t request = {0};
    snprintf(request.request_id, sizeof(request.request_id), "%s", request_id);
    request.mode = mode;
    request.timeout_ms = timeout_ms;
    if (mode == BLE_INV_MODE_GATT) {
        snprintf(request.target_mac, sizeof(request.target_mac),
                 "AA:BB:CC:DD:EE:FF");
    }
    return request;
}

static void start_gatt(ble_investigator_t *investigator)
{
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_GATT, "req-1", BLE_INV_DEFAULT_TIMEOUT_MS);
    ble_investigator_init(investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(investigator, &request, 1000));
}

static void assert_terminal_requires_resume(const ble_investigator_t *investigator,
                                            ble_investigation_state_t state)
{
    TEST_ASSERT_EQUAL(state, investigator->state);
    TEST_ASSERT_EQUAL(state, investigator->result.state);
    TEST_ASSERT_TRUE(investigator->resume_scan_required);
    TEST_ASSERT_FALSE(investigator->busy);
}

void test_ble_investigator_rejects_second_request_as_busy(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);
    ble_investigator_t expected = investigator;
    ble_investigation_request_t second = make_request(
        BLE_INV_MODE_PASSIVE_CAPTURE, "req-2", 5000);

    TEST_ASSERT_FALSE(ble_investigator_start(&investigator, &second, 1100));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &investigator, sizeof(expected));
}

void test_ble_investigator_gatt_success_reaches_complete(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);

    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CONNECTED,
    };
    ble_investigator_handle_event(&investigator, &event, 1100);
    TEST_ASSERT_EQUAL(BLE_INV_DISCOVERING, investigator.state);
    TEST_ASSERT_TRUE(investigator.connected);

    event.kind = BLE_INVESTIGATOR_EVENT_SERVICE;
    snprintf(event.uuid, sizeof(event.uuid), "180A");
    ble_investigator_handle_event(&investigator, &event, 1200);

    event.kind = BLE_INVESTIGATOR_EVENT_CHARACTERISTIC;
    snprintf(event.service_uuid, sizeof(event.service_uuid), "180A");
    snprintf(event.uuid, sizeof(event.uuid), "2A29");
    event.properties = BLE_INV_PROP_READ;
    ble_investigator_handle_event(&investigator, &event, 1300);

    static const uint8_t value[] = {'F', 'o', 'F'};
    event.kind = BLE_INVESTIGATOR_EVENT_READING_STARTED;
    ble_investigator_handle_event(&investigator, &event, 1350);
    TEST_ASSERT_EQUAL(BLE_INV_READING, investigator.state);

    event.kind = BLE_INVESTIGATOR_EVENT_READ;
    event.value = value;
    event.value_len = sizeof(value);
    ble_investigator_handle_event(&investigator, &event, 1400);

    event.kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE;
    event.value = NULL;
    event.value_len = 0;
    ble_investigator_handle_event(&investigator, &event, 1500);

    assert_terminal_requires_resume(&investigator, BLE_INV_COMPLETE);
    TEST_ASSERT_EQUAL_UINT8(1, investigator.result.service_count);
    TEST_ASSERT_EQUAL_STRING("180A", investigator.result.services[0]);
    TEST_ASSERT_EQUAL_UINT8(1, investigator.result.characteristic_count);
    TEST_ASSERT_EQUAL_STRING("2A29", investigator.result.characteristics[0].uuid);
    TEST_ASSERT_EQUAL_UINT8(1, investigator.result.read_count);
    TEST_ASSERT_EQUAL_STRING("466F46", investigator.result.reads[0].value_hex);

    ble_investigation_result_t result;
    TEST_ASSERT_TRUE(ble_investigator_take_result(&investigator, &result));
    TEST_ASSERT_EQUAL(BLE_INV_COMPLETE, result.state);
    TEST_ASSERT_EQUAL(BLE_INV_IDLE, investigator.state);
    TEST_ASSERT_FALSE(ble_investigator_take_result(&investigator, &result));
}

void test_ble_investigator_auth_error_sets_auth_required(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CONNECTED,
    };
    ble_investigator_handle_event(&investigator, &event, 1100);
    event.kind = BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED;
    event.status = 5;

    ble_investigator_handle_event(&investigator, &event, 1200);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_TRUE(investigator.result.authentication_required);
    TEST_ASSERT_EQUAL_STRING("authentication_required", investigator.result.error);
}

void test_ble_investigator_timeout_reaches_failed(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);

    TEST_ASSERT_EQUAL_INT64(13000, investigator.deadline_ms);
    ble_investigator_tick(&investigator, 12999);
    TEST_ASSERT_EQUAL(BLE_INV_CONNECTING, investigator.state);
    ble_investigator_tick(&investigator, 13000);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("timeout", investigator.result.error);
}

void test_ble_investigator_cancel_reaches_cancelled(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);

    ble_investigator_cancel(&investigator, 1200);

    assert_terminal_requires_resume(&investigator, BLE_INV_CANCELLED);
    TEST_ASSERT_EQUAL_STRING("cancelled", investigator.result.error);
}

void test_ble_investigator_every_terminal_path_requests_scan_resume(void)
{
    ble_investigator_t investigator;
    ble_investigator_event_t event = {0};

    start_gatt(&investigator);
    event.kind = BLE_INVESTIGATOR_EVENT_CONNECT_FAILED;
    event.status = -1;
    ble_investigator_handle_event(&investigator, &event, 1100);
    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);

    start_gatt(&investigator);
    event.kind = BLE_INVESTIGATOR_EVENT_CONNECTED;
    ble_investigator_handle_event(&investigator, &event, 1050);
    event.kind = BLE_INVESTIGATOR_EVENT_DISCONNECTED;
    event.status = -2;
    ble_investigator_handle_event(&investigator, &event, 1100);
    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);

    start_gatt(&investigator);
    event.kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE;
    event.status = 0;
    ble_investigator_handle_event(&investigator, &event, 1100);
    TEST_ASSERT_EQUAL(BLE_INV_CONNECTING, investigator.state);
    TEST_ASSERT_TRUE(investigator.busy);
    event.kind = BLE_INVESTIGATOR_EVENT_CONNECTED;
    ble_investigator_handle_event(&investigator, &event, 1200);
    event.kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE;
    ble_investigator_handle_event(&investigator, &event, 1300);
    assert_terminal_requires_resume(&investigator, BLE_INV_COMPLETE);

    start_gatt(&investigator);
    event.kind = BLE_INVESTIGATOR_EVENT_CONNECTED;
    ble_investigator_handle_event(&investigator, &event, 1050);
    event.kind = BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED;
    ble_investigator_handle_event(&investigator, &event, 1100);
    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);

    start_gatt(&investigator);
    ble_investigator_tick(&investigator, investigator.deadline_ms);
    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);

    start_gatt(&investigator);
    ble_investigator_cancel(&investigator, 1100);
    assert_terminal_requires_resume(&investigator, BLE_INV_CANCELLED);
}

void test_ble_investigator_passive_capture_summarizes_prompt_families(void)
{
    ble_investigator_t investigator;
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_PASSIVE_CAPTURE, "passive-1", BLE_INV_DEFAULT_TIMEOUT_MS);
    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 2000));
    TEST_ASSERT_EQUAL(BLE_INV_SCANNING, investigator.state);

    ble_fingerprint_t apple = {
        .company_id = 0x004C,
        .apple_type = 0x0F,
    };
    ble_fingerprint_t fast_pair = {
        .service_uuids = {0xFE2C},
        .svc_uuid_count = 1,
    };
    static const uint8_t xbox_swift_pair_advertisement[] = {
        0x02, 0x01, 0x06,
        0x05, 0xFF, 0x06, 0x00, 0x03, 0x02,
        0x0E, 0x09, 'X', 'b', 'o', 'x', ' ', 'W', 'i', 'r', 'e', 'l', 'e', 's',
    };
    static const uint8_t microsoft_non_swift_advertisement[] = {
        0x02, 0x01, 0x06,
        0x05, 0xFF, 0x06, 0x00, 0x01, 0x02,
    };
    ble_fingerprint_t swift_pair;
    ble_fingerprint_t microsoft_non_swift;
    ble_fingerprint_compute(xbox_swift_pair_advertisement,
                            sizeof(xbox_swift_pair_advertisement),
                            1, 0, &swift_pair);
    ble_fingerprint_compute(microsoft_non_swift_advertisement,
                            sizeof(microsoft_non_swift_advertisement),
                            1, 0, &microsoft_non_swift);
    TEST_ASSERT_EQUAL_HEX16(0x0006, swift_pair.company_id);
    TEST_ASSERT_EQUAL_UINT8(0x03, swift_pair.raw_mfr[2]);
    TEST_ASSERT_EQUAL_HEX16(0x0006, microsoft_non_swift.company_id);
    TEST_ASSERT_EQUAL_UINT8(0x01, microsoft_non_swift.raw_mfr[2]);
    TEST_ASSERT_TRUE(ble_investigator_fingerprint_is_swift_pair(&swift_pair));
    TEST_ASSERT_FALSE(ble_investigator_fingerprint_is_swift_pair(
        &microsoft_non_swift));
    ble_fingerprint_t unrelated = {
        .company_id = 0x1234,
    };
    const uint8_t mac[6] = {0, 1, 2, 3, 4, 5};

    ble_investigator_note_advertisement(
        &investigator, mac, &apple, -40, 0, 2100);
    ble_investigator_note_advertisement(
        &investigator, mac, &fast_pair, -50, 0, 2200);
    ble_investigator_note_advertisement(
        &investigator, mac, &swift_pair, -60, 0, 2300);
    ble_investigator_note_advertisement(
        &investigator, mac, &microsoft_non_swift, -60, 0, 2350);
    ble_investigator_note_advertisement(
        &investigator, mac, &unrelated, -30, 0, 2400);

    TEST_ASSERT_EQUAL(BLE_INV_SCANNING, investigator.state);
    TEST_ASSERT_TRUE(investigator.busy);
    ble_investigator_tick(&investigator, 14000);

    assert_terminal_requires_resume(&investigator, BLE_INV_COMPLETE);
    TEST_ASSERT_NOT_NULL(strstr(investigator.result.summary, "Apple=1"));
    TEST_ASSERT_NOT_NULL(strstr(investigator.result.summary, "Fast=1"));
    TEST_ASSERT_NOT_NULL(strstr(investigator.result.summary, "Swift=1"));
}

void test_ble_investigator_ignores_gatt_data_before_connected(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);
    static const uint8_t value[] = {0x01};
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_SERVICE,
    };
    snprintf(event.uuid, sizeof(event.uuid), "180A");
    ble_investigator_handle_event(&investigator, &event, 1100);

    event.kind = BLE_INVESTIGATOR_EVENT_CHARACTERISTIC;
    snprintf(event.service_uuid, sizeof(event.service_uuid), "180A");
    snprintf(event.uuid, sizeof(event.uuid), "2A29");
    event.properties = BLE_INV_PROP_READ;
    ble_investigator_handle_event(&investigator, &event, 1200);

    event.kind = BLE_INVESTIGATOR_EVENT_READ;
    event.value = value;
    event.value_len = sizeof(value);
    ble_investigator_handle_event(&investigator, &event, 1300);

    event.kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE;
    event.value = NULL;
    event.value_len = 0;
    ble_investigator_handle_event(&investigator, &event, 1400);

    TEST_ASSERT_EQUAL(BLE_INV_CONNECTING, investigator.state);
    TEST_ASSERT_TRUE(investigator.busy);
    TEST_ASSERT_EQUAL_UINT8(0, investigator.result.service_count);
    TEST_ASSERT_EQUAL_UINT8(0, investigator.result.characteristic_count);
    TEST_ASSERT_EQUAL_UINT8(0, investigator.result.read_count);
}

void test_ble_investigator_deadline_precedes_event_mutation(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CONNECTED,
    };

    ble_investigator_handle_event(&investigator, &event,
                                  investigator.deadline_ms);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_FALSE(investigator.connected);
    TEST_ASSERT_EQUAL_STRING("timeout", investigator.result.error);
}

void test_ble_investigator_enforces_discovery_and_read_phases(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_CONNECTED,
    };
    ble_investigator_handle_event(&investigator, &event, 1100);

    static const uint8_t value[] = {0x42};
    event.kind = BLE_INVESTIGATOR_EVENT_READ;
    event.value = value;
    event.value_len = sizeof(value);
    snprintf(event.uuid, sizeof(event.uuid), "2A29");
    ble_investigator_handle_event(&investigator, &event, 1200);
    TEST_ASSERT_EQUAL(BLE_INV_DISCOVERING, investigator.state);
    TEST_ASSERT_EQUAL_UINT8(0, investigator.result.read_count);

    event.kind = BLE_INVESTIGATOR_EVENT_READING_STARTED;
    event.value = NULL;
    event.value_len = 0;
    ble_investigator_handle_event(&investigator, &event, 1300);
    TEST_ASSERT_EQUAL(BLE_INV_READING, investigator.state);

    event.kind = BLE_INVESTIGATOR_EVENT_SERVICE;
    snprintf(event.uuid, sizeof(event.uuid), "180A");
    ble_investigator_handle_event(&investigator, &event, 1400);
    event.kind = BLE_INVESTIGATOR_EVENT_CHARACTERISTIC;
    snprintf(event.service_uuid, sizeof(event.service_uuid), "180A");
    snprintf(event.uuid, sizeof(event.uuid), "2A29");
    ble_investigator_handle_event(&investigator, &event, 1500);
    TEST_ASSERT_EQUAL_UINT8(0, investigator.result.service_count);
    TEST_ASSERT_EQUAL_UINT8(0, investigator.result.characteristic_count);

    event.kind = BLE_INVESTIGATOR_EVENT_READ;
    event.value = value;
    event.value_len = sizeof(value);
    ble_investigator_handle_event(&investigator, &event, 1600);
    TEST_ASSERT_EQUAL_UINT8(1, investigator.result.read_count);
}

void test_ble_investigator_prepare_procedure_checks_deadline(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);

    TEST_ASSERT_TRUE(ble_investigator_prepare_procedure(
        &investigator, BLE_INV_CONNECTING, investigator.deadline_ms - 1));
    TEST_ASSERT_FALSE(ble_investigator_prepare_procedure(
        &investigator, BLE_INV_CONNECTING, investigator.deadline_ms));

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("timeout", investigator.result.error);
}

void test_ble_investigator_procedure_failure_requires_active_gatt_phase(void)
{
    ble_investigator_t investigator;
    start_gatt(&investigator);
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_PROCEDURE_FAILED,
        .status = -7,
    };
    snprintf(event.uuid, sizeof(event.uuid), "service_discovery_failed");

    ble_investigator_handle_event(&investigator, &event, 1100);
    TEST_ASSERT_EQUAL(BLE_INV_CONNECTING, investigator.state);
    TEST_ASSERT_TRUE(investigator.busy);

    event.kind = BLE_INVESTIGATOR_EVENT_CONNECTED;
    ble_investigator_handle_event(&investigator, &event, 1200);
    event.kind = BLE_INVESTIGATOR_EVENT_PROCEDURE_FAILED;
    ble_investigator_handle_event(&investigator, &event, 1300);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("service_discovery_failed",
                             investigator.result.error);
}

void test_ble_investigator_runtime_fence_waits_for_cancel_and_scan_resume(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, generation);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, true));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));

    uint16_t conn_handle = BLE_INV_CONN_HANDLE_NONE;
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, &conn_handle));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation, false, BLE_INV_CONN_HANDLE_NONE));

    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_RESUME_SCAN,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, &conn_handle));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_scan_resumed(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_release(
        &fence, generation));
    TEST_ASSERT_FALSE(fence.active);
}

void test_ble_investigator_runtime_fence_terminates_late_connect(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, true));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));

    uint16_t conn_handle = BLE_INV_CONN_HANDLE_NONE;
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, &conn_handle));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation, true, 0x1234));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_accepts_gatt(
        &fence, generation, 0x1234));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_TERMINATE_CONNECTION,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, &conn_handle));
    TEST_ASSERT_EQUAL_HEX16(0x1234, conn_handle);
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_note_disconnected(
        &fence, generation, 0x4321));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_disconnected(
        &fence, generation, 0x1234));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
}

void test_ble_investigator_runtime_fence_rejects_old_generation_and_handle(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation_a = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation_a, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation_a));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_release(
        &fence, generation_a));

    uint32_t generation_b = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_NOT_EQUAL_UINT32(generation_a, generation_b);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation_b));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation_a, true, 7));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation_b, true, 7));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_accepts_gatt(
        &fence, generation_a, 7));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_accepts_gatt(
        &fence, generation_b, 8));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_accepts_gatt(
        &fence, generation_b, 7));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_note_disconnected(
        &fence, generation_a, 7));
    TEST_ASSERT_EQUAL_HEX16(7, fence.expected_conn_handle);
}

void test_ble_investigator_chunk_fence_orders_end_and_generations(void)
{
    ble_investigator_chunk_fence_t fence;
    ble_investigator_chunk_fence_init(&fence);
    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_open(&fence, 11));
    TEST_ASSERT_FALSE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_SERVICE));
    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_BEGIN));
    TEST_ASSERT_FALSE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_END));
    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_finish_emit(
        &fence, 11, BLE_INV_CHUNK_BEGIN, true));

    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_SERVICE));
    TEST_ASSERT_FALSE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_END));
    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_finish_emit(
        &fence, 11, BLE_INV_CHUNK_SERVICE, true));

    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_END));
    TEST_ASSERT_FALSE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_READ));
    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_finish_emit(
        &fence, 11, BLE_INV_CHUNK_END, true));
    TEST_ASSERT_FALSE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_CHARACTERISTIC));

    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_open(&fence, 12));
    TEST_ASSERT_FALSE(ble_investigator_chunk_fence_begin_emit(
        &fence, 11, BLE_INV_CHUNK_BEGIN));
    TEST_ASSERT_TRUE(ble_investigator_chunk_fence_begin_emit(
        &fence, 12, BLE_INV_CHUNK_BEGIN));
}

void test_ble_investigator_rejection_frames_begin_then_failed_end(void)
{
    ble_investigation_chunk_t chunks[2];
    TEST_ASSERT_TRUE(ble_investigator_build_rejection_chunks(
        "reject-1", (ble_investigation_mode_t)99, NULL,
        "invalid_mode", chunks));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, chunks[0].kind);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_PASSIVE_CAPTURE, chunks[0].mode);
    TEST_ASSERT_EQUAL_STRING("reject-1", chunks[0].request_id);
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, chunks[1].kind);
    TEST_ASSERT_EQUAL(BLE_INV_FAILED, chunks[1].state);
    TEST_ASSERT_EQUAL_STRING("invalid_mode", chunks[1].error);

    ble_investigation_result_t result;
    ble_investigation_result_init(&result);
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunks[0]));
    TEST_ASSERT_TRUE(ble_investigation_result_accept(&result, &chunks[1]));
    TEST_ASSERT_EQUAL(BLE_INV_FAILED, result.state);
    TEST_ASSERT_EQUAL_STRING("invalid_mode", result.error);

    TEST_ASSERT_FALSE(ble_investigator_build_rejection_chunks(
        "bad\nrequest", BLE_INV_MODE_GATT, NULL, "invalid_target", chunks));
    TEST_ASSERT_FALSE(ble_investigator_build_rejection_chunks(
        "reject-2", BLE_INV_MODE_GATT, NULL, "bad\nerror", chunks));
}

void test_ble_investigator_passive_start_requires_scanner_readiness(void)
{
    ble_investigation_request_t passive = make_request(
        BLE_INV_MODE_PASSIVE_CAPTURE, "passive-ready", 1000);
    ble_investigation_request_t gatt = make_request(
        BLE_INV_MODE_GATT, "gatt-ready", 1000);

    TEST_ASSERT_FALSE(ble_investigator_passive_start_is_ready(
        &passive, false));
    TEST_ASSERT_TRUE(ble_investigator_passive_start_is_ready(
        &passive, true));
    TEST_ASSERT_TRUE(ble_investigator_passive_start_is_ready(
        &gatt, false));
}

void test_ble_investigator_peer_cache_freshness_expires_at_thirty_seconds(void)
{
    TEST_ASSERT_TRUE(ble_investigator_peer_cache_is_fresh(1000, 1000));
    TEST_ASSERT_TRUE(ble_investigator_peer_cache_is_fresh(1000, 30999));
    TEST_ASSERT_FALSE(ble_investigator_peer_cache_is_fresh(1000, 31000));
    TEST_ASSERT_FALSE(ble_investigator_peer_cache_is_fresh(2000, 1999));
}

void test_ble_investigator_passive_scanner_outage_fails(void)
{
    ble_investigator_t investigator;
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_PASSIVE_CAPTURE, "passive-outage", 5000);
    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 1000));
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_SCANNER_UNAVAILABLE,
    };

    ble_investigator_handle_event(&investigator, &event, 1500);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("scanner_unavailable", investigator.result.error);
}

void test_ble_investigator_runtime_fence_handles_no_callback_cleanup_results(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_cleanup_action_failed(
        &fence, generation, BLE_INV_CLEANUP_CANCEL_CONNECT, true));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_reconcile_cancel(
        &fence, generation, BLE_INV_PEER_LOOKUP_NOT_CONNECTED,
        BLE_INV_CONN_HANDLE_NONE));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_release(
        &fence, generation));

    generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation, true, 9));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_TERMINATE_CONNECTION,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_cleanup_action_failed(
        &fence, generation, BLE_INV_CLEANUP_TERMINATE_CONNECTION, true));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
}

void test_ble_investigator_runtime_fence_blocks_cleanup_during_operation(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_operation(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, true));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_NONE,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_finish_operation(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_RESUME_SCAN,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
}

void test_ble_investigator_parses_display_mac_to_nimble_byte_order(void)
{
    uint8_t address[6] = {0};
    const uint8_t expected[6] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};

    TEST_ASSERT_TRUE(ble_investigator_parse_target_mac(
        "AA:BB:CC:DD:EE:FF", address));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, address, 6);
    TEST_ASSERT_FALSE(ble_investigator_parse_target_mac(
        "AA:BB:CC:DD:EE:FG", address));
    TEST_ASSERT_FALSE(ble_investigator_parse_target_mac(
        "AA:BB:CC:DD:EE:FF:00", address));
}

void test_ble_investigator_cancel_reconciliation_adopts_live_peer(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_reconcile_cancel(
        &fence, generation, BLE_INV_PEER_LOOKUP_CONNECTED, 0x0042));

    uint16_t conn_handle = BLE_INV_CONN_HANDLE_NONE;
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_TERMINATE_CONNECTION,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, &conn_handle));
    TEST_ASSERT_EQUAL_HEX16(0x0042, conn_handle);
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_note_disconnected(
        &fence, generation, 0x0043));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_disconnected(
        &fence, generation, 0x0042));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
}

void test_ble_investigator_cancel_reconciliation_retries_ambiguous_lookup(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_reconcile_cancel(
        &fence, generation, BLE_INV_PEER_LOOKUP_INDETERMINATE,
        BLE_INV_CONN_HANDLE_NONE));

    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
}

void test_ble_investigator_cancel_reconciliation_requires_definitive_no_link(void)
{
    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_CANCEL_CONNECT,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_reconcile_cancel(
        &fence, generation, BLE_INV_PEER_LOOKUP_NOT_CONNECTED,
        BLE_INV_CONN_HANDLE_NONE));

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation, false, BLE_INV_CONN_HANDLE_NONE));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_release(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_note_connect_result(
        &fence, generation, false, BLE_INV_CONN_HANDLE_NONE));
}

static uint32_t prepare_early_connect_handoff(
    ble_investigator_runtime_fence_t *fence,
    uint16_t conn_handle)
{
    ble_investigator_runtime_fence_init(fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(fence);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_operation(
        fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_mark_connecting(
        fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_connect_result(
        fence, generation, true, conn_handle));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_defer_discovery(
        fence, generation + 1, conn_handle));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_defer_discovery(
        fence, generation, (uint16_t)(conn_handle + 1)));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_defer_discovery(
        fence, generation, conn_handle));
    return generation;
}

void test_ble_investigator_early_connect_defers_discovery_until_call_returns(void)
{
    ble_investigator_runtime_fence_t fence;
    uint32_t generation = prepare_early_connect_handoff(&fence, 0x0051);
    uint16_t conn_handle = BLE_INV_CONN_HANDLE_NONE;

    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation, true, &conn_handle));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_finish_operation(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation + 1, true, &conn_handle));
    TEST_ASSERT_EQUAL_HEX16(BLE_INV_CONN_HANDLE_NONE, conn_handle);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation, true, &conn_handle));
    TEST_ASSERT_EQUAL_HEX16(0x0051, conn_handle);
}

void test_ble_investigator_deferred_discovery_obeys_timeout_and_cancel(void)
{
    ble_investigator_runtime_fence_t fence;
    uint32_t generation = prepare_early_connect_handoff(&fence, 0x0052);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_finish_operation(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation, false, NULL));

    generation = prepare_early_connect_handoff(&fence, 0x0053);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, false));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_finish_operation(
        &fence, generation));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation, true, NULL));
    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_TERMINATE_CONNECTION,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
}

void test_ble_investigator_deferred_discovery_launches_only_once(void)
{
    ble_investigator_runtime_fence_t fence;
    uint32_t generation = prepare_early_connect_handoff(&fence, 0x0054);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_finish_operation(
        &fence, generation));

    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation, true, NULL));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_take_deferred_discovery(
        &fence, generation, true, NULL));
}

void test_ble_investigator_passive_outage_precedes_deadline(void)
{
    ble_investigator_t investigator;
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_PASSIVE_CAPTURE, "passive-deadline", 5000);
    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 1000));
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_SCANNER_UNAVAILABLE,
    };

    ble_investigator_handle_event(
        &investigator, &event, investigator.deadline_ms);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("scanner_unavailable", investigator.result.error);

    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 2000));
    ble_investigator_handle_event(
        &investigator, &event, investigator.deadline_ms + 1);

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("scanner_unavailable", investigator.result.error);
}

void test_ble_investigator_passive_result_owns_restart_until_confirmed(void)
{
    ble_investigator_t investigator;
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_PASSIVE_CAPTURE, "passive-restart", 5000);
    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 1000));
    ble_investigator_event_t event = {
        .kind = BLE_INVESTIGATOR_EVENT_SCANNER_UNAVAILABLE,
    };
    ble_investigator_handle_event(&investigator, &event, 1500);

    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);
    TEST_ASSERT_TRUE(investigator.result_pending);
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_begin_cleanup(
        &fence, generation, investigator.resume_scan_required));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_end_emitted(
        &fence, generation));

    TEST_ASSERT_EQUAL(BLE_INV_CLEANUP_RESUME_SCAN,
                      ble_investigator_runtime_fence_next_cleanup_action(
                          &fence, generation, NULL));
    TEST_ASSERT_FALSE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_note_scan_resumed(
        &fence, generation));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_can_release(
        &fence, generation));
}

void test_ble_investigator_pending_restore_may_restart_scanner(void)
{
    TEST_ASSERT_FALSE(ble_investigator_scan_start_is_allowed(true, false));
    TEST_ASSERT_TRUE(ble_investigator_scan_start_is_allowed(true, true));
    TEST_ASSERT_TRUE(ble_investigator_scan_start_is_allowed(false, true));
    TEST_ASSERT_TRUE(ble_investigator_scan_start_is_allowed(false, false));
}

void test_ble_investigator_request_parser_marks_same_id_as_retransmit(void)
{
    char longest_valid[BLE_INV_REQUEST_ID_LEN];
    memset(longest_valid, 'a', sizeof(longest_valid));
    longest_valid[sizeof(longest_valid) - 1] = '\0';
    char too_long[BLE_INV_REQUEST_ID_LEN + 1];
    memset(too_long, 'b', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';

    TEST_ASSERT_TRUE(ble_investigator_request_id_is_valid("req-7"));
    TEST_ASSERT_TRUE(ble_investigator_request_id_is_valid(longest_valid));
    TEST_ASSERT_FALSE(ble_investigator_request_id_is_valid(""));
    TEST_ASSERT_FALSE(ble_investigator_request_id_is_valid("bad id"));
    TEST_ASSERT_FALSE(ble_investigator_request_id_is_valid(too_long));
    TEST_ASSERT_EQUAL(BLE_INV_REQUEST_RETRANSMIT,
                      ble_investigator_decide_request(
                          true, "req-7", "req-7"));
    TEST_ASSERT_EQUAL(BLE_INV_REQUEST_AVAILABLE,
                      ble_investigator_decide_request(
                          false, NULL, "req-7"));
    TEST_ASSERT_EQUAL(BLE_INV_REQUEST_INVALID,
                      ble_investigator_decide_request(
                          true, "req-7", "bad id"));
}

void test_ble_investigator_different_busy_id_keeps_visible_rejection(void)
{
    TEST_ASSERT_EQUAL(BLE_INV_REQUEST_BUSY_REJECTION,
                      ble_investigator_decide_request(
                          true, "req-active", "req-other"));

    ble_investigation_chunk_t chunks[2];
    TEST_ASSERT_TRUE(ble_investigator_build_rejection_chunks(
        "req-other", BLE_INV_MODE_GATT, "AA:BB:CC:DD:EE:FF",
        "busy", chunks));
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_BEGIN, chunks[0].kind);
    TEST_ASSERT_EQUAL_STRING("req-other", chunks[0].request_id);
    TEST_ASSERT_EQUAL(BLE_INV_CHUNK_END, chunks[1].kind);
    TEST_ASSERT_EQUAL(BLE_INV_FAILED, chunks[1].state);
    TEST_ASSERT_EQUAL_STRING("req-other", chunks[1].request_id);
    TEST_ASSERT_EQUAL_STRING("busy", chunks[1].error);
}

void test_ble_investigator_deadline_blocks_scan_pause_before_reserve_and_cancel(void)
{
    ble_investigator_t investigator;
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_GATT, "one-ms", 1);
    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 1000));
    TEST_ASSERT_EQUAL_INT64(1001, investigator.deadline_ms);

    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);

    TEST_ASSERT_FALSE(ble_investigator_runtime_reserve_operation(
        &investigator, &fence, generation,
        BLE_INV_CONNECTING, investigator.deadline_ms));

    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("timeout", investigator.result.error);
    TEST_ASSERT_FALSE(fence.operation_in_progress);

    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 2000));
    TEST_ASSERT_TRUE(ble_investigator_runtime_reserve_operation(
        &investigator, &fence, generation,
        BLE_INV_CONNECTING, investigator.deadline_ms - 1));
    TEST_ASSERT_TRUE(fence.operation_in_progress);
    TEST_ASSERT_FALSE(ble_investigator_prepare_procedure(
        &investigator, BLE_INV_CONNECTING, investigator.deadline_ms));
    TEST_ASSERT_TRUE(ble_investigator_runtime_fence_finish_operation(
        &fence, generation));
    TEST_ASSERT_FALSE(fence.operation_in_progress);
    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);
    TEST_ASSERT_EQUAL_STRING("timeout", investigator.result.error);
}

void test_ble_investigator_operation_reservation_fences_old_generation(void)
{
    ble_investigator_t investigator;
    ble_investigation_request_t request = make_request(
        BLE_INV_MODE_GATT, "generation", 1);
    ble_investigator_init(&investigator);
    TEST_ASSERT_TRUE(ble_investigator_start(&investigator, &request, 1000));
    ble_investigator_t expected = investigator;

    ble_investigator_runtime_fence_t fence;
    ble_investigator_runtime_fence_init(&fence);
    uint32_t generation = ble_investigator_runtime_fence_begin(&fence);

    TEST_ASSERT_FALSE(ble_investigator_runtime_reserve_operation(
        &investigator, &fence, generation + 1,
        BLE_INV_CONNECTING, investigator.deadline_ms));

    TEST_ASSERT_EQUAL_MEMORY(&expected, &investigator, sizeof(expected));
    TEST_ASSERT_FALSE(fence.operation_in_progress);
}
