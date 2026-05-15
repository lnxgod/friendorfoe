#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_DISPLAY_POLICY_VERSION 1
#define BADGE_DISPLAY_POLICY_CLASS_COUNT 13
#define BADGE_DISPLAY_POLICY_JSON_MAX 1536

typedef enum {
    BADGE_DISPLAY_CLASS_DRONE = 0,
    BADGE_DISPLAY_CLASS_META,
    BADGE_DISPLAY_CLASS_TRACKER,
    BADGE_DISPLAY_CLASS_WIFI_ATTACK,
    BADGE_DISPLAY_CLASS_SKIMMER,
    BADGE_DISPLAY_CLASS_CAMERA,
    BADGE_DISPLAY_CLASS_FLOCK,
    BADGE_DISPLAY_CLASS_LOCK,
    BADGE_DISPLAY_CLASS_HID,
    BADGE_DISPLAY_CLASS_BEACON,
    BADGE_DISPLAY_CLASS_EVENT_BADGE,
    BADGE_DISPLAY_CLASS_AURACAST,
    BADGE_DISPLAY_CLASS_SCANNER_STATUS,
} badge_display_policy_class_t;

typedef enum {
    BADGE_DISPLAY_LANE_OFF = 0,
    BADGE_DISPLAY_LANE_LOWER,
    BADGE_DISPLAY_LANE_TOP,
    BADGE_DISPLAY_LANE_BOTH,
} badge_display_lane_t;

typedef enum {
    BADGE_DISPLAY_PROX_PRESENT = 0,
    BADGE_DISPLAY_PROX_NEAR,
    BADGE_DISPLAY_PROX_CLOSE,
} badge_display_min_proximity_t;

typedef struct {
    bool enabled;
    badge_display_lane_t lane;
    badge_display_min_proximity_t min_proximity;
    uint8_t priority;
} badge_display_class_policy_t;

typedef struct {
    uint8_t version;
    badge_display_class_policy_t classes[BADGE_DISPLAY_POLICY_CLASS_COUNT];
} badge_display_policy_t;

void badge_display_policy_defaults(badge_display_policy_t *policy);
const char *badge_display_policy_class_key(badge_display_policy_class_t cls);
const char *badge_display_policy_class_label(badge_display_policy_class_t cls);
const char *badge_display_lane_name(badge_display_lane_t lane);
const char *badge_display_min_proximity_name(badge_display_min_proximity_t prox);
bool badge_display_policy_class_from_key(const char *key,
                                         badge_display_policy_class_t *out);
uint32_t badge_display_policy_hash(const badge_display_policy_t *policy);
bool badge_display_policy_parse_json(const char *json,
                                     badge_display_policy_t *out,
                                     char *err,
                                     size_t err_len);
size_t badge_display_policy_to_json(const badge_display_policy_t *policy,
                                    char *out,
                                    size_t out_len);
size_t badge_display_policy_to_command_json(const badge_display_policy_t *policy,
                                            uint32_t hash,
                                            char *out,
                                            size_t out_len);
badge_display_policy_class_t badge_display_policy_class_for_detection(
    const drone_detection_t *det);
badge_display_min_proximity_t badge_display_proximity_for_rssi(int8_t rssi);
bool badge_display_policy_is_safety_floor(badge_display_policy_class_t cls,
                                          badge_display_min_proximity_t prox,
                                          int score);
bool badge_display_policy_allows_class(const badge_display_policy_t *policy,
                                       badge_display_policy_class_t cls,
                                       badge_display_min_proximity_t prox,
                                       int score,
                                       bool *safety_floor);
bool badge_display_policy_allows_detection(const badge_display_policy_t *policy,
                                           const drone_detection_t *det,
                                           bool *safety_floor,
                                           badge_display_policy_class_t *cls_out);

#ifdef __cplusplus
}
#endif
