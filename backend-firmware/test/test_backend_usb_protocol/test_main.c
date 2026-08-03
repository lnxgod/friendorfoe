#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_hardware_profile.h"
#include "backend_identity.h"
#include "backend_usb_protocol.h"
#include "../support/backend_test_main.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static backend_usb_command_t parse_ok(const char *line)
{
    backend_usb_command_t command;
    memset(&command, 0xA5, sizeof(command));
    TEST_ASSERT_TRUE(backend_usb_protocol_parse_line(
        line, strlen(line), &command));
    return command;
}

static void assert_invalid(const char *line)
{
    backend_usb_command_t command;
    memset(&command, 0xA5, sizeof(command));
    TEST_ASSERT_FALSE(backend_usb_protocol_parse_line(
        line, strlen(line), &command));
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_INVALID, command.kind);
}

void test_exact_basic_commands_and_unknowns_are_distinguished(void)
{
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_PING,
                      parse_ok("FOF_PING").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_STATUS,
                      parse_ok("FOF_STATUS").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_CONFIG_GET,
                      parse_ok("FOF_CONFIG_GET").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_SAVE,
                      parse_ok("FOF_SAVE").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_BACKEND_STATUS,
                      parse_ok("FOF_BACKEND_STATUS").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_AP_START,
                      parse_ok("FOF_AP_START").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_UNKNOWN,
                      parse_ok("FOF_PING_EXTRA").kind);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_UNKNOWN,
                      parse_ok("FOF_STATUS ").kind);
}

void test_command_limit_accepts_2047_and_rejects_2048_bytes(void)
{
    char line[BACKEND_USB_COMMAND_MAX + 2U];
    memset(line, 'X', sizeof(line));

    backend_usb_command_t command;
    TEST_ASSERT_TRUE(backend_usb_protocol_parse_line(
        line, BACKEND_USB_COMMAND_MAX, &command));
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_UNKNOWN, command.kind);

    command.kind = BACKEND_USB_COMMAND_PING;
    TEST_ASSERT_FALSE(backend_usb_protocol_parse_line(
        line, BACKEND_USB_COMMAND_MAX + 1U, &command));
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_INVALID, command.kind);
}

void test_live_start_requires_exact_client_protocol_and_members(void)
{
    backend_usb_command_t command = parse_ok(
        "FOF_LIVE_START:{\"client\":\"new_dash\",\"protocol\":1}");
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_LIVE_START, command.kind);
    TEST_ASSERT_NOT_NULL(command.json);
    TEST_ASSERT_EQUAL_UINT32(
        strlen("{\"client\":\"new_dash\",\"protocol\":1}"),
        command.json_length);

    assert_invalid("FOF_LIVE_START:{\"client\":\"old_dash\",\"protocol\":1}");
    assert_invalid("FOF_LIVE_START:{\"client\":\"new_dash\",\"protocol\":2}");
    assert_invalid("FOF_LIVE_START:{\"client\":\"new_dash\"}");
    assert_invalid("FOF_LIVE_START:{\"protocol\":1}");
    assert_invalid(
        "FOF_LIVE_START:{\"client\":\"new_dash\",\"protocol\":1,\"extra\":true}");
    assert_invalid(
        "FOF_LIVE_START:{\"client\":\"new_dash\",\"client\":\"new_dash\",\"protocol\":1}");
    assert_invalid(
        "FOF_LIVE_START:{\"client\":\"new_dash\",\"protocol\":-1}");
    assert_invalid(
        "FOF_LIVE_START:{\"client\":\"new_dash\",\"protocol\":18446744073709551616}");
}

void test_live_ack_requires_exact_session_and_unsigned_sequence(void)
{
    backend_usb_command_t command = parse_ok(
        "FOF_LIVE_ACK:{\"session_id\":\"boot-a1\",\"sequence\":42}");
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_LIVE_ACK, command.kind);
    TEST_ASSERT_EQUAL_STRING("boot-a1", command.session_id);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(42), command.sequence);

    command = parse_ok(
        "FOF_LIVE_ACK:{\"sequence\":18446744073709551615,\"session_id\":\"s\"}");
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, command.sequence);
    TEST_ASSERT_EQUAL_STRING("s", command.session_id);

    assert_invalid("FOF_LIVE_ACK:{\"session_id\":\"s\"}");
    assert_invalid("FOF_LIVE_ACK:{\"sequence\":1}");
    assert_invalid(
        "FOF_LIVE_ACK:{\"session_id\":\"s\",\"sequence\":1,\"extra\":0}");
    assert_invalid(
        "FOF_LIVE_ACK:{\"session_id\":\"s\",\"session_id\":\"s\",\"sequence\":1}");
    assert_invalid(
        "FOF_LIVE_ACK:{\"session_id\":\"s\",\"sequence\":-1}");
    assert_invalid(
        "FOF_LIVE_ACK:{\"session_id\":\"s\",\"sequence\":18446744073709551616}");
    assert_invalid(
        "FOF_LIVE_ACK:{\"session_id\":\"123456789012345678901234567890123\",\"sequence\":1}");
}

