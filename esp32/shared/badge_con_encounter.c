#include "badge_con_encounter.h"

#include <string.h>

static bool entry_expired(const badge_con_peer_entry_t *entry,
                          uint32_t now_ms)
{
    return entry->used &&
        (uint32_t)(now_ms - entry->last_seen_ms) >
            BADGE_CON_QUORUM_WINDOW_MS;
}

static void expire_samples(badge_con_peer_entry_t *entry,
                           uint32_t now_ms)
{
    uint8_t retained = 0U;
    for (uint8_t i = 0U; i < entry->recent_count; ++i) {
        if ((uint32_t)(now_ms - entry->recent_ms[i]) <=
            BADGE_CON_QUORUM_WINDOW_MS) {
            entry->recent_sequence[retained] = entry->recent_sequence[i];
            entry->recent_ms[retained] = entry->recent_ms[i];
            ++retained;
        }
    }
    entry->recent_count = retained;
    if (retained == 0U) {
        entry->emitted = false;
        entry->last_emitted_sequence = 0U;
        entry->last_emitted_ms = 0U;
    }
}

static bool sequence_is_recent(const badge_con_peer_entry_t *entry,
                               uint8_t sequence)
{
    for (uint8_t i = 0U; i < entry->recent_count; ++i) {
        if (entry->recent_sequence[i] == sequence) {
            return true;
        }
    }
    return false;
}

static void append_sample(badge_con_peer_entry_t *entry,
                          uint8_t sequence,
                          uint32_t now_ms)
{
    if (entry->recent_count == BADGE_CON_QUORUM_PACKETS) {
        for (uint8_t i = 1U; i < BADGE_CON_QUORUM_PACKETS; ++i) {
            entry->recent_sequence[i - 1U] = entry->recent_sequence[i];
            entry->recent_ms[i - 1U] = entry->recent_ms[i];
        }
        --entry->recent_count;
    }
    uint8_t index = entry->recent_count;
    entry->recent_sequence[index] = sequence;
    entry->recent_ms[index] = now_ms;
    ++entry->recent_count;
    entry->last_seen_ms = now_ms;
}

void badge_con_encounter_init(badge_con_encounter_table_t *table)
{
    if (table) {
        memset(table, 0, sizeof(*table));
    }
}

bool badge_con_encounter_set_self(badge_con_encounter_table_t *table,
                                  uint32_t peer,
                                  uint8_t session)
{
    if (!table || peer == 0U || session == 0U) {
        return false;
    }
    table->self_peer = peer;
    table->self_session = session;
    table->self_valid = true;
    for (size_t i = 0; i < BADGE_CON_PEER_CAPACITY; ++i) {
        if (table->peer[i].used &&
            table->peer[i].peer == peer &&
            table->peer[i].session == session) {
            memset(&table->peer[i], 0, sizeof(table->peer[i]));
        }
    }
    return true;
}

badge_con_observe_result_t badge_con_encounter_consume(
    badge_con_encounter_table_t *table,
    const badge_con_packet_t *packet,
    uint32_t now_ms)
{
    if (!table || !packet) {
        return BADGE_CON_OBSERVE_DROPPED_TABLE_FULL;
    }
    if (packet->rssi < BADGE_CON_MIN_RSSI) {
        return BADGE_CON_OBSERVE_DROPPED_WEAK;
    }
    if (table->self_valid &&
        packet->peer == table->self_peer &&
        packet->session == table->self_session) {
        return BADGE_CON_OBSERVE_DROPPED_SELF;
    }

    badge_con_peer_entry_t *matching = NULL;
    badge_con_peer_entry_t *expired = NULL;
    badge_con_peer_entry_t *unused = NULL;
    for (size_t i = 0; i < BADGE_CON_PEER_CAPACITY; ++i) {
        badge_con_peer_entry_t *candidate = &table->peer[i];
        if (candidate->used &&
            candidate->peer == packet->peer &&
            candidate->session == packet->session) {
            matching = candidate;
            break;
        }
        if (!expired && entry_expired(candidate, now_ms)) {
            expired = candidate;
        } else if (!unused && !candidate->used) {
            unused = candidate;
        }
    }

    badge_con_peer_entry_t *entry = matching;
    if (!entry) {
        entry = expired ? expired : unused;
        if (!entry) {
            return BADGE_CON_OBSERVE_DROPPED_TABLE_FULL;
        }
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->peer = packet->peer;
        entry->session = packet->session;
    } else if (entry_expired(entry, now_ms)) {
        uint32_t peer = entry->peer;
        uint8_t session = entry->session;
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->peer = peer;
        entry->session = session;
    }

    expire_samples(entry, now_ms);
    if (sequence_is_recent(entry, packet->sequence)) {
        return BADGE_CON_OBSERVE_DROPPED_DUPLICATE;
    }

    append_sample(entry, packet->sequence, now_ms);
    if (entry->recent_count < BADGE_CON_QUORUM_PACKETS) {
        return BADGE_CON_OBSERVE_COUNTED;
    }
    if (entry->emitted &&
        (uint32_t)(now_ms - entry->last_emitted_ms) <
            BADGE_CON_EFFECT_RATE_MS) {
        return BADGE_CON_OBSERVE_RATE_LIMITED;
    }

    entry->emitted = true;
    entry->last_emitted_sequence = packet->sequence;
    entry->last_emitted_ms = now_ms;
    return BADGE_CON_OBSERVE_QUALIFIED;
}
