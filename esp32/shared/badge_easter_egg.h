#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_EASTER_EGG_SOURCE_NONE = 0,
    BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID,
    BADGE_EASTER_EGG_SOURCE_WIFI_SSID,
    BADGE_EASTER_EGG_SOURCE_BUTTON,
} badge_easter_egg_source_t;

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
    badge_easter_egg_source_t source;
} badge_easter_egg_machine_t;

bool badge_easter_egg_remote_id_matches(
    const badge_easter_egg_remote_id_t *remote_id);
bool badge_easter_egg_ssid_matches(const uint8_t *ssid, size_t len);
badge_easter_egg_source_t badge_easter_egg_source_from_wire(
    const char *source);
void badge_easter_egg_machine_init(badge_easter_egg_machine_t *machine);
bool badge_easter_egg_machine_trigger(badge_easter_egg_machine_t *machine,
                                      badge_easter_egg_source_t source);
bool badge_easter_egg_machine_dismiss(badge_easter_egg_machine_t *machine);

#ifdef __cplusplus
}
#endif
