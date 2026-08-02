#pragma once

#include "constants.h"

#include <math.h>
#include <stdint.h>

static inline double rssi_distance_estimate_m(int8_t rssi)
{
    double exponent = ((double)RSSI_REF - (double)rssi) /
                      (10.0 * (double)PATH_LOSS_EXPONENT);
    double dist = pow(10.0, exponent);
    if (dist < RSSI_DISTANCE_MIN_M) dist = RSSI_DISTANCE_MIN_M;
    if (dist > RSSI_DISTANCE_MAX_M) dist = RSSI_DISTANCE_MAX_M;
    return dist;
}
