#include "unity.h"

#include "badge_easter_egg.h"
#include "uart_protocol.h"

static badge_easter_egg_remote_id_t exact_remote_id(void)
{
    badge_easter_egg_remote_id_t rid = {
        .has_basic_id = true,
        .basic_id = "fof-michagain",
        .has_location = true,
        .latitude_e7 = 424347200,
        .longitude_e7 = -839850000,
        .has_geodetic_altitude = true,
        .geodetic_altitude_half_m = 1332,
    };
    return rid;
}

void test_badge_easter_remote_id_requires_every_exact_field(void)
{
    badge_easter_egg_remote_id_t rid = exact_remote_id();

    TEST_ASSERT_TRUE(badge_easter_egg_remote_id_matches(&rid));
    rid.latitude_e7++;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
    rid.latitude_e7--;
    rid.geodetic_altitude_half_m--;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
}

void test_badge_easter_remote_id_rejects_missing_or_changed_basic_id(void)
{
    badge_easter_egg_remote_id_t rid = exact_remote_id();

    rid.has_basic_id = false;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));

    rid = exact_remote_id();
    rid.basic_id = "fof-michigan";
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));

    rid.basic_id = "FOF-MICHAGAIN";
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
}

void test_badge_easter_remote_id_rejects_missing_or_nearby_location(void)
{
    badge_easter_egg_remote_id_t rid = exact_remote_id();

    rid.has_location = false;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));

    rid = exact_remote_id();
    rid.latitude_e7 = 424347200 - 1;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
    rid.latitude_e7 = 424347200 + 1;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));

    rid = exact_remote_id();
    rid.longitude_e7 = -839850000 - 1;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
    rid.longitude_e7 = -839850000 + 1;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
}

void test_badge_easter_remote_id_requires_exact_geodetic_altitude(void)
{
    badge_easter_egg_remote_id_t rid = exact_remote_id();

    rid.has_geodetic_altitude = false;
    rid.geodetic_altitude_half_m = 0;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));

    rid = exact_remote_id();
    rid.has_geodetic_altitude = false;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));

    rid = exact_remote_id();
    rid.geodetic_altitude_half_m = 1332 - 1;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
    rid.geodetic_altitude_half_m = 1332 + 1;
    TEST_ASSERT_FALSE(badge_easter_egg_remote_id_matches(&rid));
}

void test_badge_easter_ssid_is_exact_case_sensitive_bytes(void)
{
    const uint8_t embedded_nul[10] = {
        'f', 'o', 'f', '-', 'g', 'o', 'b', '\0', 'u', 'e',
    };

    TEST_ASSERT_TRUE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"fof-goblue", 10));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"FOF-GOBLUE", 10));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"xfof-goblue", 11));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"fof-goblue-x", 12));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(embedded_nul,
                                                    sizeof(embedded_nul)));
}

void test_badge_easter_machine_is_one_shot_until_init(void)
{
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_FALSE(machine.triggered_once);
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_BUTTON));
    TEST_ASSERT_TRUE(machine.triggered_once);
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss(&machine));
    TEST_ASSERT_TRUE(machine.triggered_once);
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_WIFI_SSID, machine.source);
}

void test_badge_easter_machine_rejects_none_without_consuming_latch(void)
{
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_NONE));
    TEST_ASSERT_FALSE(machine.triggered_once);
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_BUTTON));
}

void test_badge_easter_machine_rejects_out_of_range_without_consuming_latch(void)
{
    badge_easter_egg_machine_t machine;
    const badge_easter_egg_source_t invalid_source =
        (badge_easter_egg_source_t)(BADGE_EASTER_EGG_SOURCE_BUTTON + 1);

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger(&machine,
                                                       invalid_source));
    TEST_ASSERT_FALSE(machine.triggered_once);
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
}

void test_badge_easter_uart_pending_sources_coalesce(void)
{
    uint32_t pending = 0;

    pending |= badge_easter_egg_uart_pending_bit(
        BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID);
    pending |= badge_easter_egg_uart_pending_bit(
        BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID);
    TEST_ASSERT_EQUAL_HEX32(BADGE_EASTER_EGG_UART_PENDING_BLE_REMOTE_ID,
                            pending);

    pending |= badge_easter_egg_uart_pending_bit(
        BADGE_EASTER_EGG_SOURCE_WIFI_SSID);
    pending |= badge_easter_egg_uart_pending_bit(
        BADGE_EASTER_EGG_SOURCE_WIFI_SSID);
    TEST_ASSERT_EQUAL_HEX32(BADGE_EASTER_EGG_UART_PENDING_ALL, pending);

    TEST_ASSERT_EQUAL_HEX32(0, badge_easter_egg_uart_pending_bit(
        BADGE_EASTER_EGG_SOURCE_NONE));
    TEST_ASSERT_EQUAL_HEX32(0, badge_easter_egg_uart_pending_bit(
        BADGE_EASTER_EGG_SOURCE_BUTTON));
}

void test_badge_easter_uart_frames_use_fixed_allowlisted_sources(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"badge_easter_egg\",\"source\":\"ble_remote_id\"}",
        badge_easter_egg_uart_frame(
            BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"badge_easter_egg\",\"source\":\"wifi_ssid\"}",
        badge_easter_egg_uart_frame(BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
    TEST_ASSERT_NULL(badge_easter_egg_uart_frame(
        BADGE_EASTER_EGG_SOURCE_NONE));
    TEST_ASSERT_NULL(badge_easter_egg_uart_frame(
        BADGE_EASTER_EGG_SOURCE_BUTTON));
}

void test_badge_easter_source_from_wire_accepts_only_scanner_sources(void)
{
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID,
                      badge_easter_egg_source_from_wire("ble_remote_id"));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
                      badge_easter_egg_source_from_wire("wifi_ssid"));

    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_wire(NULL));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_wire(""));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_wire("button"));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_wire("unknown"));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_wire("BLE_REMOTE_ID"));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_wire("Wifi_ssid"));
}
