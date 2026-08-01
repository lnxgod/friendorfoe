#ifdef C9E_SCANNER_INGRESS_STANDALONE

#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_scanner_command_ingress_rejects_before_any_authorized_effect(void);
void test_scanner_command_ingress_discards_bare_cr_and_overflow_suffixes(void);
void test_scanner_command_ingress_stale_partial_discards_through_next_lf(void);
void test_scanner_command_ingress_hands_same_read_ota_remainder_to_binary(void);
void test_scanner_command_ingress_failed_ota_begin_discards_same_read_remainder(
    void);
void test_scanner_command_ingress_routes_all_six_firmware_schema_ids(void);
void test_scanner_command_ingress_rejects_invalid_setup_atomically(void);

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(
        test_scanner_command_ingress_rejects_before_any_authorized_effect);
    RUN_TEST(
        test_scanner_command_ingress_discards_bare_cr_and_overflow_suffixes);
    RUN_TEST(
        test_scanner_command_ingress_stale_partial_discards_through_next_lf);
    RUN_TEST(
        test_scanner_command_ingress_hands_same_read_ota_remainder_to_binary);
    RUN_TEST(
        test_scanner_command_ingress_failed_ota_begin_discards_same_read_remainder);
    RUN_TEST(
        test_scanner_command_ingress_routes_all_six_firmware_schema_ids);
    RUN_TEST(
        test_scanner_command_ingress_rejects_invalid_setup_atomically);
    return UNITY_END();
}

#endif
