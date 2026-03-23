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

void   flight_sim_init(void);
void   flight_sim_tick(void);
double flight_sim_get_lat(void);
double flight_sim_get_lon(void);
double flight_sim_get_alt(void);
float  flight_sim_get_heading(void);
float  flight_sim_get_speed(void);

#ifdef __cplusplus
}
#endif
