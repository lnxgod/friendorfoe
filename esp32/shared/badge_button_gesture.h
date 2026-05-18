#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_BUTTON_GESTURE_NONE = 0,
    BADGE_BUTTON_GESTURE_SINGLE,
    BADGE_BUTTON_GESTURE_DOUBLE,
    BADGE_BUTTON_GESTURE_LONG,
} badge_button_gesture_event_t;

typedef struct {
    uint32_t double_tap_ms;
    bool pending_single;
    uint32_t pending_ms;
} badge_button_gesture_t;

void badge_button_gesture_init(badge_button_gesture_t *state,
                               uint32_t double_tap_ms);
badge_button_gesture_event_t badge_button_gesture_note_tap(
    badge_button_gesture_t *state,
    uint32_t now_ms);
badge_button_gesture_event_t badge_button_gesture_note_long(
    badge_button_gesture_t *state,
    uint32_t now_ms);
badge_button_gesture_event_t badge_button_gesture_poll(
    badge_button_gesture_t *state,
    uint32_t now_ms);
void badge_button_gesture_cancel(badge_button_gesture_t *state);
bool badge_button_gesture_pending_single(const badge_button_gesture_t *state);

#ifdef __cplusplus
}
#endif
