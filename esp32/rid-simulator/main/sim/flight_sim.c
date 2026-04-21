/**
 * Friend or Foe — Multi-Drone Flight Simulator
 *
 * Simulates multiple drones orbiting around home coordinates at different
 * speeds, radii, and altitudes. Each drone has a unique serial number.
 */

#include "flight_sim.h"
#include <math.h>
#include <stdlib.h>

#if __has_include("local_coords.h")
#include "local_coords.h"
#endif

#define UPDATE_INTERVAL_MS 250
#define DEG_TO_RAD          (M_PI / 180.0)
#define RAD_TO_DEG          (180.0 / M_PI)
#define METERS_PER_DEG_LAT  111320.0

/* ── Per-drone configuration ─────────────────────────────────────────── */

typedef struct {
    double radius_m;
    double alt_m;
    double speed_mps;
    double start_angle;     /* Initial orbit angle (radians) */
    int    clockwise;       /* 1 = CW, -1 = CCW */
    const char *serial;
} drone_config_t;

static const drone_config_t DRONE_CONFIGS[] = {
    { .radius_m = 80.0,  .alt_m = 30.0, .speed_mps = 15.0,
      .start_angle = 0.0,        .clockwise = 1,  .serial = "FOF-SIM-001" },
    { .radius_m = 120.0, .alt_m = 50.0, .speed_mps = 10.0,
      .start_angle = M_PI,       .clockwise = -1, .serial = "FOF-SIM-002" },
};

enum { AVAILABLE_SIM_DRONES = (int)(sizeof(DRONE_CONFIGS) / sizeof(DRONE_CONFIGS[0])) };
_Static_assert(SIM_DRONE_COUNT >= 1 && SIM_DRONE_COUNT <= AVAILABLE_SIM_DRONES,
               "SIM_DRONE_COUNT must be between 1 and AVAILABLE_SIM_DRONES");

/* ── Per-drone runtime state ─────────────────────────────────────────── */

typedef struct {
    double angle_rad;
    double lat;
    double lon;
} drone_state_t;

static double s_home_lat;
static double s_home_lon;
static drone_state_t s_drones[SIM_DRONE_COUNT];

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void update_position(int idx)
{
    const drone_config_t *cfg = &DRONE_CONFIGS[idx];
    drone_state_t *st = &s_drones[idx];

    st->lat = s_home_lat + (cfg->radius_m / METERS_PER_DEG_LAT) * sin(st->angle_rad);
    st->lon = s_home_lon + (cfg->radius_m / (METERS_PER_DEG_LAT * cos(s_home_lat * DEG_TO_RAD))) * cos(st->angle_rad);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void flight_sim_init(void)
{
#ifdef FOF_HOME_LAT_OVERRIDE
    s_home_lat = FOF_HOME_LAT_OVERRIDE;
    s_home_lon = FOF_HOME_LON_OVERRIDE;
#else
    s_home_lat = atof(CONFIG_FOF_SIM_HOME_LAT);
    s_home_lon = atof(CONFIG_FOF_SIM_HOME_LON);
#endif

    for (int i = 0; i < SIM_DRONE_COUNT; i++) {
        s_drones[i].angle_rad = DRONE_CONFIGS[i].start_angle;
        update_position(i);
    }
}

void flight_sim_tick(void)
{
    double dt = UPDATE_INTERVAL_MS / 1000.0;

    for (int i = 0; i < SIM_DRONE_COUNT; i++) {
        const drone_config_t *cfg = &DRONE_CONFIGS[i];
        double angular_speed = cfg->speed_mps / cfg->radius_m;

        s_drones[i].angle_rad += cfg->clockwise * angular_speed * dt;
        if (s_drones[i].angle_rad >= 2.0 * M_PI) s_drones[i].angle_rad -= 2.0 * M_PI;
        if (s_drones[i].angle_rad < 0.0) s_drones[i].angle_rad += 2.0 * M_PI;

        update_position(i);
    }
}

double flight_sim_get_lat(int idx)   { return (idx < SIM_DRONE_COUNT) ? s_drones[idx].lat : 0; }
double flight_sim_get_lon(int idx)   { return (idx < SIM_DRONE_COUNT) ? s_drones[idx].lon : 0; }
double flight_sim_get_alt(int idx)   { return (idx < SIM_DRONE_COUNT) ? DRONE_CONFIGS[idx].alt_m : 0; }

float flight_sim_get_heading(int idx)
{
    if (idx >= SIM_DRONE_COUNT) return 0;
    double heading = s_drones[idx].angle_rad * RAD_TO_DEG + 90.0 * DRONE_CONFIGS[idx].clockwise;
    if (heading >= 360.0) heading -= 360.0;
    if (heading < 0.0) heading += 360.0;
    return (float)heading;
}

float flight_sim_get_speed(int idx)
{
    return (idx < SIM_DRONE_COUNT) ? (float)DRONE_CONFIGS[idx].speed_mps : 0;
}

const char *flight_sim_get_serial(int idx)
{
    return (idx < SIM_DRONE_COUNT) ? DRONE_CONFIGS[idx].serial : "UNKNOWN";
}
