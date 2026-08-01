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
    static const char trigger_ssid[] = "GameChangersAI-67";
    return ssid &&
           len == sizeof(trigger_ssid) - 1 &&
           memcmp(ssid, trigger_ssid, sizeof(trigger_ssid) - 1) == 0;
}

bool badge_easter_egg_consume_press_in_batch(bool *easter_visible_in_batch,
                                             bool dismiss_succeeded)
{
    if (!easter_visible_in_batch) {
        return dismiss_succeeded;
    }

    *easter_visible_in_batch =
        *easter_visible_in_batch || dismiss_succeeded;
    return *easter_visible_in_batch;
}

bool badge_easter_egg_claim_press_in_batch(bool visible_at_batch_start,
                                           bool *transition_claimed)
{
    if (!visible_at_batch_start || !transition_claimed ||
        *transition_claimed) {
        return false;
    }

    *transition_claimed = true;
    return true;
}

void badge_easter_egg_machine_init(badge_easter_egg_machine_t *machine)
{
    if (!machine) {
        return;
    }

    machine->triggered_once = false;
    machine->visible = false;
    machine->radio_cooldown_active = false;
    machine->dismissed_at_ms = 0U;
    machine->phase = BADGE_EASTER_EGG_PHASE_ARMED;
    machine->source = BADGE_EASTER_EGG_SOURCE_NONE;
}

static bool badge_easter_egg_source_is_radio(
    badge_easter_egg_source_t source)
{
    return source == BADGE_EASTER_EGG_SOURCE_BLE_REMOTE_ID ||
           source == BADGE_EASTER_EGG_SOURCE_WIFI_SSID;
}

bool badge_easter_egg_machine_trigger_at(
    badge_easter_egg_machine_t *machine,
    badge_easter_egg_source_t source,
    uint32_t now_ms)
{
    bool radio_source = badge_easter_egg_source_is_radio(source);
    bool valid_source = radio_source ||
                        source == BADGE_EASTER_EGG_SOURCE_BUTTON;

    if (!machine || !valid_source || machine->visible) {
        return false;
    }

    if (machine->triggered_once) {
        if (!radio_source || !machine->radio_cooldown_active ||
            (uint32_t)(now_ms - machine->dismissed_at_ms) <
                BADGE_EASTER_EGG_RADIO_RETRIGGER_COOLDOWN_MS) {
            return false;
        }
    }

    machine->triggered_once = true;
    machine->visible = true;
    machine->radio_cooldown_active = false;
    machine->phase = BADGE_EASTER_EGG_PHASE_THANKS;
    machine->source = source;
    return true;
}

bool badge_easter_egg_machine_trigger(badge_easter_egg_machine_t *machine,
                                      badge_easter_egg_source_t source)
{
    return badge_easter_egg_machine_trigger_at(machine, source, 0U);
}

static void badge_easter_egg_machine_consume(
    badge_easter_egg_machine_t *machine,
    uint32_t now_ms)
{
    machine->visible = false;
    machine->radio_cooldown_active = true;
    machine->dismissed_at_ms = now_ms;
    machine->phase = BADGE_EASTER_EGG_PHASE_CONSUMED;
}

bool badge_easter_egg_machine_advance_at(badge_easter_egg_machine_t *machine,
                                         uint32_t now_ms)
{
    if (!machine || !machine->visible) {
        return false;
    }

    if (machine->phase == BADGE_EASTER_EGG_PHASE_THANKS) {
        machine->phase = BADGE_EASTER_EGG_PHASE_BOUNCE;
        return true;
    }
    if (machine->phase == BADGE_EASTER_EGG_PHASE_BOUNCE) {
        badge_easter_egg_machine_consume(machine, now_ms);
        return true;
    }
    return false;
}

bool badge_easter_egg_machine_advance(badge_easter_egg_machine_t *machine)
{
    return badge_easter_egg_machine_advance_at(machine, 0U);
}

bool badge_easter_egg_machine_dismiss_at(badge_easter_egg_machine_t *machine,
                                         uint32_t now_ms)
{
    if (!machine || !machine->visible) {
        return false;
    }

    badge_easter_egg_machine_consume(machine, now_ms);
    return true;
}

bool badge_easter_egg_machine_dismiss(badge_easter_egg_machine_t *machine)
{
    return badge_easter_egg_machine_dismiss_at(machine, 0U);
}
