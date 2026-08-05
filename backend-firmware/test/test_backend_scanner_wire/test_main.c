#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_hardware_profile.h"
#include "backend_scanner_control_codec.h"
#include "backend_scanner_status_codec.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void assert_control_roundtrip(const backend_scanner_control_t *input)
{
    char line[4096] = {0};
    size_t length = backend_scanner_control_encode(
        input, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0U, length);
    TEST_ASSERT_LESS_THAN(4096U, length);
    TEST_ASSERT_EQUAL_CHAR('\0', line[length]);

    backend_scanner_control_t output;
    memset(&output, 0xa5, sizeof(output));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_DECODE_OK,
        backend_scanner_control_decode(line, length, &output));
    TEST_ASSERT_EQUAL(input->type, output.type);

    switch (input->type) {
    case BACKEND_SCANNER_CONTROL_ROLE:
        TEST_ASSERT_EQUAL_UINT32(input->payload.role.boot_id,
                                 output.payload.role.boot_id);
        TEST_ASSERT_EQUAL_UINT32(input->payload.role.generation,
                                 output.payload.role.generation);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        TEST_ASSERT_EQUAL_UINT32(input->payload.role.topology_generation,
                                 output.payload.role.topology_generation);
#endif
        TEST_ASSERT_EQUAL(input->payload.role.profile,
                          output.payload.role.profile);
        break;
    case BACKEND_SCANNER_CONTROL_TIME:
        TEST_ASSERT_EQUAL_UINT32(input->payload.time.generation,
                                 output.payload.time.generation);
        TEST_ASSERT_EQUAL(input->payload.time.valid,
                          output.payload.time.valid);
        TEST_ASSERT_EQUAL_INT64(input->payload.time.epoch_ms,
                                output.payload.time.epoch_ms);
        TEST_ASSERT_EQUAL(input->payload.time.source,
                          output.payload.time.source);
        break;
    case BACKEND_SCANNER_CONTROL_FLOW:
        TEST_ASSERT_EQUAL_UINT32(input->payload.flow.generation,
                                 output.payload.flow.generation);
        TEST_ASSERT_EQUAL(input->payload.flow.paused,
                          output.payload.flow.paused);
        break;
    case BACKEND_SCANNER_CONTROL_LED_STATE:
        TEST_ASSERT_EQUAL_STRING(input->payload.led.state,
                                 output.payload.led.state);
        TEST_ASSERT_EQUAL_UINT32(input->payload.led.generation,
                                 output.payload.led.generation);
        TEST_ASSERT_EQUAL_UINT32(input->payload.led.ttl_ms,
                                 output.payload.led.ttl_ms);
        break;
    case BACKEND_SCANNER_CONTROL_HEALTH_REQUEST:
        TEST_ASSERT_EQUAL_UINT32(input->payload.health_request.sequence,
                                 output.payload.health_request.sequence);
        break;
    case BACKEND_SCANNER_CONTROL_RECOVERY:
        TEST_ASSERT_EQUAL_UINT32(input->payload.recovery.boot_id,
                                 output.payload.recovery.boot_id);
        TEST_ASSERT_EQUAL_UINT32(input->payload.recovery.generation,
                                 output.payload.recovery.generation);
        TEST_ASSERT_EQUAL(input->payload.recovery.action,
                          output.payload.recovery.action);
        break;
    case BACKEND_SCANNER_CONTROL_INVESTIGATE:
        TEST_ASSERT_EQUAL_STRING(input->payload.investigate.command_id,
                                 output.payload.investigate.command_id);
        TEST_ASSERT_EQUAL(input->payload.investigate.has_mac,
                          output.payload.investigate.has_mac);
        TEST_ASSERT_EQUAL_STRING(input->payload.investigate.mac,
                                 output.payload.investigate.mac);
        TEST_ASSERT_EQUAL(input->payload.investigate.mode,
                          output.payload.investigate.mode);
        TEST_ASSERT_EQUAL_UINT32(input->payload.investigate.timeout_ms,
                                 output.payload.investigate.timeout_ms);
        break;
    case BACKEND_SCANNER_CONTROL_CANCEL:
        TEST_ASSERT_EQUAL_STRING(input->payload.cancel.command_id,
                                 output.payload.cancel.command_id);
        break;
    case BACKEND_SCANNER_CONTROL_OTA_BEGIN:
        TEST_ASSERT_EQUAL_UINT32(input->payload.ota_begin.session_id,
                                 output.payload.ota_begin.session_id);
        TEST_ASSERT_EQUAL_UINT32(input->payload.ota_begin.generation,
                                 output.payload.ota_begin.generation);
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        TEST_ASSERT_EQUAL_UINT32(
            input->payload.ota_begin.manifest_generation,
            output.payload.ota_begin.manifest_generation);
#endif
        TEST_ASSERT_EQUAL_UINT8(input->payload.ota_begin.component_slot,
                                output.payload.ota_begin.component_slot);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_begin.expected_mac,
                                 output.payload.ota_begin.expected_mac);
        TEST_ASSERT_EQUAL_UINT32(input->payload.ota_begin.expected_boot_id,
                                 output.payload.ota_begin.expected_boot_id);
        TEST_ASSERT_EQUAL_UINT32(
            input->payload.ota_begin.expected_topology_generation,
            output.payload.ota_begin.expected_topology_generation);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_begin.target,
                                 output.payload.ota_begin.target);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_begin.project,
                                 output.payload.ota_begin.project);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_begin.hardware,
                                 output.payload.ota_begin.hardware);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_begin.version,
                                 output.payload.ota_begin.version);
        TEST_ASSERT_EQUAL_UINT32(input->payload.ota_begin.image_size,
                                 output.payload.ota_begin.image_size);
        TEST_ASSERT_EQUAL_HEX32(input->payload.ota_begin.crc32,
                                output.payload.ota_begin.crc32);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_begin.sha256,
                                 output.payload.ota_begin.sha256);
        TEST_ASSERT_EQUAL(input->payload.ota_begin.allow_same_version,
                          output.payload.ota_begin.allow_same_version);
        TEST_ASSERT_EQUAL(input->payload.ota_begin.dry_run,
                          output.payload.ota_begin.dry_run);
        break;
    case BACKEND_SCANNER_CONTROL_OTA_END:
    case BACKEND_SCANNER_CONTROL_OTA_ABORT:
        TEST_ASSERT_EQUAL_UINT32(input->payload.ota_finish.session_id,
                                 output.payload.ota_finish.session_id);
        TEST_ASSERT_EQUAL_UINT32(input->payload.ota_finish.generation,
                                 output.payload.ota_finish.generation);
        TEST_ASSERT_EQUAL_STRING(input->payload.ota_finish.reason,
                                 output.payload.ota_finish.reason);
        break;
    default:
        TEST_FAIL_MESSAGE("unexpected scanner control tag");
    }
}

