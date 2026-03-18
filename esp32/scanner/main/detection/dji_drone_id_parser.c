/**
 * Friend or Foe — DJI DroneID Parser (ESP32-S3)
 *
 * Ported from Android DjiDroneIdParser.kt.
 * Parses DJI-proprietary vendor-specific IEs from WiFi beacons.
 *
 * IE format (after 3-byte OUI 0x26 0x37 0x12):
 *   Byte 0:      Version/type indicator
 *   Bytes 1-4:   Serial number prefix (first 4 chars, ASCII)
 *   Bytes 5-8:   Drone longitude (int32, degrees x 1e7)
 *   Bytes 9-12:  Drone latitude (int32, degrees x 1e7)
 *   Bytes 13-14: Altitude (int16, meters relative to takeoff)
 *   Bytes 15-16: Height above ground (int16, decimeters)
 *   Bytes 17-18: Speed (uint16, cm/s)
 *   Bytes 19-20: Heading (int16, degrees x 100, 0 = North)
 *   Bytes 21-24: Home longitude (int32, degrees x 1e7)
 *   Bytes 25-28: Home latitude (int32, degrees x 1e7)
 */

#include "dji_drone_id_parser.h"
#include "constants.h"

#include <string.h>
#include <math.h>
#include <esp_log.h>

static const char *TAG = "dji_parser";

/* ── Little-endian read helpers ─────────────────────────────────────────── */

static int32_t read_int32_le(const uint8_t *data, size_t offset, size_t len)
{
    if (offset + 4 > len) return 0;
    return (int32_t)(
        ((uint32_t)data[offset])           |
        ((uint32_t)data[offset + 1] << 8)  |
        ((uint32_t)data[offset + 2] << 16) |
        ((uint32_t)data[offset + 3] << 24)
    );
}

static int16_t read_int16_le(const uint8_t *data, size_t offset, size_t len)
{
    if (offset + 2 > len) return 0;
    uint16_t val = (uint16_t)(
        ((uint16_t)data[offset]) |
        ((uint16_t)data[offset + 1] << 8)
    );
    return (int16_t)val;
}

static uint16_t read_uint16_le(const uint8_t *data, size_t offset, size_t len)
{
    if (offset + 2 > len) return 0;
    return (uint16_t)(
        ((uint16_t)data[offset]) |
        ((uint16_t)data[offset + 1] << 8)
    );
}

/* ── Internal payload parser ────────────────────────────────────────────── */

/**
 * Parse the DroneID payload bytes (after OUI has been stripped).
 * `data` points to the first byte after the 3-byte OUI.
 * `data_len` is the number of bytes available after OUI.
 */
