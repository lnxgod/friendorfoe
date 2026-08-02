#include "backend_detection_router.h"

#include <limits.h>
#include <string.h>

#include "detection_policy.h"

#define BACKEND_ROUTER_KEY_SCRATCH 256U

static bool resolve_key(
    const backend_detection_observation_t *observation,
    char out[192])
{
    char scratch[BACKEND_ROUTER_KEY_SCRATCH] = {0};
    if (observation == NULL ||
        !fof_policy_detection_dedupe_key(
            &observation->detection,
            0,
            (uint32_t)BACKEND_DEDUPE_WINDOW_MS,
            scratch,
            sizeof(scratch))) {
        return false;
    }
    const size_t length = strlen(scratch);
    if (length == 0U || length >= sizeof(((backend_dedupe_bucket_t *)0)->key)) {
        return false;
    }
    memcpy(out, scratch, length + 1U);
    return true;
}

static uint8_t observation_slot_mask(
    const backend_detection_observation_t *observation)
{
    uint8_t mask = observation->detection.scanner_slots_seen;
    if (mask == 0U && observation->detection.scanner_slot < 8U) {
        mask = (uint8_t)(UINT8_C(1) << observation->detection.scanner_slot);
    }
    return mask;
}

static bool stronger_rssi(int8_t candidate, int8_t current)
{
    if (candidate >= 0) {
        return false;
    }
    return current >= 0 || candidate > current;
}

