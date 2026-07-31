#include "unity.h"

#include "badge_con_protocol.h"

#include <string.h>

_Static_assert(BADGE_CON_SERVICE_PAYLOAD_BYTES == 10U,
               "service payload size changed");
_Static_assert(BADGE_CON_LEGACY_ADV_BYTES == 31U,
               "legacy advertisement size changed");
_Static_assert(BADGE_CON_UART_LINE_CHARS == 33U,
               "UART line size changed");
_Static_assert(BADGE_CON_UART_WIRE_BYTES == 34U,
               "UART wire size changed");
_Static_assert(BADGE_CON_UART_BUFFER_BYTES == 35U,
               "UART buffer size changed");

static const uint8_t REFERENCE_ADVERTISEMENT[BADGE_CON_LEGACY_ADV_BYTES] = {
    0x02, 0x01, 0x06,
    0x1B, 0x21,
    0x73, 0x72, 0x65, 0x67, 0x6E, 0x61, 0x68, 0x63,
    0x65, 0x6D, 0x61, 0x67, 0x01, 0x4C, 0xF3, 0xF0,
    0x12, 0x22, 0xC3, 0xB2, 0xA1, 0x07, 0x2A, 0xCA, 0x00, 0x4D,
};

static const uint8_t SUPER_ADVERTISEMENT[BADGE_CON_LEGACY_ADV_BYTES] = {
    0x02, 0x01, 0x06,
    0x1B, 0x21,
    0x73, 0x72, 0x65, 0x67, 0x6E, 0x61, 0x68, 0x63,
    0x65, 0x6D, 0x61, 0x67, 0x01, 0x4C, 0xF3, 0xF0,
    0x15, 0x22, 0xC3, 0xB2, 0xA1, 0x07, 0x2A, 0x5A, 0xF0, 0xB7,
};

static void assert_reference_packet(const badge_con_packet_t *packet,
                                    int8_t rssi)
{
    TEST_ASSERT_NOT_NULL(packet);
    TEST_ASSERT_EQUAL_UINT8(1U, packet->version);
    TEST_ASSERT_EQUAL_HEX8(0x22U, packet->round);
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_IMMUNE, packet->role);
    TEST_ASSERT_FALSE(packet->super);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3U, packet->peer);
    TEST_ASSERT_EQUAL_HEX8(0x07U, packet->session);
    TEST_ASSERT_EQUAL_HEX8(0x2AU, packet->sequence);
    TEST_ASSERT_EQUAL_INT8(rssi, packet->rssi);
}

void test_badge_con_protocol_builds_exact_reference_frame(void)
{
    uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_IMMUNE, false,
        0xA1B2C3U, 0x07U, 0x2AU, advertisement));
    TEST_ASSERT_EQUAL_HEX8(0x02, advertisement[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, advertisement[1]);
    TEST_ASSERT_EQUAL_HEX8(0x06, advertisement[2]);
    TEST_ASSERT_EQUAL_HEX8(0x1B, advertisement[3]);
    TEST_ASSERT_EQUAL_HEX8(0x21, advertisement[4]);
    TEST_ASSERT_EQUAL_HEX8(0x73, advertisement[5]);
    TEST_ASSERT_EQUAL_HEX8(0xF3, advertisement[19]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, advertisement[20]);
    TEST_ASSERT_EQUAL_HEX8(0x12, advertisement[21]);
    TEST_ASSERT_EQUAL_HEX8(0x22, advertisement[22]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, advertisement[23]);
    TEST_ASSERT_EQUAL_HEX8(0xB2, advertisement[24]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, advertisement[25]);
    TEST_ASSERT_EQUAL_HEX8(0x07, advertisement[26]);
    TEST_ASSERT_EQUAL_HEX8(0x2A, advertisement[27]);
    TEST_ASSERT_EQUAL_HEX8(0xCA, advertisement[28]);
    TEST_ASSERT_EQUAL_HEX8(0x00, advertisement[29]);
    TEST_ASSERT_EQUAL_HEX8(0x4D, advertisement[30]);
}

