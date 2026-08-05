#include "backend_live_entities.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "backend_threat_policy.h"
#include "detection_types.h"

static bool bounded_text(const char *text, size_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static bool supported_event(const backend_dashboard_event_t *event)
{
    if (event == NULL ||
        !bounded_text(event->id, sizeof(event->id)) ||
        !bounded_text(event->manufacturer, sizeof(event->manufacturer)) ||
        !bounded_text(event->model, sizeof(event->model)) ||
        !bounded_text(event->badge_label, sizeof(event->badge_label)) ||
        !bounded_text(event->badge_entity_key,
                      sizeof(event->badge_entity_key)) ||
        event->badge_entity_key[0] == '\0' ||
        !bounded_text(event->badge_class, sizeof(event->badge_class)) ||
        !bounded_text(event->operator_id, sizeof(event->operator_id)) ||
        !isfinite(event->confidence) || !isfinite(event->altitude_m) ||
        !isfinite(event->aircraft_lat) || !isfinite(event->aircraft_lon) ||
        !isfinite(event->operator_lat) || !isfinite(event->operator_lon)) {
        return false;
    }
    return strcmp(event->badge_class, "drone") == 0 ||
           strcmp(event->badge_class, "meta") == 0 ||
           strcmp(event->badge_class, "meta_glasses") == 0;
}

static bool meta_event(const backend_dashboard_event_t *event)
{
    return strcmp(event->badge_class, "meta") == 0 ||
           strcmp(event->badge_class, "meta_glasses") == 0;
}

static int64_t live_window_ms(const backend_dashboard_event_t *event)
{
    if (meta_event(event)) {
        return BACKEND_META_LIVE_WINDOW_MS;
    }
    if (event->source == DETECTION_SRC_BLE_RID ||
        event->source == DETECTION_SRC_WIFI_DJI_IE ||
        event->source == DETECTION_SRC_WIFI_BEACON) {
        return BACKEND_REMOTE_ID_LIVE_WINDOW_MS;
    }
    return BACKEND_DRONE_SSID_LIVE_WINDOW_MS;
}

static bool active_at(
    const backend_live_entity_t *entity,
    int64_t now_ms)
{
    if (entity == NULL || !entity->used || now_ms < 0 ||
        entity->last_seen_ms < 0) {
        return false;
    }
    if (now_ms < entity->last_seen_ms) {
        return true;
    }
    return now_ms - entity->last_seen_ms <= live_window_ms(&entity->event);
}

static backend_live_entity_t *find_entity(
    backend_live_entities_t *state,
    const backend_dashboard_event_t *candidate)
{
    for (size_t index = 0U; index < BACKEND_LIVE_ENTITY_CAPACITY; ++index) {
        backend_live_entity_t *entity = &state->records[index];
        if (entity->used &&
            meta_event(&entity->event) == meta_event(candidate) &&
            strcmp(entity->event.badge_entity_key,
                   candidate->badge_entity_key) == 0) {
            return entity;
        }
    }
    return NULL;
}

static backend_live_entity_t *select_slot(backend_live_entities_t *state)
{
    backend_live_entity_t *oldest = NULL;
    for (size_t index = 0U; index < BACKEND_LIVE_ENTITY_CAPACITY; ++index) {
        backend_live_entity_t *entity = &state->records[index];
        if (!entity->used) {
            return entity;
        }
        if (oldest == NULL || entity->last_seen_ms < oldest->last_seen_ms) {
            oldest = entity;
        }
    }
    return oldest;
}

void backend_live_entities_init(backend_live_entities_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool backend_live_entities_ingest(
    backend_live_entities_t *state,
    const backend_dashboard_event_t *event,
    int64_t now_ms)
{
    if (state == NULL || now_ms < 0 || !supported_event(event)) {
        return false;
    }
    backend_live_entity_t *entity = find_entity(state, event);
    if (entity != NULL && !active_at(entity, now_ms)) {
        memset(entity, 0, sizeof(*entity));
        entity->used = true;
        entity->best_rssi = event->rssi;
    }
    if (entity == NULL) {
        entity = select_slot(state);
        if (entity == NULL) {
            return false;
        }
        memset(entity, 0, sizeof(*entity));
        entity->used = true;
        entity->best_rssi = event->rssi;
    }
    const uint32_t next_count = entity->event_count == UINT32_MAX
        ? UINT32_MAX : entity->event_count + 1U;
    const int8_t best_rssi = entity->event_count == 0U ||
            event->rssi > entity->best_rssi
        ? event->rssi : entity->best_rssi;
    entity->event = *event;
    entity->last_seen_ms = now_ms;
    entity->event_count = next_count;
    entity->best_rssi = best_rssi;
    return true;
}

size_t backend_live_entities_active_count(
    const backend_live_entities_t *state,
    int64_t now_ms)
{
    if (state == NULL) {
        return 0U;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < BACKEND_LIVE_ENTITY_CAPACITY; ++index) {
        if (active_at(&state->records[index], now_ms)) {
            ++count;
        }
    }
    return count;
}

static const char *source_name(uint8_t source)
{
    switch (source) {
    case DETECTION_SRC_BLE_RID: return "ble_rid";
    case DETECTION_SRC_WIFI_SSID: return "wifi_ssid";
    case DETECTION_SRC_WIFI_DJI_IE: return "wifi_dji_ie";
    case DETECTION_SRC_WIFI_BEACON: return "wifi_rid";
    case DETECTION_SRC_WIFI_OUI: return "wifi_oui";
    case DETECTION_SRC_WIFI_PROBE_REQUEST: return "wifi_probe";
    case DETECTION_SRC_BLE_FINGERPRINT: return "ble_fingerprint";
    case DETECTION_SRC_WIFI_ASSOC: return "wifi_assoc";
    case DETECTION_SRC_WIFI_AP_INVENTORY: return "wifi_inventory";
    default: return "unknown";
    }
}

static bool coordinate_pair_valid(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) &&
           (latitude != 0.0 || longitude != 0.0) &&
           latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

static bool append_text_field(
    backend_json_writer_t *writer,
    const char *key,
    const char *value)
{
    return backend_json_append(writer, ",\"") &&
           backend_json_append(writer, key) &&
           backend_json_append(writer, "\":") &&
           backend_json_append_escaped(writer, value);
}

static bool append_entity(
    backend_json_writer_t *writer,
    const backend_live_entity_t *entity,
    int64_t now_ms)
{
    const backend_dashboard_event_t *event = &entity->event;
    const bool meta = strcmp(event->badge_class, "drone") != 0;
    const bool ssid = event->source == DETECTION_SRC_WIFI_SSID ||
        event->source == DETECTION_SRC_WIFI_OUI;
    const char *threat_class = meta ? "meta" : "drone";
    const char *category = meta ? "GLASS" : (ssid ? "SSID" : "DRONE");
    const char *code = meta ? "GLS" : (ssid ? "SSID" : "DRN");
    const char *detail = event->model[0] != '\0'
        ? event->model
        : (event->manufacturer[0] != '\0' ? event->manufacturer : event->id);
    int64_t age_ms = now_ms < entity->last_seen_ms
        ? 0 : now_ms - entity->last_seen_ms;
    const int64_t age_s = age_ms / INT64_C(1000);
    int confidence_pct = (int)floorf(event->confidence * 100.0f + 0.5f);
    if (confidence_pct < 0) {
        confidence_pct = 0;
    } else if (confidence_pct > 100) {
        confidence_pct = 100;
    }

    bool ok = backend_json_append(writer, "{\"label\":") &&
        backend_json_append_escaped(writer, event->badge_label) &&
        append_text_field(writer, "detail", detail) &&
        append_text_field(writer, "evidence", source_name(event->source)) &&
        append_text_field(writer, "class", threat_class) &&
        append_text_field(writer, "category", category) &&
        append_text_field(writer, "code", code) &&
        append_text_field(writer, "display_id", event->id) &&
        append_text_field(writer, "source", source_name(event->source)) &&
        backend_json_append_format(
            writer,
            ",\"source_id\":%u,\"score\":%u,\"confidence_pct\":%d"
            ",\"age_s\":%lld,\"last_seen_s\":%lld"
            ",\"rssi\":%d,\"best_rssi\":%d"
            ",\"events\":%lu,\"seen_count\":%lu"
            ",\"group_count\":1,\"stale\":false",
            (unsigned)event->source,
            (unsigned)event->threat_score,
            confidence_pct,
            (long long)age_s,
            (long long)age_s,
            (int)event->rssi,
            (int)entity->best_rssi,
            (unsigned long)entity->event_count,
            (unsigned long)entity->event_count) &&
        append_text_field(writer, "manufacturer", event->manufacturer);
    if (ok && coordinate_pair_valid(
            event->aircraft_lat, event->aircraft_lon)) {
        ok = backend_json_append_format(
            writer,
            ",\"lat\":%.7f,\"lon\":%.7f,\"altitude_m\":%.9g",
            event->aircraft_lat,
            event->aircraft_lon,
            event->altitude_m);
    }
    if (ok && coordinate_pair_valid(
            event->operator_lat, event->operator_lon)) {
        ok = backend_json_append_format(
            writer,
            ",\"operator_lat\":%.7f,\"operator_lon\":%.7f",
            event->operator_lat,
            event->operator_lon);
    }
    if (ok && event->operator_id[0] != '\0') {
        ok = append_text_field(writer, "operator_id", event->operator_id);
    }
    return ok && backend_json_append(writer, "}");
}

bool backend_live_entities_append_json(
    backend_json_writer_t *writer,
    const backend_live_entities_t *state,
    int64_t now_ms)
{
    if (writer == NULL || state == NULL || now_ms < 0 ||
        !backend_json_append(writer, "[")) {
        return false;
    }
    bool first = true;
    for (size_t index = 0U; index < BACKEND_LIVE_ENTITY_CAPACITY; ++index) {
        const backend_live_entity_t *entity = &state->records[index];
        if (!active_at(entity, now_ms)) {
            continue;
        }
        if ((!first && !backend_json_append(writer, ",")) ||
            !append_entity(writer, entity, now_ms)) {
            return false;
        }
        first = false;
    }
    return backend_json_append(writer, "]");
}
