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

/* Scanner (ESP32-S3) pin assignment.
 * ProductionFullSize (all-S3 node, N16R8 carrier): TX=17 RX=18 (uplink S3 receives on 18+16 per config.h)
 * Seed (mini N8R8 carrier board): TX=1 RX=2 — physical connector only brings out GPIO 1/2.
 * Legacy (with ESP32 OLED uplink): TX=17 RX=16 (uplink ESP32 UART2: RX=D16, TX=D17) */
#if defined(FOF_BADGE_SCANNER_PINS) || defined(SEED_SCANNER_PINS)
#define SCANNER_S3_UART_TX_PIN      1
#define SCANNER_S3_UART_RX_PIN      2
#elif defined(LEGACY_SCANNER_PINS)
#define SCANNER_S3_UART_TX_PIN      17
#define SCANNER_S3_UART_RX_PIN      16
#else
#define SCANNER_S3_UART_TX_PIN      17
#define SCANNER_S3_UART_RX_PIN      18
#endif

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
#define MSG_TYPE_CAL_MODE_START     "cal_mode_start"
#define MSG_TYPE_CAL_MODE_STOP      "cal_mode_stop"
#define MSG_TYPE_CAL_MODE_ACK       "cal_mode_ack"
#define JSON_KEY_SCAN_MODE          "scan_mode"
#define JSON_KEY_CALIBRATION_UUID   "calibration_uuid"
#define JSON_KEY_SESSION_ID         "session_id"
/* Apple Continuity deep fields */
#define JSON_KEY_BLE_APPLE_AUTH     "ble_auth"   /* hex string "a1b2c3" */
#define JSON_KEY_BLE_ACTIVITY       "ble_act"    /* uint8 activity code */
#define JSON_KEY_BLE_RAW_MFR        "ble_mfr"    /* hex string (first 20 bytes of mfr data) */
#define JSON_KEY_BLE_ADV_INTERVAL   "ble_ival"   /* uint32 microseconds between advertisements */

#define MSG_TYPE_BLE_LOCKON         "ble_lockon"
#define MSG_TYPE_BLE_LOCKON_CANCEL  "ble_lockon_cancel"

/* OTA relay messages (uplink → scanner) */
#define MSG_TYPE_OTA_BEGIN          "ota_begin"
#define MSG_TYPE_OTA_END            "ota_end"
#define MSG_TYPE_OTA_ABORT          "ota_abort"
/* OTA relay messages (scanner → uplink) */
#define MSG_TYPE_STOP_ACK           "stop_ack"      /* scanner confirms TX loop halted (v0.59+) */
#define MSG_TYPE_TIME               "time"          /* uplink → scanner wall-clock broadcast (v0.60+) */
#define JSON_KEY_EPOCH_MS           "ms"            /* epoch milliseconds in MSG_TYPE_TIME */
#define JSON_KEY_TIME_OK            "ok"
#define JSON_KEY_TIME_SOURCE        "src"
#define MSG_TYPE_OTA_ACK            "ota_ack"
#define MSG_TYPE_OTA_NACK           "ota_nack"      /* bad CRC — retransmit the named seq */
#define MSG_TYPE_OTA_PROGRESS       "ota_progress"
#define MSG_TYPE_OTA_DONE           "ota_done"
#define MSG_TYPE_OTA_ERROR          "ota_error"

/* OTA sequence number JSON key — ack / nack payloads carry "seq": <u16> so
 * the uplink knows which chunk succeeded/failed. Additive in v0.59; older
 * scanners that emit only `ota_ack` without seq still interop (uplink falls
 * back to its existing ACK-counting timeout behavior). */
#define JSON_KEY_OTA_SEQ            "seq"
#define JSON_KEY_OTA_REASON         "reason"        /* free-text reason in ota_error */

/* Binary OTA chunk: [0xF0][seq(2)][len(2)] + data + [CRC32(4)]
 * CRC32 covers data bytes only (not header). Scanner verifies CRC
 * and ACKs/NACKs each chunk for reliable retransmission. */
#define OTA_CHUNK_MAGIC             0xF0
#define OTA_CHUNK_HEADER_SIZE       5
#define OTA_CHUNK_CRC_SIZE          4       /* CRC32 trailer */
#define OTA_CHUNK_MAX_DATA          512     /* smaller chunks = faster retransmit */
#define OTA_ACK_INTERVAL_CHUNKS     16      /* legacy, unused with per-chunk ACK */

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
#define JSON_KEY_WIFI_AUTH_MODE     "auth_m"         /* WiFi AP encryption mode (v0.61+) — distinct from existing "auth_fr" */
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
#define JSON_KEY_BLE_SVC_UUIDS      "ble_svc"    /* comma-separated hex UUIDs */
#define JSON_KEY_BLE_NAME           "ble_name"
#define JSON_KEY_CLASS_REASON       "class_reason"
/* Apple Nearby Info (Continuity type 0x10) / Nearby Action (0x0F) data-flags
 * byte. Always emitted by v0.58+ scanners — even when 0, so the backend can
 * distinguish "all flags false" from "field absent". Bit semantics (per
 * furiousMAC `nearby_info.md` reverse engineering):
 *     0x01 = AirPods connected
 *     0x02 = WiFi on
 *     0x04 = Watch paired
 *     0x08 = Primary iCloud account
 *     0x10 = Auth tag present
 *     0x20 = Screen on / active
 * Other bits reserved. Bit meanings drift across iOS versions — treat as
 * hints, not certainties. v0.57 and earlier used key "ble_ainfo" with a
 * non-zero-only emit; cold-renamed here (legacy uplinks drop the byte
 * until reflashed). */
#define JSON_KEY_BLE_APPLE_FLAGS    "ble_apple_flags"

/* ── Framing ─────────────────────────────────────────────────────────────── */

/* Each JSON message is terminated by '\n' (0x0A).
 * The receiver accumulates bytes until '\n' then parses the complete JSON. */
#define UART_MSG_DELIMITER          '\n'

#ifdef __cplusplus
}
#endif
