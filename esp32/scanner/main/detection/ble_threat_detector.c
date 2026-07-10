#include "ble_threat_detector.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define PROMPT_WINDOW_MS 8000U
#define PROMPT_MIN_UNIQUE_MACS 12U
#define PROMPT_MIN_OBSERVATIONS 24U
#define PROMPT_MIN_CHURN_NUMERATOR 3U
#define PROMPT_MIN_CHURN_DENOMINATOR 4U
#define PROMPT_MAX_RSSI_SPAN 20
#define PROMPT_MAX_RSSI_IQR 12
#define PROMPT_COOLDOWN_MS 60000U
#define STATE_CLEAR_AFTER_MS 20000U
#define DEDUPE_MS 250U
#define MAX_PROMPT_OBSERVATIONS 256U
#define MAX_SERIAL_TRACKS 64U
#define SERIAL_PERSISTENCE_OBSERVATIONS 3U
#define SERIAL_PERSISTENCE_MS 5000U
#define CLOSE_RSSI_DBM (-70)
#define SERIAL_REQUIRED_EVIDENCE (BLE_THREAT_EVIDENCE_SERIAL_UUID | \
                                  BLE_THREAT_EVIDENCE_SPARSE | \
                                  BLE_THREAT_EVIDENCE_PERSISTENT)
#define SERIAL_SUPPORTING_EVIDENCE (BLE_THREAT_EVIDENCE_GENERIC_NAME | \
                                    BLE_THREAT_EVIDENCE_CLOSE | \
                                    BLE_THREAT_EVIDENCE_CONNECTABLE | \
                                    BLE_THREAT_EVIDENCE_UNTRUSTED)

typedef struct {
    uint8_t mac[6];
    int64_t observed_ms;
    int8_t rssi;
    uint32_t structural_hash;
    ble_prompt_family_t prompt_family;
} prompt_sample_t;

typedef struct {
    bool used;
    uint8_t mac[6];
    int64_t observed_ms;
    uint32_t structural_hash;
    ble_prompt_family_t prompt_family;
} prompt_dedupe_t;

typedef struct {
    uint8_t mac[6];
    int64_t first_seen_ms;
    uint32_t structural_hash;
    ble_prompt_family_t prompt_family;
} prompt_identity_t;

typedef struct {
    bool used;
    uint8_t mac[6];
    uint16_t serial_service_uuid;
    int64_t first_seen_ms;
    int64_t last_seen_ms;
    uint32_t observation_count;
    uint64_t insertion_order;
    int8_t strongest_rssi;
    uint8_t max_unique_service_count;
    bool has_generic_name;
    bool trusted;
    bool has_pkoc_identity;
    bool connectable;
    bool alerted;
} serial_track_t;

static prompt_sample_t s_prompt_samples[MAX_PROMPT_OBSERVATIONS];
static size_t s_prompt_head;
static size_t s_prompt_count;
static prompt_dedupe_t s_prompt_dedupe[MAX_PROMPT_OBSERVATIONS];
static prompt_identity_t s_prompt_identities[MAX_PROMPT_OBSERVATIONS];
static size_t s_identity_head;
static size_t s_identity_count;
static bool s_has_prompt_signal;
static int64_t s_last_prompt_signal_ms;
static serial_track_t s_serial_tracks[MAX_SERIAL_TRACKS];
static uint64_t s_serial_insertion_order;
static bool s_has_last_observed_ms;
static int64_t s_last_observed_ms;

static bool same_mac(const uint8_t left[6], const uint8_t right[6])
{
    return memcmp(left, right, 6) == 0;
}

static bool elapsed_at_least(int64_t now_ms, int64_t then_ms, uint64_t threshold_ms)
{
    if (now_ms < then_ms) {
        return false;
    }
    return (uint64_t)now_ms - (uint64_t)then_ms >= threshold_ms;
}

static bool elapsed_more_than(int64_t now_ms, int64_t then_ms, uint64_t threshold_ms)
{
    if (now_ms < then_ms) {
        return false;
    }
    return (uint64_t)now_ms - (uint64_t)then_ms > threshold_ms;
}

static bool within_dedupe_window(int64_t now_ms, int64_t then_ms)
{
    return now_ms < then_ms || !elapsed_more_than(now_ms, then_ms, DEDUPE_MS);
}