void test_scanner_control_round_trips_every_union_payload(void)
{
    backend_scanner_control_t role = {
        .type = BACKEND_SCANNER_CONTROL_ROLE,
        .payload.role = {
            .boot_id = 77,
            .generation = 4,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            .topology_generation = 19,
#endif
            .profile = BACKEND_SCAN_PROFILE_BLE_PRIMARY,
        },
    };
    char line[4096];
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &role, line, sizeof(line)));
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"role\",\"boot_id\":77,\"generation\":4,"
        "\"topology_generation\":19,\"profile\":\"ble_primary\"}", line);
#else
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"role\",\"boot_id\":77,\"generation\":4,"
        "\"profile\":\"ble_primary\"}", line);
#endif
    assert_control_roundtrip(&role);

    backend_scanner_control_t time = {
        .type = BACKEND_SCANNER_CONTROL_TIME,
        .payload.time = {
            .generation = 5,
            .valid = true,
            .epoch_ms = INT64_C(1785600000123),
            .source = BACKEND_SCANNER_TIME_SNTP,
        },
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &time, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"time\",\"generation\":5,\"valid\":true,"
        "\"epoch_ms\":1785600000123,\"source\":\"sntp\"}", line);
    assert_control_roundtrip(&time);

    backend_scanner_control_t flow = {
        .type = BACKEND_SCANNER_CONTROL_FLOW,
        .payload.flow = {6, true},
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &flow, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"flow\",\"generation\":6,\"paused\":true}",
        line);
    assert_control_roundtrip(&flow);

    backend_scanner_control_t led = {
        .type = BACKEND_SCANNER_CONTROL_LED_STATE,
        .payload.led = {"drone_alert", 7, 4000},
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &led, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"led_state\",\"state\":\"drone_alert\","
        "\"generation\":7,\"ttl_ms\":4000}", line);
    assert_control_roundtrip(&led);

    backend_scanner_control_t health = {
        .type = BACKEND_SCANNER_CONTROL_HEALTH_REQUEST,
        .payload.health_request = {8},
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &health, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"health_request\",\"sequence\":8}", line);
    assert_control_roundtrip(&health);

    backend_scanner_control_t recovery = {
        .type = BACKEND_SCANNER_CONTROL_RECOVERY,
        .payload.recovery = {77, 9, BACKEND_SCANNER_RECOVERY_RESTART_RADIOS},
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &recovery, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"recovery\",\"boot_id\":77,\"generation\":9,"
        "\"action\":\"restart_radios\"}", line);
    assert_control_roundtrip(&recovery);

    backend_scanner_control_t investigate = {
        .type = BACKEND_SCANNER_CONTROL_INVESTIGATE,
        .payload.investigate = {
            .command_id = "0123456789abcdef0123456789abcdef",
            .has_mac = true,
            .mac = "AA:BB:CC:DD:EE:FF",
            .mode = BACKEND_SCANNER_INVESTIGATE_GATT,
            .timeout_ms = 12000,
        },
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &investigate, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"investigate\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"gatt\","
        "\"timeout_ms\":12000}", line);
    assert_control_roundtrip(&investigate);

    investigate.payload.investigate.has_mac = false;
    investigate.payload.investigate.mac[0] = '\0';
    investigate.payload.investigate.mode =
        BACKEND_SCANNER_INVESTIGATE_PASSIVE_CAPTURE;
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &investigate, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"investigate\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\","
        "\"mac\":null,\"mode\":\"passive_capture\","
        "\"timeout_ms\":12000}", line);
    assert_control_roundtrip(&investigate);

    backend_scanner_control_t cancel = {
        .type = BACKEND_SCANNER_CONTROL_CANCEL,
        .payload.cancel = {
            .command_id = "0123456789abcdef0123456789abcdef",
        },
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &cancel, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"cancel\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\"}", line);
    assert_control_roundtrip(&cancel);

    backend_scanner_control_t ota = {
        .type = BACKEND_SCANNER_CONTROL_OTA_BEGIN,
        .payload.ota_begin = {
            .session_id = 7,
            .generation = 12,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            .manifest_generation = 19,
#endif
            .component_slot = 0,
            .expected_mac = "AA:BB:CC:DD:EE:01",
            .expected_boot_id = UINT32_C(305419896),
            .expected_topology_generation = 4,
            .target = FOF_BACKEND_SCANNER_TARGET,
            .project = FOF_BACKEND_SCANNER_PROJECT,
            .hardware = FOF_BACKEND_HARDWARE,
            .version = "0.1.1-backend",
            .image_size = 1048576,
            .crc32 = UINT32_C(305419896),
            .sha256 = "0123456789abcdef0123456789abcdef"
                      "0123456789abcdef0123456789abcdef",
            .allow_same_version = false,
            .dry_run = false,
        },
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &ota, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ota_begin\",\"session_id\":7,"
        "\"generation\":12,"
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
        "\"manifest_generation\":19,"
#endif
        "\"component_slot\":0,"
        "\"expected_mac\":\"AA:BB:CC:DD:EE:01\","
        "\"expected_boot_id\":305419896,"
        "\"expected_topology_generation\":4,"
        "\"target\":\"" FOF_BACKEND_SCANNER_TARGET "\","
        "\"project\":\"" FOF_BACKEND_SCANNER_PROJECT "\","
        "\"hardware\":\"" FOF_BACKEND_HARDWARE "\","
        "\"version\":\"0.1.1-backend\",\"image_size\":1048576,"
        "\"crc32\":305419896,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef\","
        "\"allow_same_version\":false,\"dry_run\":false}", line);
    assert_control_roundtrip(&ota);

    backend_scanner_control_t ota_end = {
        .type = BACKEND_SCANNER_CONTROL_OTA_END,
        .payload.ota_finish = {7, 13, "verified"},
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &ota_end, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ota_end\",\"session_id\":7,"
        "\"generation\":13,\"reason\":\"verified\"}", line);
    assert_control_roundtrip(&ota_end);
    backend_scanner_control_t ota_abort = {
        .type = BACKEND_SCANNER_CONTROL_OTA_ABORT,
        .payload.ota_finish = {7, 14, "operator_cancel"},
    };
    TEST_ASSERT_GREATER_THAN(0U, backend_scanner_control_encode(
        &ota_abort, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ota_abort\",\"session_id\":7,"
        "\"generation\":14,\"reason\":\"operator_cancel\"}", line);
    assert_control_roundtrip(&ota_abort);
}

