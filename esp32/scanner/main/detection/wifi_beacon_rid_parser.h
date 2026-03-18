#pragma once

/**
 * Friend or Foe — WiFi Beacon Remote ID Parser (ESP32-S3)
 *
 * Parses ASTM F3411 Remote ID messages broadcast via WiFi beacon
 * vendor-specific Information Elements. Ported from Android
 * WifiBeaconRemoteIdParser.kt.
 *
 * Drones compliant with FAA Remote ID may broadcast OpenDroneID messages
 * in WiFi beacon frames using OUI FA:0B:BC (ASTM International) with
 * OUI type 0x0D. After the 4-byte header (3-byte OUI + 1-byte type),
 * the payload contains a 1-byte message counter followed by N x 25-byte
 * OpenDroneID messages.
 */

#include "open_drone_id_parser.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse ASTM F3411 WiFi Beacon Remote ID from a vendor IE payload.
 *
 * The `payload` buffer starts at the OUI bytes (0xFA, 0x0B, 0xBC).
 * This function verifies the OUI and type byte, then parses each
 * contained 25-byte OpenDroneID message into the given state accumulator.
 *
 * @param payload  IE payload starting at OUI bytes
 * @param len      Length of payload buffer
 * @param state    ODID state accumulator to update
 * @return true if at least one ODID message was parsed
 */
bool wifi_beacon_rid_parse_ie(const uint8_t *payload, size_t len, odid_state_t *state);

/**
 * Parse ASTM F3411 WiFi Beacon Remote ID from a raw 802.11 beacon frame.
 *
 * Scans the tagged parameters in the beacon frame body to find a
 * vendor-specific IE (tag 221) with ASTM OUI + type 0x0D, then parses
 * the contained OpenDroneID messages.
 *
 * @param frame  Raw 802.11 beacon frame (starting at frame control)
 * @param len    Total frame length
 * @param state  ODID state accumulator to update
 * @return true if at least one ODID message was parsed
 */
bool wifi_beacon_rid_parse_frame(const uint8_t *frame, size_t len, odid_state_t *state);

#ifdef __cplusplus
}
#endif
