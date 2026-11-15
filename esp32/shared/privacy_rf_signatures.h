#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOF_PRIVACY_MATCH_PREFIX = 0,
    FOF_PRIVACY_MATCH_CONTAINS = 1,
    FOF_PRIVACY_MATCH_EXACT = 2,
    FOF_PRIVACY_MATCH_PREFIX_HEX4 = 3,
} fof_privacy_match_type_t;

/* Camera setup suffix documented by TP-Link, shared with the standalone BLE scanner. */
static inline bool fof_privacy_is_hex4_suffix(const char *s)
{
    for (int i = 0; i < 4; ++i) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return s[4] == '\0';
}

typedef struct {
    const char *pattern;
    fof_privacy_match_type_t match_type;
    const char *manufacturer;
    const char *device_type;
    const char *privacy_kind;
    const char *class_reason;
    float confidence;
    bool has_camera;
    bool attack_tool;
} fof_privacy_wifi_signature_t;

const fof_privacy_wifi_signature_t *fof_privacy_match_wifi_ssid(const char *ssid);
const fof_privacy_wifi_signature_t *fof_privacy_wifi_signatures(size_t *count);
bool fof_privacy_pattern_is_banned_broad(const char *pattern);

#ifdef __cplusplus
}
#endif