static backend_scanner_status_t fixture_status(void)
{
    backend_scanner_status_t status = {
        .schema = 1,
        .sequence = 12,
        .boot_id = 77,
        .mac = "AA:BB:CC:DD:EE:FF",
        .target = FOF_BACKEND_SCANNER_TARGET,
        .project = FOF_BACKEND_SCANNER_PROJECT,
        .hardware = FOF_BACKEND_HARDWARE,
        .version = "0.1.0-backend",
        .profile = BACKEND_SCAN_PROFILE_BLE_PRIMARY,
        .role_generation = 4,
        .role_acked = true,
        .command_ingress = true,
        .ble_healthy = true,
        .wifi_healthy = false,
        .flow_paused = false,
        .ota_state = "idle",
        .rollback_state = "valid",
        .rx_errors = 0,
        .tx_drops = 0,
        .uptime_ms = 9000,
    };
    return status;
}

static void assert_status_equal(const backend_scanner_status_t *expected,
                                const backend_scanner_status_t *actual)
{
    TEST_ASSERT_EQUAL_UINT8(expected->schema, actual->schema);
    TEST_ASSERT_EQUAL_UINT32(expected->sequence, actual->sequence);
    TEST_ASSERT_EQUAL_UINT32(expected->boot_id, actual->boot_id);
    TEST_ASSERT_EQUAL_STRING(expected->mac, actual->mac);
    TEST_ASSERT_EQUAL_STRING(expected->target, actual->target);
    TEST_ASSERT_EQUAL_STRING(expected->project, actual->project);
    TEST_ASSERT_EQUAL_STRING(expected->hardware, actual->hardware);
    TEST_ASSERT_EQUAL_STRING(expected->version, actual->version);
    TEST_ASSERT_EQUAL(expected->profile, actual->profile);
    TEST_ASSERT_EQUAL_UINT32(expected->role_generation,
                             actual->role_generation);
    TEST_ASSERT_EQUAL(expected->role_acked, actual->role_acked);
    TEST_ASSERT_EQUAL(expected->command_ingress, actual->command_ingress);
    TEST_ASSERT_EQUAL(expected->ble_healthy, actual->ble_healthy);
    TEST_ASSERT_EQUAL(expected->wifi_healthy, actual->wifi_healthy);
    TEST_ASSERT_EQUAL(expected->flow_paused, actual->flow_paused);
    TEST_ASSERT_EQUAL_STRING(expected->ota_state, actual->ota_state);
    TEST_ASSERT_EQUAL_STRING(expected->rollback_state,
                             actual->rollback_state);
    TEST_ASSERT_EQUAL_UINT32(expected->rx_errors, actual->rx_errors);
    TEST_ASSERT_EQUAL_UINT32(expected->tx_drops, actual->tx_drops);
    TEST_ASSERT_EQUAL_UINT64(expected->uptime_ms, actual->uptime_ms);
}

