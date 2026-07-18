#include "badge_easter_egg.h"

#include <string.h>

bool badge_easter_egg_remote_id_matches(
    const badge_easter_egg_remote_id_t *remote_id)
{
    return remote_id &&
           remote_id->has_basic_id &&
           remote_id->basic_id &&
           strcmp(remote_id->basic_id, "fof-michagain") == 0 &&
           remote_id->has_location &&
           remote_id->latitude_e7 == 424347200 &&
           remote_id->longitude_e7 == -839850000 &&
           remote_id->has_geodetic_altitude &&
           remote_id->geodetic_altitude_half_m == 1332;
}

bool badge_easter_egg_ssid_matches(const uint8_t *ssid, size_t len)
{
    return ssid && len == 10 && memcmp(ssid, "fof-goblue", 10) == 0;
}

void badge_easter_egg_machine_init(badge_easter_egg_machine_t *machine)
{
    if (!machine) {
        return;
    }

    machine->triggered_once = false;
    machine->visible = false;
    machine->source = BADGE_EASTER_EGG_SOURCE_NONE;
}

bool badge_easter_egg_machine_trigger(badge_easter_egg_machine_t *machine,
                                      badge_easter_egg_source_t source)
{
    if (!machine || machine->triggered_once ||
        (source != BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID &&
         source != BADGE_EASTER_EGG_SOURCE_WIFI_SSID &&
         source != BADGE_EASTER_EGG_SOURCE_BUTTON)) {
        return false;
    }

    machine->triggered_once = true;
    machine->visible = true;
    machine->source = source;
    return true;
}

bool badge_easter_egg_machine_dismiss(badge_easter_egg_machine_t *machine)
{
    if (!machine || !machine->visible) {
        return false;
    }

    machine->visible = false;
    return true;
}