void test_live_stop_requires_one_nonempty_session_member(void)
{
    backend_usb_command_t command = parse_ok(
        "FOF_LIVE_STOP:{\"session_id\":\"boot-a1\"}");
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_LIVE_STOP, command.kind);
    TEST_ASSERT_EQUAL_STRING("boot-a1", command.session_id);

    assert_invalid("FOF_LIVE_STOP:{}");
    assert_invalid("FOF_LIVE_STOP:{\"session_id\":\"\"}");
    assert_invalid(
        "FOF_LIVE_STOP:{\"session_id\":\"boot-a1\",\"extra\":true}");
    assert_invalid(
        "FOF_LIVE_STOP:{\"session_id\":\"a\",\"session_id\":\"b\"}");
}

void test_compatibility_set_accepts_only_supported_keys_and_preserves_equals(void)
{
    static const char *const keys[] = {
        "wifi_ssid", "wifi_pass", "backend_url", "device_id", "ap_pass",
    };
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        char line[128];
        (void)snprintf(line, sizeof(line), "FOF_SET:%s=value=a=b", keys[index]);
        backend_usb_command_t command = parse_ok(line);
        TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_SET, command.kind);
        TEST_ASSERT_EQUAL_STRING(keys[index], command.key);
        TEST_ASSERT_EQUAL_STRING("value=a=b", command.value);
    }

    assert_invalid("FOF_SET:display_name=porch");
    assert_invalid("FOF_SET:wifi_ssid");
    assert_invalid("FOF_SET:=value");
    backend_usb_command_t empty_password = parse_ok("FOF_SET:wifi_pass=");
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_SET, empty_password.kind);
    TEST_ASSERT_EQUAL_STRING("", empty_password.value);
}

void test_set_rejects_physical_or_escaped_line_breaks_in_values(void)
{
    static const char carriage_return[] = "FOF_SET:wifi_ssid=bad\rvalue";
    static const char line_feed[] = "FOF_SET:wifi_ssid=bad\nvalue";
    assert_invalid(carriage_return);
    assert_invalid(line_feed);
}

void test_config_set_exposes_bounded_json_only_for_current_dispatch(void)
{
    static const char line[] =
        "FOF_CONFIG_SET:{\"backend_url\":\"http://10.0.0.2:8000\"}";
    backend_usb_command_t command = parse_ok(line);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_CONFIG_SET, command.kind);
    TEST_ASSERT_EQUAL_STRING(
        "{\"backend_url\":\"http://10.0.0.2:8000\"}", command.json);
    TEST_ASSERT_EQUAL_UINT32(strlen(command.json), command.json_length);

    assert_invalid("FOF_CONFIG_SET:");
    assert_invalid("FOF_CONFIG_SET:not-json");
}

void test_config_set_rejects_decoded_controls_in_every_json_string(void)
{
    static const char *const invalid[] = {
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\bvalue\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\fvalue\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\nvalue\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\rvalue\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\tvalue\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\u0000value\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\u0001value\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\u001fvalue\"}",
        "FOF_CONFIG_SET:{\"display_name\":\"bad\\u007fvalue\"}",
        "FOF_CONFIG_SET:{\"networks\":[{\"ssid\":\"bad\\nssid\"}]}",
        "FOF_CONFIG_SET:{\"bad\\u007fkey\":\"value\"}",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        assert_invalid(invalid[index]);
    }
}

void test_config_set_allows_escaped_punctuation_and_non_control_unicode(void)
{
    static const char line[] =
        "FOF_CONFIG_SET:{\"display_name\":"
        "\"quote \\\" slash \\\\ caf\xc3\xa9 \\u263a\"}";
    backend_usb_command_t command = parse_ok(line);
    TEST_ASSERT_EQUAL(BACKEND_USB_COMMAND_CONFIG_SET, command.kind);
    TEST_ASSERT_EQUAL_UINT32(sizeof(line) - 1U, command.json_length +
        strlen("FOF_CONFIG_SET:"));
}

