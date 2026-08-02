#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "backend_glasses_classifier.h"
#include "backend_detection_sink.h"
#include "backend_investigation_sink.h"
#include "detection_policy.h"
#include "open_drone_id_parser.h"
#include "../support/backend_test_main.h"

void test_parse_basic_id(void);
void test_parse_location(void);
void test_odid_basic_id_change_clears_only_prior_identity_location(void);
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
void test_fof_drone_test_ssids(void);
void test_budget_drone_prefixes(void);
void test_all_patterns_valid(void);
void test_null_ssid(void);
void test_parse_valid_payload(void);
void test_parse_zero_coords(void);
void test_parse_short_payload(void);
void test_parse_wrong_oui(void);
void test_initial_probability(void);
void test_single_ble_update(void);
void test_wifi_ssid_low_confidence(void);
void test_multi_source_boost(void);
void test_time_decay(void);
void test_prune(void);
void test_probe_broadcasts_still_drop(void);
void test_hard_probe_matches_keep_elevated_confidence(void);
void test_generic_targeted_probes_are_not_low_value_dropped(void);
void test_fof_drone_ssids_are_notable_but_ambient_fof_is_not(void);
void test_wifi_oui_database_includes_flock_safety(void);
void test_wifi_oui_database_assigns_explicit_detection_roles(void);
void test_wifi_oui_database_normalizes_flock_safety_mac_formats(void);
void test_notable_ssid_camera_token_avoids_campus_false_positive(void);
void test_notable_ssid_attack_tool_labels(void);
void test_privacy_wifi_signature_catalog_matches_key_ssids(void);
void test_privacy_wifi_signature_catalog_rejects_broad_patterns(void);
void test_probe_rate_aux_changes_when_identity_changes(void);
void test_queue_shedding_prefers_diagnostic_sources_first(void);
void test_ble_remote_id_is_never_shed_under_queue_pressure(void);
void test_ap_inventory_dedupe_key_uses_bssid(void);
void test_wifi_beacon_auth_mode_extracts_open_wpa_and_rsn(void);
void test_evil_twin_open_clone_same_ssid_alerts_once(void);
void test_evil_twin_same_oui_secured_mesh_does_not_alert(void);
void test_evil_twin_hidden_and_stale_observations_do_not_alert(void);
void test_evil_twin_accepts_full_length_ssid_without_overread(void);
void test_evil_twin_strong_different_oui_security_clone_alerts(void);
void test_dedupe_key_groups_probe_ie_hash_across_rotated_macs(void);
void test_dedupe_key_changes_across_time_bucket(void);
void test_ble_fingerprint_dedupe_keeps_mac_in_identity(void);
void test_ble_fingerprint_meta_name_is_case_insensitive(void);
void test_ble_fingerprint_meta_rayban_uuid_is_human_evidence(void);
void test_ble_fingerprint_meta_service_uuid_keeps_generic_meta_reason(void);
void test_ble_fingerprint_meta_feb8_is_generic_meta_device(void);
void test_ble_fingerprint_luxottica_cid_is_meta_glasses(void);
void test_ble_fingerprint_findmy_uuid_is_tracker(void);
void test_ble_fingerprint_exposure_notification_is_not_findmy_tracker(void);
void test_ble_fingerprint_apple_ibeacon_is_venue_beacon(void);
void test_ble_fingerprint_flock_name_is_not_alpr_evidence(void);
void test_ble_fingerprint_chipolo_member_uuid_is_tracker(void);
void test_ble_fingerprint_chipolo_company_id_is_tracker(void);
void test_ble_fingerprint_nordic_company_id_is_not_tile_tracker(void);
void test_ble_fingerprint_unikey_company_id_is_not_pebblebee_tracker(void);
void test_ble_fingerprint_serial_uuids_are_not_static_skimmers(void);
void test_ble_fingerprint_known_product_is_trusted_serial_identity(void);
void test_ble_fingerprint_unknown_and_serial_candidates_are_not_trusted(void);
void test_ble_fingerprint_empty_and_serial_only_reasons_are_not_trusted(void);
void test_hidden_camera_ble_is_priority_not_low_value(void);
void test_priority_ble_fingerprint_is_not_shed_under_pressure(void);
void test_priority_ble_fingerprint_uses_short_reemit_window(void);
void test_scan_profile_source_gates_normal_lanes(void);
void test_ble_meta_reacquire_triggers_when_stale_and_advancing(void);
void test_ble_meta_reacquire_blocks_calibration_or_ota(void);
void test_ble_meta_reacquire_requires_scan_sync_and_adv_delta(void);
void test_french_dri_fixture_preserves_identity_position_altitude_source_and_rssi(void);
void test_wifi_beacon_rid_fixture_preserves_identity_position_altitude_source_and_rssi(void);
void test_backend_pairing_spam_identity_survives_rotating_macs(void);
void test_backend_feature_matrix_emits_complete_detection_snapshots(void);
void test_ble_threat_swift_pair_rotating_flood_alerts_once(void);
void test_ble_threat_scan_duplicate_is_deduped(void);
void test_ble_threat_varied_crowd_does_not_alert(void);
void test_ble_threat_stable_addresses_do_not_alert(void);
void test_ble_threat_mixed_prompt_families_alert(void);
void test_ble_threat_cooldown_and_clear(void);
void test_ble_threat_persistent_sparse_ffe0_alerts(void);
void test_ble_threat_serial_skimmer_requires_minus_45_or_stronger(void);
void test_ble_threat_duplicate_serial_uuids_count_once_for_sparse_profile(void);
void test_ble_threat_exact_two_supporting_signals_alert(void);
void test_ble_threat_multi_service_profile_does_not_alert(void);
void test_ble_threat_simultaneous_prompt_and_serial_alerts_are_both_observable(void);
void test_ble_threat_observed_ms_rollback_resets_prompt_state(void);
void test_ble_threat_observed_ms_rollback_resets_serial_state(void);
void test_ble_threat_null_observation_clears_signal_output(void);
void test_ble_threat_ffe0_only_does_not_alert(void);
void test_ble_threat_trusted_product_suppresses_serial_candidate(void);
void test_ble_threat_pkoc_fff0_is_suppressed(void);



