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
#define BADGE_THREAT_DISPLAY_ID_LEN     8
#define BADGE_THREAT_VIEW_KEY_LEN       96
#define BADGE_SCANNER_STATUS_SSID_FRESH_S 30

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

typedef enum {
    BADGE_THREAT_DISPLAY_LANE_NONE = 0,
    BADGE_THREAT_DISPLAY_LANE_BLE,
    BADGE_THREAT_DISPLAY_LANE_WIFI,
} badge_threat_display_lane_t;

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
    char display_id[BADGE_THREAT_DISPLAY_ID_LEN];
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
uint32_t badge_threat_snapshot_count_active(const badge_threat_snapshot_t *snapshot,
                                            badge_threat_class_t cls,
                                            badge_threat_category_t category,
                                            bool include_stale);
uint32_t badge_threat_snapshot_drone_evidence_count(
    const badge_threat_snapshot_t *snapshot);
void badge_threat_format_drone_near_title(const badge_threat_snapshot_t *snapshot,
                                          char *out,
                                          size_t out_len);
uint32_t badge_threat_snapshot_entity_ordinal(const badge_threat_snapshot_t *snapshot,
                                              const badge_threat_snapshot_entity_t *item,
                                              badge_threat_class_t cls,
                                              badge_threat_category_t category,
                                              bool include_stale);
void badge_threat_format_drone_entity_title(const badge_threat_snapshot_t *snapshot,
                                            const badge_threat_snapshot_entity_t *item,
                                            char *out,
                                            size_t out_len);
bool badge_threat_snapshot_entity_is_remote_id_drone(
    const badge_threat_snapshot_entity_t *item);
bool badge_threat_snapshot_entity_is_meta_glasses(
    const badge_threat_snapshot_entity_t *item);
uint32_t badge_threat_snapshot_meta_glasses_count(
    const badge_threat_snapshot_t *snapshot);
const badge_threat_snapshot_entity_t *badge_threat_snapshot_best_meta_glasses(
    const badge_threat_snapshot_t *snapshot);
void badge_threat_format_meta_glasses_title(char *out, size_t out_len);
uint8_t badge_threat_snapshot_entity_proximity_percent(
    const badge_threat_snapshot_entity_t *item);
uint8_t badge_threat_heat_percent(uint8_t base_percent, uint32_t live_count);
uint16_t badge_threat_proximity_percent_to_rgb565(uint8_t percent);
bool badge_threat_snapshot_entity_view_key(
    const badge_threat_snapshot_entity_t *item,
    char *out,
    size_t out_len);
bool badge_threat_snapshot_entity_view_key_seen(
    const badge_threat_snapshot_entity_t *item,
    const char viewed_keys[][BADGE_THREAT_VIEW_KEY_LEN],
    size_t viewed_count);
badge_threat_display_lane_t badge_threat_snapshot_entity_display_lane(
    const badge_threat_snapshot_entity_t *item);
bool badge_threat_snapshot_should_show_lower_drone_evidence(
    const badge_threat_snapshot_t *snapshot,
    const badge_threat_snapshot_entity_t *item);
size_t badge_threat_marquee_offset(size_t text_len,
                                   size_t visible_chars,
                                   uint32_t frame,
                                   uint32_t step_frames);
bool badge_threat_status_ssid_is_fresh(const char *ssid, int64_t age_s);

#ifdef __cplusplus
}
#endif