void test_badge_con_protocol_matches_siphash_reference_vectors(void)
{
    static const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    uint8_t message[15];
    for (uint8_t index = 0U; index < sizeof(message); ++index) {
        message[index] = index;
    }

    TEST_ASSERT_EQUAL_UINT64(
        UINT64_C(0x726FDB47DD0E0E31),
        badge_con_siphash24(key, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT64(
        UINT64_C(0x74F839C593DC67FD),
        badge_con_siphash24(key, message, 1U));
    TEST_ASSERT_EQUAL_UINT64(
        UINT64_C(0xAB0200F58B01D137),
        badge_con_siphash24(key, message, 7U));
    TEST_ASSERT_EQUAL_UINT64(
        UINT64_C(0x93F5F5799A932462),
        badge_con_siphash24(key, message, 8U));
    TEST_ASSERT_EQUAL_UINT64(
        UINT64_C(0xA129CA6149BE45E5),
        badge_con_siphash24(key, message, 15U));
    TEST_ASSERT_EQUAL_UINT64(0U, badge_con_siphash24(NULL, message, 1U));
    TEST_ASSERT_EQUAL_UINT64(0U, badge_con_siphash24(key, NULL, 1U));
}

void test_badge_con_protocol_round_trips_all_three_roles(void)
{
    static const badge_con_role_t roles[] = {
        BADGE_CON_ROLE_NORMAL,
        BADGE_CON_ROLE_INFECTED,
        BADGE_CON_ROLE_IMMUNE,
    };
    static const uint8_t expected_vr[] = {0x10U, 0x11U, 0x12U};

    for (size_t index = 0U; index < 3U; ++index) {
        uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
        TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
            roles[index], false,
            0x010203U, 0x04U, 0x05U, advertisement));
        TEST_ASSERT_EQUAL_HEX8(expected_vr[index], advertisement[21]);

        badge_con_packet_t packet = {0};
        TEST_ASSERT_EQUAL(
            BADGE_CON_FRAME_VALID,
            badge_con_parse_advertisement(
                advertisement, sizeof(advertisement), -60, &packet));
        TEST_ASSERT_EQUAL_UINT8(1U, packet.version);
        TEST_ASSERT_EQUAL_HEX8(0x22U, packet.round);
        TEST_ASSERT_EQUAL(roles[index], packet.role);
        TEST_ASSERT_FALSE(packet.super);
        TEST_ASSERT_EQUAL_HEX32(0x010203U, packet.peer);
        TEST_ASSERT_EQUAL_HEX8(0x04U, packet.session);
        TEST_ASSERT_EQUAL_HEX8(0x05U, packet.sequence);
        TEST_ASSERT_EQUAL_INT8(-60, packet.rssi);
    }
}

void test_badge_con_protocol_rejects_zero_peer_and_session(void)
{
    uint8_t payload[BADGE_CON_SERVICE_PAYLOAD_BYTES];
    uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES];
    memset(payload, 0xA5, sizeof(payload));
    memset(advertisement, 0xA5, sizeof(advertisement));

    TEST_ASSERT_FALSE(badge_con_build_service_payload(
        BADGE_CON_ROLE_NORMAL, false, 0U, 1U, 0U, payload));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xA5, payload, sizeof(payload));
    TEST_ASSERT_FALSE(badge_con_build_service_payload(
        BADGE_CON_ROLE_NORMAL, false, 1U, 0U, 0U, payload));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xA5, payload, sizeof(payload));
    TEST_ASSERT_FALSE(badge_con_build_service_payload(
        BADGE_CON_ROLE_NORMAL, false,
        0x1000000U, 1U, 0U, payload));
    TEST_ASSERT_FALSE(badge_con_build_service_payload(
        (badge_con_role_t)3, false, 1U, 1U, 0U, payload));
    TEST_ASSERT_FALSE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_NORMAL, false, 0U, 1U, 0U, advertisement));
    TEST_ASSERT_EACH_EQUAL_HEX8(
        0xA5, advertisement, sizeof(advertisement));
}

void test_badge_con_protocol_distinguishes_non_game_from_claimed_invalid(void)
{
    static const uint8_t non_game[] = {
        0x02, 0x01, 0x06,
        0x04, 0x09, 'F', 'o', 'F',
    };
    static const uint8_t truncated_claim[] = {
        0x02, 0x01, 0x06,
        0x11, 0x21,
        0x73, 0x72, 0x65, 0x67, 0x6E, 0x61, 0x68, 0x63,
        0x65, 0x6D, 0x61, 0x67, 0x01, 0x4C, 0xF3, 0xF0,
    };
    static const uint8_t malformed_non_game[] = {
        0x02, 0x01, 0x06,
        0x20, 0x09,
        0x73, 0x72, 0x65, 0x67, 0x6E, 0x61, 0x68, 0x63,
        0x65, 0x6D, 0x61, 0x67, 0x01, 0x4C, 0xF3, 0xF0,
    };
    badge_con_packet_t sentinel;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    badge_con_packet_t before = sentinel;

    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_NOT_GAME,
        badge_con_parse_advertisement(
            non_game, sizeof(non_game), -40, &sentinel));
    TEST_ASSERT_EQUAL_MEMORY(&before, &sentinel, sizeof(sentinel));
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_parse_advertisement(
            truncated_claim, sizeof(truncated_claim), -40, &sentinel));
    TEST_ASSERT_EQUAL_MEMORY(&before, &sentinel, sizeof(sentinel));
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_NOT_GAME,
        badge_con_parse_advertisement(
            malformed_non_game, sizeof(malformed_non_game), -40, &sentinel));
    TEST_ASSERT_EQUAL_MEMORY(&before, &sentinel, sizeof(sentinel));
}

