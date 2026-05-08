#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_THREAT_KEY_LEN            96
#define BADGE_THREAT_LABEL_LEN          24
#define BADGE_THREAT_DETAIL_LEN         32
#define BADGE_THREAT_MAX_ENTITIES       32
#define BADGE_THREAT_SNAPSHOT_ENTITIES  12
#define BADGE_THREAT_TICKER_LEN         96

typedef enum {
    BADGE_THREAT_IGNORE = -1,
    BADGE_THREAT_DRONE = 0,
    BADGE_THREAT_META,
    BADGE_THREAT_TRACKER,
    BADGE_THREAT_WIFI_ANOMALY,
    BADGE_THREAT_BLE,
    BADGE_THREAT_OTHER,
    BADGE_THREAT_CLASS_COUNT
} badge_threat_class_t;

typedef enum {
    BADGE_THREAT_PROX_UNKNOWN = 0,
    BADGE_THREAT_PROX_PRESENT,
    BADGE_THREAT_PROX_NEARBY,
    BADGE_THREAT_PROX_CLOSE,
} badge_threat_proximity_t;

typedef enum {
    BADGE_THREAT_CATEGORY_NONE = 0,
    BADGE_THREAT_CATEGORY_DRONE,
    BADGE_THREAT_CATEGORY_SSID,
    BADGE_THREAT_CATEGORY_FLOCK,
    BADGE_THREAT_CATEGORY_GLASS,
    BADGE_THREAT_CATEGORY_SKIM,
    BADGE_THREAT_CATEGORY_WIFI,
    BADGE_THREAT_CATEGORY_TAG_CLOSE,
    BADGE_THREAT_CATEGORY_PRIVACY,
} badge_threat_category_t;

typedef struct {
    badge_threat_class_t cls;
    badge_threat_category_t category;
    char key[BADGE_THREAT_KEY_LEN];
    char label[BADGE_THREAT_LABEL_LEN];
    char detail[BADGE_THREAT_DETAIL_LEN];
    double latitude;
    double longitude;
    float altitude_m;
    double operator_lat;
    double operator_lon;
    char operator_id[32];
    bool has_location;
    bool has_operator_location;
    float base_score;
    uint32_t ttl_ms;
    uint8_t evidence_quality;
    int display_rank;
    bool lcd_visible;
} badge_threat_event_t;

typedef struct {
    bool active;
    badge_threat_class_t cls;
    badge_threat_category_t category;
    char key[BADGE_THREAT_KEY_LEN];
    char label[BADGE_THREAT_LABEL_LEN];
    char detail[BADGE_THREAT_DETAIL_LEN];
    int64_t first_seen_ms;
    int64_t last_seen_ms;
    double latitude;
    double longitude;
    float altitude_m;
    double operator_lat;
    double operator_lon;
    char operator_id[32];
    bool has_location;
    bool has_operator_location;
    float peak_confidence;
    float base_score;
    float current_score;
    uint8_t evidence_quality;
    int display_rank;
    int8_t strongest_rssi;
    uint32_t event_count;
} badge_threat_entity_t;

typedef struct {
    bool active;
    bool stale;
    badge_threat_class_t cls;
    badge_threat_category_t category;
    char label[BADGE_THREAT_LABEL_LEN];
    char detail[BADGE_THREAT_DETAIL_LEN];
    double latitude;
    double longitude;
    float altitude_m;
    double operator_lat;
    double operator_lon;
    char operator_id[32];
    bool has_location;
    bool has_operator_location;
    int age_s;
    int last_seen_s;
    int score;
    uint8_t evidence_quality;
    int display_rank;
    int8_t rssi;
    int8_t best_rssi;
    uint32_t event_count;
    uint32_t seen_count;
    uint32_t group_count;
    badge_threat_proximity_t proximity_level;
} badge_threat_snapshot_entity_t;

typedef struct {
    uint32_t active_counts[BADGE_THREAT_CLASS_COUNT];
    badge_threat_snapshot_entity_t entities[BADGE_THREAT_SNAPSHOT_ENTITIES];
    int entity_count;
    float threat_score;
    float class_scores[BADGE_THREAT_CLASS_COUNT];
    badge_threat_class_t dominant_class;
    badge_threat_proximity_t dominant_proximity;
    uint16_t color_rgb565;
    char top_label[BADGE_THREAT_LABEL_LEN];
    char ticker[BADGE_THREAT_TICKER_LEN];
} badge_threat_snapshot_t;

typedef struct {
    badge_threat_entity_t entities[BADGE_THREAT_MAX_ENTITIES];
    float display_score;
    int64_t last_snapshot_ms;
} badge_threat_state_t;

void badge_threat_state_init(badge_threat_state_t *state);

bool badge_threat_classify_detection(const drone_detection_t *det,
                                     badge_threat_event_t *event);

bool badge_threat_state_ingest(badge_threat_state_t *state,
                               const drone_detection_t *det,
                               int64_t now_ms,
                               badge_threat_event_t *event_out);

void badge_threat_state_snapshot(badge_threat_state_t *state,
                                 int64_t now_ms,
                                 badge_threat_snapshot_t *out);

const char *badge_threat_class_code(badge_threat_class_t cls);
const char *badge_threat_class_name(badge_threat_class_t cls);
const char *badge_threat_category_code(badge_threat_category_t category);
const char *badge_threat_category_name(badge_threat_category_t category);
uint16_t badge_threat_score_to_rgb565(float score);
bool badge_threat_label_is_lcd_safe(const char *label);

#ifdef __cplusplus
}
#endif
