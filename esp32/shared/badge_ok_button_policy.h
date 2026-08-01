#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_OK_BUTTON_ACTION_NONE = 0,
    BADGE_OK_BUTTON_ACTION_DETAIL,
    BADGE_OK_BUTTON_ACTION_EASTER,
} badge_ok_button_action_t;

typedef struct {
    uint32_t tap_cutoff_ms;
    uint32_t easter_hold_ms;
    uint32_t pressed_at_ms;
    bool tracking;
    bool easter_dispatched;
    bool suppress_until_release;
} badge_ok_button_policy_t;

void badge_ok_button_policy_init(
    badge_ok_button_policy_t *state,
    uint32_t tap_cutoff_ms,
    uint32_t easter_hold_ms,
    bool button_pressed_at_init);

badge_ok_button_action_t badge_ok_button_policy_update(
    badge_ok_button_policy_t *state,
    bool button_pressed,
    bool dispatch_allowed,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif
