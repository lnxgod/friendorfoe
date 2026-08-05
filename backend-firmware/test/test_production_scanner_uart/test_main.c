#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "backend_detection_codec.h"
#include "production_scanner_uart.h"

static void test_accepts_exact_production_combo_identity(void)
{
    production_scanner_message_t message;
    const char *frame =
        "{\"type\":\"scanner_info\","
        "\"ver\":\"0.64.41-badge-radio-fix\","
        "\"board\":\"scanner-s3-combo-fof_badge\","
        "\"chip\":\"esp32s3\",\"caps\":\"ble,wifi\","
        "\"firmware_name\":\"scanner-s3-combo-fof_badge\","
        "\"app_project\":\"fof_badge_scanner\","
        "\"hardware_type\":\"seeed_xiao_esp32s3\","
        "\"hardware_id\":\"e0:72:a1:fc:5d:60\","
        "\"boot_id\":305419896,"
        "\"scan_profile\":\"ble_primary\",\"cmd_rx\":7}";

    TEST_ASSERT_TRUE(production_scanner_uart_decode(
        frame, strlen(frame), &message));
    TEST_ASSERT_EQUAL(PRODUCTION_SCANNER_MESSAGE_INFO, message.kind);
    TEST_ASSERT_TRUE(message.identity_valid);
    TEST_ASSERT_TRUE(message.management_identity_valid);
    TEST_ASSERT_TRUE(message.boot_id_present);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(305419896), message.boot_id);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-fof_badge", message.firmware_name);
    TEST_ASSERT_EQUAL_STRING("fof_badge_scanner", message.app_project);
    TEST_ASSERT_EQUAL_STRING("seeed_xiao_esp32s3", message.hardware_type);
    TEST_ASSERT_EQUAL_STRING("e0:72:a1:fc:5d:60", message.hardware_id);
    TEST_ASSERT_TRUE(message.profile_present);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, message.profile);
    TEST_ASSERT_EQUAL_UINT32(7U, message.command_rx_count);
}

static void test_rejects_near_match_production_combo_identity(void)
{
    TEST_ASSERT_FALSE(production_scanner_identity_valid(
        "scanner-s3-combo-fof_badges", "esp32s3", "ble,wifi",
        "0.64.41-badge-radio-fix"));
    TEST_ASSERT_FALSE(production_scanner_identity_valid(
        "scanner-s3-combo-fof_badge", "esp32", "ble,wifi",
        "0.64.41-badge-radio-fix"));
    TEST_ASSERT_FALSE(production_scanner_identity_valid(
        "scanner-s3-combo-fof_badge", "esp32s3", "ble",
        "0.64.41-badge-radio-fix"));
    TEST_ASSERT_FALSE(production_scanner_identity_valid(
        "scanner-s3-combo-fof_badge", "esp32s3", "trouble,wifiish",
        "0.64.41-badge-radio-fix"));
}

static void test_encodes_native_controls(void)
{
    char output[128];

    TEST_ASSERT_EQUAL_STRING_LEN(
        "{\"type\":\"ready\"}", output,
        production_scanner_encode_ready(output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING_LEN(
        "{\"type\":\"stop\"}", output,
        production_scanner_encode_stop(output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING_LEN(
        "{\"type\":\"scan_profile\",\"scan_profile\":\"wifi_primary\","
        "\"slot_role\":\"wifi_primary\"}",
        output,
        production_scanner_encode_profile(
            BACKEND_SCAN_PROFILE_WIFI_PRIMARY, output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING_LEN(
        "{\"type\":\"time\",\"ms\":1700000000000,"
        "\"ok\":true,\"src\":\"backend\"}",
        output,
        production_scanner_encode_time(
            INT64_C(1700000000000), "backend", output, sizeof(output)));
}

static void test_decodes_slot_role_rejection(void)
{
    production_scanner_message_t message;
    const char *frame =
        "{\"type\":\"scan_profile_ack\","
        "\"scan_profile\":\"hybrid_failover\","
        "\"slot_role\":\"unassigned\",\"slot_role_ok\":false}";

    TEST_ASSERT_TRUE(production_scanner_uart_decode(
        frame, strlen(frame), &message));
    TEST_ASSERT_TRUE(message.slot_role_ok_present);
    TEST_ASSERT_FALSE(message.slot_role_ok);
}

static void test_decodes_extended_production_status(void)
{
    char frame[BACKEND_DETECTION_UART_MAX_LINE + 1U];
    int written = snprintf(
        frame, sizeof(frame),
        "{\"type\":\"status\",\"ver\":\"0.64.41-badge-radio-fix\","
        "\"board\":\"scanner-s3-combo-fof_badge\","
        "\"chip\":\"esp32s3\",\"caps\":\"ble,wifi\","
        "\"scan_profile\":\"ble_primary\",\"uptime_s\":42,"
        "\"ble_initialized\":true,\"ble_scanning\":true");
    TEST_ASSERT_GREATER_THAN(0, written);
    size_t used = (size_t)written;
    for (unsigned index = 0U; index < 132U; ++index) {
        written = snprintf(
            frame + used, sizeof(frame) - used,
            ",\"f%u\":%u", index, index);
        TEST_ASSERT_GREATER_THAN(0, written);
        TEST_ASSERT_LESS_THAN(sizeof(frame) - used, (size_t)written);
        used += (size_t)written;
    }
    TEST_ASSERT_LESS_THAN(sizeof(frame), used + 2U);
    frame[used++] = '}';
    frame[used] = '\0';

    production_scanner_message_t message;
    TEST_ASSERT_TRUE(production_scanner_uart_decode(frame, used, &message));
    TEST_ASSERT_EQUAL(PRODUCTION_SCANNER_MESSAGE_STATUS, message.kind);
    TEST_ASSERT_EQUAL_UINT64(42000U, message.uptime_ms);
    TEST_ASSERT_TRUE(message.ble_initialized_present);
    TEST_ASSERT_TRUE(message.ble_initialized);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_accepts_exact_production_combo_identity);
    RUN_TEST(test_rejects_near_match_production_combo_identity);
    RUN_TEST(test_encodes_native_controls);
    RUN_TEST(test_decodes_slot_role_rejection);
    RUN_TEST(test_decodes_extended_production_status);
    return UNITY_END();
}