static size_t prompt_sample_index(size_t logical_index)
{
    return (s_prompt_head + logical_index) % MAX_PROMPT_OBSERVATIONS;
}

static size_t prompt_identity_index(size_t logical_index)
{
    return (s_identity_head + logical_index) % MAX_PROMPT_OBSERVATIONS;
}

static bool same_prompt_key(const uint8_t mac[6],
                            uint32_t structural_hash,
                            ble_prompt_family_t prompt_family,
                            const prompt_dedupe_t *dedupe)
{
    return dedupe->used &&
           dedupe->structural_hash == structural_hash &&
           dedupe->prompt_family == prompt_family &&
           same_mac(dedupe->mac, mac);
}

static void remove_matching_dedupe(const prompt_sample_t *sample)
{
    for (size_t index = 0; index < MAX_PROMPT_OBSERVATIONS; ++index) {
        prompt_dedupe_t *dedupe = &s_prompt_dedupe[index];
        if (same_prompt_key(sample->mac,
                            sample->structural_hash,
                            sample->prompt_family,
                            dedupe) &&
            dedupe->observed_ms == sample->observed_ms) {
            dedupe->used = false;
            return;
        }
    }
}

static void evict_oldest_prompt_sample(void)
{
    if (s_prompt_count == 0) {
        return;
    }
    remove_matching_dedupe(&s_prompt_samples[s_prompt_head]);
    s_prompt_head = (s_prompt_head + 1U) % MAX_PROMPT_OBSERVATIONS;
    --s_prompt_count;
}

static void clear_prompt_state(void)
{
    memset(s_prompt_samples, 0, sizeof(s_prompt_samples));
    memset(s_prompt_dedupe, 0, sizeof(s_prompt_dedupe));
    memset(s_prompt_identities, 0, sizeof(s_prompt_identities));
    s_prompt_head = 0;
    s_prompt_count = 0;
    s_identity_head = 0;
    s_identity_count = 0;
    s_has_prompt_signal = false;
    s_last_prompt_signal_ms = 0;
}

static void prune_prompt_state(int64_t now_ms)
{
    if (s_prompt_count > 0) {
        const size_t newest_index = prompt_sample_index(s_prompt_count - 1U);
        if (elapsed_at_least(now_ms,
                             s_prompt_samples[newest_index].observed_ms,
                             STATE_CLEAR_AFTER_MS)) {
            clear_prompt_state();
            return;
        }
    }

    while (s_prompt_count > 0 &&
           elapsed_more_than(now_ms,
                             s_prompt_samples[s_prompt_head].observed_ms,
                             PROMPT_WINDOW_MS)) {
        evict_oldest_prompt_sample();
    }

    for (size_t index = 0; index < MAX_PROMPT_OBSERVATIONS; ++index) {
        if (s_prompt_dedupe[index].used &&
            elapsed_more_than(now_ms, s_prompt_dedupe[index].observed_ms, DEDUPE_MS)) {
            s_prompt_dedupe[index].used = false;
        }
    }
}

static prompt_dedupe_t *find_prompt_dedupe(const ble_threat_observation_t *observation)
{
    for (size_t index = 0; index < MAX_PROMPT_OBSERVATIONS; ++index) {
        if (same_prompt_key(observation->mac,
                            observation->structural_hash,
                            observation->prompt_family,
                            &s_prompt_dedupe[index])) {
            return &s_prompt_dedupe[index];
        }
    }
    return NULL;
}

static prompt_dedupe_t *free_prompt_dedupe(void)
{
    for (size_t index = 0; index < MAX_PROMPT_OBSERVATIONS; ++index) {
        if (!s_prompt_dedupe[index].used) {
            return &s_prompt_dedupe[index];
        }
    }
    return NULL;
}

static bool same_prompt_identity(const prompt_identity_t *identity,
                                 const ble_threat_observation_t *observation)
{
    return identity->structural_hash == observation->structural_hash &&
           identity->prompt_family == observation->prompt_family &&
           same_mac(identity->mac, observation->mac);
}

