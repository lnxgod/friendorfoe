#include "unity.h"
#include "wifi_probe_capture.h"

void test_probe_ssid_distinguishes_wildcard_named_binary_and_malformed(void)
{
    uint8_t frame[80] = {0x40};
    frame[10] = 2;
    char ssid[33];
    bool wildcard, binary;
    TEST_ASSERT_TRUE(fof_probe_ssid(frame, 26, ssid, &wildcard, &binary));
    TEST_ASSERT_TRUE(wildcard);
    frame[25] = 5;
    memcpy(frame + 26, "A,B C", 5);
    TEST_ASSERT_TRUE(fof_probe_ssid(frame, 31, ssid, &wildcard, &binary));
    TEST_ASSERT_EQUAL_STRING("A,B C", ssid);
    TEST_ASSERT_FALSE(wildcard);
    frame[28] = 0xff;
    TEST_ASSERT_TRUE(fof_probe_ssid(frame, 31, ssid, &wildcard, &binary));
    TEST_ASSERT_TRUE(binary);
    frame[28] = 0;
    TEST_ASSERT_TRUE(fof_probe_ssid(frame, 31, ssid, &wildcard, &binary));
    TEST_ASSERT_TRUE(binary);
    TEST_ASSERT_FALSE(wildcard);
    TEST_ASSERT_EQUAL_STRING("", ssid);
    TEST_ASSERT_FALSE(fof_probe_ssid(frame, 30, ssid, &wildcard, &binary));
    TEST_ASSERT_FALSE(fof_probe_ssid(frame, 33, ssid, &wildcard, &binary)); /* Duplicate SSID */
    frame[25] = 33;
    TEST_ASSERT_FALSE(fof_probe_ssid(frame, 59, ssid, &wildcard, &binary));
    frame[24] = 1;
    frame[25] = 0;
    TEST_ASSERT_FALSE(fof_probe_ssid(frame, 26, ssid, &wildcard, &binary)); /* Missing SSID */
    frame[0] = 0x50;
    TEST_ASSERT_FALSE(fof_probe_ssid(frame, 26, ssid, &wildcard, &binary)); /* Response */
}

void test_probe_budget_bounds_dense_scan_traffic(void)
{
    const uint8_t utf8[] = {0xc3, 0xa9};
    const uint8_t overlong[] = {0xc0, 0xaf};
    TEST_ASSERT_TRUE(fof_probe_text_ssid(utf8, sizeof(utf8)));
    TEST_ASSERT_FALSE(fof_probe_text_ssid(overlong, sizeof(overlong)));
    TEST_ASSERT_EQUAL_UINT16(2412, fof_probe_frequency_mhz(1));
    TEST_ASSERT_EQUAL_UINT16(2484, fof_probe_frequency_mhz(14));
    TEST_ASSERT_EQUAL_UINT16(5180, fof_probe_frequency_mhz(36));
    TEST_ASSERT_EQUAL_UINT16(0, fof_probe_frequency_mhz(0));
    fof_probe_budget_t budget = {0};
    TEST_ASSERT_TRUE(fof_probe_budget_allow(&budget, 10));
    TEST_ASSERT_TRUE(fof_probe_budget_allow(&budget, 11));
    TEST_ASSERT_TRUE(fof_probe_budget_allow(&budget, 12));
    TEST_ASSERT_FALSE(fof_probe_budget_allow(&budget, 13));
    TEST_ASSERT_FALSE(fof_probe_budget_allow(&budget, 999));
    TEST_ASSERT_TRUE(fof_probe_budget_allow(&budget, 1000));
    TEST_ASSERT_TRUE(fof_probe_budget_allow(&budget, 5)); /* Clock reset */
}
