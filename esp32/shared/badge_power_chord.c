#include "badge_power_chord.h"

void badge_power_chord_init(badge_power_chord_t *state,
                            uint32_t hold_ms,
                            bool button_one_pressed,
                            bool button_two_pressed,
                            uint32_t now_ms)
{
    if (!state) {
        return;
    }
    state->hold_ms = hold_ms == 0U ? 1U : hold_ms;
    state->hold_started_ms = now_ms;
    state->tracking = false;
    state->release_required = button_one_pressed || button_two_pressed;
}

badge_power_chord_event_t badge_power_chord_update(
    badge_power_chord_t *state,
    bool button_one_pressed,
    bool button_two_pressed,
    bool chord_allowed,
    uint32_t now_ms)
{
    if (!state) {
        return BADGE_POWER_CHORD_NONE;
    }

    if (!button_one_pressed && !button_two_pressed) {
        state->tracking = false;
        state->release_required = false;
        return BADGE_POWER_CHORD_NONE;
    }

    if (state->release_required) {
        return BADGE_POWER_CHORD_NONE;
    }

    if (!chord_allowed) {
        state->tracking = false;
        state->release_required = true;
        return BADGE_POWER_CHORD_NONE;
    }

    if (!button_one_pressed || !button_two_pressed) {
        state->tracking = false;
        return BADGE_POWER_CHORD_NONE;
    }

    if (!state->tracking) {
        state->tracking = true;
        state->hold_started_ms = now_ms;
        return BADGE_POWER_CHORD_NONE;
    }

    if ((uint32_t)(now_ms - state->hold_started_ms) < state->hold_ms) {
        return BADGE_POWER_CHORD_NONE;
    }

    state->tracking = false;
    state->release_required = true;
    return BADGE_POWER_CHORD_TOGGLE;
}
