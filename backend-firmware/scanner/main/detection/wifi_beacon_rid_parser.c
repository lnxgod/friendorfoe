/**
 * Friend or Foe — WiFi Beacon Remote ID Parser (ESP32-S3)
 *
 * Ported from Android WifiBeaconRemoteIdParser.kt.
 * Parses ASTM F3411 Remote ID vendor-specific IEs from WiFi beacons,
 * extracting OpenDroneID messages and feeding them to the shared
 * ODID parser for stateful accumulation.
 *
 * IE layout:
 *   Bytes 0-2: ASTM OUI (0xFA, 0x0B, 0xBC)
 *   Byte 3:    OUI type (0x0D for OpenDroneID)
 *   Byte 4:    Message count
 *   Bytes 5+:  N x 25-byte OpenDroneID messages
 */

#include "wifi_beacon_rid_parser.h"
#include "constants.h"

#include <string.h>
#include <esp_log.h>

static const char *TAG = "wifi_beacon_rid";

/* ── Public API ─────────────────────────────────────────────────────────── */

bool wifi_beacon_rid_parse_ie(const uint8_t *payload, size_t len, odid_state_t *state)
{
    if (payload == NULL || state == NULL) return false;

    /* Minimum: 3 OUI + 1 type + 1 counter + 25 message = 30 bytes */
    if (len < ASTM_MIN_PAYLOAD_SIZE) return false;

    /* Verify ASTM OUI */
    if (payload[0] != ASTM_OUI_0 ||
        payload[1] != ASTM_OUI_1 ||
        payload[2] != ASTM_OUI_2) {
        return false;
    }

    /* Verify OUI type byte */
    if (payload[3] != ASTM_OUI_TYPE) {
        return false;
    }

    /* Byte 4: message count */
    uint8_t message_count = payload[4];
    const size_t data_start = 5;
    size_t available_bytes = len - data_start;

    /* Calculate how many complete messages we can actually read */
    size_t actual_count = message_count;
    if (actual_count > available_bytes / ODID_MSG_SIZE) {
        actual_count = available_bytes / ODID_MSG_SIZE;
    }

    if (actual_count == 0) return false;

    bool parsed = false;

    for (size_t i = 0; i < actual_count; i++) {
        size_t offset = data_start + i * ODID_MSG_SIZE;
        odid_parse_message(&payload[offset], ODID_MSG_SIZE, state, 0);
        parsed = true;
    }

    if (parsed) {
        ESP_LOGD(TAG, "Parsed %zu ASTM F3411 WiFi Beacon RID message(s) from %s",
                 actual_count, state->device_address);
    }

    return parsed;
}

bool wifi_beacon_rid_parse_frame(const uint8_t *frame, size_t len, odid_state_t *state)
{
    if (frame == NULL || state == NULL) return false;

    /*
     * 802.11 beacon frame layout:
     *   - 24 bytes: MAC header (frame control, duration, addr1-3, seq ctrl)
     *   - 12 bytes: Fixed parameters (timestamp, beacon interval, capability)
     *   - Variable: Tagged parameters (IEs)
     *
     * We start scanning tagged parameters at offset 36.
     */
    const size_t TAGGED_PARAMS_OFFSET = 24 + 12; /* MAC header + fixed fields */

    if (len < TAGGED_PARAMS_OFFSET + 2) return false;

    size_t pos = TAGGED_PARAMS_OFFSET;
    bool found = false;

    while (pos + 2 <= len) {
        uint8_t tag_id = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        size_t ie_start = pos + 2;

        /* Bounds check */
        if (ie_start + tag_len > len) break;

        if (tag_id == IE_VENDOR_SPECIFIC && tag_len >= ASTM_MIN_PAYLOAD_SIZE) {
            /*
             * The IE payload (tag_len bytes starting at ie_start) begins
             * with the OUI bytes. Pass the full payload to wifi_beacon_rid_parse_ie
             * which will verify OUI + type.
             */
            if (wifi_beacon_rid_parse_ie(&frame[ie_start], tag_len, state)) {
                found = true;
                /* Don't break — there could be multiple ASTM IEs in one beacon */
            }
        }

        pos = ie_start + tag_len;
    }

    return found;
}
