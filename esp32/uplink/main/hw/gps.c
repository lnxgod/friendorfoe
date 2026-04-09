/**
 * Friend or Foe -- Uplink GPS Implementation
 *
 * Parses NMEA 0183 sentences ($GPGGA, $GNGGA, $GPRMC, $GNRMC) from
 * a UART-connected GPS module.  Converts NMEA coordinates to decimal
 * degrees and stores the latest valid fix.
 *
 * Disabled on UPLINK_ESP32 builds (nodes use fixed positions from backend).
 */

#include "gps.h"
#include "config.h"

#if defined(UPLINK_ESP32) || defined(UPLINK_ESP32S3)
/* GPS disabled on ESP32/S3 uplink — positions are fixed in backend */
#include <string.h>
#include "esp_log.h"
static const char *TAG = "gps";
void gps_init(void) { ESP_LOGI(TAG, "GPS disabled (fixed node positions via backend)"); }
void gps_start(void) {}
bool gps_has_fix(void) { return false; }
double gps_get_latitude(void) { return 0.0; }
double gps_get_longitude(void) { return 0.0; }
float gps_get_altitude(void) { return 0.0f; }
bool gps_get_position(gps_position_t *pos) {
    if (pos) { memset(pos, 0, sizeof(*pos)); }
    return false;
}
#else

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gps";

#define GPS_LINE_BUF_SIZE   256
#define GPS_READ_BUF_SIZE   128

static gps_position_t s_position = {0};
static portMUX_TYPE   s_lock     = portMUX_INITIALIZER_UNLOCKED;

/* ── NMEA coordinate conversion ────────────────────────────────────────── */

/**
 * Convert NMEA format DDMM.MMMMM (or DDDMM.MMMMM for lon) to
 * decimal degrees.
 *
 * @param nmea_str  Raw NMEA coordinate string (e.g., "3746.12345")
 * @param dir       Direction character: 'N', 'S', 'E', or 'W'
 * @return Decimal degrees (negative for S or W)
 */
static double nmea_to_decimal(const char *nmea_str, char dir)
{
    if (!nmea_str || nmea_str[0] == '\0') {
        return 0.0;
    }

    double raw = atof(nmea_str);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double decimal = degrees + (minutes / 60.0);

    if (dir == 'S' || dir == 'W') {
        decimal = -decimal;
    }

    return decimal;
}

/* ── NMEA field extraction ─────────────────────────────────────────────── */

/**
 * Get the Nth comma-separated field from an NMEA sentence.
 * Returns pointer into the sentence (not a copy).
 * Output is NOT null-terminated at the field boundary -- caller must
 * use the next comma or end-of-string.
 */
static const char *nmea_field(const char *sentence, int field_index)
{
    const char *p = sentence;
    int current = 0;

    while (*p && current < field_index) {
        if (*p == ',') {
            current++;
        }
        p++;
    }

    return p;
}

/**
 * Copy a comma-delimited field into a buffer.
 */
