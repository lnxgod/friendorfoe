#include "backend_threat_policy.h"

#include <limits.h>
#include <string.h>

#include "detection_policy.h"

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z'
        ? (char)(value - 'A' + 'a')
        : value;
}

static bool contains_nocase(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || *needle == '\0') {
        return false;
    }
    for (const char *start = text; *start != '\0'; ++start) {
        const char *left = start;
        const char *right = needle;
        while (*left != '\0' && *right != '\0' &&
               ascii_lower(*left) == ascii_lower(*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

static bool text_mentions_drone(const char *text)
{
    return contains_nocase(text, "drone") ||
           contains_nocase(text, "dji") ||
           contains_nocase(text, "remote id") ||
           contains_nocase(text, "remoteid");
}

static bool text_mentions_meta(const char *text)
{
    return contains_nocase(text, "meta glasses") ||
           contains_nocase(text, "ray-ban") ||
           contains_nocase(text, "rayban") ||
           contains_nocase(text, "rb meta") ||
           contains_nocase(text, "wayfarer") ||
           contains_nocase(text, "oakley") ||
           contains_nocase(text, "luxottica") ||
           contains_nocase(text, "name:meta_glasses") ||
           contains_nocase(text, "0x0d53") ||
           contains_nocase(text, "0xfd5f");
}

static bool confirmed_remote_id(uint8_t source)
{
    return source == DETECTION_SRC_BLE_RID ||
           source == DETECTION_SRC_WIFI_DJI_IE ||
           source == DETECTION_SRC_WIFI_BEACON;
}

static bool ambient_demo_ssid(const drone_detection_t *detection)
{
    const bool demo = contains_nocase(detection->ssid, "teamcharitycase") ||
        contains_nocase(detection->ssid, "friendorfoe") ||
        contains_nocase(detection->ssid, "fof-") ||
        contains_nocase(detection->ssid, "fof_");
    if (!demo) {
        return false;
    }
    return !text_mentions_drone(detection->ssid) &&
           !text_mentions_drone(detection->manufacturer) &&
           !text_mentions_drone(detection->model) &&
           !text_mentions_drone(detection->class_reason);
}

static bool candidate_drone(const drone_detection_t *detection)
{
    if (confirmed_remote_id(detection->source)) {
        return true;
    }
    if (ambient_demo_ssid(detection)) {
        return false;
    }
    if (detection->source == DETECTION_SRC_WIFI_SSID &&
        detection->ssid[0] != '\0') {
        return true;
    }
    if (detection->source == DETECTION_SRC_WIFI_OUI &&
        detection->manufacturer[0] != '\0') {
        return true;
    }
    const bool candidate_source =
        detection->source == DETECTION_SRC_WIFI_SSID ||
        detection->source == DETECTION_SRC_WIFI_OUI;
    const bool named_drone = text_mentions_drone(detection->manufacturer) ||
        text_mentions_drone(detection->model) ||
        text_mentions_drone(detection->class_reason);
    if (!candidate_source && !named_drone) {
        return false;
    }
    return detection->model[0] != '\0' ||
           detection->self_id_text[0] != '\0' ||
           detection->operator_id[0] != '\0' ||
           detection->ssid[0] != '\0' ||
           text_mentions_drone(detection->manufacturer) ||
           text_mentions_drone(detection->model) ||
           text_mentions_drone(detection->class_reason);
}

static bool ascii_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool prefixed_hash(const char *text)
{
    if (text == NULL || strncmp(text, "FP:", 3U) != 0) {
        return false;
    }
    for (size_t index = 0U; index < 8U; ++index) {
        if (!ascii_hex(text[index + 3U])) {
            return false;
        }
    }
    return text[11] == '\0' || text[11] == ':';
}

static bool has_ble_identity(const drone_detection_t *detection)
{
    return detection->bssid[0] != '\0' ||
           prefixed_hash(detection->model) ||
           prefixed_hash(detection->drone_id);
}

static bool meta_service(const drone_detection_t *detection)
{
    uint8_t count = detection->ble_svc_uuid_count;
    if (count > 4U) {
        count = 4U;
    }
    for (uint8_t index = 0U; index < count; ++index) {
        if (detection->ble_service_uuids[index] == UINT16_C(0xFD5F)) {
            return true;
        }
    }
    return contains_nocase(detection->ble_svc_uuids_raw, "fd5f");
}

static bool meta_evidence(const drone_detection_t *detection)
{
    return text_mentions_meta(detection->manufacturer) ||
           text_mentions_meta(detection->model) ||
           text_mentions_meta(detection->ble_name) ||
           text_mentions_meta(detection->class_reason) ||
           detection->ble_company_id == UINT16_C(0x0D53) ||
           meta_service(detection);
}

static bool candidate_meta(const drone_detection_t *detection)
{
    if (detection->source != DETECTION_SRC_BLE_FINGERPRINT ||
        !meta_evidence(detection)) {
        return false;
    }
    const bool identity = has_ble_identity(detection);
    const bool status_without_identity =
        (strncmp(detection->drone_id, "status:ble:meta", 15U) == 0 ||
         contains_nocase(detection->class_reason, "status:meta")) &&
        !identity;
    const bool detector_weak = !identity &&
        (contains_nocase(detection->class_reason, "weak_meta") ||
         contains_nocase(detection->class_reason, "glasses_detector") ||
         strncmp(detection->drone_id, "meta:weak", 9U) == 0);
    return !status_without_identity && !detector_weak;
}

static bool live_at(
    int64_t seen_ms,
    int64_t now_ms,
    int64_t window_ms)
{
    if (seen_ms < 0) {
        return false;
    }
    if (now_ms < seen_ms) {
        return true;
    }
    return now_ms - seen_ms <= window_ms;
}

static int64_t age_at(int64_t seen_ms, int64_t now_ms)
{
    if (seen_ms < 0) {
        return -1;
    }
    return now_ms < seen_ms ? 0 : now_ms - seen_ms;
}

static void saturating_increment(uint16_t *value)
{
    if (*value < UINT16_MAX) {
        ++*value;
    }
}

static bool entity_live(
    const backend_threat_entity_t *entity,
    int64_t now_ms)
{
    if (entity->kind == BACKEND_THREAT_ENTITY_META) {
        return live_at(
            entity->last_seen_ms, now_ms, BACKEND_META_LIVE_WINDOW_MS);
    }
    if (entity->kind != BACKEND_THREAT_ENTITY_DRONE) {
        return false;
    }
    return live_at(
               entity->last_drone_ssid_ms,
               now_ms,
               BACKEND_DRONE_SSID_LIVE_WINDOW_MS) ||
           live_at(
               entity->last_remote_id_ms,
               now_ms,
               BACKEND_REMOTE_ID_LIVE_WINDOW_MS);
}

static void recompute_live_counts(
    backend_threat_state_t *state,
    int64_t now_ms)
{
    state->drone_count = 0U;
    state->meta_count = 0U;
    for (size_t index = 0U;
         index < BACKEND_THREAT_ENTITY_CAPACITY;
         ++index) {
        backend_threat_entity_t *entity = &state->entities[index];
        if (!entity->used) {
            continue;
        }
        if (!entity_live(entity, now_ms)) {
            memset(entity, 0, sizeof(*entity));
            continue;
        }
        if (entity->kind == BACKEND_THREAT_ENTITY_META) {
            saturating_increment(&state->meta_count);
        } else {
            saturating_increment(&state->drone_count);
        }
    }
}

static bool make_entity_key(
    const drone_detection_t *detection,
    backend_threat_entity_kind_t kind,
    char out[BACKEND_THREAT_ENTITY_KEY_CAPACITY])
{
    char identity[256] = {0};
    if (!fof_policy_detection_identity_key(
            detection, identity, sizeof(identity))) {
        return false;
    }
    const size_t identity_length = strlen(identity);
    if (identity_length == 0U ||
        identity_length + 3U > BACKEND_THREAT_ENTITY_KEY_CAPACITY) {
        return false;
    }
    const char prefix = kind == BACKEND_THREAT_ENTITY_DRONE ? 'D' : 'M';
    out[0] = prefix;
    out[1] = ':';
    memcpy(&out[2], identity, identity_length + 1U);
    return true;
}

static backend_threat_entity_t *find_entity(
    backend_threat_state_t *state,
    const char *key)
{
    for (size_t index = 0U;
         index < BACKEND_THREAT_ENTITY_CAPACITY;
         ++index) {
        backend_threat_entity_t *entity = &state->entities[index];
        if (entity->used && strcmp(entity->key, key) == 0) {
            return entity;
        }
    }
    return NULL;
}

static backend_threat_entity_t *select_entity_slot(
    backend_threat_state_t *state)
{
    backend_threat_entity_t *oldest = NULL;
    for (size_t index = 0U;
         index < BACKEND_THREAT_ENTITY_CAPACITY;
         ++index) {
        backend_threat_entity_t *entity = &state->entities[index];
        if (!entity->used) {
            return entity;
        }
        if (oldest == NULL || entity->last_seen_ms < oldest->last_seen_ms) {
            oldest = entity;
        }
    }
    return oldest;
}

static void refresh_entity(
    backend_threat_state_t *state,
    const drone_detection_t *detection,
    backend_threat_entity_kind_t kind,
    bool remote_id,
    int64_t now_ms)
{
    char key[BACKEND_THREAT_ENTITY_KEY_CAPACITY] = {0};
    if (!make_entity_key(detection, kind, key)) {
        return;
    }
    backend_threat_entity_t *entity = find_entity(state, key);
    if (entity == NULL) {
        entity = select_entity_slot(state);
        if (entity == NULL) {
            return;
        }
        memset(entity, 0, sizeof(*entity));
        entity->used = true;
        entity->kind = kind;
        entity->last_seen_ms = -1;
        entity->last_drone_ssid_ms = -1;
        entity->last_remote_id_ms = -1;
        memcpy(entity->key, key, strlen(key) + 1U);
    }
    if (now_ms > entity->last_seen_ms) {
        entity->last_seen_ms = now_ms;
    }
    if (kind == BACKEND_THREAT_ENTITY_DRONE) {
        int64_t *evidence_last = remote_id
            ? &entity->last_remote_id_ms
            : &entity->last_drone_ssid_ms;
        if (now_ms > *evidence_last) {
            *evidence_last = now_ms;
        }
    }
}

void backend_threat_init(backend_threat_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->last_drone_ssid_ms = -1;
    state->last_remote_id_ms = -1;
    state->last_meta_ms = -1;
}

void backend_threat_ingest(
    backend_threat_state_t *state,
    const drone_detection_t *detection,
    int64_t now_ms)
{
    if (state == NULL || detection == NULL || now_ms < 0) {
        return;
    }
    recompute_live_counts(state, now_ms);
    if (candidate_drone(detection)) {
        const bool remote_id = confirmed_remote_id(detection->source);
        int64_t *last = remote_id
            ? &state->last_remote_id_ms
            : &state->last_drone_ssid_ms;
        if (now_ms > *last) {
            *last = now_ms;
        }
        refresh_entity(
            state,
            detection,
            BACKEND_THREAT_ENTITY_DRONE,
            remote_id,
            now_ms);
    } else if (candidate_meta(detection)) {
        if (now_ms > state->last_meta_ms) {
            state->last_meta_ms = now_ms;
        }
        refresh_entity(
            state,
            detection,
            BACKEND_THREAT_ENTITY_META,
            false,
            now_ms);
    }
    recompute_live_counts(state, now_ms);
}

void backend_threat_snapshot(
    backend_threat_state_t *state,
    int64_t now_ms,
    backend_threat_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->drone_last_seen_age_ms = -1;
    out->meta_last_seen_age_ms = -1;
    if (state == NULL) {
        return;
    }
    if (now_ms < 0) {
        now_ms = 0;
    }
    recompute_live_counts(state, now_ms);
    out->drone_live = state->drone_count != 0U;
    out->meta_live = state->meta_count != 0U;
    out->drone_count = state->drone_count;
    out->meta_count = state->meta_count;
    const int64_t latest_drone =
        state->last_drone_ssid_ms > state->last_remote_id_ms
            ? state->last_drone_ssid_ms
            : state->last_remote_id_ms;
    out->drone_last_seen_age_ms = age_at(latest_drone, now_ms);
    out->meta_last_seen_age_ms = age_at(state->last_meta_ms, now_ms);
}
