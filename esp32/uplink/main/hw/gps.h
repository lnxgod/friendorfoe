#pragma once

/**
 * Friend or Foe -- Uplink GPS Module
 *
 * Parses NMEA sentences from a UART-connected GPS module to provide
 * the uplink board's geographic position for tagging uploads.
 *
 * Hardware: UART0, TX=GPIO6, RX=GPIO7, 9600 baud
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double  latitude;       /* decimal degrees, WGS84 */
    double  longitude;      /* decimal degrees, WGS84 */
    double  altitude_m;     /* meters MSL */
    float   hdop;           /* horizontal dilution of precision */
    int     satellites;     /* number of satellites in use */
    bool    has_fix;        /* true if fix is valid */
    int64_t fix_time_ms;    /* epoch ms of last valid fix */
} gps_position_t;

/**
 * Initialize GPS UART hardware.
 */
void gps_init(void);

/**
 * Start the GPS parsing FreeRTOS task.
 */
void gps_start(void);

/**
 * Get the latest GPS position.
 *
 * @param pos  Output struct filled with current position data.
 * @return true if a valid fix is available, false otherwise.
 */
bool gps_get_position(gps_position_t *pos);

/**
 * Check if the GPS currently has a valid fix.
 */
bool gps_has_fix(void);

#ifdef __cplusplus
}
#endif