static void nmea_field_copy(const char *sentence, int field_index,
                            char *buf, size_t buf_size)
{
    const char *start = nmea_field(sentence, field_index);
    const char *end = start;
    while (*end && *end != ',' && *end != '*') {
        end++;
    }
    size_t len = end - start;
    if (len >= buf_size) {
        len = buf_size - 1;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
}

/* ── GGA sentence parsing ──────────────────────────────────────────────── */

/**
 * Parse $GPGGA or $GNGGA: Global Positioning System Fix Data
 *
 * Fields: $G?GGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,...
 *          0     1    2   3   4   5    6       7    8    9   10
 */
static void parse_gga(const char *sentence)
{
    char field[32];

    /* Fix quality (field 6): 0=invalid, 1=GPS, 2=DGPS, ... */
    nmea_field_copy(sentence, 6, field, sizeof(field));
    int quality = atoi(field);
    if (quality == 0) {
        return; /* No fix */
    }

    gps_position_t pos = {0};
    pos.has_fix = true;
    pos.fix_time_ms = esp_timer_get_time() / 1000;

    /* Latitude (field 2) + N/S (field 3) */
    char lat_str[20], lat_dir[4];
    nmea_field_copy(sentence, 2, lat_str, sizeof(lat_str));
    nmea_field_copy(sentence, 3, lat_dir, sizeof(lat_dir));
    pos.latitude = nmea_to_decimal(lat_str, lat_dir[0]);

    /* Longitude (field 4) + E/W (field 5) */
    char lon_str[20], lon_dir[4];
    nmea_field_copy(sentence, 4, lon_str, sizeof(lon_str));
    nmea_field_copy(sentence, 5, lon_dir, sizeof(lon_dir));
    pos.longitude = nmea_to_decimal(lon_str, lon_dir[0]);

    /* Satellites (field 7) */
    nmea_field_copy(sentence, 7, field, sizeof(field));
    pos.satellites = atoi(field);

    /* HDOP (field 8) */
    nmea_field_copy(sentence, 8, field, sizeof(field));
    pos.hdop = (float)atof(field);

    /* Altitude MSL (field 9) */
    nmea_field_copy(sentence, 9, field, sizeof(field));
    pos.altitude_m = atof(field);

    /* Store the fix atomically */
    portENTER_CRITICAL(&s_lock);
    s_position = pos;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGD(TAG, "GGA fix: %.6f, %.6f, alt=%.1fm, sats=%d, hdop=%.1f",
             pos.latitude, pos.longitude, pos.altitude_m,
             pos.satellites, pos.hdop);
}

/* ── RMC sentence parsing ──────────────────────────────────────────────── */

/**
 * Parse $GPRMC or $GNRMC: Recommended Minimum Specific GNSS Data
 *
 * Fields: $G?RMC,time,status,lat,N/S,lon,E/W,speed,course,date,...
 *          0     1    2      3   4   5   6    7     8      9
 */
static void parse_rmc(const char *sentence)
{
    char field[32];

    /* Status (field 2): A=active/valid, V=void */
    nmea_field_copy(sentence, 2, field, sizeof(field));
    if (field[0] != 'A') {
        return; /* Not a valid fix */
    }

    /* Latitude (field 3) + N/S (field 4) */
    char lat_str[20], lat_dir[4];
    nmea_field_copy(sentence, 3, lat_str, sizeof(lat_str));
    nmea_field_copy(sentence, 4, lat_dir, sizeof(lat_dir));
    double lat = nmea_to_decimal(lat_str, lat_dir[0]);

    /* Longitude (field 5) + E/W (field 6) */
    char lon_str[20], lon_dir[4];
    nmea_field_copy(sentence, 5, lon_str, sizeof(lon_str));
    nmea_field_copy(sentence, 6, lon_dir, sizeof(lon_dir));
    double lon = nmea_to_decimal(lon_str, lon_dir[0]);

    /* Speed over ground in knots (field 7) */
    nmea_field_copy(sentence, 7, field, sizeof(field));
    float speed_knots = (float)atof(field);
    (void)speed_knots; /* Could be used for motion detection */

    /* Course over ground (field 8) */
    nmea_field_copy(sentence, 8, field, sizeof(field));
    float course = (float)atof(field);
    (void)course;

    /* Update position if we have no GGA fix yet or for lat/lon refinement */
    portENTER_CRITICAL(&s_lock);
    if (!s_position.has_fix) {
        s_position.latitude   = lat;
        s_position.longitude  = lon;
        s_position.has_fix    = true;
        s_position.fix_time_ms = esp_timer_get_time() / 1000;
    }
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGD(TAG, "RMC fix: %.6f, %.6f", lat, lon);
}

/* ── NMEA sentence router ─────────────────────────────────────────────── */

static void process_nmea(const char *sentence)
{
    if (sentence[0] != '$') {
        return;
    }

    /*
     * Match talker+sentence IDs:
     *   $GPGGA, $GNGGA  -> GGA parser
     *   $GPRMC, $GNRMC  -> RMC parser
     * Skip the '$' and check from offset 2 for the sentence type.
     */
    const char *id = sentence + 3; /* skip "$Gx" */

    if (strncmp(id, "GGA,", 4) == 0) {
        parse_gga(sentence);
    } else if (strncmp(id, "RMC,", 4) == 0) {
        parse_rmc(sentence);
    }
    /* Other sentence types ($GSV, $GSA, etc.) are silently ignored */
}

/* ── Verify NMEA checksum ──────────────────────────────────────────────── */

static bool verify_checksum(const char *sentence)
{
    if (sentence[0] != '$') {
        return false;
    }

    uint8_t calc = 0;
    const char *p = sentence + 1; /* skip '$' */

    while (*p && *p != '*') {
        calc ^= (uint8_t)*p;
        p++;
    }

    if (*p == '*' && *(p + 1) && *(p + 2)) {
        char hex[3] = { *(p + 1), *(p + 2), '\0' };
        uint8_t expected = (uint8_t)strtoul(hex, NULL, 16);
        return calc == expected;
    }

    /* No checksum present -- accept anyway (some GPS modules omit it) */
    return true;
}

/* ── GPS task ──────────────────────────────────────────────────────────── */

static void gps_task(void *arg)
{
    char line_buf[GPS_LINE_BUF_SIZE];
    int  line_pos = 0;
    uint8_t read_buf[GPS_READ_BUF_SIZE];

    ESP_LOGI(TAG, "GPS task started");

    while (1) {
        int bytes_read = uart_read_bytes(CONFIG_GPS_UART_NUM, read_buf,
                                         sizeof(read_buf), pdMS_TO_TICKS(200));
        if (bytes_read <= 0) {
            continue;
        }

        for (int i = 0; i < bytes_read; i++) {
            char c = (char)read_buf[i];

            if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';

                    if (verify_checksum(line_buf)) {
                        process_nmea(line_buf);
                    } else {
                        ESP_LOGD(TAG, "Bad NMEA checksum: %.40s...", line_buf);
                    }

                    line_pos = 0;
                }
            } else {
                if (line_pos < GPS_LINE_BUF_SIZE - 1) {
                    line_buf[line_pos++] = c;
                } else {
                    line_pos = 0; /* overflow -- discard */
                }
            }
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void gps_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate  = CONFIG_GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CONFIG_GPS_UART_NUM, 1024, 0,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_GPS_UART_NUM,
                                 CONFIG_GPS_TX_PIN, CONFIG_GPS_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "GPS UART%d initialized: %d baud, TX=GPIO%d, RX=GPIO%d",
             CONFIG_GPS_UART_NUM, CONFIG_GPS_BAUD,
             CONFIG_GPS_TX_PIN, CONFIG_GPS_RX_PIN);
}

void gps_start(void)
{
    xTaskCreate(gps_task, "gps", CONFIG_GPS_STACK,
                NULL, CONFIG_GPS_PRIORITY, NULL);
    ESP_LOGI(TAG, "GPS task created (priority=%d, stack=%d)",
             CONFIG_GPS_PRIORITY, CONFIG_GPS_STACK);
}

bool gps_get_position(gps_position_t *pos)
{
    if (!pos) {
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    *pos = s_position;
    portEXIT_CRITICAL(&s_lock);

    return pos->has_fix;
}

bool gps_has_fix(void)
{
    bool fix;
    portENTER_CRITICAL(&s_lock);
    fix = s_position.has_fix;
    portEXIT_CRITICAL(&s_lock);
    return fix;
}

#endif /* UPLINK_ESP32 */