void test_scanner_status_round_trips_complete_exact_record(void)
{
    backend_scanner_status_t input = fixture_status();
    char line[4096] = {0};
    size_t length = backend_scanner_status_encode(
        &input, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0U, length);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"scanner_status\",\"schema\":1,"
        "\"sequence\":12,\"boot_id\":77,"
        "\"mac\":\"AA:BB:CC:DD:EE:FF\","
        "\"target\":\"" FOF_BACKEND_SCANNER_TARGET "\","
        "\"project\":\"" FOF_BACKEND_SCANNER_PROJECT "\","
        "\"hardware\":\"" FOF_BACKEND_HARDWARE "\","
        "\"version\":\"0.1.0-backend\","
        "\"profile\":\"ble_primary\",\"role_generation\":4,"
        "\"role_acked\":true,\"command_ingress\":true,"
        "\"ble_healthy\":true,\"wifi_healthy\":false,"
        "\"flow_paused\":false,\"ota_state\":\"idle\","
        "\"rollback_state\":\"valid\",\"rx_errors\":0,"
        "\"tx_drops\":0,\"uptime_ms\":9000}", line);

    backend_scanner_status_t output = {0};
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_DECODE_OK,
        backend_scanner_status_decode(line, length, &output));
    assert_status_equal(&input, &output);
}

