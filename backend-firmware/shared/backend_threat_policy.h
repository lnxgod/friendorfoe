#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_DRONE_SSID_LIVE_WINDOW_MS INT64_C(15000)
#define BACKEND_REMOTE_ID_LIVE_WINDOW_MS INT64_C(90000)
#define BACKEND_META_LIVE_WINDOW_MS INT64_C(90000)
#define BACKEND_THREAT_ENTITY_CAPACITY 64U
#define BACKEND_THREAT_ENTITY_KEY_CAPACITY 192U

typedef enum {
    BACKEND_THREAT_ENTITY_DRONE = 0,
    BACKEND_THREAT_ENTITY_META,
} backend_threat_entity_kind_t;

typedef struct {
    bool used;
    backend_threat_entity_kind_t kind;
    int64_t last_seen_ms;
    int64_t last_drone_ssid_ms;
    int64_t last_remote_id_ms;
    char key[BACKEND_THREAT_ENTITY_KEY_CAPACITY];
} backend_threat_entity_t;

typedef struct {
    int64_t last_drone_ssid_ms;
    int64_t last_remote_id_ms;
    int64_t last_meta_ms;
    uint16_t drone_count;
    uint16_t meta_count;
    backend_threat_entity_t entities[BACKEND_THREAT_ENTITY_CAPACITY];
} backend_threat_state_t;

typedef struct {
    bool drone_live;
    bool meta_live;
    uint16_t drone_count;
    uint16_t meta_count;
    int64_t drone_last_seen_age_ms;
    int64_t meta_last_seen_age_ms;
} backend_threat_snapshot_t;

void backend_threat_init(backend_threat_state_t *state);

void backend_threat_ingest(
    backend_threat_state_t *state,
    const drone_detection_t *detection,
    int64_t now_ms);

void backend_threat_snapshot(
    backend_threat_state_t *state,
    int64_t now_ms,
    backend_threat_snapshot_t *out);

#ifdef __cplusplus
}
#endif