static void remember_first_seen(const ble_threat_observation_t *observation)
{
    for (size_t logical = 0; logical < s_identity_count; ++logical) {
        if (same_prompt_identity(&s_prompt_identities[prompt_identity_index(logical)],
                                 observation)) {
            return;
        }
    }

    if (s_identity_count == MAX_PROMPT_OBSERVATIONS) {
        s_identity_head = (s_identity_head + 1U) % MAX_PROMPT_OBSERVATIONS;
        --s_identity_count;
    }

    const size_t tail = prompt_identity_index(s_identity_count);
    prompt_identity_t *identity = &s_prompt_identities[tail];
    memcpy(identity->mac, observation->mac, sizeof(identity->mac));
    identity->first_seen_ms = observation->observed_ms;
    identity->structural_hash = observation->structural_hash;
    identity->prompt_family = observation->prompt_family;
    ++s_identity_count;
}

static bool identity_first_seen_at_or_after(const prompt_sample_t *sample,
                                            int64_t window_start_ms)
{
    for (size_t logical = 0; logical < s_identity_count; ++logical) {
        const prompt_identity_t *identity =
            &s_prompt_identities[prompt_identity_index(logical)];
        if (identity->structural_hash == sample->structural_hash &&
            identity->prompt_family == sample->prompt_family &&
            same_mac(identity->mac, sample->mac)) {
            return identity->first_seen_ms >= window_start_ms;
        }
    }
    return false;
}

static void insert_sorted_rssi(int8_t sorted_rssi[MAX_PROMPT_OBSERVATIONS],
                               size_t sorted_count,
                               int8_t rssi)
{
    size_t index = sorted_count;
    while (index > 0 && sorted_rssi[index - 1U] > rssi) {
        sorted_rssi[index] = sorted_rssi[index - 1U];
        --index;
    }
    sorted_rssi[index] = rssi;
}

static uint32_t fnv1a_append(uint32_t hash, uint8_t value)
{
    return (hash ^ value) * 16777619U;
}

static uint32_t hash_text(const char *text)
{
    uint32_t hash = 2166136261U;
    while (*text != '\0') {
        hash = fnv1a_append(hash, (uint8_t)*text);
        ++text;
    }
    return hash;
}

static uint32_t serial_entity_hash(const uint8_t mac[6])
{
    static const char prefix[] = "ble:serial-skimmer:";
    static const char hex[] = "0123456789ABCDEF";
    uint32_t hash = hash_text(prefix);

    for (size_t index = 0; index < 6; ++index) {
        if (index > 0) {
            hash = fnv1a_append(hash, ':');
        }
        hash = fnv1a_append(hash, (uint8_t)hex[mac[index] >> 4]);
        hash = fnv1a_append(hash, (uint8_t)hex[mac[index] & 0x0F]);
    }
    return hash;
}

static bool pairing_spam_signal(int64_t now_ms, ble_threat_signal_t *signal_out)
{
    if (s_has_prompt_signal &&
        !elapsed_at_least(now_ms, s_last_prompt_signal_ms, PROMPT_COOLDOWN_MS)) {
        return false;
    }
    if (s_prompt_count < PROMPT_MIN_OBSERVATIONS) {
        return false;
    }

    size_t unique_macs = 0;
    size_t churned_macs = 0;
    uint8_t family_mask = 0;
    int8_t sorted_rssi[MAX_PROMPT_OBSERVATIONS];
    const int64_t window_start_ms = s_prompt_samples[s_prompt_head].observed_ms;

    for (size_t logical = 0; logical < s_prompt_count; ++logical) {
        const prompt_sample_t *sample = &s_prompt_samples[prompt_sample_index(logical)];
        bool mac_appears_later = false;

        insert_sorted_rssi(sorted_rssi, logical, sample->rssi);
        family_mask |= (uint8_t)sample->prompt_family;
        for (size_t later = logical + 1U; later < s_prompt_count; ++later) {
            if (same_mac(sample->mac, s_prompt_samples[prompt_sample_index(later)].mac)) {
                mac_appears_later = true;
                break;
            }
        }
        if (!mac_appears_later) {
            ++unique_macs;
            if (identity_first_seen_at_or_after(sample, window_start_ms)) {
                ++churned_macs;
            }
        }
    }

    if (unique_macs < PROMPT_MIN_UNIQUE_MACS ||
        churned_macs * PROMPT_MIN_CHURN_DENOMINATOR <
            unique_macs * PROMPT_MIN_CHURN_NUMERATOR) {
        return false;
    }

    const int rssi_span = (int)sorted_rssi[s_prompt_count - 1U] - sorted_rssi[0];
    const int rssi_iqr = (int)sorted_rssi[(s_prompt_count * 3U) / 4U] -
                         sorted_rssi[s_prompt_count / 4U];
    if (rssi_span > PROMPT_MAX_RSSI_SPAN || rssi_iqr > PROMPT_MAX_RSSI_IQR) {
        return false;
    }

    signal_out->kind = BLE_THREAT_PAIRING_SPAM;
    signal_out->entity_hash = hash_text("ble:pairing-spam");
    signal_out->prompt_family_mask = family_mask;
    signal_out->unique_macs = (uint16_t)unique_macs;
    signal_out->observation_count = (uint16_t)s_prompt_count;
    signal_out->strongest_rssi = sorted_rssi[s_prompt_count - 1U];
    signal_out->rssi_span = (uint8_t)rssi_span;
    signal_out->confidence = 1.0f;
    s_has_prompt_signal = true;
    s_last_prompt_signal_ms = now_ms;
    return true;
}

