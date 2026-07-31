#ifdef C9D_SCANNER_UPLINK_STANDALONE

#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_scanner_uplink_ingress_routes_real_nonfirmware_variants(void);
void test_scanner_uplink_ingress_routes_c4_firmware_without_fallback(void);
void test_scanner_uplink_ingress_routes_c9c_ble_only_from_slot_zero(void);
void test_scanner_uplink_ingress_rejects_ack_shape_corruption(void);
void test_scanner_uplink_ingress_requires_exact_easter_fastpath(void);
void test_scanner_uplink_ingress_rejects_selector_corruption(void);
void test_scanner_uplink_ingress_rejects_invalid_arguments_atomically(void);

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(
        test_scanner_uplink_ingress_routes_real_nonfirmware_variants);
    RUN_TEST(
        test_scanner_uplink_ingress_routes_c4_firmware_without_fallback);
    RUN_TEST(
        test_scanner_uplink_ingress_routes_c9c_ble_only_from_slot_zero);
    RUN_TEST(
        test_scanner_uplink_ingress_rejects_ack_shape_corruption);
    RUN_TEST(
        test_scanner_uplink_ingress_requires_exact_easter_fastpath);
    RUN_TEST(
        test_scanner_uplink_ingress_rejects_selector_corruption);
    RUN_TEST(
        test_scanner_uplink_ingress_rejects_invalid_arguments_atomically);
    return UNITY_END();
}

#endif