void test_badge_con_protocol_rejects_reserved_role_bits_wrong_round_and_tag(void)
{
    uint8_t candidate[BADGE_CON_LEGACY_ADV_BYTES];
    badge_con_packet_t packet = {0};

    memcpy(candidate, REFERENCE_ADVERTISEMENT, sizeof(candidate));
    candidate[21] |= 0x04U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_parse_advertisement(
            candidate, sizeof(candidate), -50, &packet));

    memcpy(candidate, REFERENCE_ADVERTISEMENT, sizeof(candidate));
    candidate[21] = 0x13U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_parse_advertisement(
            candidate, sizeof(candidate), -50, &packet));

    memcpy(candidate, REFERENCE_ADVERTISEMENT, sizeof(candidate));
    candidate[22] = 0x21U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_parse_advertisement(
            candidate, sizeof(candidate), -50, &packet));

    memcpy(candidate, REFERENCE_ADVERTISEMENT, sizeof(candidate));
    candidate[30] ^= 0x01U;
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_parse_advertisement(
            candidate, sizeof(candidate), -50, &packet));
}

void test_badge_con_protocol_round_trips_authenticated_super_infected(void)
{
    uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
    TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_INFECTED, true,
        0xA1B2C3U, 0x07U, 0x2AU, advertisement));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        SUPER_ADVERTISEMENT, advertisement, sizeof(advertisement));

    badge_con_packet_t packet = {0};
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_VALID,
        badge_con_parse_advertisement(
            advertisement, sizeof(advertisement), -44, &packet));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, packet.role);
    TEST_ASSERT_TRUE(packet.super);
    TEST_ASSERT_EQUAL_INT8(-44, packet.rssi);
}

void test_badge_con_protocol_rejects_invalid_authenticated_super_combinations(void)
{
    uint8_t output[BADGE_CON_LEGACY_ADV_BYTES];
    memset(output, 0xA5, sizeof(output));
    TEST_ASSERT_FALSE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_NORMAL, true,
        0xA1B2C3U, 0x07U, 0x2AU, output));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xA5, output, sizeof(output));
    TEST_ASSERT_FALSE(badge_con_build_legacy_advertisement(
        BADGE_CON_ROLE_IMMUNE, true,
        0xA1B2C3U, 0x07U, 0x2AU, output));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xA5, output, sizeof(output));

    static const uint8_t invalid_payloads[][BADGE_CON_SERVICE_PAYLOAD_BYTES] = {
        {0x14, 0x22, 0xC3, 0xB2, 0xA1, 0x07, 0x2A, 0xD8, 0x55, 0x6C},
        {0x16, 0x22, 0xC3, 0xB2, 0xA1, 0x07, 0x2A, 0x59, 0x23, 0x19},
        {0x1D, 0x22, 0xC3, 0xB2, 0xA1, 0x07, 0x2A, 0x3B, 0x27, 0x78},
    };
    for (size_t index = 0U;
         index < sizeof(invalid_payloads) / sizeof(invalid_payloads[0]);
         ++index) {
        uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES];
        memcpy(advertisement, SUPER_ADVERTISEMENT, sizeof(advertisement));
        memcpy(advertisement + 21U, invalid_payloads[index],
               BADGE_CON_SERVICE_PAYLOAD_BYTES);
        badge_con_packet_t packet;
        memset(&packet, 0xA5, sizeof(packet));
        badge_con_packet_t before = packet;
        TEST_ASSERT_EQUAL(
            BADGE_CON_FRAME_INVALID,
            badge_con_parse_advertisement(
                advertisement, sizeof(advertisement), -50, &packet));
        TEST_ASSERT_EQUAL_MEMORY(&before, &packet, sizeof(packet));
    }

    uint8_t stale_tag[BADGE_CON_LEGACY_ADV_BYTES];
    memcpy(stale_tag, REFERENCE_ADVERTISEMENT, sizeof(stale_tag));
    stale_tag[21] = 0x16U;
    badge_con_packet_t packet = {0};
    TEST_ASSERT_EQUAL(
        BADGE_CON_FRAME_INVALID,
        badge_con_parse_advertisement(
            stale_tag, sizeof(stale_tag), -50, &packet));
}

