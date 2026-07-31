#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_LANE_GLOBAL_1 = 0,
    BADGE_LANE_GLOBAL_2,
    BADGE_LANE_BLE,
    BADGE_LANE_WIFI,
    BADGE_LANE_INVALID,
} badge_display_contract_lane_t;

enum {
    BADGE_DISPLAY_FOCUS_CAPACITY = 4,
    BADGE_DISPLAY_HEALTH_STRIP_Y = 148,
    BADGE_DISPLAY_HEALTH_STRIP_HEIGHT = 12,
    BADGE_DISPLAY_HEALTH_VALUE_Y_OFFSET = 7,
    BADGE_DISPLAY_HEART_WIDTH = 7,
    BADGE_DISPLAY_HEART_HEIGHT = 5,
};

int badge_display_contract_focus_capacity(void);
int badge_display_contract_health_strip_y(void);
badge_display_contract_lane_t badge_display_contract_lane(int focus_index);
int badge_display_contract_lane_y(int focus_index);
int badge_display_contract_lane_height(int focus_index);

#ifdef __cplusplus
}
#endif
