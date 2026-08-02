#pragma once

/**
 * Friend or Foe -- French "Signalement Electronique a Distance" Parser (ESP32-S3)
 *
 * Parses French DRI (Electronic Remote Identification) data broadcast via WiFi
 * beacon vendor-specific Information Elements per Arrete du 27 decembre 2019.
 *
 * French drones use OUI 6A:5C:35 with VS type 0x01. The payload uses TLV
 * (Type-Length-Value) encoding with UTF-8 string values for coordinates,
 * altitude, speed, and the 30-character ID_FR identifier.
 *
 * IE layout:
 *   Bytes 0-2: French OUI (0x6A, 0x5C, 0x35)
 *   Byte 3:    VS type (0x01)
 *   Bytes 4+:  TLV-encoded fields
 */

#include "open_drone_id_parser.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse French DRI from a vendor IE payload.
 *
 * The `payload` buffer starts at the OUI bytes (0x6A, 0x5C, 0x35).
 * This function verifies the OUI and type byte, then parses TLV fields
 * into the given ODID state accumulator.
 *
 * @param payload  IE payload starting at OUI bytes
 * @param len      Length of payload buffer
 * @param state    ODID state accumulator to update
 * @return true if French DRI data was parsed
 */
bool french_dri_parse_ie(const uint8_t *payload, size_t len, odid_state_t *state);

/**
 * Parse French DRI from a raw 802.11 beacon frame.
 *
 * Scans the tagged parameters for a vendor-specific IE (tag 221) with
 * French OUI + type 0x01, then parses the TLV-encoded fields.
 *
 * @param frame  Raw 802.11 beacon frame (starting at frame control)
 * @param len    Total frame length
 * @param state  ODID state accumulator to update
 * @return true if French DRI data was parsed
 */
bool french_dri_parse_frame(const uint8_t *frame, size_t len, odid_state_t *state);

#ifdef __cplusplus
}
#endif
