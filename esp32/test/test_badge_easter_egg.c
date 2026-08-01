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

static size_t source_occurrence_count(const char *source, const char *needle)
{
    size_t count = 0U;
    size_t needle_len = strlen(needle);
    const char *cursor = source;

    while (cursor && needle_len > 0U &&
           (cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_len;
    }
    return count;
}

static bool source_line_starts_with(const char *line, const char *prefix)
{
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static bool source_position_is_canary_guarded(
    const char *source,
    const char *position)
{
    enum {
        SOURCE_MAX_PREPROCESSOR_DEPTH = 32,
        SOURCE_GUARD_OTHER = 0,
        SOURCE_GUARD_CANARY = 1,
        SOURCE_GUARD_NON_CANARY = -1,
    };
    int guards[SOURCE_MAX_PREPROCESSOR_DEPTH] = {0};
    size_t depth = 0U;
    const char *line = source;

    while (line && line < position && *line) {
        while (line < position && (*line == ' ' || *line == '\t')) {
            line++;
        }
        if (source_line_starts_with(
                line, "#if defined(FOF_DC34_GAME_CANARY)") ||
            source_line_starts_with(
                line, "#ifdef FOF_DC34_GAME_CANARY")) {
            TEST_ASSERT_LESS_THAN_UINT(
                SOURCE_MAX_PREPROCESSOR_DEPTH, depth);
            guards[depth++] = SOURCE_GUARD_CANARY;
        } else if (
            source_line_starts_with(
                line, "#if !defined(FOF_DC34_GAME_CANARY)") ||
            source_line_starts_with(
                line, "#ifndef FOF_DC34_GAME_CANARY")) {
            TEST_ASSERT_LESS_THAN_UINT(
                SOURCE_MAX_PREPROCESSOR_DEPTH, depth);
            guards[depth++] = SOURCE_GUARD_NON_CANARY;
        } else if (source_line_starts_with(line, "#if") ||
                   source_line_starts_with(line, "#ifdef") ||
                   source_line_starts_with(line, "#ifndef")) {
            TEST_ASSERT_LESS_THAN_UINT(
                SOURCE_MAX_PREPROCESSOR_DEPTH, depth);
            guards[depth++] = SOURCE_GUARD_OTHER;
        } else if (source_line_starts_with(line, "#else") && depth > 0U) {
            guards[depth - 1U] = -guards[depth - 1U];
        } else if (source_line_starts_with(line, "#endif") && depth > 0U) {
            depth--;
        }

        line = strchr(line, '\n');
        if (line) {
            line++;
        }
    }

    for (size_t i = 0U; i < depth; i++) {
        if (guards[i] == SOURCE_GUARD_CANARY) {
            return true;
        }
    }
    return false;
}

static const char *assert_canary_source_site(
    const char *source,
    const char *needle)
{
    const char *site = strstr(source, needle);
    TEST_ASSERT_NOT_NULL(site);
    TEST_ASSERT_TRUE(source_position_is_canary_guarded(source, site));
    return site;
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
    const uint8_t embedded_nul[17] = {
        'G', 'a', 'm', 'e', 'C', 'h', 'a', 'n', '\0',
        'e', 'r', 's', 'A', 'I', '-', '6', '7',
    };

    TEST_ASSERT_TRUE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"GameChangersAI-67", 17));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"gamechangersai-67", 17));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"xGameChangersAI-67", 18));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"GameChangersAI-67x", 18));
    TEST_ASSERT_FALSE(badge_easter_egg_ssid_matches(
        (const uint8_t *)"fof-goblue", 10));
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

void test_badge_easter_radio_retrigger_waits_exactly_90_seconds(void)
{
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 1000U));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, 2000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss_at(&machine, 3000U));
    TEST_ASSERT_TRUE(machine.radio_cooldown_active);
    TEST_ASSERT_EQUAL_UINT32(3000U, machine.dismissed_at_ms);

    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 92999U));
    TEST_ASSERT_EQUAL_UINT32(3000U, machine.dismissed_at_ms);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, 92999U));
    TEST_ASSERT_EQUAL_UINT32(3000U, machine.dismissed_at_ms);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, 93000U));
    TEST_ASSERT_TRUE(machine.visible);
    TEST_ASSERT_FALSE(machine.radio_cooldown_active);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_THANKS, machine.phase);
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, machine.source);
}

