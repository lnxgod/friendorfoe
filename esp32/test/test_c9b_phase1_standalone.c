#ifdef C9B_PHASE1_STANDALONE

#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_scanner_uart_line_framer_accepts_lf_and_crlf_frames(void);
void test_scanner_uart_line_framer_handles_every_lf_and_crlf_split(void);
void test_scanner_uart_line_framer_skips_empty_lines_and_preserves_remainder(void);
void test_scanner_uart_line_framer_accepts_exact_4095_byte_payload(void);
void test_scanner_uart_line_framer_rejects_overflow_without_suffix_resurrection(
    void);
void test_scanner_uart_line_framer_rejects_bare_cr_without_suffix_resurrection(
    void);
void test_scanner_uart_line_framer_stale_partial_discards_until_next_lf(void);
void test_scanner_uart_line_framer_pending_cr_timeout_discards_split_suffix(
    void);
void test_scanner_uart_line_framer_preserves_embedded_nul_for_span_validation(
    void);
void test_scanner_uart_line_framer_rejects_invalid_api_arguments(void);

void test_mac_address_policy_normalizes_supported_wire_forms(void);
void test_mac_address_policy_empty_is_only_valid_when_explicitly_allowed(void);
void test_mac_address_policy_rejects_malformed_or_ambiguous_forms(void);
void test_mac_address_policy_canonical_check_requires_upper_colon_form(void);
void test_mac_address_policy_supports_in_place_normalization(void);
void test_mac_address_policy_clears_output_on_invalid_arguments(void);

void test_scanner_command_registry_accepts_every_exact_nonfirmware_fixture(void);
void test_scanner_command_registry_returns_validated_semantic_payloads(void);
void test_scanner_command_registry_routes_c4_firmware_before_nonfirmware(void);
void test_scanner_command_registry_rejects_bad_selectors_without_fallback(void);
void test_scanner_command_registry_refuses_routine_rom_mutation_commands(void);
void test_scanner_command_registry_keeps_incoming_easter_frames_separate(void);
void test_scanner_command_registry_enforces_ble_timeout_boundaries(void);
void test_scanner_command_registry_enforces_ble_request_target_correlation(void);
void test_scanner_command_registry_enforces_lockon_ranges_and_canonical_macs(
    void);
void test_scanner_command_registry_enforces_calibration_identity_correlation(
    void);
void test_scanner_command_registry_enforces_badge_and_nonbadge_profiles(void);
void test_scanner_command_registry_enforces_normalized_display_shapes(void);
void test_scanner_command_registry_enforces_display_policy_hash_and_nested_shape(
    void);
void test_scanner_command_registry_enforces_time_value_correlation(void);
void test_scanner_command_registry_rejects_structural_corruption_and_raw_nul(
    void);
void test_scanner_command_registry_rejects_invalid_api_arguments(void);
void test_scanner_command_producer_normalizes_backend_lockon_frames(void);
void test_scanner_command_producer_rejects_invalid_lockon_without_output(void);
void test_scanner_command_producer_builds_correlated_calibration_frames(void);
void test_scanner_command_producer_rejects_uncorrelated_calibration_identity(
    void);
void test_scanner_command_producer_builds_only_closed_display_shapes(void);

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scanner_uart_line_framer_accepts_lf_and_crlf_frames);
    RUN_TEST(test_scanner_uart_line_framer_handles_every_lf_and_crlf_split);
    RUN_TEST(
        test_scanner_uart_line_framer_skips_empty_lines_and_preserves_remainder);
    RUN_TEST(
        test_scanner_uart_line_framer_accepts_exact_4095_byte_payload);
    RUN_TEST(
        test_scanner_uart_line_framer_rejects_overflow_without_suffix_resurrection);
    RUN_TEST(
        test_scanner_uart_line_framer_rejects_bare_cr_without_suffix_resurrection);
    RUN_TEST(
        test_scanner_uart_line_framer_stale_partial_discards_until_next_lf);
    RUN_TEST(
        test_scanner_uart_line_framer_pending_cr_timeout_discards_split_suffix);
    RUN_TEST(
        test_scanner_uart_line_framer_preserves_embedded_nul_for_span_validation);
    RUN_TEST(test_scanner_uart_line_framer_rejects_invalid_api_arguments);

    RUN_TEST(test_mac_address_policy_normalizes_supported_wire_forms);
    RUN_TEST(
        test_mac_address_policy_empty_is_only_valid_when_explicitly_allowed);
    RUN_TEST(
        test_mac_address_policy_rejects_malformed_or_ambiguous_forms);
    RUN_TEST(
        test_mac_address_policy_canonical_check_requires_upper_colon_form);
    RUN_TEST(test_mac_address_policy_supports_in_place_normalization);
    RUN_TEST(test_mac_address_policy_clears_output_on_invalid_arguments);

    RUN_TEST(
        test_scanner_command_registry_accepts_every_exact_nonfirmware_fixture);
    RUN_TEST(
        test_scanner_command_registry_returns_validated_semantic_payloads);
    RUN_TEST(
        test_scanner_command_registry_routes_c4_firmware_before_nonfirmware);
    RUN_TEST(
        test_scanner_command_registry_rejects_bad_selectors_without_fallback);
    RUN_TEST(
        test_scanner_command_registry_refuses_routine_rom_mutation_commands);
    RUN_TEST(
        test_scanner_command_registry_keeps_incoming_easter_frames_separate);
    RUN_TEST(
        test_scanner_command_registry_enforces_ble_timeout_boundaries);
    RUN_TEST(
        test_scanner_command_registry_enforces_ble_request_target_correlation);
    RUN_TEST(
        test_scanner_command_registry_enforces_lockon_ranges_and_canonical_macs);
    RUN_TEST(
        test_scanner_command_registry_enforces_calibration_identity_correlation);
    RUN_TEST(
        test_scanner_command_registry_enforces_badge_and_nonbadge_profiles);
    RUN_TEST(
        test_scanner_command_registry_enforces_normalized_display_shapes);
    RUN_TEST(
        test_scanner_command_registry_enforces_display_policy_hash_and_nested_shape);
    RUN_TEST(
        test_scanner_command_registry_enforces_time_value_correlation);
    RUN_TEST(
        test_scanner_command_registry_rejects_structural_corruption_and_raw_nul);
    RUN_TEST(
        test_scanner_command_registry_rejects_invalid_api_arguments);
    RUN_TEST(
        test_scanner_command_producer_normalizes_backend_lockon_frames);
    RUN_TEST(
        test_scanner_command_producer_rejects_invalid_lockon_without_output);
    RUN_TEST(
        test_scanner_command_producer_builds_correlated_calibration_frames);
    RUN_TEST(
        test_scanner_command_producer_rejects_uncorrelated_calibration_identity);
    RUN_TEST(
        test_scanner_command_producer_builds_only_closed_display_shapes);
    return UNITY_END();
}

#endif
