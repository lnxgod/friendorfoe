#pragma once

/**
 * Friend or Foe — DJI DroneID Parser (ESP32-S3)
 *
 * Parses DJI-proprietary vendor-specific Information Elements found in
 * WiFi beacon frames. Ported from Android DjiDroneIdParser.kt.
 *
 * DJI drones embed flight telemetry in vendor-specific IEs using
 * OUI 26:37:12. The IE payload contains GPS coordinates, altitude,
 * speed, serial number, and the operator's home point.
 */

#include "detection_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parsed DJI DroneID telemetry from a WiFi beacon vendor-specific IE.
 */
typedef struct {
    double latitude;              /* Drone latitude (WGS84 degrees) */
    double longitude;             /* Drone longitude (WGS84 degrees) */
    double altitude_m;            /* Altitude relative to takeoff (meters) */
    double height_above_ground_m; /* Height above ground (meters) */
    float  speed_mps;             /* Ground speed (m/s) */
    float  heading_deg;           /* Heading (0-360, true north) */
    double home_lat;              /* Operator/home latitude (0 if unavailable) */
    double home_lon;              /* Operator/home longitude (0 if unavailable) */
    char   serial_prefix[8];     /* First 4 chars of serial, null-terminated */
    uint8_t version;              /* DroneID protocol version byte */
    bool   has_home;              /* true if home_lat/home_lon are valid */
} dji_drone_id_data_t;

/**
 * Parse DJI DroneID from a vendor IE payload (after stripping the IE header).
 *
 * The `payload` buffer starts at the OUI bytes (0x26, 0x37, 0x12).
 * This function verifies the OUI, strips it, and parses the remaining data.
 *
 * @param payload  IE payload starting at OUI bytes
 * @param len      Length of payload buffer
 * @param out      Output struct (written only on success)
 * @return true if parsing succeeded with valid coordinates
 */
bool dji_parse_vendor_ie(const uint8_t *payload, size_t len, dji_drone_id_data_t *out);

/**
 * Parse DJI DroneID from a raw 802.11 beacon frame.
 *
 * Scans the tagged parameters in the beacon frame body to find a
 * vendor-specific IE (tag 221) with DJI OUI, then parses the payload.
 *
 * @param frame  Raw 802.11 beacon frame (starting at frame control)
 * @param len    Total frame length
 * @param out    Output struct (written only on success)
 * @return true if a valid DJI DroneID IE was found and parsed
 */
bool dji_parse_beacon_frame(const uint8_t *frame, size_t len, dji_drone_id_data_t *out);

#ifdef __cplusplus
}
#endif
