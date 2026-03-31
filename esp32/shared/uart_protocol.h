#pragma once

/**
 * Friend or Foe — UART Protocol Definitions
 *
 * Inter-board communication between the Scanner (ESP32-S3) and Uplink
 * (ESP32-C3) over a dedicated UART link. Messages are newline-delimited
 * JSON objects.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── UART hardware configuration ─────────────────────────────────────────── */

#define UART_BAUD_RATE              921600

/* Scanner (ESP32-S3) pin assignment */
#define SCANNER_S3_UART_TX_PIN      17
#define SCANNER_S3_UART_RX_PIN      18

/* Scanner (plain ESP32) pin assignment */
#define SCANNER_ESP32_UART_TX_PIN   17
#define SCANNER_ESP32_UART_RX_PIN   16

/* Scanner (ESP32-C5) pin assignment */
#define SCANNER_C5_UART_TX_PIN      4
#define SCANNER_C5_UART_RX_PIN      5

/* Select pins based on target chip */
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define SCANNER_UART_TX_PIN         SCANNER_C5_UART_TX_PIN
#define SCANNER_UART_RX_PIN         SCANNER_C5_UART_RX_PIN
#elif defined(WIFI_SCANNER_ONLY)
#define SCANNER_UART_TX_PIN         SCANNER_ESP32_UART_TX_PIN
#define SCANNER_UART_RX_PIN         SCANNER_ESP32_UART_RX_PIN
#else
#define SCANNER_UART_TX_PIN         SCANNER_S3_UART_TX_PIN
#define SCANNER_UART_RX_PIN         SCANNER_S3_UART_RX_PIN
#endif

/* Uplink (ESP32-C3) pin assignment */
#define UPLINK_UART_RX_PIN          20
#define UPLINK_UART_TX_PIN          21

/* Buffer sizes */
#define UART_BUF_SIZE               2048
#define UART_JSON_MAX_SIZE          1024

/* ── Message types ───────────────────────────────────────────────────────── */

#define MSG_TYPE_DETECTION          "detection"
#define MSG_TYPE_STATUS             "status"
#define MSG_TYPE_CONFIG             "config"
#define MSG_TYPE_ACK                "ack"
#define MSG_TYPE_LOCKON             "lockon"
#define MSG_TYPE_LOCKON_CANCEL      "lockon_cancel"

/* ── JSON key names (short to save bandwidth at 921600 baud) ─────────────── */

#define JSON_KEY_TYPE               "type"
#define JSON_KEY_DRONE_ID           "drone_id"
#define JSON_KEY_SOURCE             "src"
#define JSON_KEY_CONFIDENCE         "conf"
#define JSON_KEY_LATITUDE           "lat"
#define JSON_KEY_LONGITUDE          "lon"
#define JSON_KEY_ALTITUDE           "alt"
#define JSON_KEY_RSSI               "rssi"
#define JSON_KEY_TIMESTAMP          "ts"
#define JSON_KEY_HEADING            "hdg"
#define JSON_KEY_SPEED              "spd"
#define JSON_KEY_VSPEED             "vspd"
#define JSON_KEY_DISTANCE           "dist"
#define JSON_KEY_MANUFACTURER       "mfr"
#define JSON_KEY_MODEL              "model"
#define JSON_KEY_OPERATOR_LAT       "op_lat"
#define JSON_KEY_OPERATOR_LON       "op_lon"
#define JSON_KEY_OPERATOR_ID        "op_id"
#define JSON_KEY_UA_TYPE            "ua_type"
#define JSON_KEY_ID_TYPE            "id_type"
#define JSON_KEY_SELF_ID            "self_id"
#define JSON_KEY_HEIGHT_AGL         "h_agl"
#define JSON_KEY_GEODETIC_ALT       "g_alt"
#define JSON_KEY_H_ACCURACY         "h_acc"
#define JSON_KEY_V_ACCURACY         "v_acc"
#define JSON_KEY_SSID               "ssid"
#define JSON_KEY_BSSID              "bssid"
#define JSON_KEY_FREQ               "freq"
#define JSON_KEY_CHANNEL_WIDTH      "ch_w"
#define JSON_KEY_FUSED_CONFIDENCE   "fused"
#define JSON_KEY_FIRST_SEEN         "first"
#define JSON_KEY_LAST_UPDATED       "last"
#define JSON_KEY_SEQ                "seq"
/* WiFi probe request fields */
#define JSON_KEY_PROBED_SSIDS       "probed"
/* BLE fingerprinting fields */
#define JSON_KEY_BLE_COMPANY_ID     "ble_cid"
#define JSON_KEY_BLE_APPLE_TYPE     "ble_at"
#define JSON_KEY_BLE_AD_TYPES       "ble_adt"
#define JSON_KEY_BLE_PAYLOAD_LEN    "ble_pl"
#define JSON_KEY_BLE_ADDR_TYPE      "ble_atype"
#define JSON_KEY_BLE_JA3            "ble_ja3"

/* ── Framing ─────────────────────────────────────────────────────────────── */

/* Each JSON message is terminated by '\n' (0x0A).
 * The receiver accumulates bytes until '\n' then parses the complete JSON. */
#define UART_MSG_DELIMITER          '\n'

#ifdef __cplusplus
}
#endif
