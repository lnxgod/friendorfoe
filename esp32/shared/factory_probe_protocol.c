#include "factory_probe_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static bool is_hex_char(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}


bool fof_factory_probe_session_valid(const char *session)
{
    if (!session || strlen(session) != FOF_FACTORY_PROBE_SESSION_HEX) {
        return false;
    }
    for (size_t i = 0; i < FOF_FACTORY_PROBE_SESSION_HEX; ++i) {
        if (!is_hex_char(session[i])) {
            return false;
        }
    }
    return true;
}


bool fof_factory_probe_mac_valid(const char *mac)
{
    if (!mac || strlen(mac) != FOF_FACTORY_PROBE_MAC_TEXT) {
        return false;
    }
    for (size_t i = 0; i < FOF_FACTORY_PROBE_MAC_TEXT; ++i) {
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') {
                return false;
            }
        } else if (!is_hex_char(mac[i])) {
            return false;
        }
    }
    return true;
}


uint32_t fof_factory_probe_crc32(const void *data, size_t length)
{
    if (!data && length > 0) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xFFFFFFFF);
}


static void uppercase_mac_copy(char *out, const char *mac)
{
    for (size_t i = 0; i < FOF_FACTORY_PROBE_MAC_TEXT; ++i) {
        out[i] = (char)toupper((unsigned char)mac[i]);
    }
    out[FOF_FACTORY_PROBE_MAC_TEXT] = '\0';
}


bool fof_factory_probe_frame_encode(const fof_factory_probe_frame_t *frame,
                                    char *out, size_t out_size)
{
    if (!frame || !out || out_size == 0 ||
        !fof_factory_probe_session_valid(frame->session) ||
        !fof_factory_probe_mac_valid(frame->mac) ||
        (frame->link != 'a' && frame->link != 'b')) {
        return false;
    }

    char mac[FOF_FACTORY_PROBE_MAC_TEXT + 1] = {0};
    uppercase_mac_copy(mac, frame->mac);
    char body[FOF_FACTORY_PROBE_FRAME_MAX] = {0};
    int body_len = snprintf(body, sizeof(body), "FOFP1|%s|%s|%c|%lu",
                            frame->session, mac, frame->link,
                            (unsigned long)frame->sequence);
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) {
        return false;
    }
    uint32_t crc = fof_factory_probe_crc32(body, (size_t)body_len);
    int written = snprintf(out, out_size, "%s|%08lx\n", body,
                           (unsigned long)crc);
    return written >= 0 && (size_t)written < out_size;
}


bool fof_factory_probe_frame_parse(const char *encoded,
                                   fof_factory_probe_frame_t *out)
{
    if (!encoded || !out) {
        return false;
    }
    size_t encoded_len = strnlen(encoded, FOF_FACTORY_PROBE_FRAME_MAX);
    if (encoded_len == 0 || encoded_len >= FOF_FACTORY_PROBE_FRAME_MAX) {
        return false;
    }

    char copy[FOF_FACTORY_PROBE_FRAME_MAX] = {0};
    memcpy(copy, encoded, encoded_len);
    while (encoded_len > 0 &&
           (copy[encoded_len - 1] == '\n' || copy[encoded_len - 1] == '\r')) {
        copy[--encoded_len] = '\0';
    }

    char *crc_sep = strrchr(copy, '|');
    if (!crc_sep || strlen(crc_sep + 1) != 8) {
        return false;
    }
    for (const char *cursor = crc_sep + 1; *cursor; ++cursor) {
        if (!is_hex_char(*cursor)) {
            return false;
        }
    }
    char *crc_end = NULL;
    unsigned long parsed_crc = strtoul(crc_sep + 1, &crc_end, 16);
    if (!crc_end || *crc_end != '\0' || parsed_crc > UINT32_MAX) {
        return false;
    }
    *crc_sep = '\0';
    uint32_t actual_crc = fof_factory_probe_crc32(copy, strlen(copy));
    if (actual_crc != (uint32_t)parsed_crc) {
        return false;
    }

    char *save = NULL;
    char *prefix = strtok_r(copy, "|", &save);
    char *session = strtok_r(NULL, "|", &save);
    char *mac = strtok_r(NULL, "|", &save);
    char *link = strtok_r(NULL, "|", &save);
    char *sequence = strtok_r(NULL, "|", &save);
    char *extra = strtok_r(NULL, "|", &save);
    if (!prefix || strcmp(prefix, "FOFP1") != 0 || !session || !mac ||
        !link || strlen(link) != 1 || !sequence || extra ||
        !fof_factory_probe_session_valid(session) ||
        !fof_factory_probe_mac_valid(mac) ||
        (link[0] != 'a' && link[0] != 'b')) {
        return false;
    }

    char *sequence_end = NULL;
    unsigned long parsed_sequence = strtoul(sequence, &sequence_end, 10);
    if (!sequence_end || *sequence_end != '\0' || parsed_sequence > UINT32_MAX) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->session, session, sizeof(out->session) - 1);
    uppercase_mac_copy(out->mac, mac);
    out->link = link[0];
    out->sequence = (uint32_t)parsed_sequence;
    return true;
}