static bool observe_prompt(const ble_threat_observation_t *observation,
                           ble_threat_signal_t *signal_out)
{
    if (observation->prompt_family == BLE_PROMPT_NONE) {
        return false;
    }

    prune_prompt_state(observation->observed_ms);
    prompt_dedupe_t *dedupe = find_prompt_dedupe(observation);
    if (dedupe != NULL && within_dedupe_window(observation->observed_ms,
                                               dedupe->observed_ms)) {
        return false;
    }

    if (dedupe == NULL) {
        dedupe = free_prompt_dedupe();
        if (dedupe == NULL && s_prompt_count == MAX_PROMPT_OBSERVATIONS) {
            evict_oldest_prompt_sample();
            dedupe = free_prompt_dedupe();
        }
        if (dedupe == NULL) {
            return false;
        }
    }
    dedupe->used = true;
    memcpy(dedupe->mac, observation->mac, sizeof(dedupe->mac));
    dedupe->observed_ms = observation->observed_ms;
    dedupe->structural_hash = observation->structural_hash;
    dedupe->prompt_family = observation->prompt_family;

    remember_first_seen(observation);
    if (s_prompt_count == MAX_PROMPT_OBSERVATIONS) {
        evict_oldest_prompt_sample();
    }
    const size_t tail = prompt_sample_index(s_prompt_count);
    prompt_sample_t *sample = &s_prompt_samples[tail];
    memcpy(sample->mac, observation->mac, sizeof(sample->mac));
    sample->observed_ms = observation->observed_ms;
    sample->rssi = observation->rssi;
    sample->structural_hash = observation->structural_hash;
    sample->prompt_family = observation->prompt_family;
    ++s_prompt_count;

    return pairing_spam_signal(observation->observed_ms, signal_out);
}

static bool serial_service_uuid(const ble_threat_observation_t *observation,
                                uint16_t *service_uuid_out)
{
    const size_t count = observation->service_uuid_count < 4U ?
                         observation->service_uuid_count : 4U;
    for (size_t index = 0; index < count; ++index) {
        if (observation->service_uuids[index] == 0xFFE0 ||
            observation->service_uuids[index] == 0xFFF0) {
            *service_uuid_out = observation->service_uuids[index];
            return true;
        }
    }
    return false;
}

static uint8_t unique_service_uuid_count(const ble_threat_observation_t *observation)
{
    const size_t count = observation->service_uuid_count < 4U ?
                         observation->service_uuid_count : 4U;
    uint8_t unique_count = 0;

    for (size_t index = 0; index < count; ++index) {
        bool seen = false;
        for (size_t prior = 0; prior < index; ++prior) {
            if (observation->service_uuids[index] == observation->service_uuids[prior]) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ++unique_count;
        }
    }
    return unique_count;
}

static bool trimmed_name_equals(const char *name, const char *expected)
{
    if (name == NULL) {
        return false;
    }
    while (*name != '\0' && isspace((unsigned char)*name)) {
        ++name;
    }
    const char *end = name + strlen(name);
    while (end > name && isspace((unsigned char)end[-1])) {
        --end;
    }
    const size_t length = (size_t)(end - name);
    if (length != strlen(expected)) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (toupper((unsigned char)name[index]) != expected[index]) {
            return false;
        }
    }
    return true;
}

