#include "mac_address_policy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int mac_hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static size_t mac_bounded_length(const char *input)
{
    if (!input) {
        return FOF_MAC_CANONICAL_BUFFER_SIZE;
    }
    size_t length = 0U;
    while (length < FOF_MAC_CANONICAL_BUFFER_SIZE &&
           input[length] != '\0') {
        length++;
    }
    return length;
}

static bool mac_parse_hex_pair(const char *input, uint8_t *value_out)
{
    int high = mac_hex_nibble(input[0]);
    int low = mac_hex_nibble(input[1]);
    if (high < 0 || low < 0 || !value_out) {
        return false;
    }
    *value_out = (uint8_t)((high << 4) | low);
    return true;
}

static bool mac_parse_compact(const char *input, uint8_t bytes[6])
{
    for (size_t i = 0U; i < 6U; ++i) {
        if (!mac_parse_hex_pair(input + (i * 2U), &bytes[i])) {
            return false;
        }
    }
    return true;
}

static bool mac_parse_octet_separated(const char *input,
                                      char separator,
                                      uint8_t bytes[6])
{
    for (size_t i = 0U; i < 6U; ++i) {
        size_t offset = i * 3U;
        if (!mac_parse_hex_pair(input + offset, &bytes[i]) ||
            (i < 5U && input[offset + 2U] != separator)) {
            return false;
        }
    }
    return true;
}

static bool mac_parse_cisco_dotted(const char *input, uint8_t bytes[6])
{
    static const size_t hex_offsets[6] = {0U, 2U, 5U, 7U, 10U, 12U};
    if (input[4] != '.' || input[9] != '.') {
        return false;
    }
    for (size_t i = 0U; i < 6U; ++i) {
        if (!mac_parse_hex_pair(input + hex_offsets[i], &bytes[i])) {
            return false;
        }
    }
    return true;
}

static char mac_upper_hex(uint8_t nibble)
{
    if (nibble < 10U) {
        return (char)((uint8_t)'0' + nibble);
    }
    return (char)((uint8_t)'A' + (uint8_t)(nibble - 10U));
}

bool fof_mac_normalize(
    const char *input,
    bool allow_empty,
    char output[FOF_MAC_CANONICAL_BUFFER_SIZE])
{
    if (!output) {
        return false;
    }
    if (!input) {
        output[0] = '\0';
        return false;
    }

    size_t length = mac_bounded_length(input);
    if (length == 0U) {
        output[0] = '\0';
        return allow_empty;
    }
    if (length >= FOF_MAC_CANONICAL_BUFFER_SIZE) {
        output[0] = '\0';
        return false;
    }

    uint8_t bytes[6] = {0};
    bool parsed = false;
    if (length == 12U) {
        parsed = mac_parse_compact(input, bytes);
    } else if (length == 17U && input[2] == ':') {
        parsed = mac_parse_octet_separated(input, ':', bytes);
    } else if (length == 17U && input[2] == '-') {
        parsed = mac_parse_octet_separated(input, '-', bytes);
    } else if (length == 14U) {
        parsed = mac_parse_cisco_dotted(input, bytes);
    }
    if (!parsed) {
        output[0] = '\0';
        return false;
    }

    for (size_t i = 0U; i < 6U; ++i) {
        size_t offset = i * 3U;
        output[offset] = mac_upper_hex((uint8_t)(bytes[i] >> 4U));
        output[offset + 1U] =
            mac_upper_hex((uint8_t)(bytes[i] & 0x0fU));
        if (i < 5U) {
            output[offset + 2U] = ':';
        }
    }
    output[FOF_MAC_CANONICAL_TEXT_SIZE] = '\0';
    return true;
}

bool fof_mac_is_canonical_upper(const char *input, bool allow_empty)
{
    if (!input) {
        return false;
    }
    char normalized[FOF_MAC_CANONICAL_BUFFER_SIZE];
    if (!fof_mac_normalize(input, allow_empty, normalized)) {
        return false;
    }
    return strcmp(input, normalized) == 0;
}
