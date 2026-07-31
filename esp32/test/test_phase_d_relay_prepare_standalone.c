#ifdef PHASE_D_RELAY_PREPARE_STANDALONE

#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_fw_manifest_clear_exact_tuple_clears_only_valid_once(void);
void test_fw_manifest_clear_missing_invalid_and_newer_do_not_write(void);
void test_fw_manifest_clear_read_errors_close_once_without_writes(void);
void test_fw_manifest_clear_write_error_fails_closed_without_size_claim(void);
void test_fw_relay_prepare_success_retains_then_releases_exactly_once(void);
void test_fw_relay_prepared_release_retains_token_when_end_rejected(void);
void test_fw_relay_prepare_failure_preserves_rejected_token_for_retry(void);
void test_fw_relay_prepare_busy_paths_release_only_owned_resources(void);
void test_fw_relay_prepare_restage_during_token_observes_new_generation(void);
void test_fw_relay_prepare_read_and_partition_failures_release_all(void);
void test_fw_relay_prepare_exact_validation_failure_compare_clears(void);
void test_fw_relay_prepare_never_clears_newer_or_missing_manifest(void);
void test_fw_relay_prepare_clear_io_failure_is_storage_error(void);
void test_fw_relay_prepare_invalid_arguments_touch_no_hooks(void);

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fw_manifest_clear_exact_tuple_clears_only_valid_once);
    RUN_TEST(test_fw_manifest_clear_missing_invalid_and_newer_do_not_write);
    RUN_TEST(test_fw_manifest_clear_read_errors_close_once_without_writes);
    RUN_TEST(test_fw_manifest_clear_write_error_fails_closed_without_size_claim);
    RUN_TEST(test_fw_relay_prepare_success_retains_then_releases_exactly_once);
    RUN_TEST(test_fw_relay_prepared_release_retains_token_when_end_rejected);
    RUN_TEST(test_fw_relay_prepare_failure_preserves_rejected_token_for_retry);
    RUN_TEST(test_fw_relay_prepare_busy_paths_release_only_owned_resources);
    RUN_TEST(test_fw_relay_prepare_restage_during_token_observes_new_generation);
    RUN_TEST(test_fw_relay_prepare_read_and_partition_failures_release_all);
    RUN_TEST(test_fw_relay_prepare_exact_validation_failure_compare_clears);
    RUN_TEST(test_fw_relay_prepare_never_clears_newer_or_missing_manifest);
    RUN_TEST(test_fw_relay_prepare_clear_io_failure_is_storage_error);
    RUN_TEST(test_fw_relay_prepare_invalid_arguments_touch_no_hooks);
    return UNITY_END();
}

#endif
