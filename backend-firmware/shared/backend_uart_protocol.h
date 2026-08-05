#pragma once

#include <stdint.h>

#include "ble_investigation_types.h"

/* Bounded newline-delimited JSON control/detection channel. */
#define UART_JSON_MAX_SIZE          1024
#define UART_MSG_DELIMITER          '\n'

/* Backend commands consumed by scanner/uplink runtime tasks. */
#define MSG_TYPE_DETECTION          "detection"
#define MSG_TYPE_STATUS             "status"
#define MSG_TYPE_CONFIG             "config"
#define MSG_TYPE_ACK                "ack"
#define MSG_TYPE_LOCKON             "lockon"
#define MSG_TYPE_LOCKON_CANCEL      "lockon_cancel"
#define MSG_TYPE_BLE_LOCKON         "ble_lockon"
#define MSG_TYPE_BLE_LOCKON_CANCEL  "ble_lockon_cancel"
#define MSG_TYPE_SCANNER_QUIET      "scanner_quiet"
#define MSG_TYPE_SCANNER_QUIET_ACK  "scanner_quiet_ack"
#define MSG_TYPE_TIME               "time"

#define MSG_TYPE_OTA_BEGIN          "ota_begin"
#define MSG_TYPE_OTA_END            "ota_end"
#define MSG_TYPE_OTA_ABORT          "ota_abort"
#define MSG_TYPE_OTA_ACK            "ota_ack"
#define MSG_TYPE_OTA_NACK           "ota_nack"
#define MSG_TYPE_OTA_PROGRESS       "ota_progress"
#define MSG_TYPE_OTA_STAGED         "ota_staged"
#define MSG_TYPE_OTA_DONE           "ota_done"
#define MSG_TYPE_OTA_ERROR          "ota_error"
#define MSG_TYPE_FW_CHECK           "fw_check"
#define MSG_TYPE_FW_OFFER           "fw_offer"
#define MSG_TYPE_FW_READY           "fw_ready"
#define MSG_TYPE_FW_CHECK_NOW       "fw_check_now"

/* Binary OTA framing copied without target/pin or variant branches. */
#define OTA_CHUNK_MAGIC             0xF0
#define OTA_CHUNK_HEADER_SIZE       5
#define OTA_CHUNK_CRC_SIZE          4
#define OTA_ABORT_SENTINEL_BYTE     0xFF
#define OTA_ABORT_SENTINEL_COUNT    8
#define OTA_CHUNK_MAX_DATA          512
#define OTA_ACK_INTERVAL_CHUNKS     16

/* Compact detection keys used by the backend wire encoder. */
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
#define JSON_KEY_WIFI_AUTH_MODE     "auth_m"
#define JSON_KEY_FUSED_CONFIDENCE   "fused"
#define JSON_KEY_FIRST_SEEN         "first"
#define JSON_KEY_LAST_UPDATED       "last"
#define JSON_KEY_SEQ                "seq"
#define JSON_KEY_PROBED_SSIDS       "probed"
#define JSON_KEY_BLE_COMPANY_ID     "ble_cid"
#define JSON_KEY_BLE_APPLE_TYPE     "ble_at"
#define JSON_KEY_BLE_AD_TYPES       "ble_adt"
#define JSON_KEY_BLE_PAYLOAD_LEN    "ble_pl"
#define JSON_KEY_BLE_ADDR_TYPE      "ble_atype"
#define JSON_KEY_BLE_JA3            "ble_ja3"
#define JSON_KEY_BLE_SVC_UUIDS      "ble_svc"
#define JSON_KEY_BLE_NAME           "ble_name"
#define JSON_KEY_CLASS_REASON       "class_reason"
#define JSON_KEY_BLE_APPLE_AUTH     "ble_auth"
#define JSON_KEY_BLE_ACTIVITY       "ble_act"
#define JSON_KEY_BLE_RAW_MFR        "ble_mfr"
#define JSON_KEY_BLE_ADV_INTERVAL   "ble_ival"
#define JSON_KEY_BLE_APPLE_FLAGS    "ble_apple_flags"
#define JSON_KEY_BLE_THREAT_KIND    "ble_tk"
#define JSON_KEY_BLE_PROMPT_FAMILIES "ble_pf"
#define JSON_KEY_BLE_UNIQUE_MACS    "ble_um"
#define JSON_KEY_BLE_OBSERVATIONS   "ble_oc"
#define JSON_KEY_BLE_SERIAL_UUID    "ble_su"
#define JSON_KEY_BLE_THREAT_EVIDENCE "ble_ev"

/* Explicit backend names for the BLE investigation property bit contract. */
#define BACKEND_BLE_INV_PROP_BROADCAST BLE_INV_PROP_BROADCAST
#define BACKEND_BLE_INV_PROP_READ BLE_INV_PROP_READ
#define BACKEND_BLE_INV_PROP_WRITE_WITHOUT_RESPONSE \
    BLE_INV_PROP_WRITE_WITHOUT_RESPONSE
#define BACKEND_BLE_INV_PROP_WRITE BLE_INV_PROP_WRITE
#define BACKEND_BLE_INV_PROP_NOTIFY BLE_INV_PROP_NOTIFY
#define BACKEND_BLE_INV_PROP_INDICATE BLE_INV_PROP_INDICATE
#define BACKEND_BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES \
    BLE_INV_PROP_AUTHENTICATED_SIGNED_WRITES
#define BACKEND_BLE_INV_PROP_EXTENDED_PROPERTIES \
    BLE_INV_PROP_EXTENDED_PROPERTIES
