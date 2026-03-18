#pragma once

/**
 * Friend or Foe -- Uplink Battery Monitor
 *
 * Reads Li-Ion battery voltage through an ADC pin with a 2:1 voltage
 * divider and converts to a percentage estimate.
 *
 * Hardware: ADC on GPIO3 (ADC1), 2:1 voltage divider
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the ADC for battery voltage measurement.
 */
void battery_init(void);

/**
 * Get the current battery voltage in volts.
 * Averaged over 10 samples to reduce noise.
 */
float battery_get_voltage(void);

/**
 * Get the battery charge level as a percentage (0.0-100.0).
 * Uses a 3-segment linear approximation for Li-Ion cells:
 *   4.2V = 100%, 3.7V = 50%, 3.0V = 0%
 */
float battery_get_percentage(void);

#ifdef __cplusplus
}
#endif
