#include "badge_con_protocol.h"

#include <string.h>

static const uint8_t BADGE_CON_SIPHASH_KEY[16] = {
    'F', 'o', 'F', '-', 'D', 'C', '3', '4',
    '-', 'C', 'O', 'N', 'C', 'R', 'U', 'D',
};

static const uint8_t BADGE_CON_UUID_LE[16] = {
    0x73, 0x72, 0x65, 0x67, 0x6E, 0x61, 0x68, 0x63,
    0x65, 0x6D, 0x61, 0x67, 0x01, 0x4C, 0xF3, 0xF0,
};

static bool role_is_valid(badge_con_role_t role)
{
    return role >= BADGE_CON_ROLE_NORMAL &&
        role <= BADGE_CON_ROLE_IMMUNE;
}

static bool role_super_is_valid(badge_con_role_t role, bool super)
{
    return role_is_valid(role) &&
        (!super || role == BADGE_CON_ROLE_INFECTED);
}

static bool identity_is_valid(uint32_t peer, uint8_t session)
{
    return peer != 0U && peer <= UINT32_C(0xFFFFFF) &&
        session != 0U;
}

static uint64_t load_u64_le(const uint8_t bytes[8])
{
    uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static uint64_t rotate_left(uint64_t value, unsigned bits)
{
    return (value << bits) | (value >> (64U - bits));
}

static void sip_round(uint64_t *v0,
                      uint64_t *v1,
                      uint64_t *v2,
                      uint64_t *v3)
{
    *v0 += *v1;
    *v1 = rotate_left(*v1, 13U);
    *v1 ^= *v0;
    *v0 = rotate_left(*v0, 32U);
    *v2 += *v3;
    *v3 = rotate_left(*v3, 16U);
    *v3 ^= *v2;
    *v0 += *v3;
    *v3 = rotate_left(*v3, 21U);
    *v3 ^= *v0;
    *v2 += *v1;
    *v1 = rotate_left(*v1, 17U);
    *v1 ^= *v2;
    *v2 = rotate_left(*v2, 32U);
}

uint64_t badge_con_siphash24(const uint8_t key[16],
                             const uint8_t *bytes,
                             size_t byte_count)
{
    if (!key || (!bytes && byte_count != 0U)) {
        return 0U;
    }

    uint64_t k0 = load_u64_le(key);
    uint64_t k1 = load_u64_le(key + 8U);
    uint64_t v0 = UINT64_C(0x736F6D6570736575) ^ k0;
    uint64_t v1 = UINT64_C(0x646F72616E646F6D) ^ k1;
    uint64_t v2 = UINT64_C(0x6C7967656E657261) ^ k0;
    uint64_t v3 = UINT64_C(0x7465646279746573) ^ k1;

    size_t offset = 0U;
    while (byte_count - offset >= 8U) {
        uint64_t message = load_u64_le(bytes + offset);
        v3 ^= message;
        sip_round(&v0, &v1, &v2, &v3);
        sip_round(&v0, &v1, &v2, &v3);
        v0 ^= message;
        offset += 8U;
    }

    uint64_t final = (uint64_t)byte_count << 56U;
    for (size_t index = 0U; offset + index < byte_count; ++index) {
        final |= (uint64_t)bytes[offset + index] << (index * 8U);
    }

    v3 ^= final;
    sip_round(&v0, &v1, &v2, &v3);
    sip_round(&v0, &v1, &v2, &v3);
    v0 ^= final;
    v2 ^= UINT64_C(0xFF);
    sip_round(&v0, &v1, &v2, &v3);
    sip_round(&v0, &v1, &v2, &v3);
    sip_round(&v0, &v1, &v2, &v3);
    sip_round(&v0, &v1, &v2, &v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

bool badge_con_build_service_payload(
    badge_con_role_t role,
    bool super,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_SERVICE_PAYLOAD_BYTES])
{
    if (!out || !role_super_is_valid(role, super) ||
        !identity_is_valid(peer, session)) {
        return false;
    }

    uint8_t payload[BADGE_CON_SERVICE_PAYLOAD_BYTES] = {
        (uint8_t)((BADGE_CON_PROTOCOL_VERSION << 4U) |
                  (super ? 0x04U : 0U) |
                  (uint8_t)role),
        BADGE_CON_ROUND,
        (uint8_t)(peer & 0xFFU),
        (uint8_t)((peer >> 8U) & 0xFFU),
        (uint8_t)((peer >> 16U) & 0xFFU),
        session,
        sequence,
        0U,
        0U,
        0U,
    };
    uint64_t tag = badge_con_siphash24(
        BADGE_CON_SIPHASH_KEY, payload, 7U);
    payload[7] = (uint8_t)(tag & 0xFFU);
    payload[8] = (uint8_t)((tag >> 8U) & 0xFFU);
    payload[9] = (uint8_t)((tag >> 16U) & 0xFFU);
    memcpy(out, payload, sizeof(payload));
    return true;
}

bool badge_con_build_legacy_advertisement(
    badge_con_role_t role,
    bool super,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_LEGACY_ADV_BYTES])
{
    if (!out) {
        return false;
    }

    uint8_t advertisement[BADGE_CON_LEGACY_ADV_BYTES] = {
        0x02U, 0x01U, 0x06U,
        0x1BU, 0x21U,
    };
    memcpy(advertisement + 5U, BADGE_CON_UUID_LE,
           sizeof(BADGE_CON_UUID_LE));
    if (!badge_con_build_service_payload(
            role, super, peer, session, sequence, advertisement + 21U)) {
        return false;
    }
    memcpy(out, advertisement, sizeof(advertisement));
    return true;
}

static bool uuid_matches(const uint8_t *bytes, size_t byte_count)
{
    return bytes && byte_count >= sizeof(BADGE_CON_UUID_LE) &&
        memcmp(bytes, BADGE_CON_UUID_LE,
               sizeof(BADGE_CON_UUID_LE)) == 0;
}

static badge_con_frame_result_t parse_claimed_payload(
    const uint8_t payload[BADGE_CON_SERVICE_PAYLOAD_BYTES],
    int8_t rssi,
    badge_con_packet_t *out)
{
    if (!payload || !out) {
        return BADGE_CON_FRAME_INVALID;
    }

    uint8_t version_role = payload[0];
    uint8_t version = version_role >> 4U;
    uint8_t role_value = version_role & 0x03U;
    bool super = (version_role & 0x04U) != 0U;
    if (version != BADGE_CON_PROTOCOL_VERSION ||
        (version_role & 0x08U) != 0U ||
        role_value > (uint8_t)BADGE_CON_ROLE_IMMUNE ||
        !role_super_is_valid((badge_con_role_t)role_value, super) ||
        payload[1] != BADGE_CON_ROUND) {
        return BADGE_CON_FRAME_INVALID;
    }

    uint32_t peer = (uint32_t)payload[2] |
        ((uint32_t)payload[3] << 8U) |
        ((uint32_t)payload[4] << 16U);
    uint8_t session = payload[5];
    if (!identity_is_valid(peer, session)) {
        return BADGE_CON_FRAME_INVALID;
    }

    uint64_t tag = badge_con_siphash24(
        BADGE_CON_SIPHASH_KEY, payload, 7U);
    uint8_t tag_difference =
        (uint8_t)(payload[7] ^ (uint8_t)(tag & 0xFFU));
    tag_difference |=
        (uint8_t)(payload[8] ^ (uint8_t)((tag >> 8U) & 0xFFU));
    tag_difference |=
        (uint8_t)(payload[9] ^ (uint8_t)((tag >> 16U) & 0xFFU));
    if (tag_difference != 0U) {
        return BADGE_CON_FRAME_INVALID;
    }

    badge_con_packet_t parsed = {
        .version = version,
        .round = payload[1],
        .role = (badge_con_role_t)role_value,
        .super = super,
        .peer = peer,
        .session = session,
        .sequence = payload[6],
        .rssi = rssi,
    };
    *out = parsed;
    return BADGE_CON_FRAME_VALID;
}

badge_con_frame_result_t badge_con_parse_advertisement(
    const uint8_t *advertisement,
    size_t advertisement_size,
    int8_t rssi,
    badge_con_packet_t *out)
{
    if (!advertisement || advertisement_size == 0U) {
        return BADGE_CON_FRAME_NOT_GAME;
    }

    size_t offset = 0U;
    while (offset < advertisement_size) {
        uint8_t structure_length = advertisement[offset];
        if (structure_length == 0U) {
            break;
        }

        size_t available = advertisement_size - offset - 1U;
        if (available < structure_length) {
            if (available >= 17U &&
                advertisement[offset + 1U] == 0x21U &&
                uuid_matches(advertisement + offset + 2U,
                             available - 1U)) {
                return BADGE_CON_FRAME_INVALID;
            }
            return BADGE_CON_FRAME_NOT_GAME;
        }

        uint8_t type = advertisement[offset + 1U];
        if (type == 0x21U && structure_length >= 17U &&
            uuid_matches(advertisement + offset + 2U,
                         structure_length - 1U)) {
            if (structure_length != 27U) {
                return BADGE_CON_FRAME_INVALID;
            }
            return parse_claimed_payload(
                advertisement + offset + 18U, rssi, out);
        }
        offset += (size_t)structure_length + 1U;
    }
    return BADGE_CON_FRAME_NOT_GAME;
}

static char hex_upper(uint8_t value)
{
    value &= 0x0FU;
    return value < 10U
        ? (char)('0' + value)
        : (char)('A' + (value - 10U));
}

static void render_hex(uint32_t value, unsigned digits, char *out)
{
    for (unsigned index = 0U; index < digits; ++index) {
        unsigned shift = (digits - index - 1U) * 4U;
        out[index] = hex_upper((uint8_t)(value >> shift));
    }
}

static bool parse_hex(const uint8_t *bytes,
                      size_t byte_count,
                      uint32_t *value_out)
{
    if (!bytes || byte_count == 0U || !value_out) {
        return false;
    }
    uint32_t value = 0U;
    for (size_t index = 0U; index < byte_count; ++index) {
        uint8_t byte = bytes[index];
        uint8_t nibble;
        if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
            nibble = (uint8_t)(byte - (uint8_t)'0');
        } else if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
            nibble = (uint8_t)(10U + byte - (uint8_t)'A');
        } else {
            return false;
        }
        value = (value << 4U) | nibble;
    }
    *value_out = value;
    return true;
}

