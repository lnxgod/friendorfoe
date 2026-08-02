#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_command_client.h"
#include "../support/backend_test_main.h"

#define COMMAND_ID "0123456789abcdef0123456789abcdef"
#define OTHER_COMMAND_ID "ffffffffffffffffffffffffffffffff"

static const char GATT_COMMAND[] =
    "{\"command_id\":\"" COMMAND_ID "\","
    "\"type\":\"ble_investigate\","
    "\"request_id\":\"" COMMAND_ID "\","
    "\"mode\":\"gatt\","
    "\"target\":\"AA:BB:CC:DD:EE:FF\","
    "\"timeout_ms\":12000,\"next_sequence\":0,\"result_state\":null}";

static const char PASSIVE_COMMAND[] =
    "{\"command_id\":\"" COMMAND_ID "\","
    "\"type\":\"ble_investigate\","
    "\"request_id\":\"" COMMAND_ID "\","
    "\"mode\":\"passive_capture\",\"target\":null,"
    "\"timeout_ms\":1,\"next_sequence\":0,\"result_state\":null}";

static const char CANCEL_COMMAND[] =
    "{\"command_id\":\"" COMMAND_ID "\","
    "\"type\":\"ble_investigate_cancel\","
    "\"request_id\":\"" COMMAND_ID "\","
    "\"mode\":\"gatt\","
    "\"target\":\"AA:BB:CC:DD:EE:FF\","
    "\"timeout_ms\":12000,\"next_sequence\":0,\"result_state\":null}";

static const char RESUMED_COMMAND[] =
    "{\"command_id\":\"" COMMAND_ID "\","
    "\"type\":\"ble_investigate\","
    "\"request_id\":\"" COMMAND_ID "\","
    "\"mode\":\"gatt\","
    "\"target\":\"AA:BB:CC:DD:EE:FF\","
    "\"timeout_ms\":12000,\"next_sequence\":37,"
    "\"result_state\":\"reading\"}";

static const char RESUMED_CANCEL[] =
    "{\"command_id\":\"" COMMAND_ID "\","
    "\"type\":\"ble_investigate_cancel\","
    "\"request_id\":\"" COMMAND_ID "\","
    "\"mode\":\"gatt\","
    "\"target\":\"AA:BB:CC:DD:EE:FF\","
    "\"timeout_ms\":12000,\"next_sequence\":1,"
    "\"result_state\":\"queued\"}";

void setUp(void)
{
}

void tearDown(void)
{
}

static backend_command_envelope_t decode_ok(const char *json)
{
    backend_command_envelope_t envelope;
    memset(&envelope, 0xA5, sizeof(envelope));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_DECODE_OK,
        backend_command_envelope_decode(
            json, strlen(json), &envelope));
    return envelope;
}

static void assert_decode_rejected(const char *json)
{
    backend_command_envelope_t envelope;
    memset(&envelope, 0xA5, sizeof(envelope));
    TEST_ASSERT_NOT_EQUAL(BACKEND_COMMAND_DECODE_OK,
        backend_command_envelope_decode(
            json, strlen(json), &envelope));
    backend_command_envelope_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &envelope, sizeof(envelope));
}

static backend_command_result_t begin_result(void)
{
    backend_command_result_t result;
    memset(&result, 0, sizeof(result));
    result.sequence = 0U;
    strcpy(result.type, "ble_inv_begin");
    strcpy(result.json,
        "{\"sequence\":0,\"type\":\"ble_inv_begin\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target_mac\":\"AA:BB:CC:DD:EE:FF\"}");
    result.json_length = (uint16_t)strlen(result.json);
    return result;
}

static backend_command_result_t resumed_failed_result(void)
{
    backend_command_result_t result;
    memset(&result, 0, sizeof(result));
    result.sequence = 37U;
    strcpy(result.type, "ble_inv_end");
    strcpy(result.state, "failed");
    strcpy(result.json,
        "{\"sequence\":37,\"type\":\"ble_inv_end\","
        "\"request_id\":\"" COMMAND_ID "\",\"state\":\"failed\","
        "\"summary\":\"\",\"error\":\"device_restarted\","
        "\"authentication_required\":false,\"truncated\":false}");
    result.json_length = (uint16_t)strlen(result.json);
    return result;
}