void setUp(void)
{
}

void tearDown(void)
{
    backend_detection_sink_register(NULL, NULL);
    backend_investigation_sink_register(NULL, NULL);
}

static const drone_detection_t *s_detection_caller_pointer;
static drone_detection_t s_detection_snapshot;

static bool capture_detection(void *context,
                              const drone_detection_t *detection,
                              int64_t observed_monotonic_ms)
{
    TEST_ASSERT_EQUAL_PTR(&s_detection_snapshot, context);
    TEST_ASSERT_NOT_EQUAL(s_detection_caller_pointer, detection);
    TEST_ASSERT_EQUAL_INT64(4242, observed_monotonic_ms);
    s_detection_snapshot = *detection;
    return true;
}

static const ble_investigation_chunk_t *s_investigation_caller_pointer;
static ble_investigation_chunk_t s_investigation_snapshot;

static bool capture_investigation(void *context,
                                  const ble_investigation_chunk_t *chunk)
{
    TEST_ASSERT_EQUAL_PTR(&s_investigation_snapshot, context);
    TEST_ASSERT_NOT_EQUAL(s_investigation_caller_pointer, chunk);
    s_investigation_snapshot = *chunk;
    return true;
}

void test_ported_open_drone_id_and_meta_detection_baseline(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);
    uint8_t rid_payload[25] = {0};
    rid_payload[0] = 0x00;
    rid_payload[1] = (1U << 4) | 2U;
    memcpy(&rid_payload[2], "BACKEND-RID-1", 13);
    odid_parse_message(rid_payload, sizeof(rid_payload), &state, 0);
    TEST_ASSERT_TRUE(state.has_basic_id);
    TEST_ASSERT_EQUAL_STRING("BACKEND-RID-1", state.drone_id);

    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    glasses_detection_t glasses = {0};
    TEST_ASSERT_TRUE(backend_glasses_classify_advertisement(
        mac, "Ray-Ban Meta", 12, NULL, 0, NULL, 0, 0, -47, 1000, &glasses));
    TEST_ASSERT_EQUAL_STRING("Meta", glasses.manufacturer);
    TEST_ASSERT_EQUAL_STRING("Smart Glasses", glasses.device_type);
    TEST_ASSERT_TRUE(glasses.has_camera);
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_BLE_FINGERPRINT));
    TEST_ASSERT_FALSE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_WIFI_AP_INVENTORY));
}

