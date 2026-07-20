#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "badge_easter_egg.h"
#include "badge_easter_egg_animation.h"
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
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_ARMED, machine.phase);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_BUTTON));
    TEST_ASSERT_TRUE(machine.triggered_once);
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_THANKS, machine.phase);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_advance(&machine));
    TEST_ASSERT_TRUE(machine.triggered_once);
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_BOUNCE, machine.phase);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_advance(&machine));
    TEST_ASSERT_TRUE(machine.triggered_once);
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_CONSUMED, machine.phase);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_advance(&machine));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BUTTON, machine.source);

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_ARMED, machine.phase);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID));
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_THANKS, machine.phase);
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

void test_badge_easter_uart_parser_accepts_only_exact_fixed_frames(void)
{
    TEST_ASSERT_EQUAL(
        BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID,
        badge_easter_egg_source_from_uart_frame(
            BADGE_EASTER_EGG_UART_FRAME_BLE_REMOTE_ID,
            BADGE_EASTER_EGG_UART_FRAME_BLE_REMOTE_ID_LEN));
    TEST_ASSERT_EQUAL(
        BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
        badge_easter_egg_source_from_uart_frame(
            BADGE_EASTER_EGG_UART_FRAME_WIFI_SSID,
            BADGE_EASTER_EGG_UART_FRAME_WIFI_SSID_LEN));

    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,
                      badge_easter_egg_source_from_uart_frame(NULL, 0));
    TEST_ASSERT_EQUAL(
        BADGE_EASTER_EGG_SOURCE_NONE,
        badge_easter_egg_source_from_uart_frame(
            BADGE_EASTER_EGG_UART_FRAME_BLE_REMOTE_ID,
            BADGE_EASTER_EGG_UART_FRAME_BLE_REMOTE_ID_LEN - 1));
    TEST_ASSERT_EQUAL(
        BADGE_EASTER_EGG_SOURCE_NONE,
        badge_easter_egg_source_from_uart_frame(
            BADGE_EASTER_EGG_UART_FRAME_WIFI_SSID "\n",
            BADGE_EASTER_EGG_UART_FRAME_WIFI_SSID_LEN + 1));
}

void test_badge_easter_uart_parser_rejects_noncanonical_and_escaped_nul(void)
{
    static const char escaped_nul_type[] =
        "{\"type\":\"badge_easter_egg\\u0000junk\","
        "\"source\":\"ble_remote_id\"}";
    static const char escaped_nul_source[] =
        "{\"type\":\"badge_easter_egg\","
        "\"source\":\"ble_remote_id\\u0000junk\"}";
    static const char escaped_nul_wifi_source[] =
        "{\"type\":\"badge_easter_egg\","
        "\"source\":\"wifi_ssid\\u0000junk\"}";
    static const char actual_nul_type[] =
        "{\"type\":\"badge_easter_egg\0junk\","
        "\"source\":\"ble_remote_id\"}";
    static const char extra_field[] =
        "{\"type\":\"badge_easter_egg\","
        "\"source\":\"ble_remote_id\",\"extra\":true}";
    static const char reordered[] =
        "{\"source\":\"ble_remote_id\","
        "\"type\":\"badge_easter_egg\"}";
    static const char duplicate_field[] =
        "{\"type\":\"badge_easter_egg\","
        "\"source\":\"ble_remote_id\",\"source\":\"wifi_ssid\"}";
    static const char whitespace[] =
        "{ \"type\": \"badge_easter_egg\", "
        "\"source\": \"ble_remote_id\" }";
    static const char leading_whitespace[] =
        " " BADGE_EASTER_EGG_UART_FRAME_BLE_REMOTE_ID;
    static const char changed_type_case[] =
        "{\"type\":\"Badge_easter_egg\","
        "\"source\":\"ble_remote_id\"}";
    static const char changed_source_case[] =
        "{\"type\":\"badge_easter_egg\","
        "\"source\":\"BLE_REMOTE_ID\"}";
    static const char trailing_bytes[] =
        BADGE_EASTER_EGG_UART_FRAME_BLE_REMOTE_ID "junk";

#define ASSERT_REJECTED_FRAME(frame)                                        \
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_NONE,                         \
                      badge_easter_egg_source_from_uart_frame(              \
                          (frame), sizeof(frame) - 1))

    ASSERT_REJECTED_FRAME(escaped_nul_type);
    ASSERT_REJECTED_FRAME(escaped_nul_source);
    ASSERT_REJECTED_FRAME(escaped_nul_wifi_source);
    ASSERT_REJECTED_FRAME(actual_nul_type);
    ASSERT_REJECTED_FRAME(extra_field);
    ASSERT_REJECTED_FRAME(reordered);
    ASSERT_REJECTED_FRAME(duplicate_field);
    ASSERT_REJECTED_FRAME(whitespace);
    ASSERT_REJECTED_FRAME(leading_whitespace);
    ASSERT_REJECTED_FRAME(changed_type_case);
    ASSERT_REJECTED_FRAME(changed_source_case);
    ASSERT_REJECTED_FRAME(trailing_bytes);

    TEST_ASSERT_TRUE(badge_easter_egg_uart_type_claims_event(
        MSG_TYPE_BADGE_EASTER_EGG));
    TEST_ASSERT_TRUE(badge_easter_egg_uart_type_claims_event(
        "badge_easter_egg\0junk"));
    TEST_ASSERT_FALSE(badge_easter_egg_uart_type_claims_event(
        "Badge_easter_egg"));

