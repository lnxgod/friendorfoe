#ifdef C9C_BLE_INVESTIGATION_STANDALONE

#include "unity.h"

void test_ble_inv_ingress_accepts_every_real_producer_frame(void);
void test_ble_inv_ingress_accepts_nullable_begin_and_end_variants(void);
void test_ble_inv_ingress_accepts_zero_length_read_value(void);
void test_ble_inv_ingress_enforces_terminal_text_capacity(void);
void test_ble_inv_ingress_requires_exact_scanner_slot(void);
void
test_ble_inv_ingress_rejects_prefix_near_matches_and_selector_corruption(void);
void test_ble_inv_ingress_rejects_exact_schema_corruption(void);
void test_ble_inv_ingress_enforces_type_specific_value_policy(void);
void test_ble_inv_ingress_is_order_independent_and_output_atomic(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ble_inv_ingress_accepts_every_real_producer_frame);
    RUN_TEST(test_ble_inv_ingress_accepts_nullable_begin_and_end_variants);
    RUN_TEST(test_ble_inv_ingress_accepts_zero_length_read_value);
    RUN_TEST(test_ble_inv_ingress_enforces_terminal_text_capacity);
    RUN_TEST(test_ble_inv_ingress_requires_exact_scanner_slot);
    RUN_TEST(
        test_ble_inv_ingress_rejects_prefix_near_matches_and_selector_corruption);
    RUN_TEST(test_ble_inv_ingress_rejects_exact_schema_corruption);
    RUN_TEST(test_ble_inv_ingress_enforces_type_specific_value_policy);
    RUN_TEST(test_ble_inv_ingress_is_order_independent_and_output_atomic);
    return UNITY_END();
}

#endif