void test_scanner_wire_rejects_unknown_duplicate_missing_and_bounds(void)
{
    static const char unknown_control[] = "{\"type\":\"surprise\"}";
    static const char duplicate_control[] =
        "{\"type\":\"health_request\",\"sequence\":8,\"sequence\":9}";
    static const char missing_control[] =
        "{\"type\":\"role\",\"boot_id\":77,\"generation\":4}";
    static const char zero_control[] =
        "{\"type\":\"health_request\",\"sequence\":0}";
    static const char extra_cancel_field[] =
        "{\"type\":\"cancel\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef\","
        "\"target\":\"AA:BB:CC:DD:EE:FF\"}";
    backend_scanner_control_t control;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(unknown_control,
            sizeof(unknown_control) - 1, &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_MALFORMED,
        backend_scanner_control_decode(duplicate_control,
            sizeof(duplicate_control) - 1, &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(missing_control,
            sizeof(missing_control) - 1, &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(zero_control,
            sizeof(zero_control) - 1, &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(extra_cancel_field,
            sizeof(extra_cancel_field) - 1, &control));

    backend_scanner_status_t status = fixture_status();
    char line[4096];
    size_t length = backend_scanner_status_encode(
        &status, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0U, length);
    memset(line + length, ' ', 4095U - length);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_DECODE_OK,
        backend_scanner_status_decode(line, 4095U, &status));

    length = backend_scanner_status_encode(&status, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0U, length);
    TEST_ASSERT_EQUAL_CHAR('}', line[length - 1U]);
    static const char aggregate[] = ",\"radio_healthy\":true}";
    memcpy(line + length - 1U, aggregate, sizeof(aggregate));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH,
        backend_scanner_status_decode(
            line, length - 1U + sizeof(aggregate) - 1U, &status));

    line[4095] = 'X';
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_TOO_LARGE,
        backend_scanner_status_decode(line, 4096U, &status));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_TOO_LARGE,
        backend_scanner_control_decode(line, 4096U, &control));

    static const char zero_status[] =
        "{\"type\":\"scanner_status\",\"schema\":1,\"sequence\":0,"
        "\"boot_id\":77}";
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH,
        backend_scanner_status_decode(zero_status,
            sizeof(zero_status) - 1, &status));
}

