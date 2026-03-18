#pragma once

/**
 * Friend or Foe — OpenDroneID Parser (ESP32-S3)
 *
 * Shared parser for OpenDroneID messages per ASTM F3411-22a.
 * Ported from Android OpenDroneIdParser.kt.
 *
 * Used by both BLE Remote ID scanning and WiFi Beacon Remote ID
 * parsing. The wire format of the 25-byte messages is identical
 * regardless of transport.
 */

#include "detection_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Partial state accumulator for OpenDroneID.
 *
 * OpenDroneID messages may arrive in separate packets (BLE advertisements
 * or beacon IEs), so we accumulate data per device identifier until we
 * have enough to emit a drone_detection_t.
 */
typedef struct {
    char device_address[18]; /* BLE MAC or WiFi BSSID */
    int64_t first_seen_ms;
    int64_t last_updated_ms;

    /* Basic ID (Type 0) */
    char drone_id[64];
    uint8_t ua_type;
    uint8_t id_type;

    /* Location (Type 1) */
    bool has_location;
    double latitude;
    double longitude;
    double altitude_m;
    float heading_deg;
    float speed_mps;
    float vertical_speed_mps;
    double geodetic_alt_m;
    double height_agl_m;
    uint8_t h_accuracy_code;
    uint8_t v_accuracy_code;
    uint16_t location_timestamp;

    /* System (Type 4) */
    double operator_lat;
    double operator_lon;
    uint16_t area_count;
    uint16_t area_radius;
    double area_ceiling;
    double area_floor;
    uint8_t classification_type;

    /* Operator ID (Type 5) */
    char operator_id[24];

    /* Self-ID (Type 3) */
    uint8_t self_id_desc_type;
    char self_id_text[24];

    /* RF */
    int8_t rssi;
} odid_state_t;

/**
 * Parse a single OpenDroneID message and update the accumulator state.
 *
 * @param data   Raw 25-byte message (first nibble of byte 0 is message type)
 * @param len    Length of data buffer
 * @param state  Mutable accumulator to update with parsed fields
 * @param depth  Recursion depth for message packs (caller should pass 0)
 */
void odid_parse_message(const uint8_t *data, size_t len, odid_state_t *state, int depth);

/**
 * Convert accumulated ODID state into a drone_detection_t if we have
 * minimum required data (at least a drone ID or device address).
 *
 * @param state      Accumulated ODID state
 * @param id_prefix  Prefix for the detection ID (e.g. "rid_" or "nan_")
 * @param source     DETECTION_SRC_* value
 * @param out        Output detection struct
 * @return true if conversion succeeded
 */
bool odid_state_to_detection(const odid_state_t *state, const char *id_prefix,
                             uint8_t source, drone_detection_t *out);

/**
 * Convert ASTM F3411-22a accuracy code (0-12) to meters.
 * Returns -1.0f if the code is unknown (0 or out of range).
 */
float odid_accuracy_code_to_meters(uint8_t code);

/**
 * Initialise an odid_state_t for a new device.
 *
 * @param state           State to initialise (zeroed, then populated)
 * @param device_address  BLE MAC or WiFi BSSID string
 * @param now_ms          Current timestamp in milliseconds since boot
 */
void odid_state_init(odid_state_t *state, const char *device_address, int64_t now_ms);

#ifdef __cplusplus
}
#endif
