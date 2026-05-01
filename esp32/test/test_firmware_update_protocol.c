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
