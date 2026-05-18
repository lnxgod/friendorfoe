#include "badge_button_gesture.h"

void badge_button_gesture_init(badge_button_gesture_t *state,
                               uint32_t double_tap_ms)
{
    if (!state) {
        return;
    }
    state->double_tap_ms = double_tap_ms;
    state->pending_single = false;
    state->pending_ms = 0;
}

badge_button_gesture_event_t badge_button_gesture_note_tap(
    badge_button_gesture_t *state,
    uint32_t now_ms)
{
    if (!state) {
        return BADGE_BUTTON_GESTURE_NONE;
    }
    uint32_t window = state->double_tap_ms;
    if (window == 0) {
        window = 1;
    }
    if (state->pending_single && (uint32_t)(now_ms - state->pending_ms) <= window) {
        state->pending_single = false;
        return BADGE_BUTTON_GESTURE_DOUBLE;
    }
    state->pending_single = true;
    state->pending_ms = now_ms;
    return BADGE_BUTTON_GESTURE_NONE;
}

badge_button_gesture_event_t badge_button_gesture_note_long(
    badge_button_gesture_t *state,
    uint32_t now_ms)
{
    (void)now_ms;
    if (state) {
        state->pending_single = false;
    }
    return BADGE_BUTTON_GESTURE_LONG;
}

badge_button_gesture_event_t badge_button_gesture_poll(
    badge_button_gesture_t *state,
    uint32_t now_ms)
{
    if (!state || !state->pending_single) {
        return BADGE_BUTTON_GESTURE_NONE;
    }
    uint32_t window = state->double_tap_ms;
    if (window == 0) {
        window = 1;
    }
    if ((uint32_t)(now_ms - state->pending_ms) <= window) {
        return BADGE_BUTTON_GESTURE_NONE;
    }
    state->pending_single = false;
    return BADGE_BUTTON_GESTURE_SINGLE;
}

void badge_button_gesture_cancel(badge_button_gesture_t *state)
{
    if (!state) {
        return;
    }
    state->pending_single = false;
}

bool badge_button_gesture_pending_single(const badge_button_gesture_t *state)
{
    return state && state->pending_single;
}
