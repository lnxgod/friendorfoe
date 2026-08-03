#include "backend_dashboard_event.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "backend_json_writer.h"
#include "detection_policy.h"
#include "rssi_distance.h"

#define BACKEND_FOF_DET_CAPACITY 1535U

_Static_assert(sizeof(backend_dashboard_event_t) <= 512U,
               "dashboard event exceeds 512 bytes");
_Static_assert(128U * sizeof(backend_dashboard_event_t) <= 65536U,
               "dashboard ring exceeds 64 KiB");

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z'
        ? (char)(value - 'A' + 'a')
        : value;
}

static bool contains_nocase(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    for (const char *start = text; start[0] != '\0'; ++start) {
        const char *left = start;
        const char *right = needle;
        while (left[0] != '\0' && right[0] != '\0' &&
               ascii_lower(left[0]) == ascii_lower(right[0])) {
            ++left;
            ++right;
        }
        if (right[0] == '\0') {
            return true;
        }
    }
    return false;
}

static bool text_mentions_meta_glasses(const char *text)
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

static bool mentions_meta_glasses(const drone_detection_t *detection)
{
    return text_mentions_meta_glasses(detection->manufacturer) ||
           text_mentions_meta_glasses(detection->model) ||
           text_mentions_meta_glasses(detection->class_reason);
}

static bool drone_source(uint8_t source)
{
    return source == DETECTION_SRC_BLE_RID ||
           source == DETECTION_SRC_WIFI_SSID ||
           source == DETECTION_SRC_WIFI_DJI_IE ||
           source == DETECTION_SRC_WIFI_BEACON ||
           source == DETECTION_SRC_WIFI_OUI;
}