static bool token_present(
    const char *list,
    const char *token,
    size_t token_length)
{
    const char *cursor = list;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, ',');
        const size_t length = end == NULL
            ? strlen(cursor)
            : (size_t)(end - cursor);
        if (length == token_length &&
            memcmp(cursor, token, token_length) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

static void merge_probe_lists(
    const char *existing,
    const char *incoming,
    char out[128])
{
    char merged[128] = {0};
    const size_t existing_length = strnlen(existing, sizeof(merged));
    if (existing_length >= sizeof(merged)) {
        return;
    }
    memcpy(merged, existing, existing_length + 1U);
    size_t used = existing_length;

    const char *cursor = incoming;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, ',');
        const size_t length = end == NULL
            ? strlen(cursor)
            : (size_t)(end - cursor);
        if (length > 0U && !token_present(merged, cursor, length)) {
            const size_t separator = used > 0U ? 1U : 0U;
            if (used + separator + length >= sizeof(merged)) {
                memcpy(out, existing, existing_length + 1U);
                return;
            }
            if (separator != 0U) {
                merged[used++] = ',';
            }
            memcpy(&merged[used], cursor, length);
            used += length;
            merged[used] = '\0';
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    memcpy(out, merged, used + 1U);
}

static void merge_observation(
    backend_detection_observation_t *current,
    const backend_detection_observation_t *incoming)
{
    char merged_probes[128] = {0};
    merge_probe_lists(
        current->detection.probed_ssids,
        incoming->detection.probed_ssids,
        merged_probes);

    const uint8_t slots = (uint8_t)(
        observation_slot_mask(current) |
        observation_slot_mask(incoming));
    const bool current_time_valid = current->timestamp_valid;
    const int64_t current_time = current->timestamp_epoch_ms;
    const bool incoming_time_valid = incoming->timestamp_valid;
    const int64_t incoming_time = incoming->timestamp_epoch_ms;

    if (stronger_rssi(incoming->detection.rssi,
                      current->detection.rssi)) {
        *current = *incoming;
    }
    current->detection.scanner_slots_seen = slots;
    memcpy(current->detection.probed_ssids,
           merged_probes,
           sizeof(current->detection.probed_ssids));

    if (current_time_valid && incoming_time_valid) {
        current->timestamp_valid = true;
        current->timestamp_epoch_ms = current_time < incoming_time
            ? current_time
            : incoming_time;
    } else if (current_time_valid) {
        current->timestamp_valid = true;
        current->timestamp_epoch_ms = current_time;
    } else if (incoming_time_valid) {
        current->timestamp_valid = true;
        current->timestamp_epoch_ms = incoming_time;
    } else {
        current->timestamp_valid = false;
        current->timestamp_epoch_ms = 0;
    }
}

static backend_dedupe_bucket_t *find_bucket(
    backend_detection_router_t *router,
    const char *key)
{
    for (size_t index = 0U;
         index < BACKEND_DEDUPE_BUCKET_CAPACITY;
         ++index) {
        backend_dedupe_bucket_t *bucket = &router->buckets[index];
        if (bucket->used && strcmp(bucket->key, key) == 0) {
            return bucket;
        }
    }
    return NULL;
}

static backend_dedupe_bucket_t *free_bucket(
    backend_detection_router_t *router)
{
    for (size_t index = 0U;
         index < BACKEND_DEDUPE_BUCKET_CAPACITY;
         ++index) {
        if (!router->buckets[index].used) {
            return &router->buckets[index];
        }
    }
    return NULL;
}

static backend_dedupe_bucket_t *oldest_bucket(
    backend_detection_router_t *router,
    int64_t now_ms,
    bool expired_only)
{
    backend_dedupe_bucket_t *oldest = NULL;
    for (size_t index = 0U;
         index < BACKEND_DEDUPE_BUCKET_CAPACITY;
         ++index) {
        backend_dedupe_bucket_t *candidate = &router->buckets[index];
        if (!candidate->used) {
            continue;
        }
        if (expired_only &&
            (now_ms < candidate->opened_monotonic_ms ||
             now_ms - candidate->opened_monotonic_ms <
                 BACKEND_DEDUPE_WINDOW_MS)) {
            continue;
        }
        if (oldest == NULL ||
            candidate->insertion_order < oldest->insertion_order) {
            oldest = candidate;
        }
    }
    return oldest;
}

static bool bucket_expired(
    const backend_dedupe_bucket_t *bucket,
    int64_t now_ms)
{
    return bucket != NULL && bucket->used &&
           now_ms >= bucket->opened_monotonic_ms &&
           now_ms - bucket->opened_monotonic_ms >=
               BACKEND_DEDUPE_WINDOW_MS;
}

static bool move_to_ready(
    backend_detection_router_t *router,
    backend_dedupe_bucket_t *bucket)
{
    if (router->ready_count >= BACKEND_DEDUPE_READY_CAPACITY ||
        bucket == NULL || !bucket->used) {
        return false;
    }
    const size_t tail =
        ((size_t)router->ready_head + (size_t)router->ready_count) %
        BACKEND_DEDUPE_READY_CAPACITY;
    router->ready[tail] = bucket->observation;
    ++router->ready_count;
    memset(bucket, 0, sizeof(*bucket));
    return true;
}

void backend_observation_resolve(
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *scanner_stamp,
    int64_t uplink_epoch_ms,
    backend_detection_observation_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (detection != NULL) {
        out->detection = *detection;
    }
    if (scanner_stamp != NULL && scanner_stamp->time_valid &&
        scanner_stamp->observed_epoch_ms >= BACKEND_DETECTION_EPOCH_MIN_MS) {
        out->timestamp_valid = true;
        out->timestamp_epoch_ms = scanner_stamp->observed_epoch_ms;
    } else if (uplink_epoch_ms >= BACKEND_DETECTION_EPOCH_MIN_MS) {
        out->timestamp_valid = true;
        out->timestamp_epoch_ms = uplink_epoch_ms;
    }
}

void backend_detection_router_init(backend_detection_router_t *router)
{
    if (router == NULL) {
        return;
    }
    memset(router, 0, sizeof(*router));
    router->next_insertion_order = 1U;
}

backend_detection_route_result_t backend_detection_router_ingest(
    backend_detection_router_t *router,
    const backend_detection_observation_t *observation,
    int64_t monotonic_now_ms)
{
    backend_detection_route_result_t result = {0};
    char key[192] = {0};
    if (router == NULL || observation == NULL || monotonic_now_ms < 0 ||
        !resolve_key(observation, key)) {
        return result;
    }

    backend_dedupe_bucket_t *bucket = find_bucket(router, key);
    if (bucket_expired(bucket, monotonic_now_ms)) {
        if (!move_to_ready(router, bucket)) {
            result.backpressure = true;
            return result;
        }
        bucket = NULL;
    }
    if (bucket != NULL) {
        merge_observation(&bucket->observation, observation);
        result.accepted_for_upload = true;
        result.update_local_threat = true;
        return result;
    }

    bucket = free_bucket(router);
    if (bucket == NULL) {
        backend_dedupe_bucket_t *oldest =
            oldest_bucket(router, monotonic_now_ms, false);
        if (!move_to_ready(router, oldest)) {
            result.backpressure = true;
            return result;
        }
        bucket = free_bucket(router);
    }
    if (bucket == NULL) {
        result.backpressure = true;
        return result;
    }

    memset(bucket, 0, sizeof(*bucket));
    bucket->used = true;
    memcpy(bucket->key, key, strlen(key) + 1U);
    bucket->opened_monotonic_ms = monotonic_now_ms;
    bucket->insertion_order = router->next_insertion_order++;
    if (router->next_insertion_order == 0U) {
        router->next_insertion_order = 1U;
    }
    bucket->observation = *observation;
    bucket->observation.detection.scanner_slots_seen =
        observation_slot_mask(observation);
    result.accepted_for_upload = true;
    result.update_local_threat = true;
    return result;
}

size_t backend_detection_router_tick(
    backend_detection_router_t *router,
    int64_t monotonic_now_ms)
{
    if (router == NULL || monotonic_now_ms < 0) {
        return 0U;
    }
    size_t moved = 0U;
    while (router->ready_count < BACKEND_DEDUPE_READY_CAPACITY) {
        backend_dedupe_bucket_t *bucket =
            oldest_bucket(router, monotonic_now_ms, true);
        if (bucket == NULL || !move_to_ready(router, bucket)) {
            break;
        }
        ++moved;
    }
    return moved;
}

bool backend_detection_router_next_upload(
    backend_detection_router_t *router,
    backend_detection_observation_t *out)
{
    if (router == NULL || out == NULL || router->ready_count == 0U) {
        return false;
    }
    *out = router->ready[router->ready_head];
    memset(&router->ready[router->ready_head],
           0,
           sizeof(router->ready[router->ready_head]));
    router->ready_head = (uint8_t)(
        ((size_t)router->ready_head + 1U) %
        BACKEND_DEDUPE_READY_CAPACITY);
    --router->ready_count;
    return true;
}
