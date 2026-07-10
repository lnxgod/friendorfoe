#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "badge_threat_policy.h"
#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_HOLD_PAIR_PHONE = 0,
    BADGE_HOLD_SHOW_DETAIL,
    BADGE_HOLD_INVESTIGATE_GATT,
    BADGE_HOLD_INVESTIGATE_PASSIVE,
} badge_hold_action_t;

typedef struct {
    bool has_entity;
    uint8_t source;
    badge_threat_category_t category;
    char key[BADGE_THREAT_KEY_LEN];
    char bssid[18];
} badge_investigation_selection_t;

void badge_investigation_selection_copy(
    badge_investigation_selection_t *out,
    bool has_entity,
    uint8_t source,
    badge_threat_category_t category,
    const char *key,
    const char *bssid);

bool badge_investigation_mac_is_valid(const char *mac);
badge_hold_action_t badge_investigation_hold_action(
    const badge_investigation_selection_t *selection);

bool badge_investigation_state_is_active(ble_investigation_state_t state);
bool badge_investigation_state_is_terminal(ble_investigation_state_t state);
int badge_investigation_normalize_page(ble_investigation_state_t state,
                                       int page);
int badge_investigation_next_page(ble_investigation_state_t state, int page);

#ifdef __cplusplus
}
#endif