static bool copy_text(
    char *destination,
    size_t destination_capacity,
    const char *source,
    size_t source_capacity)
{
    if (destination == NULL || destination_capacity == 0U || source == NULL) {
        return false;
    }
    const char *end = memchr(source, '\0', source_capacity);
    if (end == NULL) {
        destination[0] = '\0';
        return false;
    }
    const size_t length = (size_t)(end - source);
    if (length >= destination_capacity) {
        destination[0] = '\0';
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool coordinates_valid(const drone_detection_t *detection)
{
    return isfinite(detection->latitude) &&
           detection->latitude >= -90.0 && detection->latitude <= 90.0 &&
           isfinite(detection->longitude) &&
           detection->longitude >= -180.0 && detection->longitude <= 180.0 &&
           isfinite(detection->operator_lat) &&
           detection->operator_lat >= -90.0 &&
           detection->operator_lat <= 90.0 &&
           isfinite(detection->operator_lon) &&
           detection->operator_lon >= -180.0 &&
           detection->operator_lon <= 180.0;
}

static uint8_t threat_score(const drone_detection_t *detection)
{
    float confidence = detection->confidence;
    if (detection->fused_confidence > confidence) {
        confidence = detection->fused_confidence;
    }
    if (confidence < 0.0f) {
        confidence = 0.0f;
    } else if (confidence > 1.0f) {
        confidence = 1.0f;
    }
    return (uint8_t)floorf(confidence * 100.0f + 0.5f);
}

bool backend_dashboard_event_project(
    const backend_detection_observation_t *observation,
    backend_dashboard_event_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (observation == NULL ||
        !isfinite(observation->detection.confidence) ||
        !isfinite(observation->detection.fused_confidence) ||
        !coordinates_valid(&observation->detection)) {
        return false;
    }

    backend_dashboard_event_t projected = {0};
    if (!copy_text(projected.id, sizeof(projected.id),
                   observation->detection.drone_id,
                   sizeof(observation->detection.drone_id)) ||
        !copy_text(projected.manufacturer, sizeof(projected.manufacturer),
                   observation->detection.manufacturer,
                   sizeof(observation->detection.manufacturer)) ||
        !copy_text(projected.model, sizeof(projected.model),
                   observation->detection.model,
                   sizeof(observation->detection.model)) ||
        !fof_policy_detection_identity_key(
            &observation->detection,
            projected.badge_entity_key,
            sizeof(projected.badge_entity_key))) {
        return false;
    }

    projected.timestamp_valid = observation->timestamp_valid;
    projected.timestamp_epoch_ms = observation->timestamp_epoch_ms;
    projected.source = observation->detection.source;
    projected.confidence = observation->detection.confidence;
    projected.threat_score = threat_score(&observation->detection);
    projected.rssi = observation->detection.rssi;
    projected.distance_m = rssi_distance_estimate_m(
        observation->detection.rssi);
    projected.aircraft_lat = observation->detection.latitude;
    projected.aircraft_lon = observation->detection.longitude;
    projected.operator_lat = observation->detection.operator_lat;
    projected.operator_lon = observation->detection.operator_lon;
    projected.scanner_slot_mask =
        observation->detection.scanner_slots_seen;

    if (mentions_meta_glasses(&observation->detection)) {
        copy_text(projected.badge_label, sizeof(projected.badge_label),
                  "Meta Glasses", sizeof("Meta Glasses"));
        copy_text(projected.badge_class, sizeof(projected.badge_class),
                  "meta_glasses", sizeof("meta_glasses"));
    } else if (drone_source(observation->detection.source)) {
        copy_text(projected.badge_label, sizeof(projected.badge_label),
                  "Drone", sizeof("Drone"));
        copy_text(projected.badge_class, sizeof(projected.badge_class),
                  "drone", sizeof("drone"));
    }

    *out = projected;
    return true;
}

static bool event_text_valid(const char *text, size_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static bool event_valid(const backend_dashboard_event_t *event)
{
    return event != NULL &&
           event_text_valid(event->id, sizeof(event->id)) &&
           event_text_valid(event->manufacturer, sizeof(event->manufacturer)) &&
           event_text_valid(event->model, sizeof(event->model)) &&
           event_text_valid(event->badge_label, sizeof(event->badge_label)) &&
           event_text_valid(event->badge_class, sizeof(event->badge_class)) &&
           event_text_valid(
               event->badge_entity_key, sizeof(event->badge_entity_key)) &&
           isfinite(event->confidence) && isfinite(event->distance_m) &&
           isfinite(event->aircraft_lat) && isfinite(event->aircraft_lon) &&
           isfinite(event->operator_lat) && isfinite(event->operator_lon);
}

static bool append_string_field(
    backend_json_writer_t *writer,
    const char *key,
    const char *value)
{
    return backend_json_append(writer, ",\"") &&
           backend_json_append(writer, key) &&
           backend_json_append(writer, "\":") &&
           backend_json_append_escaped(writer, value);
}

size_t backend_dashboard_event_encode_json(
    const backend_dashboard_event_t *event,
    char *output,
    size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (output == NULL || capacity == 0U || !event_valid(event)) {
        return 0U;
    }

    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, capacity);
    backend_json_append_format(
        &writer,
        "{\"sequence\":%" PRIu64
        ",\"timestamp_valid\":%s"
        ",\"timestamp_epoch_ms\":%" PRId64,
        event->sequence,
        event->timestamp_valid ? "true" : "false",
        event->timestamp_epoch_ms);
    append_string_field(&writer, "id", event->id);
    append_string_field(&writer, "manufacturer", event->manufacturer);
    append_string_field(&writer, "model", event->model);
    append_string_field(&writer, "badge_label", event->badge_label);
    append_string_field(&writer, "badge_class", event->badge_class);
    append_string_field(
        &writer, "badge_entity_key", event->badge_entity_key);
    backend_json_append_format(
        &writer,
        ",\"source\":%u,\"confidence\":%.9g"
        ",\"threat_score\":%u,\"rssi\":%d"
        ",\"distance_m\":%.15g,\"aircraft_lat\":%.15g"
        ",\"aircraft_lon\":%.15g,\"operator_lat\":%.15g"
        ",\"operator_lon\":%.15g,\"scanner_slot_mask\":%u}",
        (unsigned)event->source,
        (double)event->confidence,
        (unsigned)event->threat_score,
        (int)event->rssi,
        event->distance_m,
        event->aircraft_lat,
        event->aircraft_lon,
        event->operator_lat,
        event->operator_lon,
        (unsigned)event->scanner_slot_mask);
    return backend_json_writer_finish(&writer);
}

size_t backend_dashboard_event_encode_fof_det(
    const backend_dashboard_event_t *event,
    char *output,
    size_t capacity)
{
    if (output != NULL && capacity > 0U) {
        output[0] = '\0';
    }
    if (output == NULL || capacity == 0U || !event_valid(event)) {
        return 0U;
    }

    const size_t bounded_capacity = capacity < BACKEND_FOF_DET_CAPACITY
        ? capacity
        : BACKEND_FOF_DET_CAPACITY;
    backend_json_writer_t writer;
    backend_json_writer_init(&writer, output, bounded_capacity);
    backend_json_append(&writer, "FOF_DET:{\"id\":");
    backend_json_append_escaped(&writer, event->id);
    append_string_field(&writer, "manufacturer", event->manufacturer);
    append_string_field(&writer, "badge_label", event->badge_label);
    append_string_field(&writer, "badge_class", event->badge_class);
    append_string_field(
        &writer, "badge_entity_key", event->badge_entity_key);
    backend_json_append_format(
        &writer,
        ",\"source\":%u,\"confidence\":%.9g"
        ",\"threat_score\":%u,\"rssi\":%d}\n",
        (unsigned)event->source,
        (double)event->confidence,
        (unsigned)event->threat_score,
        (int)event->rssi);
    return backend_json_writer_finish(&writer);
}