void test_badge_easter_radio_retrigger_is_wrap_safe(void)
{
    const uint32_t dismissed_at = UINT32_MAX - 44999U;
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
        dismissed_at - 1000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss_at(
        &machine, dismissed_at));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 44999U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 45000U));
}

void test_badge_easter_button_stays_one_shot_after_radio_cooldown(void)
{
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BUTTON, 1000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss_at(&machine, 2000U));
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BUTTON, 92000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 92000U));
}

void test_badge_easter_bounce_exit_starts_radio_cooldown(void)
{
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 1000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_advance_at(&machine, 2000U));
    TEST_ASSERT_EQUAL(BADGE_EASTER_EGG_PHASE_BOUNCE, machine.phase);
    TEST_ASSERT_FALSE(machine.radio_cooldown_active);

    TEST_ASSERT_TRUE(badge_easter_egg_machine_advance_at(&machine, 3000U));
    TEST_ASSERT_FALSE(machine.visible);
    TEST_ASSERT_TRUE(machine.radio_cooldown_active);
    TEST_ASSERT_EQUAL_UINT32(3000U, machine.dismissed_at_ms);
    TEST_ASSERT_FALSE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 92999U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 93000U));
}

void test_badge_easter_init_clears_radio_cooldown(void)
{
    badge_easter_egg_machine_t machine;

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_WIFI_SSID, 1000U));
    TEST_ASSERT_TRUE(badge_easter_egg_machine_dismiss_at(&machine, 2000U));
    TEST_ASSERT_TRUE(machine.radio_cooldown_active);

    badge_easter_egg_machine_init(&machine);
    TEST_ASSERT_FALSE(machine.radio_cooldown_active);
    TEST_ASSERT_EQUAL_UINT32(0U, machine.dismissed_at_ms);
    TEST_ASSERT_TRUE(badge_easter_egg_machine_trigger_at(
        &machine, BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID, 2000U));
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
    TEST_ASSERT_EQUAL_UINT(
        1U, source_occurrence_count(source, "badge_con_presentation_hud("));
    TEST_ASSERT_EQUAL_UINT(
        1U, source_occurrence_count(source, "badge_ok_button_policy_update("));
    TEST_ASSERT_EQUAL_UINT(
        2U, source_occurrence_count(source, "fb_draw_heart_7x5("));

    const char *button_task = strstr(source, "static void badge_button_task(");
    TEST_ASSERT_NOT_NULL(button_task);
    const char *policy_init = assert_canary_source_site(
        source,
        "badge_ok_button_policy_init(\n"
        "        &ok_policy,\n"
        "        BADGE_BUTTON_LONG_MS,\n"
        "        BADGE_BUTTON_EASTER_HOLD_MS,\n"
        "        triforce_pressed_at_boot);");
    const char *policy_update = assert_canary_source_site(
        source,
        "badge_ok_button_action_t ok_action = badge_ok_button_policy_update(\n"
        "            &ok_policy,\n"
        "            buttons[0].stable_pressed,\n"
        "            !suppress_single_button_dispatch &&\n"
        "                !easter_visible_at_batch_start &&\n"
        "                !badge_power_runtime_is_quiet(),\n"
        "            (uint32_t)badge_now_ms());");
    const char *reset_branch = strstr(
        button_task, "if (power_event == BADGE_POWER_CHORD_RESET) {");
    const char *normal_edges = strstr(
        button_task, "if (!suppress_single_button_dispatch) {");
    const char *action_dispatch = assert_canary_source_site(
        source,
        "if (ok_action == BADGE_OK_BUTTON_ACTION_DETAIL) {\n"
        "            badge_button_working_double_press();\n"
        "        } else if (ok_action == BADGE_OK_BUTTON_ACTION_EASTER) {\n"
        "            (void)badge_easter_egg_runtime_trigger(\n"
        "                BADGE_EASTER_EGG_SOURCE_BUTTON);");
    TEST_ASSERT_NOT_NULL(reset_branch);
    TEST_ASSERT_NOT_NULL(normal_edges);
    TEST_ASSERT_TRUE(button_task < policy_init);
    TEST_ASSERT_TRUE(policy_update < reset_branch);
    TEST_ASSERT_TRUE(normal_edges < action_dispatch);
    TEST_ASSERT_EQUAL_UINT(
        1U, source_occurrence_count(source, "badge_ok_button_policy_init("));
    TEST_ASSERT_EQUAL_UINT(
        1U,
        source_occurrence_count(
            source, "if (ok_action == BADGE_OK_BUTTON_ACTION_DETAIL) {"));

    const char *strip = strstr(
        source, "static void draw_scanner_bottom_strip(");
    TEST_ASSERT_NOT_NULL(strip);
    const char *strip_end = strstr(strip, "static void draw_watch_eye(");
    const char *hud_call = assert_canary_source_site(
        source,
        "badge_con_hud_plan_t hud =\n"
        "        badge_con_presentation_hud(&s_con_render.snapshot);");
    const char *pulse = assert_canary_source_site(
        source, "bg = rgb565_mix_color(bg, activity_tint, mix);");
    const char *strip_fill = strstr(
        strip, "fb_fill_rect(0, y, LCD_W, LCD_H - y, bg);");
    const char *hud_gate = assert_canary_source_site(
        source, "if (hud.visible && ok && !safe_usb) {");
    const char *heart_definition = assert_canary_source_site(
        source, "static void fb_draw_heart_7x5(");
    const char *heart_call = assert_canary_source_site(
        source,
        "fb_draw_heart_7x5(left, value_y, hud.color_rgb565);");
    const char *value_draw = assert_canary_source_site(
        source,
        "fb_draw_tiny_string(\n"
        "            left + BADGE_DISPLAY_HEART_WIDTH + 2,\n"
        "            value_y,\n"
        "            value,\n"
        "            hud.color_rgb565,\n"
        "            value_bg);");
    const char *value_backing = assert_canary_source_site(
        source,
        "uint16_t value_bg = badge_theme_contrast_floor(\n"
        "            COL_WHITE, hud.color_rgb565);\n"
        "        fb_fill_rect(\n"
        "            left - 2,\n"
        "            value_y,\n"
        "            total_w + 4,\n"
        "            BADGE_DISPLAY_HEART_HEIGHT,\n"
        "            value_bg);");
    TEST_ASSERT_NOT_NULL(strip_end);
    TEST_ASSERT_NOT_NULL(strip_fill);
    TEST_ASSERT_NOT_EQUAL(heart_definition, heart_call);
    TEST_ASSERT_TRUE(strip <= hud_call && hud_call < strip_end);
    TEST_ASSERT_TRUE(strip <= pulse && pulse < strip_end);
    TEST_ASSERT_TRUE(pulse < strip_fill);
    TEST_ASSERT_TRUE(strip_fill < hud_gate);
    TEST_ASSERT_TRUE(hud_gate < strip_end);
    TEST_ASSERT_TRUE(hud_gate < value_backing);
    TEST_ASSERT_TRUE(value_backing < heart_call);
    TEST_ASSERT_TRUE(heart_call < value_draw);
    TEST_ASSERT_TRUE(value_draw < strip_end);
    TEST_ASSERT_EQUAL_UINT(
        1U,
        source_occurrence_count(
            source, "bg = rgb565_mix_color(bg, activity_tint, mix);"));
    TEST_ASSERT_NOT_NULL(strstr(
        strip,
        "if (hud.visible && ok && !safe_usb && activity_tint != 0U) {"));
    TEST_ASSERT_NOT_NULL(strstr(
        strip, "const char *role = role_names[hud.state];"));
    TEST_ASSERT_NOT_NULL(strstr(
        strip,
        "snprintf(value, sizeof(value), \"%u/%u\", "
        "hud.current, hud.maximum);"));
    TEST_ASSERT_NULL(strstr(source, "\"UNDER ATTACK\""));
    TEST_ASSERT_NULL(strstr(source, "\"HEALING\""));
    TEST_ASSERT_NULL(strstr(source, "\"CURE FADING\""));
    TEST_ASSERT_NULL(strstr(source, "\"HEALTH\""));
    TEST_ASSERT_NULL(strstr(source, "\"CURE\""));
    TEST_ASSERT_NULL(strstr(source, "Welcome to Hell"));
    TEST_ASSERT_NULL(strstr(source, "Just Kidding"));
    TEST_ASSERT_NULL(strstr(source, "Defcon 34 FoF"));
    free(source);
}