static bool packet_is_wire_valid(const badge_con_packet_t *packet)
{
    return packet &&
        packet->version == BADGE_CON_PROTOCOL_VERSION &&
        packet->round == BADGE_CON_ROUND &&
        role_super_is_valid(packet->role, packet->super) &&
        identity_is_valid(packet->peer, packet->session) &&
        packet->rssi <= -1 && packet->rssi >= -127;
}

bool badge_con_render_uart_line(const badge_con_packet_t *packet,
                                char *out,
                                size_t out_size,
                                size_t *wire_size_out)
{
    static const char prefix[] = "FOF_CRUD:";
    if (!packet_is_wire_valid(packet) || !out ||
        out_size < BADGE_CON_UART_BUFFER_BYTES) {
        return false;
    }

    char line[BADGE_CON_UART_BUFFER_BYTES];
    memcpy(line, prefix, sizeof(prefix) - 1U);
    line[9] = (char)('0' + packet->version);
    line[10] = ',';
    render_hex(packet->round, 2U, line + 11U);
    line[13] = ',';
    uint8_t wire_role = (uint8_t)packet->role |
        (packet->super ? 0x04U : 0U);
    line[14] = (char)('0' + wire_role);
    line[15] = ',';
    render_hex(packet->peer, 6U, line + 16U);
    line[22] = ',';
    render_hex(packet->session, 2U, line + 23U);
    line[25] = ',';
    render_hex(packet->sequence, 2U, line + 26U);
    line[28] = ',';
    line[29] = '-';
    unsigned magnitude = (unsigned)(-(int)packet->rssi);
    line[30] = (char)('0' + (magnitude / 100U));
    line[31] = (char)('0' + ((magnitude / 10U) % 10U));
    line[32] = (char)('0' + (magnitude % 10U));
    line[33] = '\n';
    line[34] = '\0';

    memcpy(out, line, sizeof(line));
    if (wire_size_out) {
        *wire_size_out = BADGE_CON_UART_WIRE_BYTES;
    }
    return true;
}

