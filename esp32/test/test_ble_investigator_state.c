#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "ble_investigator.h"

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
        .kind = BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED,
        .status = 5,
    };

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
    event.kind = BLE_INVESTIGATOR_EVENT_DISCONNECTED;
    event.status = -2;
    ble_investigator_handle_event(&investigator, &event, 1100);
    assert_terminal_requires_resume(&investigator, BLE_INV_FAILED);

    start_gatt(&investigator);
    event.kind = BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE;
    event.status = 0;
    ble_investigator_handle_event(&investigator, &event, 1100);
    assert_terminal_requires_resume(&investigator, BLE_INV_COMPLETE);

    start_gatt(&investigator);
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
    ble_fingerprint_t swift_pair = {
        .company_id = 0x0006,
    };
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
        &investigator, mac, &unrelated, -30, 0, 2400);

    TEST_ASSERT_EQUAL(BLE_INV_SCANNING, investigator.state);
    TEST_ASSERT_TRUE(investigator.busy);
    ble_investigator_tick(&investigator, 14000);

    assert_terminal_requires_resume(&investigator, BLE_INV_COMPLETE);
    TEST_ASSERT_NOT_NULL(strstr(investigator.result.summary, "Apple=1"));
    TEST_ASSERT_NOT_NULL(strstr(investigator.result.summary, "Fast=1"));
    TEST_ASSERT_NOT_NULL(strstr(investigator.result.summary, "Swift=1"));
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
