#include "unity.h"

#include <string.h>

#include "badge_investigation_policy.h"

static badge_investigation_selection_t make_selection(
    bool has_entity,
    uint8_t source,
    badge_threat_category_t category,
    const char *key,
    const char *bssid)
{
    badge_investigation_selection_t selection;
    badge_investigation_selection_copy(&selection, has_entity, source,
                                       category, key, bssid);
    return selection;
}

void test_badge_hold_on_ble_entity_starts_gatt_investigation(void)
{
    badge_investigation_selection_t selection = make_selection(
        true, DETECTION_SRC_BLE_FINGERPRINT, BADGE_THREAT_CATEGORY_SKIM,
        "SKIM:AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF");

    TEST_ASSERT_EQUAL(BADGE_HOLD_INVESTIGATE_GATT,
                      badge_investigation_hold_action(&selection));
}

void test_badge_hold_on_pairing_spam_starts_passive_capture(void)
{
    badge_investigation_selection_t selection = make_selection(
        true, DETECTION_SRC_BLE_FINGERPRINT,
        BADGE_THREAT_CATEGORY_BLE_SPAM, "BLE_SPAM", "");

    TEST_ASSERT_EQUAL(BADGE_HOLD_INVESTIGATE_PASSIVE,
                      badge_investigation_hold_action(&selection));
}

void test_badge_hold_on_non_ble_entity_opens_deepest_detail(void)
{
    badge_investigation_selection_t selection = make_selection(
        true, DETECTION_SRC_WIFI_SSID, BADGE_THREAT_CATEGORY_WIFI,
        "WIFI:alert", "AA:BB:CC:DD:EE:FF");

    TEST_ASSERT_EQUAL(BADGE_HOLD_SHOW_DETAIL,
                      badge_investigation_hold_action(&selection));
}

void test_badge_hold_without_entity_keeps_pairing_action(void)
{
    badge_investigation_selection_t selection = make_selection(
        false, 0, BADGE_THREAT_CATEGORY_NONE, "IDLE:BLE", "");

    TEST_ASSERT_EQUAL(BADGE_HOLD_PAIR_PHONE,
                      badge_investigation_hold_action(&selection));
}

void test_badge_investigation_copies_target_before_snapshot_reorder(void)
{
    char source_key[BADGE_THREAT_KEY_LEN] = "SKIM:original";
    char source_bssid[18] = "AA:BB:CC:DD:EE:FF";
    badge_investigation_selection_t selection;

    badge_investigation_selection_copy(
        &selection, true, DETECTION_SRC_BLE_FINGERPRINT,
        BADGE_THREAT_CATEGORY_SKIM, source_key, source_bssid);
    strcpy(source_key, "SKIM:replacement");
    strcpy(source_bssid, "11:22:33:44:55:66");

    TEST_ASSERT_EQUAL_STRING("SKIM:original", selection.key);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", selection.bssid);
    TEST_ASSERT_EQUAL(BADGE_HOLD_INVESTIGATE_GATT,
                      badge_investigation_hold_action(&selection));
}

void test_badge_hold_on_ble_entity_without_valid_mac_opens_detail(void)
{
    badge_investigation_selection_t selection = make_selection(
        true, DETECTION_SRC_BLE_FINGERPRINT,
        BADGE_THREAT_CATEGORY_SKIM, "SKIM:no-mac", "not-a-mac");

    TEST_ASSERT_EQUAL(BADGE_HOLD_SHOW_DETAIL,
                      badge_investigation_hold_action(&selection));
}

void test_badge_investigation_terminal_pages_advance_and_wrap(void)
{
    TEST_ASSERT_EQUAL_INT(2, badge_investigation_next_page(BLE_INV_COMPLETE, 1));
    TEST_ASSERT_EQUAL_INT(3, badge_investigation_next_page(BLE_INV_FAILED, 2));
    TEST_ASSERT_EQUAL_INT(1, badge_investigation_next_page(BLE_INV_CANCELLED, 3));
}

void test_badge_investigation_active_and_error_states_normalize_overlay_pages(void)
{
    TEST_ASSERT_TRUE(badge_investigation_state_is_active(BLE_INV_CONNECTING));
    TEST_ASSERT_EQUAL_INT(0,
        badge_investigation_normalize_page(BLE_INV_READING, 3));
    TEST_ASSERT_FALSE(badge_investigation_state_is_active(BLE_INV_FAILED));
    TEST_ASSERT_EQUAL_INT(1,
        badge_investigation_normalize_page(BLE_INV_FAILED, 0));
    TEST_ASSERT_EQUAL_INT(3,
        badge_investigation_normalize_page(BLE_INV_COMPLETE, 3));
}

void test_badge_investigation_successful_read_evidence_is_surfaced_and_bounded(void)
{
    ble_investigation_read_t read = {0};
    char evidence[BADGE_INVESTIGATION_READ_EVIDENCE_LEN];
    strcpy(read.uuid, "0000ffe1-0000-1000-8000-00805f9b34fb");
    strcpy(read.value_hex, "4142434445464748494A4B4C4D4E4F50");

    TEST_ASSERT_TRUE(badge_investigation_format_read_evidence(
        &read, evidence, sizeof(evidence)));
    TEST_ASSERT_EQUAL_STRING("0000ffe1 4142434445464748", evidence);
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(evidence), strlen(evidence));
}

void test_badge_investigation_read_evidence_sanitizes_nonprintable_text(void)
{
    ble_investigation_read_t read = {0};
    char evidence[18];
    strcpy(read.uuid, "0000f\ne1-0000-1000-8000-00805f9b34fb");
    strcpy(read.value_hex, "4142\t34445464748");

    TEST_ASSERT_TRUE(badge_investigation_format_read_evidence(
        &read, evidence, sizeof(evidence)));
    TEST_ASSERT_NULL(strchr(evidence, '\n'));
    TEST_ASSERT_NULL(strchr(evidence, '\t'));
    TEST_ASSERT_NOT_NULL(strchr(evidence, '?'));
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(evidence), strlen(evidence));
}

void test_badge_investigation_security_view_marks_untransported_links_unknown(void)
{
    badge_investigation_security_view_t view;

    badge_investigation_security_view(true, &view);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", view.connectable);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", view.bonded);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", view.encrypted);
    TEST_ASSERT_EQUAL_STRING("REQUIRED", view.authentication);

    badge_investigation_security_view(false, &view);
    TEST_ASSERT_EQUAL_STRING("NOT REQUIRED", view.authentication);
}
