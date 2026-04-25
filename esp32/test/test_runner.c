#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_initial_probability(void);
void test_single_ble_update(void);
void test_wifi_ssid_low_confidence(void);
void test_multi_source_boost(void);
void test_time_decay(void);
void test_prune(void);

void test_parse_valid_payload(void);
void test_parse_zero_coords(void);
void test_parse_short_payload(void);
void test_parse_wrong_oui(void);

void test_parse_basic_id(void);
void test_parse_location(void);
void test_parse_system(void);
void test_parse_operator_id(void);
void test_parse_self_id(void);
void test_parse_message_pack(void);
void test_accuracy_code_to_meters(void);
void test_invalid_location_zero(void);
void test_state_to_detection(void);

void test_dji_match(void);
void test_tello_match(void);
void test_case_insensitive(void);
void test_no_match(void);
void test_hover_air(void);
void test_generic_drone(void);
void test_all_patterns_valid(void);
void test_null_ssid(void);
void test_probe_broadcasts_still_drop(void);
void test_hard_probe_matches_keep_elevated_confidence(void);
void test_generic_targeted_probes_are_not_low_value_dropped(void);
void test_probe_rate_aux_changes_when_identity_changes(void);
void test_queue_shedding_prefers_diagnostic_sources_first(void);
void test_calibration_ble_uuid_is_recognized_and_kept(void);
void test_dedupe_key_groups_probe_ie_hash_across_rotated_macs(void);
void test_dedupe_key_changes_across_time_bucket(void);
void test_scan_profiles_assign_slot_roles_and_calibration_override(void);
void test_time_message_validity_tracks_ok_flag_and_epoch(void);
void test_backend_epoch_resteers_only_when_drift_exceeds_threshold(void);
void test_sntp_synced_uplink_ignores_backend_overwrite(void);
void test_stale_timeout_marks_scanner_state_stale(void);

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_initial_probability);
    RUN_TEST(test_single_ble_update);
    RUN_TEST(test_wifi_ssid_low_confidence);
    RUN_TEST(test_multi_source_boost);
    RUN_TEST(test_time_decay);
    RUN_TEST(test_prune);

    RUN_TEST(test_parse_valid_payload);
    RUN_TEST(test_parse_zero_coords);
    RUN_TEST(test_parse_short_payload);
    RUN_TEST(test_parse_wrong_oui);

    RUN_TEST(test_parse_basic_id);
    RUN_TEST(test_parse_location);
    RUN_TEST(test_parse_system);
    RUN_TEST(test_parse_operator_id);
    RUN_TEST(test_parse_self_id);
    RUN_TEST(test_parse_message_pack);
    RUN_TEST(test_accuracy_code_to_meters);
    RUN_TEST(test_invalid_location_zero);
    RUN_TEST(test_state_to_detection);

    RUN_TEST(test_dji_match);
    RUN_TEST(test_tello_match);
    RUN_TEST(test_case_insensitive);
    RUN_TEST(test_no_match);
    RUN_TEST(test_hover_air);
    RUN_TEST(test_generic_drone);
    RUN_TEST(test_all_patterns_valid);
    RUN_TEST(test_null_ssid);
    RUN_TEST(test_probe_broadcasts_still_drop);
    RUN_TEST(test_hard_probe_matches_keep_elevated_confidence);
    RUN_TEST(test_generic_targeted_probes_are_not_low_value_dropped);
    RUN_TEST(test_probe_rate_aux_changes_when_identity_changes);
    RUN_TEST(test_queue_shedding_prefers_diagnostic_sources_first);
    RUN_TEST(test_calibration_ble_uuid_is_recognized_and_kept);
    RUN_TEST(test_dedupe_key_groups_probe_ie_hash_across_rotated_macs);
    RUN_TEST(test_dedupe_key_changes_across_time_bucket);
    RUN_TEST(test_scan_profiles_assign_slot_roles_and_calibration_override);
    RUN_TEST(test_time_message_validity_tracks_ok_flag_and_epoch);
    RUN_TEST(test_backend_epoch_resteers_only_when_drift_exceeds_threshold);
    RUN_TEST(test_sntp_synced_uplink_ignores_backend_overwrite);
    RUN_TEST(test_stale_timeout_marks_scanner_state_stale);

    return UNITY_END();
}