bool badge_con_parse_uart_line(const uint8_t *bytes,
                               size_t byte_count,
                               badge_con_packet_t *out)
{
    static const uint8_t prefix[] = "FOF_CRUD:";
    if (!bytes || !out || byte_count != BADGE_CON_UART_LINE_CHARS ||
        memcmp(bytes, prefix, sizeof(prefix) - 1U) != 0 ||
        bytes[9] != (uint8_t)'1' ||
        bytes[10] != (uint8_t)',' ||
        bytes[13] != (uint8_t)',' ||
        bytes[15] != (uint8_t)',' ||
        bytes[22] != (uint8_t)',' ||
        bytes[25] != (uint8_t)',' ||
        bytes[28] != (uint8_t)',' ||
        bytes[29] != (uint8_t)'-') {
        return false;
    }

    uint32_t round = 0U;
    uint32_t peer = 0U;
    uint32_t session = 0U;
    uint32_t sequence = 0U;
    uint8_t wire_role = (uint8_t)(bytes[14] - (uint8_t)'0');
    bool super = (wire_role & 0x04U) != 0U;
    badge_con_role_t role =
        (badge_con_role_t)(wire_role & 0x03U);
    if (!parse_hex(bytes + 11U, 2U, &round) ||
        !parse_hex(bytes + 16U, 6U, &peer) ||
        !parse_hex(bytes + 23U, 2U, &session) ||
        !parse_hex(bytes + 26U, 2U, &sequence) ||
        round != BADGE_CON_ROUND ||
        bytes[14] < (uint8_t)'0' || bytes[14] > (uint8_t)'7' ||
        !role_super_is_valid(role, super) ||
        !identity_is_valid(peer, (uint8_t)session)) {
        return false;
    }

    if (bytes[30] < (uint8_t)'0' || bytes[30] > (uint8_t)'9' ||
        bytes[31] < (uint8_t)'0' || bytes[31] > (uint8_t)'9' ||
        bytes[32] < (uint8_t)'0' || bytes[32] > (uint8_t)'9') {
        return false;
    }
    unsigned magnitude =
        (unsigned)(bytes[30] - (uint8_t)'0') * 100U +
        (unsigned)(bytes[31] - (uint8_t)'0') * 10U +
        (unsigned)(bytes[32] - (uint8_t)'0');
    if (magnitude == 0U || magnitude > 127U) {
        return false;
    }

    badge_con_packet_t parsed = {
        .version = BADGE_CON_PROTOCOL_VERSION,
        .round = (uint8_t)round,
        .role = role,
        .super = super,
        .peer = peer,
        .session = (uint8_t)session,
        .sequence = (uint8_t)sequence,
        .rssi = (int8_t)(-(int)magnitude),
    };
    *out = parsed;
    return true;
}

