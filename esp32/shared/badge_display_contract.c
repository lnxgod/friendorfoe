#include "badge_display_contract.h"

#include <stddef.h>

typedef struct {
    badge_display_contract_lane_t lane;
    int y;
    int height;
} badge_display_lane_contract_t;

static const badge_display_lane_contract_t LANES[BADGE_DISPLAY_FOCUS_CAPACITY] = {
    {BADGE_LANE_GLOBAL_1, 0, 37},
    {BADGE_LANE_GLOBAL_2, 39, 37},
    {BADGE_LANE_BLE, 78, 34},
    {BADGE_LANE_WIFI, 113, 34},
};

static const badge_display_lane_contract_t *lane_at(int focus_index)
{
    if (focus_index < 0 ||
        (size_t)focus_index >= sizeof(LANES) / sizeof(LANES[0])) {
        return NULL;
    }
    return &LANES[focus_index];
}

int badge_display_contract_focus_capacity(void)
{
    return BADGE_DISPLAY_FOCUS_CAPACITY;
}

int badge_display_contract_health_strip_y(void)
{
    return BADGE_DISPLAY_HEALTH_STRIP_Y;
}

badge_display_contract_lane_t badge_display_contract_lane(int focus_index)
{
    const badge_display_lane_contract_t *contract = lane_at(focus_index);
    return contract ? contract->lane : BADGE_LANE_INVALID;
}

int badge_display_contract_lane_y(int focus_index)
{
    const badge_display_lane_contract_t *contract = lane_at(focus_index);
    return contract ? contract->y : -1;
}

int badge_display_contract_lane_height(int focus_index)
{
    const badge_display_lane_contract_t *contract = lane_at(focus_index);
    return contract ? contract->height : -1;
}
