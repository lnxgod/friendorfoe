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
    return BADGE_POWER_CHORD_RESET;
}

void badge_power_chord_dispatch_gate_init(
    badge_power_chord_dispatch_gate_t *gate,
    bool button_one_pressed,
    bool button_two_pressed)
{
    if (!gate) {
        return;
    }
    gate->suppress_until_full_release =
        button_one_pressed && button_two_pressed;
}

bool badge_power_chord_dispatch_gate_update(
    badge_power_chord_dispatch_gate_t *gate,
    bool button_one_pressed,
    bool button_two_pressed,
    badge_power_chord_event_t event)
{
    bool both_held = button_one_pressed && button_two_pressed;
    bool event_requires_suppression = event == BADGE_POWER_CHORD_RESET;
    if (!gate) {
        return both_held || event_requires_suppression;
    }

    bool suppress =
        gate->suppress_until_full_release ||
        both_held ||
        event_requires_suppression;
    if (both_held || event_requires_suppression) {
        gate->suppress_until_full_release = true;
    } else if (gate->suppress_until_full_release &&
               !button_one_pressed &&
               !button_two_pressed) {
        gate->suppress_until_full_release = false;
    }
    return suppress;
}
