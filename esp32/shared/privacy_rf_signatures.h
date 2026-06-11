#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOF_PRIVACY_MATCH_PREFIX = 0,
    FOF_PRIVACY_MATCH_CONTAINS = 1,
    FOF_PRIVACY_MATCH_EXACT = 2,
} fof_privacy_match_type_t;

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
