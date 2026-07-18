#ifdef FOF_BADGE_VARIANT

#include "badge_easter_egg_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static badge_easter_egg_machine_t s_machine;
static bool s_initialized = false;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

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

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        triggered = badge_easter_egg_machine_trigger(&s_machine, source);
    }
    portEXIT_CRITICAL(&s_lock);
    return triggered;
}

bool badge_easter_egg_runtime_dismiss(void)
{
    bool dismissed = false;

    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        dismissed = badge_easter_egg_machine_dismiss(&s_machine);
    }
    portEXIT_CRITICAL(&s_lock);
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
