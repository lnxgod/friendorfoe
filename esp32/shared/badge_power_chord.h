#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_POWER_CHORD_NONE = 0,
    BADGE_POWER_CHORD_RESET,
} badge_power_chord_event_t;

typedef struct {
    uint32_t hold_ms;
    uint32_t hold_started_ms;
    bool tracking;
    bool release_required;
} badge_power_chord_t;

typedef struct {
    bool suppress_until_full_release;
} badge_power_chord_dispatch_gate_t;

void badge_power_chord_init(badge_power_chord_t *state,
                            uint32_t hold_ms,
                            bool button_one_pressed,
                            bool button_two_pressed,
                            uint32_t now_ms);

badge_power_chord_event_t badge_power_chord_update(
    badge_power_chord_t *state,
    bool button_one_pressed,
    bool button_two_pressed,
    bool chord_allowed,
    uint32_t now_ms);

void badge_power_chord_dispatch_gate_init(
    badge_power_chord_dispatch_gate_t *gate,
    bool button_one_pressed,
    bool button_two_pressed);

bool badge_power_chord_dispatch_gate_update(
    badge_power_chord_dispatch_gate_t *gate,
    bool button_one_pressed,
    bool button_two_pressed,
    badge_power_chord_event_t event);

#ifdef __cplusplus
}
#endif
