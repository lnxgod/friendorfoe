/**
 * Friend or Foe — Flight Simulator Engine
 *
 * Circle orbit around configurable home coordinates.
 * Advances angle by (speed / radius) * dt each tick.
 * Set HOME_LAT/LON via KConfig (menuconfig → FoF RID Simulator Flight).
 */

#include "flight_sim.h"
#include <math.h>
#include <stdlib.h>

/* ── Orbit parameters ────────────────────────────────────────────────────── */

#define HOME_LAT            atof(CONFIG_FOF_SIM_HOME_LAT)
#define HOME_LON            atof(CONFIG_FOF_SIM_HOME_LON)
#define ORBIT_RADIUS_M      60.0
#define ORBIT_ALT_M         30.0
#define ORBIT_SPEED_MPS      5.0
#define UPDATE_INTERVAL_MS 250

/* ── State ─────────────────────────────────────────────────────────────── */

static double s_angle_rad = 0.0;
static double s_lat       = HOME_LAT;
static double s_lon       = HOME_LON;

/* ── Constants ─────────────────────────────────────────────────────────── */

#define DEG_TO_RAD          (M_PI / 180.0)
#define RAD_TO_DEG          (180.0 / M_PI)
#define METERS_PER_DEG_LAT  111320.0

/* ── Public API ────────────────────────────────────────────────────────── */

void flight_sim_init(void)
{
    s_angle_rad = 0.0;
    s_lat = HOME_LAT + (ORBIT_RADIUS_M / METERS_PER_DEG_LAT) * sin(s_angle_rad);
    s_lon = HOME_LON + (ORBIT_RADIUS_M / (METERS_PER_DEG_LAT * cos(HOME_LAT * DEG_TO_RAD))) * cos(s_angle_rad);
}

void flight_sim_tick(void)
{
    double dt = UPDATE_INTERVAL_MS / 1000.0;
    double angular_speed = ORBIT_SPEED_MPS / ORBIT_RADIUS_M;

    s_angle_rad += angular_speed * dt;
    if (s_angle_rad >= 2.0 * M_PI) {
        s_angle_rad -= 2.0 * M_PI;
    }

    s_lat = HOME_LAT + (ORBIT_RADIUS_M / METERS_PER_DEG_LAT) * sin(s_angle_rad);
    s_lon = HOME_LON + (ORBIT_RADIUS_M / (METERS_PER_DEG_LAT * cos(HOME_LAT * DEG_TO_RAD))) * cos(s_angle_rad);
}

double flight_sim_get_lat(void)
{
    return s_lat;
}

double flight_sim_get_lon(void)
{
    return s_lon;
}

double flight_sim_get_alt(void)
{
    return ORBIT_ALT_M;
}

float flight_sim_get_heading(void)
{
    /* Tangent direction = angle + 90 degrees */
    double heading = s_angle_rad * RAD_TO_DEG + 90.0;
    if (heading >= 360.0) heading -= 360.0;
    if (heading < 0.0) heading += 360.0;
    return (float)heading;
}

float flight_sim_get_speed(void)
{
    return (float)ORBIT_SPEED_MPS;
}
