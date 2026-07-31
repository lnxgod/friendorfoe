#include "badge_con_runtime.h"

#if !defined(FOF_DC34_GAME_CANARY)
#error "badge_con_runtime is private to the explicit game canary"
#endif

#include "badge_runtime.h"
#include "nvs_config.h"

#include <string.h>

#ifndef UNIT_TESTING
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#else
void esp_fill_random(void *buffer, size_t length);
int64_t esp_timer_get_time(void);
#endif

#define BADGE_CON_NVS_KEY "game_state_v1"

static badge_con_game_state_t s_state;
static bool s_initialized;
static bool s_mutation_in_progress;
static uint32_t s_peer;
static uint8_t s_session;
static uint8_t s_sequence_start;
static uint32_t s_self_ack_peer;
static uint8_t s_self_ack_session;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t random_nonzero_peer(void)
{
    uint32_t value = 0U;
    for (unsigned attempt = 0U; attempt < 4U && value == 0U; attempt++) {
        esp_fill_random(&value, sizeof(value));
        value &= 0x00ffffffU;
    }
    return value == 0U ? 1U : value;
}

static uint8_t random_nonzero_byte(void)
{
    uint8_t value = 0U;
    for (unsigned attempt = 0U; attempt < 4U && value == 0U; attempt++) {
        esp_fill_random(&value, sizeof(value));
    }
    return value == 0U ? 1U : value;
}

static void write_rtc_state(const badge_con_game_state_t *state,
                            uint32_t generation)
{
    uint8_t record[BADGE_CON_RTC_RECORD_BYTES] = {0};
    if (!state) {
        return;
    }
    if (generation == 0U) {
        generation = 1U;
    }
    if (!badge_con_game_encode_rtc(state, generation, record)) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    (void)badge_runtime_game_rtc_write(record, sizeof(record));
    portEXIT_CRITICAL(&s_lock);
}

static bool expected_reboot_hook(uint32_t generation)
{
    uint8_t record[BADGE_CON_RTC_RECORD_BYTES] = {0};
    bool written = false;
    /*
     * Lock order is game -> badge_runtime. badge_runtime invokes this hook
     * without its own lock held, so there is no reverse edge or ABBA cycle.
     * Keep the write inside s_lock so a concurrent mutation cannot overwrite
     * a newer generation record out of order.
     */
    portENTER_CRITICAL(&s_lock);
    if (s_initialized &&
        badge_con_game_encode_rtc(&s_state, generation, record)) {
        written = badge_runtime_game_rtc_write(record, sizeof(record));
    }
    portEXIT_CRITICAL(&s_lock);
    return written;
}

static bool begin_mutation(badge_con_game_state_t *candidate)
{
    if (!candidate) {
        return false;
    }
    bool allowed = false;
    portENTER_CRITICAL(&s_lock);
    if (s_initialized && !s_mutation_in_progress) {
        s_mutation_in_progress = true;
        *candidate = s_state;
        allowed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return allowed;
}

static void cancel_mutation(void)
{
    portENTER_CRITICAL(&s_lock);
    s_mutation_in_progress = false;
    portEXIT_CRITICAL(&s_lock);
}

static bool states_equal(const badge_con_game_state_t *left,
                         const badge_con_game_state_t *right)
{
    return left && right &&
           left->seed == right->seed &&
           left->role == right->role &&
           left->active == right->active &&
           left->shield == right->shield &&
           left->scar_level == right->scar_level &&
           left->dead == right->dead &&
           left->last_decay_ms == right->last_decay_ms;
}

static void commit_mutation(const badge_con_game_state_t *candidate)
{
    portENTER_CRITICAL(&s_lock);
    s_state = *candidate;
    s_mutation_in_progress = false;
    portEXIT_CRITICAL(&s_lock);
}

static bool persist_state(const badge_con_game_state_t *state)
{
    uint8_t record[BADGE_CON_NVS_RECORD_BYTES] = {0};
    return badge_con_game_encode_nvs(state, record) &&
           nvs_config_set_blob(BADGE_CON_NVS_KEY, record, sizeof(record));
}

void badge_con_runtime_init(void)
{
    badge_con_game_state_t restored;
    badge_con_game_defaults(&restored);

    uint8_t nvs_record[BADGE_CON_NVS_RECORD_BYTES] = {0};
    size_t nvs_size = 0U;
    if (nvs_config_read_blob(
            BADGE_CON_NVS_KEY, nvs_record, sizeof(nvs_record), &nvs_size) ==
        NVS_CONFIG_BLOB_PRESENT) {
        badge_con_game_state_t decoded;
        badge_con_nvs_decode_result_t decoded_result =
            badge_con_game_decode_nvs(nvs_record, nvs_size, &decoded);
        if (decoded_result == BADGE_CON_NVS_VALID ||
            decoded_result == BADGE_CON_NVS_SEED_ONLY) {
            restored = decoded;
        }
    }

    uint32_t current_ms = now_ms();
    restored.last_decay_ms = current_ms;

    uint32_t peer = random_nonzero_peer();
    uint8_t session = random_nonzero_byte();
    uint8_t sequence = 0U;
    esp_fill_random(&sequence, sizeof(sequence));

    portENTER_CRITICAL(&s_lock);
    s_state = restored;
    s_peer = peer;
    s_session = session;
    s_sequence_start = sequence;
    s_self_ack_peer = 0U;
    s_self_ack_session = 0U;
    s_mutation_in_progress = false;
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);

    write_rtc_state(&restored, 1U);
    badge_runtime_set_expected_reboot_hook(expected_reboot_hook);
}