static void bind_fresh_result_state(
    backend_command_client_state_t *state,
    const backend_command_envelope_t *envelope)
{
    backend_command_client_init(state);
    TEST_ASSERT_TRUE(backend_command_client_bind(
        state, "uplink_CB77A4", envelope));
}

void test_exact_gatt_and_passive_envelopes_decode(void)
{
    backend_command_envelope_t gatt = decode_ok(GATT_COMMAND);
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_KIND_INVESTIGATE, gatt.kind);
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, gatt.command_id);
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, gatt.request.request_id);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_GATT, gatt.request.mode);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", gatt.request.target_mac);
    TEST_ASSERT_EQUAL_UINT32(12000U, gatt.request.timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, gatt.next_sequence);
    TEST_ASSERT_FALSE(gatt.has_result_state);

    backend_command_envelope_t passive = decode_ok(PASSIVE_COMMAND);
    TEST_ASSERT_EQUAL(BLE_INV_MODE_PASSIVE_CAPTURE, passive.request.mode);
    TEST_ASSERT_EQUAL_STRING("", passive.request.target_mac);
    TEST_ASSERT_EQUAL_UINT32(1U, passive.request.timeout_ms);

    backend_command_envelope_t cancel = decode_ok(CANCEL_COMMAND);
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_KIND_CANCEL, cancel.kind);
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, cancel.request.request_id);
}

void test_envelope_requires_exactly_eight_unique_known_fields(void)
{
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\","
        "\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\","
        "\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":null,\"extra\":true}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\","
        "\"type\":\"ble_investigate\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"reboot\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":null}");
}

void test_envelope_rejects_noncanonical_ids_and_mismatch(void)
{
    assert_decode_rejected(
        "{\"command_id\":\"0123456789abcdef0123456789abcdeF\","
        "\"type\":\"ble_investigate\","
        "\"request_id\":\"0123456789abcdef0123456789abcdeF\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12000,\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"0123456789abcdef0123456789abcde\","
        "\"type\":\"ble_investigate\","
        "\"request_id\":\"0123456789abcdef0123456789abcde\","
        "\"mode\":\"gatt\",\"target\":\"AA:BB:CC:DD:EE:FF\","
        "\"timeout_ms\":12000,\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\","
        "\"type\":\"ble_investigate\","
        "\"request_id\":\"" OTHER_COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":null}");
}

void test_mode_target_timeout_sequence_and_state_are_strict(void)
{
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":null,\"timeout_ms\":12000,\"next_sequence\":0,"
        "\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"passive_capture\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"aa:bb:cc:dd:ee:ff\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":0,"
        "\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12001,"
        "\"next_sequence\":0,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":4294967296,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":1,\"result_state\":\"pending\"}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":0,\"result_state\":\"queued\"}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":1,\"result_state\":null}");
    assert_decode_rejected(
        "{\"command_id\":\"" COMMAND_ID "\",\"type\":\"ble_investigate\","
        "\"request_id\":\"" COMMAND_ID "\",\"mode\":\"gatt\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\",\"timeout_ms\":12000,"
        "\"next_sequence\":1,\"result_state\":\"complete\"}");

    backend_command_envelope_t resumed = decode_ok(RESUMED_COMMAND);
    TEST_ASSERT_TRUE(resumed.has_result_state);
    TEST_ASSERT_EQUAL(BLE_INV_READING, resumed.result_state);
}

