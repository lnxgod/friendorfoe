/**
 * Friend or Foe -- Uplink GPS Implementation
 *
 * Current ESP32-S3 uplinks use registered backend node positions. Keep this
 * module as a small no-GPS adapter so status surfaces have a stable API.
 */

#include "gps.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "gps";

void gps_init(void)
{
    ESP_LOGI(TAG, "GPS disabled (fixed node positions via backend)");
}

void gps_start(void)
{
}

bool gps_has_fix(void)
{
    return false;
}

double gps_get_latitude(void)
{
    return 0.0;
}

double gps_get_longitude(void)
{
    return 0.0;
}

float gps_get_altitude(void)
{
    return 0.0f;
}

bool gps_get_position(gps_position_t *pos)
{
    if (pos) {
        memset(pos, 0, sizeof(*pos));
    }
    return false;
}