bool badge_con_runtime_set_factory_seed(badge_con_role_t seed)
{
    if (badge_con_role_name(seed) == NULL) {
        return false;
    }
    badge_con_game_state_t candidate;
    if (!begin_mutation(&candidate)) {
        return false;
    }
    badge_con_game_apply_factory_seed(&candidate, seed, now_ms());
    if (!persist_state(&candidate)) {
        cancel_mutation();
        return false;
    }
    commit_mutation(&candidate);
    return true;
}

bool badge_con_runtime_activate_after_easter(void)
{
    badge_con_game_state_t candidate;
    if (!begin_mutation(&candidate)) {
        return false;
    }
    if (!badge_con_game_activate(&candidate, now_ms())) {
        cancel_mutation();
        return false;
    }
    commit_mutation(&candidate);
    return true;
}

badge_con_effect_t badge_con_runtime_apply_qualified_peer(
    const badge_con_packet_t *packet)
{
    if (!packet) {
        return BADGE_CON_EFFECT_NONE;
    }
    badge_con_game_state_t candidate;
    if (!begin_mutation(&candidate)) {
        return BADGE_CON_EFFECT_NONE;
    }

    badge_con_game_state_t before = candidate;
    badge_con_effect_t effect = badge_con_game_apply_peer(
        &candidate, packet->role, packet->super, packet->rssi, now_ms());
    bool changed = !states_equal(&before, &candidate);
    if (!changed) {
        cancel_mutation();
        return effect;
    }

    commit_mutation(&candidate);
    return effect;
}

bool badge_con_runtime_snapshot(badge_con_snapshot_t *out)
{
    if (!out) {
        return false;
    }
    *out = (badge_con_snapshot_t) {
        .seed = BADGE_CON_ROLE_NORMAL,
        .role = BADGE_CON_ROLE_NORMAL,
        .active = false,
        .shield = 0U,
        .maximum = 100U,
        .scar_level = 0U,
        .cured = false,
        .dead = false,
        .super = false,
    };
    badge_con_game_state_t candidate;
    if (!begin_mutation(&candidate)) {
        bool initialized;
        portENTER_CRITICAL(&s_lock);
        initialized = s_initialized;
        candidate = s_state;
        portEXIT_CRITICAL(&s_lock);
        if (!initialized) {
            return false;
        }
        badge_con_game_snapshot(&candidate, now_ms(), out);
        return true;
    }
    badge_con_game_state_t before = candidate;
    badge_con_game_snapshot(&candidate, now_ms(), out);
    bool changed = !states_equal(&before, &candidate);
    if (!changed) {
        cancel_mutation();
        return true;
    }
    commit_mutation(&candidate);
    return true;
}

bool badge_con_runtime_identity(uint32_t *peer_out, uint8_t *session_out)
{
    if (!peer_out || !session_out) {
        return false;
    }
    *peer_out = 0U;
    *session_out = 0U;
    bool initialized;
    portENTER_CRITICAL(&s_lock);
    initialized = s_initialized;
    if (initialized) {
        *peer_out = s_peer;
        *session_out = s_session;
    }
    portEXIT_CRITICAL(&s_lock);
    return initialized;
}

bool badge_con_runtime_sequence_start(uint8_t *sequence_out)
{
    if (!sequence_out) {
        return false;
    }
    *sequence_out = 0U;
    bool initialized;
    portENTER_CRITICAL(&s_lock);
    initialized = s_initialized;
    if (initialized) {
        *sequence_out = s_sequence_start;
    }
    portEXIT_CRITICAL(&s_lock);
    return initialized;
}

bool badge_con_runtime_self_ack_matches(uint32_t peer, uint8_t session)
{
    bool matches;
    portENTER_CRITICAL(&s_lock);
    matches = s_initialized &&
              peer == s_peer &&
              session == s_session &&
              s_self_ack_peer == s_peer &&
              s_self_ack_session == s_session;
    portEXIT_CRITICAL(&s_lock);
    return matches;
}

void badge_con_runtime_note_self_ack(uint32_t peer, uint8_t session)
{
    portENTER_CRITICAL(&s_lock);
    if (s_initialized && peer == s_peer && session == s_session) {
        s_self_ack_peer = peer;
        s_self_ack_session = session;
    }
    portEXIT_CRITICAL(&s_lock);
}

void badge_con_runtime_clear_self_ack(void)
{
    portENTER_CRITICAL(&s_lock);
    s_self_ack_peer = 0U;
    s_self_ack_session = 0U;
    portEXIT_CRITICAL(&s_lock);
}

#ifdef UNIT_TESTING
void badge_con_runtime_test_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_state, 0, sizeof(s_state));
    badge_runtime_game_rtc_clear();
    s_initialized = false;
    s_mutation_in_progress = false;
    s_peer = 0U;
    s_session = 0U;
    s_sequence_start = 0U;
    s_self_ack_peer = 0U;
    s_self_ack_session = 0U;
    portEXIT_CRITICAL(&s_lock);
}
#endif