static bool generic_serial_name(const char *name)
{
    static const char *const names[] = {
        "BT", "BLE", "UART", "SERIAL", "HC-05", "HC-06",
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (trimmed_name_equals(name, names[index])) {
            return true;
        }
    }
    return false;
}

static bool pkoc_identity(const char *name)
{
    if (name == NULL) {
        return false;
    }
    while (*name != '\0' && isspace((unsigned char)*name)) {
        ++name;
    }
    static const char prefix[] = "PKOC";
    for (size_t index = 0; index < sizeof(prefix) - 1U; ++index) {
        if (name[index] == '\0' ||
            toupper((unsigned char)name[index]) != prefix[index]) {
            return false;
        }
    }
    return true;
}

static void prune_serial_tracks(int64_t now_ms)
{
    for (size_t index = 0; index < MAX_SERIAL_TRACKS; ++index) {
        if (s_serial_tracks[index].used &&
            elapsed_more_than(now_ms,
                              s_serial_tracks[index].last_seen_ms,
                              STATE_CLEAR_AFTER_MS)) {
            memset(&s_serial_tracks[index], 0, sizeof(s_serial_tracks[index]));
        }
    }
}

static serial_track_t *find_serial_track(const uint8_t mac[6])
{
    for (size_t index = 0; index < MAX_SERIAL_TRACKS; ++index) {
        if (s_serial_tracks[index].used && same_mac(s_serial_tracks[index].mac, mac)) {
            return &s_serial_tracks[index];
        }
    }
    return NULL;
}

static serial_track_t *serial_track_slot(void)
{
    serial_track_t *oldest = NULL;
    for (size_t index = 0; index < MAX_SERIAL_TRACKS; ++index) {
        serial_track_t *track = &s_serial_tracks[index];
        if (!track->used) {
            return track;
        }
        if (oldest == NULL || track->last_seen_ms < oldest->last_seen_ms ||
            (track->last_seen_ms == oldest->last_seen_ms &&
             track->insertion_order < oldest->insertion_order)) {
            oldest = track;
        }
    }
    return oldest;
}

static serial_track_t *create_serial_track(const ble_threat_observation_t *observation,
                                           uint16_t service_uuid)
{
    serial_track_t *track = serial_track_slot();
    if (track == NULL) {
        return NULL;
    }
    memset(track, 0, sizeof(*track));
    track->used = true;
    memcpy(track->mac, observation->mac, sizeof(track->mac));
    track->serial_service_uuid = service_uuid;
    track->first_seen_ms = observation->observed_ms;
    track->last_seen_ms = observation->observed_ms;
    track->observation_count = 1;
    track->insertion_order = s_serial_insertion_order;
    if (s_serial_insertion_order < UINT64_MAX) {
        ++s_serial_insertion_order;
    }
    track->strongest_rssi = observation->rssi;
    track->max_unique_service_count = unique_service_uuid_count(observation);
    track->has_generic_name = generic_serial_name(observation->local_name);
    track->trusted = observation->trusted_identity;
    track->has_pkoc_identity = pkoc_identity(observation->local_name);
    track->connectable = observation->connectable;
    return track;
}

static void update_serial_track(serial_track_t *track,
                                const ble_threat_observation_t *observation)
{
    track->last_seen_ms = observation->observed_ms;
    if (track->observation_count < UINT32_MAX) {
        ++track->observation_count;
    }
    if (observation->rssi > track->strongest_rssi) {
        track->strongest_rssi = observation->rssi;
    }
    const uint8_t unique_service_count = unique_service_uuid_count(observation);
    if (unique_service_count > track->max_unique_service_count) {
        track->max_unique_service_count = unique_service_count;
    }
    track->has_generic_name = track->has_generic_name ||
                              generic_serial_name(observation->local_name);
    track->trusted = track->trusted || observation->trusted_identity;
    track->has_pkoc_identity = track->has_pkoc_identity ||
                               pkoc_identity(observation->local_name);
    track->connectable = track->connectable || observation->connectable;
}

static unsigned evidence_count(uint8_t evidence)
{
    unsigned count = 0;
    while (evidence != 0) {
        count += evidence & 1U;
        evidence >>= 1U;
    }
    return count;
}