static bool render_self_message(const char *prefix,
                                uint32_t peer,
                                uint8_t session,
                                char *out,
                                size_t out_size)
{
    static const char middle[] = "\",\"session\":\"";
    static const char suffix[] = "\"}";
    if (!prefix || !out || !identity_is_valid(peer, session)) {
        return false;
    }

    size_t prefix_size = strlen(prefix);
    size_t middle_size = sizeof(middle) - 1U;
    size_t suffix_size = sizeof(suffix) - 1U;
    size_t message_size =
        prefix_size + 6U + middle_size + 2U + suffix_size;
    if (out_size <= message_size) {
        return false;
    }

    char message[96];
    if (message_size + 1U > sizeof(message)) {
        return false;
    }
    size_t offset = 0U;
    memcpy(message + offset, prefix, prefix_size);
    offset += prefix_size;
    render_hex(peer, 6U, message + offset);
    offset += 6U;
    memcpy(message + offset, middle, middle_size);
    offset += middle_size;
    render_hex(session, 2U, message + offset);
    offset += 2U;
    memcpy(message + offset, suffix, suffix_size);
    offset += suffix_size;
    message[offset] = '\0';
    memcpy(out, message, offset + 1U);
    return true;
}

bool badge_con_render_self_command(uint32_t peer,
                                   uint8_t session,
                                   char *out,
                                   size_t out_size)
{
    static const char prefix[] =
        "{\"type\":\"crud_self\",\"v\":1,\"round\":34,\"peer\":\"";
    return render_self_message(prefix, peer, session, out, out_size);
}

bool badge_con_render_self_ack(uint32_t peer,
                               uint8_t session,
                               char *out,
                               size_t out_size)
{
    static const char prefix[] =
        "{\"type\":\"crud_self_ack\",\"v\":1,\"round\":34,\"peer\":\"";
    return render_self_message(prefix, peer, session, out, out_size);
}
