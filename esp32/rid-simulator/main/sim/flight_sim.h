#pragma once

/**
 * Friend or Foe — Flight Simulator Engine
 *
 * Generates a circle orbit around home coordinates for RID simulation.
 * Updates position every tick based on constant speed and radius.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Number of simulated drones. Tests can override this with -DSIM_DRONE_COUNT=1. */
#ifndef SIM_DRONE_COUNT
#define SIM_DRONE_COUNT 2
#endif

/** Per-drone state accessible by index (0..SIM_DRONE_COUNT-1) */
void   flight_sim_init(void);
void   flight_sim_tick(void);
double flight_sim_get_lat(int drone_idx);
double flight_sim_get_lon(int drone_idx);
double flight_sim_get_alt(int drone_idx);
float  flight_sim_get_heading(int drone_idx);
float  flight_sim_get_speed(int drone_idx);
const char *flight_sim_get_serial(int drone_idx);

#ifdef __cplusplus
}
#endif
