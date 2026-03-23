#pragma once

/**
 * Friend or Foe — OpenDroneID Message Encoder
 *
 * Builds raw 25-byte ODID messages per ASTM F3411-22a for BLE broadcast.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void odid_encode_basic_id(uint8_t *msg25, const char *serial,
                           uint8_t ua_type, uint8_t id_type);

void odid_encode_location(uint8_t *msg25, double lat, double lon, double alt_m,
                           float heading_deg, float speed_mps, float vspeed_mps);

void odid_encode_system(uint8_t *msg25, double op_lat, double op_lon,
                         uint16_t area_count, double area_radius_m);

void odid_encode_operator_id(uint8_t *msg25, const char *operator_id);

#ifdef __cplusplus
}
#endif
