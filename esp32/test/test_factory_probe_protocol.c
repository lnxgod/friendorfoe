#include "unity.h"
#include "factory_probe_protocol.h"

#include <string.h>

#define SESSION_A "0123456789abcdef0123456789abcdef"
#define SESSION_B "ffffffffffffffffffffffffffffffff"
#define MAC_SELF  "E0:72:A1:00:00:01"
#define MAC_A     "E0:72:A1:00:00:02"
#define MAC_B     "E0:72:A1:00:00:03"

void test_factory_probe_frame_round_trips(void)
{
    fof_factory_probe_frame_t source = {0};
    strncpy(source.session, SESSION_A, sizeof(source.session) - 1);
    strncpy(source.mac, MAC_A, sizeof(source.mac) - 1);
    source.link = 'a';
    source.sequence = 42;

    char encoded[FOF_FACTORY_PROBE_FRAME_MAX] = {0};
    TEST_ASSERT_TRUE(fof_factory_probe_frame_encode(
        &source, encoded, sizeof(encoded)));
    TEST_ASSERT_LESS_THAN(sizeof(encoded), strlen(encoded));

    fof_factory_probe_frame_t parsed = {0};
    TEST_ASSERT_TRUE(fof_factory_probe_frame_parse(encoded, &parsed));
    TEST_ASSERT_EQUAL_STRING(SESSION_A, parsed.session);
    TEST_ASSERT_EQUAL_STRING(MAC_A, parsed.mac);
    TEST_ASSERT_EQUAL_CHAR('a', parsed.link);
    TEST_ASSERT_EQUAL_UINT32(42, parsed.sequence);
}

void test_factory_probe_frame_rejects_crc_and_grammar_corruption(void)
{
    fof_factory_probe_frame_t source = {0};
    strncpy(source.session, SESSION_A, sizeof(source.session) - 1);
    strncpy(source.mac, MAC_A, sizeof(source.mac) - 1);
    source.link = 'b';
    source.sequence = 7;

    char encoded[FOF_FACTORY_PROBE_FRAME_MAX] = {0};
    TEST_ASSERT_TRUE(fof_factory_probe_frame_encode(
        &source, encoded, sizeof(encoded)));
    encoded[8] = encoded[8] == 'a' ? 'b' : 'a';
    fof_factory_probe_frame_t parsed = {0};
    TEST_ASSERT_FALSE(fof_factory_probe_frame_parse(encoded, &parsed));
    TEST_ASSERT_FALSE(fof_factory_probe_frame_parse("FOFP1|bad", &parsed));
}

void test_factory_probe_peer_table_requires_session_and_distinct_peers(void)
{
    fof_factory_probe_peer_table_t peers = {0};
    fof_factory_probe_frame_t frame = {0};
    strncpy(frame.session, SESSION_B, sizeof(frame.session) - 1);
    strncpy(frame.mac, MAC_A, sizeof(frame.mac) - 1);
    frame.link = 'a';

    TEST_ASSERT_FALSE(fof_factory_probe_peer_observe(
        &peers, MAC_SELF, SESSION_A, 'a', &frame));

    strncpy(frame.session, SESSION_A, sizeof(frame.session) - 1);
    TEST_ASSERT_TRUE(fof_factory_probe_peer_observe(
        &peers, MAC_SELF, SESSION_A, 'a', &frame));
    TEST_ASSERT_TRUE(peers.has_a);
    TEST_ASSERT_EQUAL_STRING(MAC_A, peers.peer_a);

    TEST_ASSERT_TRUE(fof_factory_probe_peer_observe(
        &peers, MAC_SELF, SESSION_A, 'a', &frame));

    strncpy(frame.mac, MAC_B, sizeof(frame.mac) - 1);
    TEST_ASSERT_FALSE(fof_factory_probe_peer_observe(
        &peers, MAC_SELF, SESSION_A, 'a', &frame));
    TEST_ASSERT_FALSE(fof_factory_probe_peer_observe(
        &peers, MAC_SELF, SESSION_A, 'c', &frame));

    strncpy(frame.mac, MAC_SELF, sizeof(frame.mac) - 1);
    TEST_ASSERT_FALSE(fof_factory_probe_peer_observe(
        &peers, MAC_SELF, SESSION_A, 'b', &frame));
}

void test_factory_probe_report_is_bounded_and_canonical(void)
{
    fof_factory_probe_peer_table_t peers = {0};
    peers.has_a = true;
    peers.has_b = true;
    strncpy(peers.peer_a, MAC_A, sizeof(peers.peer_a) - 1);
    strncpy(peers.peer_b, MAC_B, sizeof(peers.peer_b) - 1);

    char report[FOF_FACTORY_PROBE_REPORT_MAX] = {0};
    TEST_ASSERT_TRUE(fof_factory_probe_report_build(
        MAC_SELF, SESSION_A, &peers, report, sizeof(report)));
    TEST_ASSERT_NOT_NULL(strstr(report, "FOF_FACTORY_PROBE:"));
    TEST_ASSERT_NOT_NULL(strstr(report, "\"schema\":1"));
    TEST_ASSERT_NOT_NULL(strstr(report, "\"a\":\"" MAC_A "\""));
    TEST_ASSERT_NOT_NULL(strstr(report, "\"b\":\"" MAC_B "\""));
    TEST_ASSERT_NOT_NULL(strstr(report, "\"crc32\":\""));

    char tiny[48] = {0};
    TEST_ASSERT_FALSE(fof_factory_probe_report_build(
        MAC_SELF, SESSION_A, &peers, tiny, sizeof(tiny)));
}

void test_factory_probe_validators_reject_invalid_values(void)
{
    TEST_ASSERT_TRUE(fof_factory_probe_session_valid(SESSION_A));
    TEST_ASSERT_FALSE(fof_factory_probe_session_valid("short"));
    TEST_ASSERT_TRUE(fof_factory_probe_mac_valid(MAC_SELF));
    TEST_ASSERT_FALSE(fof_factory_probe_mac_valid("E0:72:A1:00:00"));
    TEST_ASSERT_FALSE(fof_factory_probe_mac_valid("E0:72:A1:00:00:GG"));
}