void test_badge_con_protocol_accepts_sequence_zero_and_ff(void)
{
    static const uint8_t sequences[] = {0x00U, 0xFFU};
    for (size_t index = 0U; index < 2U; ++index) {
        uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {0};
        badge_con_packet_t packet = {0};
        TEST_ASSERT_TRUE(badge_con_build_legacy_advertisement(
            BADGE_CON_ROLE_INFECTED, false, 0xABCDEFU, 0xFEU,
            sequences[index], advertisement));
        TEST_ASSERT_EQUAL(
            BADGE_CON_FRAME_VALID,
            badge_con_parse_advertisement(
                advertisement, sizeof(advertisement), -1, &packet));
        TEST_ASSERT_EQUAL_HEX8(sequences[index], packet.sequence);
    }
}

void test_badge_con_uart_renders_exact_reference_line(void)
{
    char line[BADGE_CON_UART_BUFFER_BYTES] = {0};
    size_t wire_size = 0U;
    badge_con_packet_t packet = {
        .version = 1U,
        .round = 0x22U,
        .role = BADGE_CON_ROLE_IMMUNE,
        .super = false,
        .peer = 0xA1B2C3U,
        .session = 0x07U,
        .sequence = 0x2AU,
        .rssi = -58,
    };
    TEST_ASSERT_TRUE(badge_con_render_uart_line(
        &packet, line, sizeof(line), &wire_size));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-058\n", line);
    TEST_ASSERT_EQUAL_UINT(BADGE_CON_UART_WIRE_BYTES, wire_size);

    badge_con_packet_t parsed = {0};
    TEST_ASSERT_TRUE(badge_con_parse_uart_line(
        (const uint8_t *)line, BADGE_CON_UART_LINE_CHARS, &parsed));
    assert_reference_packet(&parsed, -58);
}

void test_badge_con_uart_round_trips_super_infected_without_growing(void)
{
    char line[BADGE_CON_UART_BUFFER_BYTES] = {0};
    size_t wire_size = 0U;
    badge_con_packet_t packet = {
        .version = 1U,
        .round = 0x22U,
        .role = BADGE_CON_ROLE_INFECTED,
        .super = true,
        .peer = 0xA1B2C3U,
        .session = 0x07U,
        .sequence = 0x2AU,
        .rssi = -58,
    };
    TEST_ASSERT_TRUE(badge_con_render_uart_line(
        &packet, line, sizeof(line), &wire_size));
    TEST_ASSERT_EQUAL_STRING(
        "FOF_CRUD:1,22,5,A1B2C3,07,2A,-058\n", line);
    TEST_ASSERT_EQUAL_UINT(BADGE_CON_UART_WIRE_BYTES, wire_size);

    badge_con_packet_t parsed = {0};
    TEST_ASSERT_TRUE(badge_con_parse_uart_line(
        (const uint8_t *)line, BADGE_CON_UART_LINE_CHARS, &parsed));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, parsed.role);
    TEST_ASSERT_TRUE(parsed.super);

    packet.role = BADGE_CON_ROLE_NORMAL;
    TEST_ASSERT_FALSE(badge_con_render_uart_line(
        &packet, line, sizeof(line), &wire_size));
    packet.role = BADGE_CON_ROLE_IMMUNE;
    TEST_ASSERT_FALSE(badge_con_render_uart_line(
        &packet, line, sizeof(line), &wire_size));
}

void test_badge_con_uart_accepts_rssi_minus_127_and_minus_001(void)
{
    static const uint8_t minus_127[] =
        "FOF_CRUD:1,22,0,000001,01,00,-127";
    static const uint8_t minus_001[] =
        "FOF_CRUD:1,22,1,FFFFFF,FF,FF,-001";
    badge_con_packet_t packet = {0};

    TEST_ASSERT_TRUE(badge_con_parse_uart_line(
        minus_127, sizeof(minus_127) - 1U, &packet));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_NORMAL, packet.role);
    TEST_ASSERT_FALSE(packet.super);
    TEST_ASSERT_EQUAL_HEX32(1U, packet.peer);
    TEST_ASSERT_EQUAL_INT8(-127, packet.rssi);

    TEST_ASSERT_TRUE(badge_con_parse_uart_line(
        minus_001, sizeof(minus_001) - 1U, &packet));
    TEST_ASSERT_EQUAL(BADGE_CON_ROLE_INFECTED, packet.role);
    TEST_ASSERT_FALSE(packet.super);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFU, packet.peer);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, packet.session);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, packet.sequence);
    TEST_ASSERT_EQUAL_INT8(-1, packet.rssi);
}

