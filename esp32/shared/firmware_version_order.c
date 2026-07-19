#include "firmware_version_order.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t core[3];
    const char *normalized;
} parsed_firmware_version_t;

static bool suffix_char_is_valid(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '-' || ch == '.';
}

static bool parse_component(const char **cursor, uint32_t *out)
{
    if (!cursor || !*cursor || !out || **cursor < '0' || **cursor > '9') {
        return false;
    }
    const char *p = *cursor;
    uint32_t value = 0;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *out = value;
    return true;
}

static bool parse_version(const char *input, parsed_firmware_version_t *out)
{
    if (!input || !out || input[0] == '\0') {
        return false;
    }
    const char *normalized = input;
    if (*normalized == 'v' || *normalized == 'V') {
        normalized++;
    }
    if (*normalized == '\0') {
        return false;
    }

    const char *p = normalized;
    uint32_t core[3] = {0};
    for (int i = 0; i < 3; ++i) {
        if (!parse_component(&p, &core[i])) {
            return false;
        }
        if (i < 2) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }

    if (*p != '\0') {
        if (*p != '-' || p[1] == '\0') {
            return false;
        }
        p++;
        for (; *p != '\0'; ++p) {
            if (!suffix_char_is_valid(*p)) {
                return false;
            }
        }
    }

    memcpy(out->core, core, sizeof(core));
    out->normalized = normalized;
    return true;
}

fof_firmware_version_relation_t fof_firmware_version_compare(
    const char *candidate,
    const char *current)
{
    parsed_firmware_version_t left = {0};
    parsed_firmware_version_t right = {0};
    if (!parse_version(candidate, &left) || !parse_version(current, &right)) {
        return FOF_VERSION_INVALID;
    }

    for (int i = 0; i < 3; ++i) {
        if (left.core[i] > right.core[i]) {
            return FOF_VERSION_NEWER;
        }
        if (left.core[i] < right.core[i]) {
            return FOF_VERSION_OLDER;
        }
    }

    return strcmp(left.normalized, right.normalized) == 0
        ? FOF_VERSION_EQUAL
        : FOF_VERSION_UNORDERED;
}

bool fof_firmware_version_is_strictly_newer(const char *candidate,
                                            const char *current)
{
    return fof_firmware_version_compare(candidate, current) ==
           FOF_VERSION_NEWER;
}