void test_intent_distinguishes_fresh_active_conflict_and_reboot(void)
{
    backend_command_envelope_t investigate = decode_ok(GATT_COMMAND);
    backend_command_envelope_t cancel = decode_ok(CANCEL_COMMAND);
    backend_command_envelope_t resumed = decode_ok(RESUMED_COMMAND);
    backend_command_envelope_t resumed_cancel = decode_ok(RESUMED_CANCEL);

    backend_ble_investigation_state_t active;
    backend_ble_investigation_init(&active);

    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_START,
        backend_command_select_intent(&investigate, &active));
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &active, investigate.command_id, &investigate.request,
        BACKEND_SCANNER_SLOT_BLE, 0));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_ALREADY_ACTIVE,
        backend_command_select_intent(&investigate, &active));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_CANCEL_FIRST_SEEN,
        backend_command_select_intent(&cancel, NULL));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_CANCEL_ACTIVE,
        backend_command_select_intent(&cancel, &active));

    backend_command_envelope_t changed_request = investigate;
    strcpy(changed_request.request.target_mac, "11:22:33:44:55:66");
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_CONFLICT,
        backend_command_select_intent(&changed_request, &active));
    changed_request = investigate;
    changed_request.request.timeout_ms = 1000U;
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_CONFLICT,
        backend_command_select_intent(&changed_request, &active));

    backend_ble_investigation_state_t other_active;
    backend_ble_investigation_init(&other_active);
    backend_command_envelope_t other = investigate;
    strcpy(other.command_id, OTHER_COMMAND_ID);
    strcpy(other.request.request_id, OTHER_COMMAND_ID);
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &other_active, other.command_id, &other.request,
        BACKEND_SCANNER_SLOT_BLE, 0));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_CONFLICT,
        backend_command_select_intent(&investigate, &other_active));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_RESUME_FAILED,
        backend_command_select_intent(&resumed, NULL));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_RESUME_CANCELLED,
        backend_command_select_intent(&resumed_cancel, NULL));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_ALREADY_ACTIVE,
        backend_command_select_intent(&resumed, &active));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_INTENT_CANCEL_ACTIVE,
        backend_command_select_intent(&resumed_cancel, &active));
}

void test_scanner_codec_maps_only_target_to_mac_and_cancel_is_minimal(void)
{
    char line[256];
    backend_command_envelope_t gatt = decode_ok(GATT_COMMAND);
    size_t length = backend_command_scanner_line_encode(
        &gatt, BACKEND_COMMAND_INTENT_START, line, sizeof(line));
    TEST_ASSERT_EQUAL_UINT(strlen(line), length);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"investigate\",\"command_id\":\"" COMMAND_ID "\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"gatt\","
        "\"timeout_ms\":12000}", line);
    TEST_ASSERT_NULL(strstr(line, "target"));
    TEST_ASSERT_NULL(strstr(line, "request_id"));

    backend_command_envelope_t passive = decode_ok(PASSIVE_COMMAND);
    TEST_ASSERT_NOT_EQUAL(0U, backend_command_scanner_line_encode(
        &passive, BACKEND_COMMAND_INTENT_START, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"investigate\",\"command_id\":\"" COMMAND_ID "\","
        "\"mac\":null,\"mode\":\"passive_capture\",\"timeout_ms\":1}",
        line);

    backend_command_envelope_t cancel = decode_ok(CANCEL_COMMAND);
    TEST_ASSERT_NOT_EQUAL(0U, backend_command_scanner_line_encode(
        &cancel, BACKEND_COMMAND_INTENT_CANCEL_ACTIVE, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"cancel\",\"command_id\":\"" COMMAND_ID "\"}", line);
    TEST_ASSERT_NULL(strstr(line, "mac"));
    TEST_ASSERT_NULL(strstr(line, "mode"));
    TEST_ASSERT_NULL(strstr(line, "timeout"));

    TEST_ASSERT_EQUAL_UINT(0U, backend_command_scanner_line_encode(
        &cancel, BACKEND_COMMAND_INTENT_CANCEL_FIRST_SEEN, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("", line);
}

void test_poll_and_result_paths_are_exact_and_bounded(void)
{
    char path[BACKEND_COMMAND_PATH_CAPACITY];
    TEST_ASSERT_TRUE(backend_command_build_poll_path(
        "uplink_CB77A4", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/nodes/uplink_CB77A4/commands/next", path);
    TEST_ASSERT_TRUE(backend_command_build_result_path(
        "uplink_CB77A4", COMMAND_ID, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(
        "/nodes/uplink_CB77A4/commands/" COMMAND_ID "/result", path);

    char short_path[12] = "unchanged";
    TEST_ASSERT_FALSE(backend_command_build_poll_path(
        "uplink_CB77A4", short_path, sizeof(short_path)));
    TEST_ASSERT_EQUAL_STRING("", short_path);
    TEST_ASSERT_FALSE(backend_command_build_poll_path(
        "../badge", path, sizeof(path)));
    TEST_ASSERT_FALSE(backend_command_build_result_path(
        "uplink_CB77A4", "BAD", path, sizeof(path)));
}

void test_result_post_body_is_stable_and_only_exact_retry_is_eligible(void)
{
    backend_command_envelope_t envelope = decode_ok(GATT_COMMAND);
    backend_command_client_state_t state;
    bind_fresh_result_state(&state, &envelope);
    backend_command_result_t result = begin_result();

    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    TEST_ASSERT_TRUE(state.pending);
    TEST_ASSERT_EQUAL_UINT32(1U, state.attempt_count);
    TEST_ASSERT_FALSE(state.replay_eligible);
    TEST_ASSERT_EQUAL_STRING(
        "/nodes/uplink_CB77A4/commands/" COMMAND_ID "/result",
        state.result_path);
    TEST_ASSERT_EQUAL_UINT(result.json_length, state.post_body_length);
    TEST_ASSERT_EQUAL_MEMORY(
        result.json, state.post_body, result.json_length + 1U);

    backend_command_result_t changed = result;
    char *target = strstr(changed.json, "AA:BB");
    TEST_ASSERT_NOT_NULL(target);
    target[0] = 'F';
    TEST_ASSERT_FALSE(backend_command_result_prepare(&state, &changed));
    TEST_ASSERT_EQUAL_UINT32(1U, state.attempt_count);
    TEST_ASSERT_FALSE(state.replay_eligible);
    TEST_ASSERT_EQUAL_MEMORY(
        result.json, state.post_body, result.json_length + 1U);

    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    TEST_ASSERT_EQUAL_UINT32(2U, state.attempt_count);
    TEST_ASSERT_TRUE(state.replay_eligible);
    TEST_ASSERT_EQUAL_MEMORY(
        result.json, state.post_body, result.json_length + 1U);
}

void test_exact_correlated_ack_validates_then_commit_advances_once(void)
{
    backend_command_envelope_t envelope = decode_ok(GATT_COMMAND);
    backend_command_client_state_t state;
    bind_fresh_result_state(&state, &envelope);
    backend_command_result_t result = begin_result();
    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    static const char ack_json[] =
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":false,"
        "\"duplicate\":false}";
    backend_command_result_ack_t ack;
    TEST_ASSERT_TRUE(backend_command_result_ack_validate(
        &state, ack_json, sizeof(ack_json) - 1U, &ack));
    TEST_ASSERT_EQUAL_STRING(COMMAND_ID, ack.command_id);
    TEST_ASSERT_EQUAL_UINT32(0U, ack.accepted_sequence);
    TEST_ASSERT_EQUAL_UINT32(1U, ack.next_sequence);
    TEST_ASSERT_EQUAL(BLE_INV_QUEUED, ack.result_state);
    TEST_ASSERT_FALSE(ack.terminal);
    TEST_ASSERT_FALSE(ack.duplicate);

    TEST_ASSERT_TRUE(backend_command_result_ack_commit(&state, &ack));
    TEST_ASSERT_FALSE(state.pending);
    TEST_ASSERT_TRUE(state.bound);
    TEST_ASSERT_EQUAL_UINT32(1U, state.next_sequence);
    TEST_ASSERT_TRUE(state.has_result_state);
    TEST_ASSERT_EQUAL(BLE_INV_QUEUED, state.result_state);
    TEST_ASSERT_FALSE(backend_command_result_ack_commit(&state, &ack));
    TEST_ASSERT_EQUAL_UINT32(1U, state.next_sequence);
}

void test_duplicate_ack_requires_an_exact_local_body_replay(void)
{
    backend_command_envelope_t envelope = decode_ok(GATT_COMMAND);
    backend_command_client_state_t state;
    bind_fresh_result_state(&state, &envelope);
    backend_command_result_t result = begin_result();
    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    static const char duplicate_ack[] =
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":false,"
        "\"duplicate\":true}";
    backend_command_result_ack_t ack;
    TEST_ASSERT_FALSE(backend_command_result_ack_validate(
        &state, duplicate_ack, sizeof(duplicate_ack) - 1U, &ack));
    TEST_ASSERT_TRUE(state.pending);

    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    TEST_ASSERT_TRUE(backend_command_result_ack_validate(
        &state, duplicate_ack, sizeof(duplicate_ack) - 1U, &ack));
}

void test_wrong_or_malformed_ack_never_changes_pending_result(void)
{
    static const char *invalid_acks[] = {
        "{\"ok\":false,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" OTHER_COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":1,\"next_sequence\":2,"
        "\"result_state\":\"queued\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":2,"
        "\"result_state\":\"queued\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"scanning\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"mystery\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":true,\"duplicate\":false}",
        "{\"ok\":true,\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":false,\"duplicate\":false}",
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":0,\"next_sequence\":1,"
        "\"result_state\":\"queued\",\"terminal\":false,"
        "\"duplicate\":false,\"extra\":0}",
        "{not-json",
    };
    backend_command_envelope_t envelope = decode_ok(GATT_COMMAND);
    backend_command_client_state_t state;
    bind_fresh_result_state(&state, &envelope);
    backend_command_result_t result = begin_result();
    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    const backend_command_client_state_t expected = state;

    for (size_t index = 0U;
         index < sizeof(invalid_acks) / sizeof(invalid_acks[0]);
         ++index) {
        backend_command_result_ack_t ack;
        memset(&ack, 0xA5, sizeof(ack));
        TEST_ASSERT_FALSE(backend_command_result_ack_validate(
            &state, invalid_acks[index], strlen(invalid_acks[index]), &ack));
        backend_command_result_ack_t zero = {0};
        TEST_ASSERT_EQUAL_MEMORY(&zero, &ack, sizeof(ack));
        TEST_ASSERT_EQUAL_MEMORY(&expected, &state, sizeof(state));
    }
}

void test_resume_terminal_ack_requires_exact_sequence_state_and_terminal(void)
{
    backend_command_envelope_t envelope = decode_ok(RESUMED_COMMAND);
    backend_command_client_state_t state;
    bind_fresh_result_state(&state, &envelope);
    backend_command_result_t result = resumed_failed_result();
    TEST_ASSERT_TRUE(backend_command_result_prepare(&state, &result));
    static const char ack_json[] =
        "{\"ok\":true,\"command_id\":\"" COMMAND_ID "\","
        "\"accepted_sequence\":37,\"next_sequence\":38,"
        "\"result_state\":\"failed\",\"terminal\":true,"
        "\"duplicate\":false}";
    backend_command_result_ack_t ack;
    TEST_ASSERT_TRUE(backend_command_result_ack_validate(
        &state, ack_json, sizeof(ack_json) - 1U, &ack));
    TEST_ASSERT_TRUE(backend_command_result_ack_commit(&state, &ack));
    TEST_ASSERT_FALSE(state.bound);
    TEST_ASSERT_FALSE(state.pending);
}

void test_command_poll_runs_immediately_then_every_exact_five_seconds(void)
{
    backend_command_http_state_t http;
    TEST_ASSERT_TRUE(backend_command_http_state_init(&http, 1000));
    TEST_ASSERT_TRUE(backend_command_poll_due(&http, 1000));
    TEST_ASSERT_TRUE(backend_command_poll_started(&http, 1000));
    TEST_ASSERT_FALSE(backend_command_poll_due(&http, 5999));
    TEST_ASSERT_TRUE(backend_command_poll_due(&http, 6000));
    TEST_ASSERT_TRUE(backend_command_poll_started(&http, 9000));
    TEST_ASSERT_FALSE(backend_command_poll_due(&http, 13999));
    TEST_ASSERT_TRUE(backend_command_poll_due(&http, 14000));

    TEST_ASSERT_FALSE(backend_command_http_state_init(&http, -1));
    TEST_ASSERT_FALSE(backend_command_poll_due(&http, -1));
    TEST_ASSERT_TRUE(backend_command_http_state_init(&http, INT64_MAX - 1));
    TEST_ASSERT_TRUE(backend_command_poll_started(&http, INT64_MAX - 1));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, http.next_poll_ms);
}

void test_poll_http_policy_distinguishes_idle_body_retry_and_quarantine(void)
{
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_poll_http_action(false, 0));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_IDLE,
        backend_command_poll_http_action(true, 204));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_BODY,
        backend_command_poll_http_action(true, 200));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_QUARANTINE,
        backend_command_poll_http_action(true, 299));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_poll_http_action(true, 408));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_poll_http_action(true, 429));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_poll_http_action(true, 503));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_QUARANTINE,
        backend_command_poll_http_action(true, 404));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_QUARANTINE,
        backend_command_poll_http_action(true, 0));
}

