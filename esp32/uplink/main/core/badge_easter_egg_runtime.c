#ifdef FOF_BADGE_VARIANT

#include "badge_easter_egg_runtime.h"

#if defined(FOF_DC34_GAME_CANARY)
#include "badge_con_runtime.h"
#ifndef BADGE_EASTER_GAME_ACTIVATE_FN
#define BADGE_EASTER_GAME_ACTIVATE_FN \
    badge_con_runtime_activate_after_easter
#endif
#endif

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static badge_easter_egg_machine_t s_machine;
static bool s_initialized = false;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t badge_easter_egg_now_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

void badge_easter_egg_runtime_init(void)
{
    portENTER_CRITICAL(&s_lock);
    badge_easter_egg_machine_init(&s_machine);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
}

bool badge_easter_egg_runtime_trigger(badge_easter_egg_source_t source)
{
    bool triggered = false;
    uint32_t now_ms = badge_easter_egg_now_ms();

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        triggered = badge_easter_egg_machine_trigger_at(&s_machine, source,
                                                        now_ms);
    }
    portEXIT_CRITICAL(&s_lock);
    return triggered;
}

bool badge_easter_egg_runtime_advance(void)
{
    bool advanced = false;
#if defined(FOF_DC34_GAME_CANARY)
    bool activate_game = false;
    badge_easter_egg_source_t completed_source =
        BADGE_EASTER_EGG_SOURCE_NONE;
#endif
    uint32_t now_ms = badge_easter_egg_now_ms();

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
#if defined(FOF_DC34_GAME_CANARY)
        bool was_visible = s_machine.visible;
        badge_easter_egg_phase_t prior_phase = s_machine.phase;
#endif
        advanced = badge_easter_egg_machine_advance_at(&s_machine, now_ms);
#if defined(FOF_DC34_GAME_CANARY)
        activate_game =
            advanced &&
            was_visible &&
            prior_phase != BADGE_EASTER_EGG_PHASE_CONSUMED &&
            s_machine.phase == BADGE_EASTER_EGG_PHASE_CONSUMED;
        if (activate_game) {
            completed_source = s_machine.source;
        }
#endif
    }
    portEXIT_CRITICAL(&s_lock);
#if defined(FOF_DC34_GAME_CANARY)
    if (activate_game) {
        (void)completed_source;
        (void)BADGE_EASTER_GAME_ACTIVATE_FN();
    }
#endif
    return advanced;
}

bool badge_easter_egg_runtime_dismiss(void)
{
    bool dismissed = false;
#if defined(FOF_DC34_GAME_CANARY)
    bool activate_game = false;
    badge_easter_egg_source_t completed_source =
        BADGE_EASTER_EGG_SOURCE_NONE;
#endif
    uint32_t now_ms = badge_easter_egg_now_ms();

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
#if defined(FOF_DC34_GAME_CANARY)
        bool was_visible = s_machine.visible;
        badge_easter_egg_phase_t prior_phase = s_machine.phase;
#endif
        dismissed = badge_easter_egg_machine_dismiss_at(&s_machine, now_ms);
#if defined(FOF_DC34_GAME_CANARY)
        activate_game =
            dismissed &&
            was_visible &&
            prior_phase != BADGE_EASTER_EGG_PHASE_CONSUMED &&
            s_machine.phase == BADGE_EASTER_EGG_PHASE_CONSUMED;
        if (activate_game) {
            completed_source = s_machine.source;
        }
#endif
    }
    portEXIT_CRITICAL(&s_lock);
#if defined(FOF_DC34_GAME_CANARY)
    if (activate_game) {
        (void)completed_source;
        (void)BADGE_EASTER_GAME_ACTIVATE_FN();
    }
#endif
    return dismissed;
}

bool badge_easter_egg_runtime_snapshot(badge_easter_egg_machine_t *out)
{
    bool ready = false;

    if (!out) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        *out = s_machine;
        ready = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

#endif /* FOF_BADGE_VARIANT */
