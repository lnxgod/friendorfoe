#include "badge_ok_button_policy.h"

static void finish_press(badge_ok_button_policy_t *state)
{
    state->tracking = false;
    state->easter_dispatched = false;
    state->suppress_until_release = false;
}

void badge_ok_button_policy_init(
    badge_ok_button_policy_t *state,
    uint32_t tap_cutoff_ms,
    uint32_t easter_hold_ms,
    bool button_pressed_at_init)
{
    if (!state) {
        return;
    }
    state->tap_cutoff_ms = tap_cutoff_ms == 0U ? 1U : tap_cutoff_ms;
    state->easter_hold_ms = easter_hold_ms == 0U ? 1U : easter_hold_ms;
    state->pressed_at_ms = 0U;
    state->tracking = button_pressed_at_init;
    state->easter_dispatched = false;
    state->suppress_until_release = button_pressed_at_init;
}

badge_ok_button_action_t badge_ok_button_policy_update(
    badge_ok_button_policy_t *state,
    bool button_pressed,
    bool dispatch_allowed,
    uint32_t now_ms)
{
    if (!state) {
        return BADGE_OK_BUTTON_ACTION_NONE;
    }

    if (button_pressed) {
        if (!state->tracking) {
            state->tracking = true;
            state->pressed_at_ms = now_ms;
            state->easter_dispatched = false;
            state->suppress_until_release = false;
        }
        if (!dispatch_allowed) {
            state->suppress_until_release = true;
        }
        if (!state->suppress_until_release &&
            !state->easter_dispatched &&
            (uint32_t)(now_ms - state->pressed_at_ms) >=
                state->easter_hold_ms) {
            state->easter_dispatched = true;
            return BADGE_OK_BUTTON_ACTION_EASTER;
        }
        return BADGE_OK_BUTTON_ACTION_NONE;
    }

    if (!state->tracking) {
        return BADGE_OK_BUTTON_ACTION_NONE;
    }
    uint32_t held_ms = now_ms - state->pressed_at_ms;
    bool easter = !state->suppress_until_release &&
        !state->easter_dispatched &&
        held_ms >= state->easter_hold_ms;
    bool detail = !state->suppress_until_release &&
        !state->easter_dispatched &&
        held_ms < state->tap_cutoff_ms;
    finish_press(state);
    return easter
        ? BADGE_OK_BUTTON_ACTION_EASTER
        : (detail ? BADGE_OK_BUTTON_ACTION_DETAIL
                  : BADGE_OK_BUTTON_ACTION_NONE);
}