void test_result_http_policy_advances_only_valid_ack_and_tracks_telemetry(void)
{
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_result_http_action(false, 0, false));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_ACK,
        backend_command_result_http_action(true, 200, true));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_QUARANTINE,
        backend_command_result_http_action(true, 200, false));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_QUARANTINE,
        backend_command_result_http_action(true, 422, false));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_result_http_action(true, 408, false));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_result_http_action(true, 429, false));
    TEST_ASSERT_EQUAL(BACKEND_COMMAND_HTTP_RETRY,
        backend_command_result_http_action(true, 500, false));

    backend_command_http_state_t http;
    TEST_ASSERT_TRUE(backend_command_http_state_init(&http, 0));
    backend_command_http_note(
        &http, BACKEND_COMMAND_HTTP_RETRY, 503, true);
    TEST_ASSERT_EQUAL_UINT32(1, http.retryable_errors);
    TEST_ASSERT_EQUAL_UINT32(0, http.quarantined_errors);
    TEST_ASSERT_FALSE(http.result_quarantined);
    backend_command_http_note(
        &http, BACKEND_COMMAND_HTTP_QUARANTINE, 422, true);
    TEST_ASSERT_EQUAL_UINT32(1, http.retryable_errors);
    TEST_ASSERT_EQUAL_UINT32(1, http.quarantined_errors);
    TEST_ASSERT_EQUAL_INT(422, http.last_status_code);
    TEST_ASSERT_TRUE(http.result_quarantined);
    backend_command_http_note(
        &http, BACKEND_COMMAND_HTTP_ACK, 200, true);
    TEST_ASSERT_FALSE(http.result_quarantined);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_exact_gatt_and_passive_envelopes_decode);
    BACKEND_RUN_TEST(test_envelope_requires_exactly_eight_unique_known_fields);
    BACKEND_RUN_TEST(test_envelope_rejects_noncanonical_ids_and_mismatch);
    BACKEND_RUN_TEST(test_mode_target_timeout_sequence_and_state_are_strict);
    BACKEND_RUN_TEST(test_intent_distinguishes_fresh_active_conflict_and_reboot);
    BACKEND_RUN_TEST(
        test_scanner_codec_maps_only_target_to_mac_and_cancel_is_minimal);
    BACKEND_RUN_TEST(test_poll_and_result_paths_are_exact_and_bounded);
    BACKEND_RUN_TEST(
        test_result_post_body_is_stable_and_only_exact_retry_is_eligible);
    BACKEND_RUN_TEST(
        test_exact_correlated_ack_validates_then_commit_advances_once);
    BACKEND_RUN_TEST(test_duplicate_ack_requires_an_exact_local_body_replay);
    BACKEND_RUN_TEST(test_wrong_or_malformed_ack_never_changes_pending_result);
    BACKEND_RUN_TEST(
        test_resume_terminal_ack_requires_exact_sequence_state_and_terminal);
    BACKEND_RUN_TEST(
        test_command_poll_runs_immediately_then_every_exact_five_seconds);
    BACKEND_RUN_TEST(
        test_poll_http_policy_distinguishes_idle_body_retry_and_quarantine);
    BACKEND_RUN_TEST(
        test_result_http_policy_advances_only_valid_ack_and_tracks_telemetry);
    return UNITY_END();
}
