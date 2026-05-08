#include "unity.h"
#include "uart_protocol.h"

#include <string.h>

void test_firmware_update_protocol_message_names_are_stable(void)
{
    TEST_ASSERT_EQUAL_STRING("fw_check", MSG_TYPE_FW_CHECK);
    TEST_ASSERT_EQUAL_STRING("fw_offer", MSG_TYPE_FW_OFFER);
    TEST_ASSERT_EQUAL_STRING("fw_ready", MSG_TYPE_FW_READY);
    TEST_ASSERT_EQUAL_STRING("fw_check_now", MSG_TYPE_FW_CHECK_NOW);
}

void test_firmware_update_protocol_carries_crc_and_target_metadata(void)
{
    TEST_ASSERT_EQUAL_STRING("target_ver", JSON_KEY_FW_TARGET_VERSION);
    TEST_ASSERT_EQUAL_STRING("fw_name", JSON_KEY_FW_NAME);
    TEST_ASSERT_EQUAL_STRING("size", JSON_KEY_FW_SIZE);
    TEST_ASSERT_EQUAL_STRING("crc", JSON_KEY_FW_CRC32);
    TEST_ASSERT_EQUAL_STRING("fw_state", JSON_KEY_FW_STATE);
}

void test_ota_protocol_carries_session_id_for_exact_ack_matching(void)
{
    TEST_ASSERT_EQUAL_STRING("session_id", JSON_KEY_OTA_SESSION_ID);
    TEST_ASSERT_EQUAL_STRING(JSON_KEY_SESSION_ID, JSON_KEY_OTA_SESSION_ID);
}

void test_badge_uart_ota_uses_fast_bounded_chunks(void)
{
    TEST_ASSERT_EQUAL_UINT16(512, OTA_CHUNK_DEFAULT_MAX_DATA);
    TEST_ASSERT_EQUAL_UINT16(1024, OTA_CHUNK_BADGE_MAX_DATA);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(5, OTA_RELAY_BADGE_NACK_DRAIN_MS);
    TEST_ASSERT_EQUAL_UINT32(32 * 1024, OTA_RELAY_PROGRESS_INTERVAL_BYTES);
}

void test_badge_relay_timeout_policy_is_bounded(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(180000, OTA_RELAY_TIMEOUT_MIN_MS);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(240000,
        OTA_RELAY_TIMEOUT_FOR_SIZE_MS(1200000UL));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(900000,
        OTA_RELAY_TIMEOUT_FOR_SIZE_MS(1200000UL));
}
