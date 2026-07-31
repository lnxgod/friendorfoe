#ifndef BADGE_BLE_RSSI_POLICY_H
#define BADGE_BLE_RSSI_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define BADGE_BLE_LOW_EFFORT_MIN_RSSI_DBM (-50)

static inline bool badge_ble_low_effort_rssi_allowed(int8_t rssi)
{
    return rssi >= BADGE_BLE_LOW_EFFORT_MIN_RSSI_DBM && rssi < 0;
}

static inline bool badge_ble_low_effort_detection_allowed(bool low_effort,
                                                           int8_t rssi)
{
    return !low_effort || badge_ble_low_effort_rssi_allowed(rssi);
}

#endif
