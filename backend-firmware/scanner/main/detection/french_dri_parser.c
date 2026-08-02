/**
 * Friend or Foe -- French "Signalement Electronique a Distance" Parser (ESP32-S3)
 *
 * Ported from Android FrenchDriParser.kt.
 * Parses French DRI vendor-specific IEs from WiFi beacons using
 * OUI 6A:5C:35, VS type 0x01, with TLV-encoded UTF-8 fields.
 *
 * TLV types:
 *   1  = Protocol version
 *   2  = ID_FR (30-char French identifier: trigram + model + serial)
 *   3  = ANSI/CTA-2063 ID (physical serial number)
 *   4  = Latitude (ASCII decimal degrees)
 *   5  = Longitude (ASCII decimal degrees)
 *   6  = Altitude MSL (ASCII meters)
 *   7  = Height AGL (ASCII meters)
 *   8  = Takeoff latitude (ASCII decimal degrees)
 *   9  = Takeoff longitude (ASCII decimal degrees)
 *   10 = Ground speed (ASCII m/s)
 *   11 = Heading (ASCII degrees 0-359)
 */

#include "french_dri_parser.h"
#include "constants.h"

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>

static const char *TAG = "french_dri";

/* French DRI OUI: 6A:5C:35 */
#define FRENCH_OUI_0    0x6A
#define FRENCH_OUI_1    0x5C
#define FRENCH_OUI_2    0x35
#define FRENCH_VS_TYPE  0x01

/* Minimum payload: 3 OUI + 1 type + 3 (one minimal TLV) = 7 bytes */
#define FRENCH_MIN_PAYLOAD_SIZE  7

/* TLV type codes */
#define TLV_PROTOCOL_VERSION  1
#define TLV_ID_FR             2
#define TLV_ANSI_CTA_ID       3
#define TLV_LATITUDE          4
#define TLV_LONGITUDE         5
#define TLV_ALTITUDE_MSL      6
#define TLV_HEIGHT_AGL        7
#define TLV_TAKEOFF_LAT       8
#define TLV_TAKEOFF_LON       9
#define TLV_GROUND_SPEED      10
#define TLV_HEADING           11

/* ── Helper: safe string-to-double from non-null-terminated buffer ────────── */

static double parse_ascii_double(const uint8_t *buf, size_t len)
{
    /* Copy to a null-terminated stack buffer (max 32 chars) */
    char tmp[33];
    size_t n = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
    memcpy(tmp, buf, n);
    tmp[n] = '\0';
    return atof(tmp);
}

static float parse_ascii_float(const uint8_t *buf, size_t len)
{
    return (float)parse_ascii_double(buf, len);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool french_dri_parse_ie(const uint8_t *payload, size_t len, odid_state_t *state)
{
    if (payload == NULL || state == NULL) return false;
    if (len < FRENCH_MIN_PAYLOAD_SIZE) return false;

    /* Verify French OUI */
    if (payload[0] != FRENCH_OUI_0 ||
        payload[1] != FRENCH_OUI_1 ||
        payload[2] != FRENCH_OUI_2) {
        return false;
    }

    /* Verify VS type */
    if (payload[3] != FRENCH_VS_TYPE) {
        return false;
    }

    /* Parse TLV fields starting at offset 4 */
    size_t offset = 4;
    bool parsed = false;

    while (offset + 2 <= len) {
        uint8_t tlv_type = payload[offset];
        uint8_t tlv_len  = payload[offset + 1];
        offset += 2;

        if (offset + tlv_len > len) break;

        const uint8_t *value = &payload[offset];

        switch (tlv_type) {
        case TLV_ID_FR:
            if (tlv_len > 0 && tlv_len < sizeof(state->drone_id)) {
                memcpy(state->drone_id, value, tlv_len);
                state->drone_id[tlv_len] = '\0';
                parsed = true;
            }
            break;

        case TLV_ANSI_CTA_ID:
            /* Use as drone_id if ID_FR not yet set */
            if (state->drone_id[0] == '\0' && tlv_len > 0 &&
                tlv_len < sizeof(state->drone_id)) {
                memcpy(state->drone_id, value, tlv_len);
                state->drone_id[tlv_len] = '\0';
            }
            break;

        case TLV_LATITUDE: {
            double lat = parse_ascii_double(value, tlv_len);
            if (lat >= -90.0 && lat <= 90.0 && lat != 0.0) {
                state->latitude = lat;
                state->has_location = true;
                parsed = true;
            }
            break;
        }

        case TLV_LONGITUDE: {
            double lon = parse_ascii_double(value, tlv_len);
            if (lon >= -180.0 && lon <= 180.0 && lon != 0.0) {
                state->longitude = lon;
                state->has_location = true;
                parsed = true;
            }
            break;
        }

        case TLV_ALTITUDE_MSL:
            state->altitude_m = parse_ascii_double(value, tlv_len);
            break;

        case TLV_HEIGHT_AGL:
            state->height_agl_m = parse_ascii_double(value, tlv_len);
            break;

        case TLV_TAKEOFF_LAT: {
            double lat = parse_ascii_double(value, tlv_len);
            if (lat >= -90.0 && lat <= 90.0 && lat != 0.0) {
                state->operator_lat = lat;
                /* operator location set */
            }
            break;
        }

        case TLV_TAKEOFF_LON: {
            double lon = parse_ascii_double(value, tlv_len);
            if (lon >= -180.0 && lon <= 180.0 && lon != 0.0) {
                state->operator_lon = lon;
                /* operator location set */
            }
            break;
        }

        case TLV_GROUND_SPEED:
            state->speed_mps = parse_ascii_float(value, tlv_len);
            break;

        case TLV_HEADING: {
            float hdg = parse_ascii_float(value, tlv_len);
            if (hdg >= 0.0f && hdg <= 360.0f) {
                state->heading_deg = hdg;
            }
            break;
        }

        default:
            /* Unknown TLV type — skip */
            break;
        }

        offset += tlv_len;
    }

    if (parsed) {
        ESP_LOGD(TAG, "Parsed French DRI: id=%s lat=%.6f lon=%.6f alt=%.0fm",
                 state->drone_id, state->latitude, state->longitude,
                 state->altitude_m);
    }

    return parsed;
}

bool french_dri_parse_frame(const uint8_t *frame, size_t len, odid_state_t *state)
{
    if (frame == NULL || state == NULL) return false;

    /* Tagged parameters start after MAC header (24) + fixed fields (12) */
    const size_t TAGGED_PARAMS_OFFSET = 24 + 12;

    if (len < TAGGED_PARAMS_OFFSET + 2) return false;

    size_t pos = TAGGED_PARAMS_OFFSET;
    bool found = false;

    while (pos + 2 <= len) {
        uint8_t tag_id  = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        size_t ie_start = pos + 2;

        if (ie_start + tag_len > len) break;

        if (tag_id == IE_VENDOR_SPECIFIC && tag_len >= FRENCH_MIN_PAYLOAD_SIZE) {
            if (french_dri_parse_ie(&frame[ie_start], tag_len, state)) {
                found = true;
            }
        }

        pos = ie_start + tag_len;
    }

    return found;
}