void test_backend_sinks_fail_closed_and_dispatch_copied_snapshots(void)
{
    drone_detection_t detection = {0};
    strcpy(detection.drone_id, "sink-detection");
    ble_investigation_chunk_t chunk = {0};
    strcpy(chunk.request_id, "sink-investigation");

    TEST_ASSERT_FALSE(backend_detection_sink_emit(&detection, 4242));
    TEST_ASSERT_FALSE(backend_investigation_sink_emit(&chunk));

    s_detection_caller_pointer = &detection;
    backend_detection_sink_register(capture_detection, &s_detection_snapshot);
    TEST_ASSERT_TRUE(backend_detection_sink_emit(&detection, 4242));
    TEST_ASSERT_EQUAL_STRING("sink-detection", s_detection_snapshot.drone_id);

    s_investigation_caller_pointer = &chunk;
    backend_investigation_sink_register(
        capture_investigation, &s_investigation_snapshot);
    TEST_ASSERT_TRUE(backend_investigation_sink_emit(&chunk));
    TEST_ASSERT_EQUAL_STRING(
        "sink-investigation", s_investigation_snapshot.request_id);
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_ported_open_drone_id_and_meta_detection_baseline);
    BACKEND_RUN_TEST(
        test_backend_sinks_fail_closed_and_dispatch_copied_snapshots);
    BACKEND_RUN_TEST(test_parse_basic_id);
    BACKEND_RUN_TEST(test_parse_location);
    BACKEND_RUN_TEST(test_odid_basic_id_change_clears_only_prior_identity_location);
    BACKEND_RUN_TEST(test_parse_system);
    BACKEND_RUN_TEST(test_parse_operator_id);
    BACKEND_RUN_TEST(test_parse_self_id);
    BACKEND_RUN_TEST(test_parse_message_pack);
    BACKEND_RUN_TEST(test_accuracy_code_to_meters);
    BACKEND_RUN_TEST(test_invalid_location_zero);
    BACKEND_RUN_TEST(test_state_to_detection);
    BACKEND_RUN_TEST(test_dji_match);
    BACKEND_RUN_TEST(test_tello_match);
    BACKEND_RUN_TEST(test_case_insensitive);
    BACKEND_RUN_TEST(test_no_match);
    BACKEND_RUN_TEST(test_hover_air);
    BACKEND_RUN_TEST(test_generic_drone);
    BACKEND_RUN_TEST(test_fof_drone_test_ssids);
    BACKEND_RUN_TEST(test_budget_drone_prefixes);
    BACKEND_RUN_TEST(test_all_patterns_valid);
    BACKEND_RUN_TEST(test_null_ssid);
    BACKEND_RUN_TEST(test_parse_valid_payload);
    BACKEND_RUN_TEST(test_parse_zero_coords);
    BACKEND_RUN_TEST(test_parse_short_payload);
    BACKEND_RUN_TEST(test_parse_wrong_oui);
    BACKEND_RUN_TEST(test_initial_probability);
    BACKEND_RUN_TEST(test_single_ble_update);
    BACKEND_RUN_TEST(test_wifi_ssid_low_confidence);
    BACKEND_RUN_TEST(test_multi_source_boost);
    BACKEND_RUN_TEST(test_time_decay);
    BACKEND_RUN_TEST(test_prune);
    BACKEND_RUN_TEST(test_probe_broadcasts_still_drop);
    BACKEND_RUN_TEST(test_hard_probe_matches_keep_elevated_confidence);
    BACKEND_RUN_TEST(test_generic_targeted_probes_are_not_low_value_dropped);
    BACKEND_RUN_TEST(test_fof_drone_ssids_are_notable_but_ambient_fof_is_not);
    BACKEND_RUN_TEST(test_wifi_oui_database_includes_flock_safety);
    BACKEND_RUN_TEST(test_wifi_oui_database_assigns_explicit_detection_roles);
    BACKEND_RUN_TEST(test_wifi_oui_database_normalizes_flock_safety_mac_formats);
    BACKEND_RUN_TEST(test_notable_ssid_camera_token_avoids_campus_false_positive);
    BACKEND_RUN_TEST(test_notable_ssid_attack_tool_labels);
    BACKEND_RUN_TEST(test_privacy_wifi_signature_catalog_matches_key_ssids);
    BACKEND_RUN_TEST(test_privacy_wifi_signature_catalog_rejects_broad_patterns);
    BACKEND_RUN_TEST(test_probe_rate_aux_changes_when_identity_changes);
    BACKEND_RUN_TEST(test_queue_shedding_prefers_diagnostic_sources_first);
    BACKEND_RUN_TEST(test_ble_remote_id_is_never_shed_under_queue_pressure);
    BACKEND_RUN_TEST(test_ap_inventory_dedupe_key_uses_bssid);
    BACKEND_RUN_TEST(test_wifi_beacon_auth_mode_extracts_open_wpa_and_rsn);
    BACKEND_RUN_TEST(test_evil_twin_open_clone_same_ssid_alerts_once);
    BACKEND_RUN_TEST(test_evil_twin_same_oui_secured_mesh_does_not_alert);
    BACKEND_RUN_TEST(test_evil_twin_hidden_and_stale_observations_do_not_alert);
    BACKEND_RUN_TEST(test_evil_twin_accepts_full_length_ssid_without_overread);
    BACKEND_RUN_TEST(test_evil_twin_strong_different_oui_security_clone_alerts);
    BACKEND_RUN_TEST(test_dedupe_key_groups_probe_ie_hash_across_rotated_macs);
    BACKEND_RUN_TEST(test_dedupe_key_changes_across_time_bucket);
    BACKEND_RUN_TEST(test_ble_fingerprint_dedupe_keeps_mac_in_identity);
    BACKEND_RUN_TEST(test_ble_fingerprint_meta_name_is_case_insensitive);
    BACKEND_RUN_TEST(test_ble_fingerprint_meta_rayban_uuid_is_human_evidence);
    BACKEND_RUN_TEST(test_ble_fingerprint_meta_service_uuid_keeps_generic_meta_reason);
    BACKEND_RUN_TEST(test_ble_fingerprint_meta_feb8_is_generic_meta_device);
    BACKEND_RUN_TEST(test_ble_fingerprint_luxottica_cid_is_meta_glasses);
    BACKEND_RUN_TEST(test_ble_fingerprint_findmy_uuid_is_tracker);
    BACKEND_RUN_TEST(test_ble_fingerprint_exposure_notification_is_not_findmy_tracker);
    BACKEND_RUN_TEST(test_ble_fingerprint_apple_ibeacon_is_venue_beacon);
    BACKEND_RUN_TEST(test_ble_fingerprint_flock_name_is_not_alpr_evidence);
    BACKEND_RUN_TEST(test_ble_fingerprint_chipolo_member_uuid_is_tracker);
    BACKEND_RUN_TEST(test_ble_fingerprint_chipolo_company_id_is_tracker);
    BACKEND_RUN_TEST(test_ble_fingerprint_nordic_company_id_is_not_tile_tracker);
    BACKEND_RUN_TEST(test_ble_fingerprint_unikey_company_id_is_not_pebblebee_tracker);
    BACKEND_RUN_TEST(test_ble_fingerprint_serial_uuids_are_not_static_skimmers);
    BACKEND_RUN_TEST(test_ble_fingerprint_known_product_is_trusted_serial_identity);
    BACKEND_RUN_TEST(test_ble_fingerprint_unknown_and_serial_candidates_are_not_trusted);
    BACKEND_RUN_TEST(test_ble_fingerprint_empty_and_serial_only_reasons_are_not_trusted);
    BACKEND_RUN_TEST(test_hidden_camera_ble_is_priority_not_low_value);
    BACKEND_RUN_TEST(test_priority_ble_fingerprint_is_not_shed_under_pressure);
    BACKEND_RUN_TEST(test_priority_ble_fingerprint_uses_short_reemit_window);
    BACKEND_RUN_TEST(test_scan_profile_source_gates_normal_lanes);
    BACKEND_RUN_TEST(test_ble_meta_reacquire_triggers_when_stale_and_advancing);
    BACKEND_RUN_TEST(test_ble_meta_reacquire_blocks_calibration_or_ota);
    BACKEND_RUN_TEST(test_ble_meta_reacquire_requires_scan_sync_and_adv_delta);
    BACKEND_RUN_TEST(test_french_dri_fixture_preserves_identity_position_altitude_source_and_rssi);
    BACKEND_RUN_TEST(test_wifi_beacon_rid_fixture_preserves_identity_position_altitude_source_and_rssi);
    BACKEND_RUN_TEST(test_backend_pairing_spam_identity_survives_rotating_macs);
    BACKEND_RUN_TEST(test_backend_feature_matrix_emits_complete_detection_snapshots);
    BACKEND_RUN_TEST(test_ble_threat_swift_pair_rotating_flood_alerts_once);
    BACKEND_RUN_TEST(test_ble_threat_scan_duplicate_is_deduped);
    BACKEND_RUN_TEST(test_ble_threat_varied_crowd_does_not_alert);
    BACKEND_RUN_TEST(test_ble_threat_stable_addresses_do_not_alert);
    BACKEND_RUN_TEST(test_ble_threat_mixed_prompt_families_alert);
    BACKEND_RUN_TEST(test_ble_threat_cooldown_and_clear);
    BACKEND_RUN_TEST(test_ble_threat_persistent_sparse_ffe0_alerts);
    BACKEND_RUN_TEST(test_ble_threat_serial_skimmer_requires_minus_45_or_stronger);
    BACKEND_RUN_TEST(test_ble_threat_duplicate_serial_uuids_count_once_for_sparse_profile);
    BACKEND_RUN_TEST(test_ble_threat_exact_two_supporting_signals_alert);
    BACKEND_RUN_TEST(test_ble_threat_multi_service_profile_does_not_alert);
    BACKEND_RUN_TEST(test_ble_threat_simultaneous_prompt_and_serial_alerts_are_both_observable);
    BACKEND_RUN_TEST(test_ble_threat_observed_ms_rollback_resets_prompt_state);
    BACKEND_RUN_TEST(test_ble_threat_observed_ms_rollback_resets_serial_state);
    BACKEND_RUN_TEST(test_ble_threat_null_observation_clears_signal_output);
    BACKEND_RUN_TEST(test_ble_threat_ffe0_only_does_not_alert);
    BACKEND_RUN_TEST(test_ble_threat_trusted_product_suppresses_serial_candidate);
    BACKEND_RUN_TEST(test_ble_threat_pkoc_fff0_is_suppressed);
    return UNITY_END();
}