void test_scanner_status_rejects_wrong_identity_and_overlong_fields(void)
{
    backend_scanner_status_t status = fixture_status();
    strcpy(status.target, "scanner-s3-combo-fof_badge");
    char line[4096] = {0};
    TEST_ASSERT_EQUAL_UINT(0U, backend_scanner_status_encode(
        &status, line, sizeof(line)));
    TEST_ASSERT_EQUAL_CHAR('\0', line[0]);

    static const char overlong_mac[] =
        "{\"type\":\"scanner_status\",\"schema\":1,\"sequence\":1,"
        "\"boot_id\":1,\"mac\":\"AA:BB:CC:DD:EE:FFX\","
        "\"target\":\"" FOF_BACKEND_SCANNER_TARGET "\","
        "\"project\":\"" FOF_BACKEND_SCANNER_PROJECT "\","
        "\"hardware\":\"" FOF_BACKEND_HARDWARE "\","
        "\"version\":\"0.1.0-backend\",\"profile\":\"quiescent\","
        "\"role_generation\":0,\"role_acked\":false,"
        "\"command_ingress\":true,\"ble_healthy\":false,"
        "\"wifi_healthy\":false,\"flow_paused\":false,"
        "\"ota_state\":\"idle\",\"rollback_state\":\"valid\","
        "\"rx_errors\":0,\"tx_drops\":0,\"uptime_ms\":1}";
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH,
        backend_scanner_status_decode(overlong_mac,
            sizeof(overlong_mac) - 1, &status));
}

void test_control_decoder_rejects_string_enum_time_and_identity_boundaries(void)
{
    static const char overlong_command_id[] =
        "{\"type\":\"cancel\","
        "\"command_id\":\"0123456789abcdef0123456789abcdef0\"}";
    static const char unknown_profile[] =
        "{\"type\":\"role\",\"boot_id\":77,\"generation\":4,"
        "\"profile\":\"bluetoothish\"}";
    static const char threshold_time[] =
        "{\"type\":\"time\",\"generation\":5,\"valid\":true,"
        "\"epoch_ms\":1700000000000,\"source\":\"sntp\"}";
    static const char invalid_time_source[] =
        "{\"type\":\"time\",\"generation\":5,\"valid\":true,"
        "\"epoch_ms\":1700000000001,\"source\":\"none\"}";
    backend_scanner_control_t control;

    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(
            overlong_command_id, sizeof(overlong_command_id) - 1U,
            &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(
            unknown_profile, sizeof(unknown_profile) - 1U, &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(
            threshold_time, sizeof(threshold_time) - 1U, &control));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(
            invalid_time_source, sizeof(invalid_time_source) - 1U,
            &control));

    backend_scanner_control_t time = {
        .type = BACKEND_SCANNER_CONTROL_TIME,
        .payload.time = {
            .generation = 5U,
            .valid = true,
            .epoch_ms = INT64_C(1700000000000),
            .source = BACKEND_SCANNER_TIME_SNTP,
        },
    };
    char line[4096] = {0};
    TEST_ASSERT_EQUAL_UINT(0U, backend_scanner_control_encode(
        &time, line, sizeof(line)));

    backend_scanner_control_t ota = {
        .type = BACKEND_SCANNER_CONTROL_OTA_BEGIN,
        .payload.ota_begin = {
            .session_id = 7U,
            .generation = 12U,
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
            .manifest_generation = 19U,
#endif
            .component_slot = 0U,
            .expected_mac = "AA:BB:CC:DD:EE:01",
            .expected_boot_id = 77U,
            .expected_topology_generation = 4U,
            .target = FOF_BACKEND_SCANNER_TARGET,
            .project = FOF_BACKEND_SCANNER_PROJECT,
            .hardware = FOF_BACKEND_HARDWARE,
            .version = "0.1.1-backend",
            .image_size = 1048576U,
            .crc32 = UINT32_C(305419896),
            .sha256 = "0123456789abcdef0123456789abcdef"
                      "0123456789abcdef0123456789abcdef",
        },
    };
    size_t length = backend_scanner_control_encode(
        &ota, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0U, length);
    char *target = strstr(line, FOF_BACKEND_SCANNER_TARGET);
    TEST_ASSERT_NOT_NULL(target);
    target[0] = 'X';
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_CONTROL_SCHEMA_MISMATCH,
        backend_scanner_control_decode(line, length, &control));
}

