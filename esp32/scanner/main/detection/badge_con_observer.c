#include "badge_con_observer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>

static bool s_ble_primary;
static uint32_t s_requested_self_word;
static uint32_t s_applied_self_word;
static badge_con_encounter_table_t s_encounters;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_pending_valid;
static badge_con_packet_t s_pending_packet;

#if defined(UNIT_TESTING)
__attribute__((weak))
void badge_con_observer_test_note_critical(bool entering)
{
    (void)entering;
}

__attribute__((weak))
void badge_con_observer_test_note_work(void)
{
}
#define BADGE_CON_OBSERVER_NOTE_CRITICAL(value) \
    badge_con_observer_test_note_critical(value)
#define BADGE_CON_OBSERVER_NOTE_WORK() \
    badge_con_observer_test_note_work()
#else
#define BADGE_CON_OBSERVER_NOTE_CRITICAL(value) ((void)(value))
#define BADGE_CON_OBSERVER_NOTE_WORK() ((void)0)
#endif

static void pending_lock(void)
{
    portENTER_CRITICAL(&s_pending_lock);
    BADGE_CON_OBSERVER_NOTE_CRITICAL(true);
}

static void pending_unlock(void)
{
    BADGE_CON_OBSERVER_NOTE_CRITICAL(false);
    portEXIT_CRITICAL(&s_pending_lock);
}

static uint32_t pack_self(uint32_t peer, uint8_t session)
{
    return (peer << 8U) | session;
}

static void clear_pending_self(uint32_t peer, uint8_t session)
{
    pending_lock();
    if (s_pending_valid &&
        s_pending_packet.peer == peer &&
        s_pending_packet.session == session) {
        memset(&s_pending_packet, 0, sizeof(s_pending_packet));
        s_pending_valid = false;
    }
    pending_unlock();
}

static void apply_requested_self(void)
{
    uint32_t requested =
        __atomic_load_n(&s_requested_self_word, __ATOMIC_ACQUIRE);
    if (requested == 0U || requested == s_applied_self_word) {
        return;
    }

    uint32_t peer = requested >> 8U;
    uint8_t session = (uint8_t)requested;
    BADGE_CON_OBSERVER_NOTE_WORK();
    if (badge_con_encounter_set_self(&s_encounters, peer, session)) {
        s_applied_self_word = requested;
        clear_pending_self(peer, session);
    }
}

void badge_con_observer_init(bool ble_primary)
{
    s_ble_primary = ble_primary;
    __atomic_store_n(&s_requested_self_word, 0U, __ATOMIC_RELEASE);
    s_applied_self_word = 0U;
    badge_con_encounter_init(&s_encounters);
    pending_lock();
    memset(&s_pending_packet, 0, sizeof(s_pending_packet));
    s_pending_valid = false;
    pending_unlock();
}

bool badge_con_observer_set_self(uint32_t peer, uint8_t session)
{
    if (!s_ble_primary ||
        peer == 0U ||
        peer > 0xFFFFFFU ||
        session == 0U) {
        return false;
    }

    clear_pending_self(peer, session);
    __atomic_store_n(
        &s_requested_self_word,
        pack_self(peer, session),
        __ATOMIC_RELEASE);
    return true;
}

badge_con_frame_result_t badge_con_observer_consume(
    const uint8_t *advertisement,
    size_t advertisement_size,
    int8_t rssi,
    uint32_t now_ms,
    badge_con_observe_result_t *observe_result_out)
{
    if (s_ble_primary) {
        apply_requested_self();
    }

    badge_con_packet_t packet = {0};
    BADGE_CON_OBSERVER_NOTE_WORK();
    badge_con_frame_result_t frame = badge_con_parse_advertisement(
        advertisement, advertisement_size, rssi, &packet);
    if (frame != BADGE_CON_FRAME_VALID) {
        return frame;
    }
    if (!s_ble_primary) {
        if (observe_result_out) {
            *observe_result_out = BADGE_CON_OBSERVE_DROPPED_TABLE_FULL;
        }
        return frame;
    }

    BADGE_CON_OBSERVER_NOTE_WORK();
    badge_con_observe_result_t observed =
        badge_con_encounter_consume(&s_encounters, &packet, now_ms);
    if (observe_result_out) {
        *observe_result_out = observed;
    }
    if (observed == BADGE_CON_OBSERVE_QUALIFIED) {
        pending_lock();
        s_pending_packet = packet;
        s_pending_valid = true;
        pending_unlock();
    }
    return frame;
}

bool badge_con_observer_take_pending(badge_con_packet_t *out)
{
    if (!out) {
        return false;
    }

    bool taken = false;
    pending_lock();
    if (s_pending_valid) {
        *out = s_pending_packet;
        memset(&s_pending_packet, 0, sizeof(s_pending_packet));
        s_pending_valid = false;
        taken = true;
    }
    pending_unlock();
    return taken;
}