static bool parse_payload(const uint8_t *data, size_t data_len, dji_drone_id_data_t *out)
{
    if (data_len < DJI_MIN_PAYLOAD_LENGTH) {
        ESP_LOGD(TAG, "DroneID IE too short: %zu bytes (need %d)", data_len, DJI_MIN_PAYLOAD_LENGTH);
        return false;
    }

    memset(out, 0, sizeof(*out));

    out->version = data[0];

    /* Drone position: degrees x 1e7 as signed 32-bit integer */
    double drone_lon = (double)read_int32_le(data, 5, data_len) / 1e7;
    double drone_lat = (double)read_int32_le(data, 9, data_len) / 1e7;

    /* Validate coordinates */
    if (drone_lat < -90.0 || drone_lat > 90.0 || drone_lon < -180.0 || drone_lon > 180.0) {
        ESP_LOGD(TAG, "Invalid coordinates in DroneID: lat=%.7f, lon=%.7f", drone_lat, drone_lon);
        return false;
    }
    if (drone_lat == 0.0 && drone_lon == 0.0) {
        ESP_LOGD(TAG, "Zero coordinates in DroneID — GPS not acquired");
        return false;
    }

    out->latitude = drone_lat;
    out->longitude = drone_lon;

    /* Altitude relative to takeoff (int16, meters) */
    out->altitude_m = (double)read_int16_le(data, 13, data_len);

    /* Height above ground (int16, decimeters -> meters) */
    out->height_above_ground_m = (double)read_int16_le(data, 15, data_len) / 10.0;

    /* Speed (uint16, cm/s -> m/s) */
    uint16_t speed_cms = read_uint16_le(data, 17, data_len);
    out->speed_mps = (float)speed_cms / 100.0f;

    /* Heading (int16, degrees x 100 -> degrees) */
    int16_t heading_raw = read_int16_le(data, 19, data_len);
    out->heading_deg = (float)heading_raw / 100.0f;

    /* Home point / operator position */
    double home_lon = (double)read_int32_le(data, 21, data_len) / 1e7;
    double home_lat = (double)read_int32_le(data, 25, data_len) / 1e7;

    if (home_lat != 0.0 || home_lon != 0.0) {
        out->home_lat = home_lat;
        out->home_lon = home_lon;
        out->has_home = true;
    } else {
        out->home_lat = 0.0;
        out->home_lon = 0.0;
        out->has_home = false;
    }

    /* Serial number prefix: ASCII bytes at offset 1-4 */
    size_t serial_copy = 4;
    if (serial_copy > data_len - 1) {
        serial_copy = data_len - 1;
    }
    memcpy(out->serial_prefix, &data[1], serial_copy);
    out->serial_prefix[serial_copy] = '\0';

    /* Trim trailing nulls from serial */
    for (int i = (int)serial_copy - 1; i >= 0; i--) {
        if (out->serial_prefix[i] == '\0' || out->serial_prefix[i] == ' ') {
            out->serial_prefix[i] = '\0';
        } else {
            break;
        }
    }

    ESP_LOGD(TAG, "Parsed DroneID: lat=%.7f, lon=%.7f, alt=%.0fm, "
             "speed=%.1fm/s, heading=%.1f°, serial=%s",
             out->latitude, out->longitude, out->altitude_m,
             out->speed_mps, out->heading_deg, out->serial_prefix);

    return true;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool dji_parse_vendor_ie(const uint8_t *payload, size_t len, dji_drone_id_data_t *out)
{
    if (payload == NULL || out == NULL) return false;

    /* Need at least 3 OUI bytes + MIN_PAYLOAD_LENGTH data bytes */
    if (len < 3 + DJI_MIN_PAYLOAD_LENGTH) return false;

    /* Verify DJI OUI */
    if (payload[0] != DJI_OUI_0 || payload[1] != DJI_OUI_1 || payload[2] != DJI_OUI_2) {
        return false;
    }

    /* Strip OUI and parse the remaining payload */
    return parse_payload(&payload[3], len - 3, out);
}

bool dji_parse_beacon_frame(const uint8_t *frame, size_t len, dji_drone_id_data_t *out)
{
    if (frame == NULL || out == NULL) return false;

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

    while (pos + 2 <= len) {
        uint8_t tag_id = frame[pos];
        uint8_t tag_len = frame[pos + 1];
        size_t ie_start = pos + 2;

        /* Bounds check */
        if (ie_start + tag_len > len) break;

        if (tag_id == IE_VENDOR_SPECIFIC && tag_len >= 3 + DJI_MIN_PAYLOAD_LENGTH) {
            /* Check DJI OUI at the start of the IE payload */
            if (frame[ie_start] == DJI_OUI_0 &&
                frame[ie_start + 1] == DJI_OUI_1 &&
                frame[ie_start + 2] == DJI_OUI_2) {

                /* Strip OUI and parse */
                const uint8_t *data = &frame[ie_start + 3];
                size_t data_len = tag_len - 3;
                if (parse_payload(data, data_len, out)) {
                    return true;
                }
            }
        }

        pos = ie_start + tag_len;
    }

    return false;
}
