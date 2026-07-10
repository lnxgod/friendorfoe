#include "badge_investigation_policy.h"

#include <string.h>

static void copy_text(char *out, size_t out_len, const char *text)
{
    if (!out || out_len == 0) return;
    size_t len = 0;
    if (text) {
        while (len + 1 < out_len && text[len] != '\0') ++len;
        if (len > 0) memcpy(out, text, len);
    }
    out[len] = '\0';
}

void badge_investigation_selection_copy(
    badge_investigation_selection_t *out,
    bool has_entity,
    uint8_t source,
    badge_threat_category_t category,
    const char *key,
    const char *bssid)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->has_entity = has_entity;
    out->source = source;
    out->category = category;
    copy_text(out->key, sizeof(out->key), key);
    copy_text(out->bssid, sizeof(out->bssid), bssid);
}

static bool hex_digit(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

bool badge_investigation_mac_is_valid(const char *mac)
{
    if (!mac) return false;
    for (int i = 0; i < 17; ++i) {
        if (mac[i] == '\0') return false;
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') return false;
        } else if (!hex_digit(mac[i])) {
            return false;
        }
    }
    return mac[17] == '\0';
}

badge_hold_action_t badge_investigation_hold_action(
    const badge_investigation_selection_t *selection)
{
    if (!selection || !selection->has_entity) {
        return BADGE_HOLD_PAIR_PHONE;
    }
    if (selection->category == BADGE_THREAT_CATEGORY_BLE_SPAM) {
        return BADGE_HOLD_INVESTIGATE_PASSIVE;
    }
    if (selection->source == DETECTION_SRC_BLE_FINGERPRINT &&
        badge_investigation_mac_is_valid(selection->bssid)) {
        return BADGE_HOLD_INVESTIGATE_GATT;
    }
    return BADGE_HOLD_SHOW_DETAIL;
}

bool badge_investigation_state_is_active(ble_investigation_state_t state)
{
    return state >= BLE_INV_QUEUED && state <= BLE_INV_READING;
}

bool badge_investigation_state_is_terminal(ble_investigation_state_t state)
{
    return state >= BLE_INV_COMPLETE && state <= BLE_INV_CANCELLED;
}

int badge_investigation_normalize_page(ble_investigation_state_t state,
                                       int page)
{
    if (badge_investigation_state_is_active(state)) return 0;
    if (!badge_investigation_state_is_terminal(state)) return 0;
    return page >= 1 && page <= 3 ? page : 1;
}

int badge_investigation_next_page(ble_investigation_state_t state, int page)
{
    int normalized = badge_investigation_normalize_page(state, page);
    if (!badge_investigation_state_is_terminal(state)) return normalized;
    return normalized >= 3 ? 1 : normalized + 1;
}

static size_t append_sanitized(char *out, size_t out_len, size_t used,
                               const char *text, size_t text_bound,
                               size_t max_chars)
{
    size_t source_used = 0;
    while (text && source_used < text_bound && source_used < max_chars &&
           text[source_used] && used + 1 < out_len) {
        unsigned char ch = (unsigned char)text[source_used++];
        out[used++] = ch >= 0x20 && ch <= 0x7e ? (char)ch : '?';
    }
    out[used] = '\0';
    return used;
}

bool badge_investigation_format_read_evidence(
    const ble_investigation_read_t *read,
    char *out,
    size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!read || read->uuid[0] == '\0') return false;

    size_t used = append_sanitized(out, out_len, 0,
                                   read->uuid, sizeof(read->uuid), 8);
    if (used + 1 < out_len) {
        out[used++] = ' ';
        out[used] = '\0';
    }
    if (read->value_hex[0]) {
        used = append_sanitized(out, out_len, used,
                                read->value_hex, sizeof(read->value_hex), 16);
    } else {
        used = append_sanitized(out, out_len, used, "empty", 6, 5);
    }
    return used > 0;
}
