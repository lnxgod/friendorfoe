#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BACKEND_BLE_LOW_EFFORT_MIN_RSSI_DBM (-50)

static inline bool backend_ble_low_effort_rssi_allowed(int8_t rssi)
{
    return rssi >= BACKEND_BLE_LOW_EFFORT_MIN_RSSI_DBM && rssi < 0;
}

static inline bool backend_ble_low_effort_detection_allowed(bool low_effort,
                                                             int8_t rssi)
{
    return !low_effort || backend_ble_low_effort_rssi_allowed(rssi);
}