static uint8_t serial_evidence(const serial_track_t *track)
{
    uint8_t evidence = BLE_THREAT_EVIDENCE_SERIAL_UUID;
    if (track->max_unique_service_count == 1U) {
        evidence |= BLE_THREAT_EVIDENCE_SPARSE;
    }
    if (track->has_generic_name) {
        evidence |= BLE_THREAT_EVIDENCE_GENERIC_NAME;
    }
    if (track->observation_count >= SERIAL_PERSISTENCE_OBSERVATIONS &&
        elapsed_at_least(track->last_seen_ms,
                         track->first_seen_ms,
                         SERIAL_PERSISTENCE_MS)) {
        evidence |= BLE_THREAT_EVIDENCE_PERSISTENT;
    }
    if (track->strongest_rssi >= CLOSE_RSSI_DBM) {
        evidence |= BLE_THREAT_EVIDENCE_CLOSE;
    }
    if (track->connectable) {
        evidence |= BLE_THREAT_EVIDENCE_CONNECTABLE;
    }
    if (!track->trusted && !track->has_pkoc_identity) {
        evidence |= BLE_THREAT_EVIDENCE_UNTRUSTED;
    }
    return evidence;
}

static bool observe_serial(const ble_threat_observation_t *observation,
                           ble_threat_signal_t *signal_out,
                           bool consume_signal)
{
    prune_serial_tracks(observation->observed_ms);
    uint16_t service_uuid = 0;
    if (!serial_service_uuid(observation, &service_uuid)) {
        return false;
    }

    serial_track_t *track = find_serial_track(observation->mac);
    if (track != NULL && within_dedupe_window(observation->observed_ms,
                                              track->last_seen_ms)) {
        return false;
    }
    if (track == NULL) {
        track = create_serial_track(observation, service_uuid);
        if (track == NULL) {
            return false;
        }
    } else {
        update_serial_track(track, observation);
    }
    if (track->alerted || track->trusted || track->has_pkoc_identity) {
        return false;
    }

    const uint8_t evidence = serial_evidence(track);
    if ((evidence & SERIAL_REQUIRED_EVIDENCE) != SERIAL_REQUIRED_EVIDENCE ||
        evidence_count(evidence & SERIAL_SUPPORTING_EVIDENCE) < 2U) {
        return false;
    }

    track->alerted = consume_signal;
    signal_out->kind = BLE_THREAT_SERIAL_SKIMMER;
    signal_out->entity_hash = serial_entity_hash(track->mac);
    signal_out->observation_count = track->observation_count > UINT16_MAX ?
                                    UINT16_MAX : (uint16_t)track->observation_count;
    signal_out->serial_service_uuid = track->serial_service_uuid;
    signal_out->evidence_mask = evidence;
    signal_out->strongest_rssi = track->strongest_rssi;
    signal_out->confidence = (float)evidence_count(evidence) / 7.0f;
    return true;
}

void ble_threat_detector_init(void)
{
    ble_threat_detector_reset();
}

bool ble_threat_detector_observe(const ble_threat_observation_t *observation,
                                 ble_threat_signal_t *signal_out)
{
    if (signal_out == NULL) {
        return false;
    }
    memset(signal_out, 0, sizeof(*signal_out));
    if (observation == NULL) {
        return false;
    }

    if (s_has_last_observed_ms && observation->observed_ms < s_last_observed_ms) {
        ble_threat_detector_reset();
    }
    s_has_last_observed_ms = true;
    s_last_observed_ms = observation->observed_ms;

    const bool prompt_signal = observe_prompt(observation, signal_out);
    ble_threat_signal_t serial_signal = {0};
    const bool has_serial_signal = observe_serial(observation,
                                                  &serial_signal,
                                                  !prompt_signal);
    if (prompt_signal) {
        return true;
    }
    if (has_serial_signal) {
        *signal_out = serial_signal;
        return true;
    }
    return false;
}

void ble_threat_detector_reset(void)
{
    clear_prompt_state();
    memset(s_serial_tracks, 0, sizeof(s_serial_tracks));
    s_serial_insertion_order = 0;
    s_has_last_observed_ms = false;
    s_last_observed_ms = 0;
}