void test_ready_and_pong_are_complete_truthful_lite_frames(void)
{
    char output[256];
    TEST_ASSERT_EQUAL_UINT32(
        strlen("FOF_READY\n"),
        backend_usb_protocol_encode_ready(output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("FOF_READY\n", output);

    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("badge_lite", identity->product_family);
    TEST_ASSERT_EQUAL_STRING("uplink-s3-backend", identity->target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_uplink", identity->project);
    TEST_ASSERT_EQUAL_STRING("seeed_xiao_esp32s3", identity->hardware);
    TEST_ASSERT_EQUAL_UINT32(
        strlen("FOF_PONG:0.2.0-backend\n"),
        backend_usb_protocol_encode_pong(identity, output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("FOF_PONG:0.2.0-backend\n", output);
    TEST_ASSERT_NULL(strstr(output, "fof_badge"));
}

void test_live_frame_encoders_emit_exact_newline_delimited_json(void)
{
    char output[256];
    TEST_ASSERT_EQUAL_UINT32(
        strlen("FOF_LIVE_READY:{\"session_id\":\"boot-a1\",\"heartbeat_ms\":5000,\"lease_ms\":15000}\n"),
        backend_usb_protocol_encode_live_ready(
            "boot-a1", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_LIVE_READY:{\"session_id\":\"boot-a1\",\"heartbeat_ms\":5000,\"lease_ms\":15000}\n",
        output);

    TEST_ASSERT_EQUAL_UINT32(
        strlen("FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":7}\n"),
        backend_usb_protocol_encode_live_heartbeat(
            "boot-a1", UINT64_C(7), output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_LIVE_HEARTBEAT:{\"session_id\":\"boot-a1\",\"sequence\":7}\n",
        output);
}

void test_investigation_wrapper_enforces_json_and_det_frame_bound(void)
{
    char output[BACKEND_USB_DET_MAX + 1U];
    static const char json[] = "{\"type\":\"ble_inv_end\",\"summary\":\"done\"}";
    TEST_ASSERT_EQUAL_UINT32(
        strlen("FOF_INV:") + strlen(json) + 1U,
        backend_usb_protocol_encode_investigation(
            json, strlen(json), output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_INV:{\"type\":\"ble_inv_end\",\"summary\":\"done\"}\n",
        output);

    char large_json[BACKEND_USB_DET_MAX];
    large_json[0] = '{';
    memset(large_json + 1, ' ', sizeof(large_json) - 3U);
    large_json[sizeof(large_json) - 2U] = '}';
    large_json[sizeof(large_json) - 1U] = '\0';
    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_usb_protocol_encode_investigation(
               large_json, strlen(large_json), output, sizeof(output)));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
}

void test_investigation_wrapper_rejects_physical_controls_and_clears_output(void)
{
    static const char pretty_lf[] =
        "{\n  \"type\": \"ble_inv_end\"\n}";
    static const char pretty_crlf[] =
        "{\r\n  \"type\": \"ble_inv_end\"\r\n}";
    static const char trailing_tab[] =
        "{\"type\":\"ble_inv_end\"}\t";
    static const char raw_del[] =
        "{\"summary\":\"bad\x7fvalue\"}";
    static const struct {
        const char *json;
        size_t length;
    } invalid[] = {
        {pretty_lf, sizeof(pretty_lf) - 1U},
        {pretty_crlf, sizeof(pretty_crlf) - 1U},
        {trailing_tab, sizeof(trailing_tab) - 1U},
        {raw_del, sizeof(raw_del) - 1U},
    };
    char output[256];
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        memset(output, 'X', sizeof(output));
        TEST_ASSERT_EQUAL_UINT32(
            0, backend_usb_protocol_encode_investigation(
                   invalid[index].json, invalid[index].length,
                   output, sizeof(output)));
        TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
    }

    static const char escaped_newline[] =
        "{\"summary\":\"still\\none physical line\"}";
    TEST_ASSERT_GREATER_THAN_UINT32(
        0, backend_usb_protocol_encode_investigation(
               escaped_newline, sizeof(escaped_newline) - 1U,
               output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_INV:{\"summary\":\"still\\none physical line\"}\n",
        output);
}

void test_all_bounded_encoders_fail_closed_without_partial_frames(void)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    char output[24];

    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_usb_protocol_encode_ready(output, 4));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);

    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_usb_protocol_encode_pong(identity, output, 16));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);

    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_usb_protocol_encode_live_ready(
               "boot-a1", output, sizeof(output)));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);

    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_usb_protocol_encode_live_heartbeat(
               "boot-a1", 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);

    memset(output, 'X', sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_usb_protocol_encode_investigation(
               "{}", 2, output, 4));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_exact_basic_commands_and_unknowns_are_distinguished);
    BACKEND_RUN_TEST(test_command_limit_accepts_2047_and_rejects_2048_bytes);
    BACKEND_RUN_TEST(test_live_start_requires_exact_client_protocol_and_members);
    BACKEND_RUN_TEST(test_live_ack_requires_exact_session_and_unsigned_sequence);
    BACKEND_RUN_TEST(test_live_stop_requires_one_nonempty_session_member);
    BACKEND_RUN_TEST(test_compatibility_set_accepts_only_supported_keys_and_preserves_equals);
    BACKEND_RUN_TEST(test_set_rejects_physical_or_escaped_line_breaks_in_values);
    BACKEND_RUN_TEST(test_config_set_exposes_bounded_json_only_for_current_dispatch);
    BACKEND_RUN_TEST(test_config_set_rejects_decoded_controls_in_every_json_string);
    BACKEND_RUN_TEST(test_config_set_allows_escaped_punctuation_and_non_control_unicode);
    BACKEND_RUN_TEST(test_ready_and_pong_are_complete_truthful_lite_frames);
    BACKEND_RUN_TEST(test_live_frame_encoders_emit_exact_newline_delimited_json);
    BACKEND_RUN_TEST(test_investigation_wrapper_enforces_json_and_det_frame_bound);
    BACKEND_RUN_TEST(test_investigation_wrapper_rejects_physical_controls_and_clears_output);
    BACKEND_RUN_TEST(test_all_bounded_encoders_fail_closed_without_partial_frames);
    return UNITY_END();
}