#undef ASSERT_REJECTED_FRAME
}

void test_badge_easter_button_batch_claims_only_one_transition(void)
{
    bool claimed = false;

    TEST_ASSERT_TRUE(badge_easter_egg_claim_press_in_batch(true, &claimed));
    TEST_ASSERT_TRUE(claimed);
    TEST_ASSERT_FALSE(badge_easter_egg_claim_press_in_batch(true, &claimed));
    TEST_ASSERT_TRUE(claimed);

    claimed = false;
    TEST_ASSERT_FALSE(badge_easter_egg_claim_press_in_batch(false, &claimed));
    TEST_ASSERT_FALSE(claimed);
    TEST_ASSERT_FALSE(badge_easter_egg_claim_press_in_batch(true, NULL));
}

void test_badge_easter_animation_initializes_and_moves_without_collision(void)
{
    badge_easter_egg_animation_t animation;

    badge_easter_egg_animation_init(&animation);
    TEST_ASSERT_EQUAL_INT16(8, animation.x);
    TEST_ASSERT_EQUAL_INT16(12, animation.y);
    TEST_ASSERT_EQUAL_INT8(3, animation.vx);
    TEST_ASSERT_EQUAL_INT8(2, animation.vy);
    TEST_ASSERT_EQUAL_UINT8(0, animation.color_index);

    TEST_ASSERT_FALSE(badge_easter_egg_animation_step(
        &animation, 128, 160, 64, 64, 6));
    TEST_ASSERT_EQUAL_INT16(11, animation.x);
    TEST_ASSERT_EQUAL_INT16(14, animation.y);
    TEST_ASSERT_EQUAL_INT8(3, animation.vx);
    TEST_ASSERT_EQUAL_INT8(2, animation.vy);
    TEST_ASSERT_EQUAL_UINT8(0, animation.color_index);
}

void test_badge_easter_animation_clamps_edges_and_cycles_color_once(void)
{
    badge_easter_egg_animation_t animation = {
        .x = 63,
        .y = 95,
        .vx = 3,
        .vy = 2,
        .color_index = 5,
    };

    TEST_ASSERT_TRUE(badge_easter_egg_animation_step(
        &animation, 128, 160, 64, 64, 6));
    TEST_ASSERT_EQUAL_INT16(64, animation.x);
    TEST_ASSERT_EQUAL_INT16(96, animation.y);
    TEST_ASSERT_EQUAL_INT8(-3, animation.vx);
    TEST_ASSERT_EQUAL_INT8(-2, animation.vy);
    TEST_ASSERT_EQUAL_UINT8(0, animation.color_index);

    animation.x = 1;
    animation.y = 1;
    animation.vx = -3;
    animation.vy = -2;
    animation.color_index = 0;
    TEST_ASSERT_TRUE(badge_easter_egg_animation_step(
        &animation, 128, 160, 64, 64, 6));
    TEST_ASSERT_EQUAL_INT16(0, animation.x);
    TEST_ASSERT_EQUAL_INT16(0, animation.y);
    TEST_ASSERT_EQUAL_INT8(3, animation.vx);
    TEST_ASSERT_EQUAL_INT8(2, animation.vy);
    TEST_ASSERT_EQUAL_UINT8(1, animation.color_index);
}

void test_badge_easter_animation_rejects_invalid_bounds_safely(void)
{
    badge_easter_egg_animation_t animation = {
        .x = 90,
        .y = 90,
        .vx = 3,
        .vy = 2,
        .color_index = 4,
    };

    TEST_ASSERT_FALSE(badge_easter_egg_animation_step(
        &animation, 63, 160, 64, 64, 6));
    TEST_ASSERT_EQUAL_INT16(0, animation.x);
    TEST_ASSERT_EQUAL_INT16(0, animation.y);
    TEST_ASSERT_EQUAL_UINT8(0, animation.color_index);
    TEST_ASSERT_FALSE(badge_easter_egg_animation_step(
        NULL, 128, 160, 64, 64, 6));
}

void test_badge_easter_renderer_uses_only_approved_presentation_copy(void)
{
    FILE *source_file = fopen("uplink/main/hw/display_st7735.c", "rb");
    TEST_ASSERT_NOT_NULL(source_file);
    TEST_ASSERT_EQUAL_INT(0, fseek(source_file, 0, SEEK_END));
    long source_size = ftell(source_file);
    TEST_ASSERT_GREATER_THAN(0, source_size);
    TEST_ASSERT_EQUAL_INT(0, fseek(source_file, 0, SEEK_SET));

    char *source = malloc((size_t)source_size + 1U);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_UINT((size_t)source_size,
                           fread(source, 1, (size_t)source_size, source_file));
    source[source_size] = '\0';
    fclose(source_file);

    TEST_ASSERT_NOT_NULL(strstr(source, "Thank you from"));
    TEST_ASSERT_NOT_NULL(strstr(source, "GameChangers AI"));
    TEST_ASSERT_NOT_NULL(strstr(source, "GAMECHANGERSAI_LOGO_WIDTH"));
    TEST_ASSERT_NOT_NULL(strstr(source, "BADGE_EASTER_EGG_PHASE_BOUNCE"));
    TEST_ASSERT_NULL(strstr(source, "Welcome to Hell"));
    TEST_ASSERT_NULL(strstr(source, "Just Kidding"));
    TEST_ASSERT_NULL(strstr(source, "Defcon 34 FoF"));
    free(source);
}
