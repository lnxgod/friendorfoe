#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_EASTER_EGG_RADIO_RETRIGGER_COOLDOWN_MS 90000U

typedef enum {
    BADGE_EASTER_EGG_SOURCE_NONE = 0,
    BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID,
    BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
    BADGE_EASTER_EGG_SOURCE_BUTTON,
} badge_easter_egg_source_t;

typedef enum {
    BADGE_EASTER_EGG_PHASE_ARMED = 0,
    BADGE_EASTER_EGG_PHASE_THANKS,
    BADGE_EASTER_EGG_PHASE_BOUNCE,
    BADGE_EASTER_EGG_PHASE_CONSUMED,
} badge_easter_egg_phase_t;

typedef struct {
    bool has_basic_id;
    const char *basic_id;
    bool has_location;
    int32_t latitude_e7;
    int32_t longitude_e7;
    bool has_geodetic_altitude;
    int32_t geodetic_altitude_half_m;
} badge_easter_egg_remote_id_t;

typedef struct {
    bool triggered_once;
    bool visible;
    bool radio_cooldown_active;
    uint32_t dismissed_at_ms;
    badge_easter_egg_phase_t phase;
    badge_easter_egg_source_t source;
} badge_easter_egg_machine_t;

bool badge_easter_egg_remote_id_matches(
    const badge_easter_egg_remote_id_t *remote_id);
bool badge_easter_egg_ssid_matches(const uint8_t *ssid, size_t len);
bool badge_easter_egg_consume_press_in_batch(bool *easter_visible_in_batch,
                                             bool dismiss_succeeded);
bool badge_easter_egg_claim_press_in_batch(bool visible_at_batch_start,
                                           bool *transition_claimed);
void badge_easter_egg_machine_init(badge_easter_egg_machine_t *machine);
bool badge_easter_egg_machine_trigger_at(
    badge_easter_egg_machine_t *machine,
    badge_easter_egg_source_t source,
    uint32_t now_ms);
bool badge_easter_egg_machine_trigger(badge_easter_egg_machine_t *machine,
                                      badge_easter_egg_source_t source);
bool badge_easter_egg_machine_advance_at(badge_easter_egg_machine_t *machine,
                                         uint32_t now_ms);
bool badge_easter_egg_machine_advance(badge_easter_egg_machine_t *machine);
bool badge_easter_egg_machine_dismiss_at(badge_easter_egg_machine_t *machine,
                                         uint32_t now_ms);
bool badge_easter_egg_machine_dismiss(badge_easter_egg_machine_t *machine);

#ifdef __cplusplus
}
#endif