void test_status_decoder_rejects_unknown_duplicate_missing_and_wrong_identity(void)
{
    backend_scanner_status_t status = fixture_status();
    char line[4096] = {0};
    size_t length = backend_scanner_status_encode(
        &status, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0U, length);

    char duplicate[4096] = {0};
    memcpy(duplicate, line, length - 1U);
    static const char duplicate_suffix[] = ",\"sequence\":13}";
    memcpy(duplicate + length - 1U,
           duplicate_suffix, sizeof(duplicate_suffix));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_MALFORMED,
        backend_scanner_status_decode(
            duplicate,
            length - 1U + sizeof(duplicate_suffix) - 1U,
            &status));

    char missing[4096] = {0};
    memcpy(missing, line, length + 1U);
    static const char missing_field[] = ",\"rollback_state\":\"valid\"";
    char *field = strstr(missing, missing_field);
    TEST_ASSERT_NOT_NULL(field);
    const size_t offset = (size_t)(field - missing);
    memmove(field, field + sizeof(missing_field) - 1U,
            length - offset - (sizeof(missing_field) - 1U) + 1U);
    const size_t missing_length = length - (sizeof(missing_field) - 1U);
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH,
        backend_scanner_status_decode(missing, missing_length, &status));

    char wrong_identity[4096] = {0};
    memcpy(wrong_identity, line, length + 1U);
    char *target = strstr(wrong_identity, FOF_BACKEND_SCANNER_TARGET);
    TEST_ASSERT_NOT_NULL(target);
    target[0] = 'X';
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_SCHEMA_MISMATCH,
        backend_scanner_status_decode(
            wrong_identity, length, &status));
}

void test_scanner_status_sequence_acceptance_is_boot_scoped(void)
{
    backend_scanner_status_tracker_t tracker;
    backend_scanner_status_tracker_init(&tracker);
    backend_scanner_status_t status = fixture_status();

    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_ACCEPTED,
        backend_scanner_status_tracker_accept(&tracker, &status));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_REFRESHED,
        backend_scanner_status_tracker_accept(&tracker, &status));

    backend_scanner_status_t conflict = status;
    conflict.wifi_healthy = true;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_CONFLICT,
        backend_scanner_status_tracker_accept(&tracker, &conflict));

    backend_scanner_status_t stale = status;
    stale.sequence = 11;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_STALE,
        backend_scanner_status_tracker_accept(&tracker, &stale));

    backend_scanner_status_t newer = status;
    newer.sequence = 13;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_ACCEPTED,
        backend_scanner_status_tracker_accept(&tracker, &newer));

    backend_scanner_status_t rebooted = status;
    rebooted.boot_id = 88;
    rebooted.sequence = 1;
    rebooted.role_generation = 0;
    rebooted.role_acked = false;
    rebooted.profile = BACKEND_SCAN_PROFILE_QUIESCENT;
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_STATUS_CHANGED_BOOT,
        backend_scanner_status_tracker_accept(&tracker, &rebooted));
    TEST_ASSERT_EQUAL_UINT32(88, tracker.boot_id);
    TEST_ASSERT_EQUAL_UINT32(1, tracker.sequence);
    TEST_ASSERT_FALSE(tracker.status.role_acked);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT,
                      tracker.status.profile);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_scanner_control_round_trips_every_union_payload);
    BACKEND_RUN_TEST(test_scanner_status_round_trips_complete_exact_record);
    BACKEND_RUN_TEST(
        test_scanner_wire_rejects_unknown_duplicate_missing_and_bounds);
    BACKEND_RUN_TEST(
        test_scanner_status_rejects_wrong_identity_and_overlong_fields);
    BACKEND_RUN_TEST(
        test_control_decoder_rejects_string_enum_time_and_identity_boundaries);
    BACKEND_RUN_TEST(
        test_status_decoder_rejects_unknown_duplicate_missing_and_wrong_identity);
    BACKEND_RUN_TEST(test_scanner_status_sequence_acceptance_is_boot_scoped);
    return UNITY_END();
}
