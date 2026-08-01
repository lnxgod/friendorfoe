#include "badge_con_presentation.h"

badge_con_present_state_t badge_con_presentation_select(
    const badge_con_snapshot_t *snapshot)
{
    if (!snapshot || !snapshot->active) {
        return BADGE_CON_PRESENT_INACTIVE;
    }
    if (snapshot->dead) {
        return snapshot->super
            ? BADGE_CON_PRESENT_DEAD_SUPER
            : BADGE_CON_PRESENT_DEAD;
    }
    if (snapshot->super) {
        return BADGE_CON_PRESENT_SUPER;
    }
    if (snapshot->role == BADGE_CON_ROLE_IMMUNE) {
        return BADGE_CON_PRESENT_IMMUNE;
    }
    if (snapshot->role == BADGE_CON_ROLE_INFECTED) {
        return BADGE_CON_PRESENT_INFECTED;
    }
    return BADGE_CON_PRESENT_HUMAN;
}

badge_con_hud_plan_t badge_con_presentation_hud(
    const badge_con_snapshot_t *snapshot)
{
    badge_con_hud_plan_t hud = {0};
    hud.state = badge_con_presentation_select(snapshot);
    if (!snapshot) {
        return hud;
    }

    switch (hud.state) {
    case BADGE_CON_PRESENT_HUMAN:
    case BADGE_CON_PRESENT_CURED:
        hud.color_rgb565 = BADGE_CON_HUD_HUMAN_RGB565;
        hud.maximum = snapshot->maximum;
        break;
    case BADGE_CON_PRESENT_INFECTED:
    case BADGE_CON_PRESENT_SUPER:
        hud.color_rgb565 = BADGE_CON_HUD_INFECTED_RGB565;
        hud.maximum = 100U;
        break;
    case BADGE_CON_PRESENT_IMMUNE:
        hud.color_rgb565 = BADGE_CON_HUD_HEALER_RGB565;
        hud.maximum = snapshot->maximum;
        break;
    case BADGE_CON_PRESENT_INACTIVE:
    case BADGE_CON_PRESENT_DEAD:
    case BADGE_CON_PRESENT_DEAD_SUPER:
    default:
        return hud;
    }

    hud.current = snapshot->shield;
    hud.visible = true;
    return hud;
}

bool badge_con_presentation_game_led_allowed(
    bool update_active,
    bool safe_or_recovery,
    bool both_scanners_healthy,
    bool hardware_error)
{
    return !update_active && !safe_or_recovery &&
        both_scanners_healthy && !hardware_error;
}
