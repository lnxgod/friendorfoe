#pragma once

#include "badge_con_game.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_CON_PRESENT_INACTIVE = 0,
    BADGE_CON_PRESENT_HUMAN,
    BADGE_CON_PRESENT_CURED,
    BADGE_CON_PRESENT_INFECTED,
    BADGE_CON_PRESENT_IMMUNE,
    BADGE_CON_PRESENT_SUPER,
    BADGE_CON_PRESENT_DEAD,
    BADGE_CON_PRESENT_DEAD_SUPER,
} badge_con_present_state_t;

enum {
    BADGE_CON_HUD_HUMAN_RGB565 = 0xF800U,
    BADGE_CON_HUD_INFECTED_RGB565 = 0x07E0U,
    BADGE_CON_HUD_HEALER_RGB565 = 0xF81FU,
};

typedef struct {
    badge_con_present_state_t state;
    uint16_t color_rgb565;
    uint8_t current;
    uint8_t maximum;
    bool visible;
} badge_con_hud_plan_t;

badge_con_present_state_t badge_con_presentation_select(
    const badge_con_snapshot_t *snapshot);

badge_con_hud_plan_t badge_con_presentation_hud(
    const badge_con_snapshot_t *snapshot);

bool badge_con_presentation_game_led_allowed(
    bool update_active,
    bool safe_or_recovery,
    bool both_scanners_healthy,
    bool hardware_error);

#ifdef __cplusplus
}
#endif