bool fof_factory_probe_peer_observe(fof_factory_probe_peer_table_t *table,
                                    const char *self_mac,
                                    const char *expected_session,
                                    char received_link,
                                    const fof_factory_probe_frame_t *frame)
{
    if (!table || !frame || !fof_factory_probe_mac_valid(self_mac) ||
        !fof_factory_probe_session_valid(expected_session) ||
        !fof_factory_probe_session_valid(frame->session) ||
        !fof_factory_probe_mac_valid(frame->mac) ||
        strcmp(frame->session, expected_session) != 0 ||
        (received_link != 'a' && received_link != 'b')) {
        return false;
    }

    char self_upper[FOF_FACTORY_PROBE_MAC_TEXT + 1] = {0};
    char peer_upper[FOF_FACTORY_PROBE_MAC_TEXT + 1] = {0};
    uppercase_mac_copy(self_upper, self_mac);
    uppercase_mac_copy(peer_upper, frame->mac);
    if (strcmp(self_upper, peer_upper) == 0) {
        return false;
    }

    bool *has = received_link == 'a' ? &table->has_a : &table->has_b;
    char *slot = received_link == 'a' ? table->peer_a : table->peer_b;
    const char *other = received_link == 'a' ? table->peer_b : table->peer_a;
    bool other_has = received_link == 'a' ? table->has_b : table->has_a;
    if (other_has && strcmp(other, peer_upper) == 0) {
        return false;
    }
    if (*has) {
        return strcmp(slot, peer_upper) == 0;
    }
    strncpy(slot, peer_upper, FOF_FACTORY_PROBE_MAC_TEXT);
    slot[FOF_FACTORY_PROBE_MAC_TEXT] = '\0';
    *has = true;
    return true;
}


bool fof_factory_probe_report_build(
    const char *self_mac,
    const char *session,
    const fof_factory_probe_peer_table_t *table,
    char *out,
    size_t out_size)
{
    if (!table || !out || out_size == 0 ||
        !fof_factory_probe_mac_valid(self_mac) ||
        !fof_factory_probe_session_valid(session) ||
        (table->has_a && !fof_factory_probe_mac_valid(table->peer_a)) ||
        (table->has_b && !fof_factory_probe_mac_valid(table->peer_b)) ||
        (table->has_a && table->has_b &&
         strcmp(table->peer_a, table->peer_b) == 0)) {
        return false;
    }

    char mac[FOF_FACTORY_PROBE_MAC_TEXT + 1] = {0};
    uppercase_mac_copy(mac, self_mac);
    char peers[96] = {0};
    int peers_len = 0;
    if (table->has_a && table->has_b) {
        peers_len = snprintf(peers, sizeof(peers),
            "{\"a\":\"%s\",\"b\":\"%s\"}", table->peer_a, table->peer_b);
    } else if (table->has_a) {
        peers_len = snprintf(peers, sizeof(peers),
            "{\"a\":\"%s\"}", table->peer_a);
    } else if (table->has_b) {
        peers_len = snprintf(peers, sizeof(peers),
            "{\"b\":\"%s\"}", table->peer_b);
    } else {
        peers_len = snprintf(peers, sizeof(peers), "{}");
    }
    if (peers_len < 0 || (size_t)peers_len >= sizeof(peers)) {
        return false;
    }

    char canonical[224] = {0};
    int canonical_len = snprintf(canonical, sizeof(canonical),
        "{\"mac\":\"%s\",\"peers\":%s,\"schema\":%d,\"session\":\"%s\"}",
        mac, peers, FOF_FACTORY_PROBE_SCHEMA, session);
    if (canonical_len < 0 || (size_t)canonical_len >= sizeof(canonical)) {
        return false;
    }
    uint32_t crc = fof_factory_probe_crc32(canonical, (size_t)canonical_len);

    int written = snprintf(out, out_size,
        FOF_FACTORY_PROBE_PREFIX
        "{\"crc32\":\"%08lx\",\"mac\":\"%s\",\"peers\":%s,"
        "\"schema\":%d,\"session\":\"%s\"}\n",
        (unsigned long)crc, mac, peers, FOF_FACTORY_PROBE_SCHEMA, session);
    return written >= 0 && (size_t)written < out_size;
}