void test_badge_con_uart_rejects_weak_width_case_whitespace_nul_and_suffixes(void)
{
    static const char *invalid_text[] = {
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-58",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-0058",
        "FOF_CRUD:1,22,2,a1B2C3,07,2A,-058",
        "FOF_CRUD:1,22,2,A1B2C3,0a,2A,-058",
        " FOF_CRUD:1,22,2,A1B2C3,07,2A,-058",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-058 ",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,+058",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-000",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-128",
        "FOF_CRUD:1,22,3,A1B2C3,07,2A,-058",
        "FOF_CRUD:1,22,4,A1B2C3,07,2A,-058",
        "FOF_CRUD:1,22,6,A1B2C3,07,2A,-058",
        "FOF_CRUD:1,22,7,A1B2C3,07,2A,-058",
        "FOF_CRUD:1,21,2,A1B2C3,07,2A,-058",
        "FOF_CRUD:2,22,2,A1B2C3,07,2A,-058",
        "FOF_CRUD:1,22,2,000000,07,2A,-058",
        "FOF_CRUD:1,22,2,A1B2C3,00,2A,-058",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-058\n",
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-058X",
    };
    badge_con_packet_t packet;
    memset(&packet, 0xA5, sizeof(packet));
    badge_con_packet_t before = packet;

    for (size_t index = 0U;
         index < sizeof(invalid_text) / sizeof(invalid_text[0]);
         ++index) {
        TEST_ASSERT_FALSE(badge_con_parse_uart_line(
            (const uint8_t *)invalid_text[index],
            strlen(invalid_text[index]), &packet));
        TEST_ASSERT_EQUAL_MEMORY(&before, &packet, sizeof(packet));
    }

    uint8_t embedded_nul[] =
        "FOF_CRUD:1,22,2,A1B2C3,07,2A,-058";
    embedded_nul[10] = '\0';
    TEST_ASSERT_FALSE(badge_con_parse_uart_line(
        embedded_nul, sizeof(embedded_nul) - 1U, &packet));
    TEST_ASSERT_EQUAL_MEMORY(&before, &packet, sizeof(packet));
    TEST_ASSERT_FALSE(badge_con_parse_uart_line(NULL, 0U, &packet));
    TEST_ASSERT_FALSE(badge_con_parse_uart_line(
        (const uint8_t *)invalid_text[0], strlen(invalid_text[0]), NULL));
}

void test_badge_con_self_messages_are_canonical_and_bounded(void)
{
    static const char expected_command[] =
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}";
    static const char expected_ack[] =
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,"
        "\"peer\":\"A1B2C3\",\"session\":\"07\"}";
    char command[sizeof(expected_command)];
    char ack[sizeof(expected_ack)];

    TEST_ASSERT_TRUE(badge_con_render_self_command(
        0xA1B2C3U, 0x07U, command, sizeof(command)));
    TEST_ASSERT_EQUAL_STRING(expected_command, command);
    TEST_ASSERT_TRUE(badge_con_render_self_ack(
        0xA1B2C3U, 0x07U, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_STRING(expected_ack, ack);

    char too_small[sizeof(expected_command) - 1U];
    memset(too_small, 'X', sizeof(too_small));
    char before[sizeof(too_small)];
    memcpy(before, too_small, sizeof(before));
    TEST_ASSERT_FALSE(badge_con_render_self_command(
        0xA1B2C3U, 0x07U, too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_MEMORY(before, too_small, sizeof(too_small));
    TEST_ASSERT_FALSE(badge_con_render_self_command(
        0U, 0x07U, command, sizeof(command)));
    TEST_ASSERT_FALSE(badge_con_render_self_command(
        0xA1B2C3U, 0U, command, sizeof(command)));
    TEST_ASSERT_FALSE(badge_con_render_self_ack(
        0x1000000U, 0x07U, ack, sizeof(ack)));
    TEST_ASSERT_FALSE(badge_con_render_self_ack(
        0xA1B2C3U, 0x07U, NULL, 0U));
}
