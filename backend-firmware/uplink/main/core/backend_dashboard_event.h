#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_detection_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t sequence;
    bool timestamp_valid;
    int64_t timestamp_epoch_ms;
    char id[64];
    char manufacturer[32];
    char model[32];
    char badge_label[24];
    char badge_class[24];
    char badge_entity_key[192];
    uint8_t source;
    float confidence;
    uint8_t threat_score;
    int8_t rssi;
    double distance_m;
    double aircraft_lat;
    double aircraft_lon;
    double operator_lat;
    double operator_lon;
    uint8_t scanner_slot_mask;
} backend_dashboard_event_t;

bool backend_dashboard_event_project(
    const backend_detection_observation_t *observation,
    backend_dashboard_event_t *out);

size_t backend_dashboard_event_encode_json(
    const backend_dashboard_event_t *event,
    char *output,
    size_t capacity);

size_t backend_dashboard_event_encode_fof_det(
    const backend_dashboard_event_t *event,
    char *output,
    size_t capacity);

#ifdef __cplusplus
}
#endif
