#include "backend_status_led.h"

#include "backend_hardware_profile.h"

#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
#include "backend_yellow_led.h"
#else
#include "backend_fullsize_rgb_led.h"
#endif

bool backend_status_led_init(backend_led_state_t initial)
{
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    return backend_yellow_led_init(initial);
#else
    return backend_fullsize_rgb_led_init(initial);
#endif
}

bool backend_status_led_set_state(backend_led_state_t state)
{
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    return backend_yellow_led_set_state(state);
#else
    return backend_fullsize_rgb_led_set_state(state);
#endif
}

backend_led_state_t backend_status_led_state(void)
{
#if defined(FOF_BACKEND_PROFILE_BADGE_LITE)
    return backend_yellow_led_state();
#else
    return backend_fullsize_rgb_led_state();
#endif
}
