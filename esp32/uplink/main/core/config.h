#pragma once

/**
 * Friend or Foe -- Uplink Configuration Defaults
 *
 * Compile-time defaults for all configurable parameters.
 * Values can be overridden at runtime via NVS (see nvs_config.h).
 */

#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── WiFi credentials (override via NVS) ───────────────────────────────── */

#define CONFIG_WIFI_SSID            "CasaChomp_2g"
#define CONFIG_WIFI_PASSWORD        "CHANGE_ME_PASSWORD"

/* ── Backend URL ───────────────────────────────────────────────────────── */

#define CONFIG_BACKEND_URL          "http://192.168.42.145:8000"
#define CONFIG_UPLOAD_ENDPOINT      "/detections/drones"

/* ── Device identity ───────────────────────────────────────────────────── */

#define CONFIG_DEVICE_ID            "uplink_1"

/* ── Upload settings ───────────────────────────────────────────────────── */

#define CONFIG_BATCH_INTERVAL_MS    200
#define CONFIG_BATCH_IDLE_FLUSH_MS  75
#define CONFIG_MAX_BATCH_SIZE       10
#define CONFIG_TARGET_BATCH_BYTES   1400
#define CONFIG_MAX_OFFLINE_BATCHES  10
#define CONFIG_MAX_RETRY_DELAY_MS   60000
#define CONFIG_HEARTBEAT_INTERVAL_MS 60000

/* ── Display refresh interval ──────────────────────────────────────────── */

#define CONFIG_DISPLAY_UPDATE_MS    500

/* ── Scanner UART inputs ──────────────────────────────────────────────── */

#ifdef UPLINK_ESP32
/* ideaspark ESP32 OLED — match physical wiring:
 *   GPIO17 = BLE scanner (S3)    RX input
 *   GPIO25 = WiFi scanner (C5)   RX input */
#define CONFIG_BLE_SCANNER_UART     UART_NUM_2
#define CONFIG_BLE_SCANNER_RX_PIN   16
#define CONFIG_BLE_SCANNER_TX_PIN   17
#define CONFIG_WIFI_SCANNER_UART    UART_NUM_1
#define CONFIG_WIFI_SCANNER_RX_PIN  25
#define CONFIG_WIFI_SCANNER_TX_PIN  26
#define CONFIG_DUAL_SCANNER         1
#else
/* ESP32-C3 (2 hardware UARTs): UART1=single scanner, UART0=GPS */
#define CONFIG_BLE_SCANNER_UART     UART_NUM_1
#define CONFIG_BLE_SCANNER_RX_PIN   20
#define CONFIG_BLE_SCANNER_TX_PIN   21
#define CONFIG_DUAL_SCANNER         0
/* GPS uses UART0 on C3 */
#define CONFIG_GPS_UART_NUM         UART_NUM_0
#define CONFIG_GPS_TX_PIN           6
#define CONFIG_GPS_RX_PIN           7
#define CONFIG_GPS_BAUD             9600
#endif

/* ── Detection queue ───────────────────────────────────────────────────── */

#define CONFIG_DETECTION_QUEUE_SIZE 50

/* ── Task stack sizes (bytes) ──────────────────────────────────────────── */

#define CONFIG_UART_RX_STACK        4096
#define CONFIG_HTTP_UPLOAD_STACK   16384
#define CONFIG_GPS_STACK            4096
#define CONFIG_DISPLAY_STACK        4096
#define CONFIG_LED_STACK            2048

/* ── WiFi AP defaults (override via NVS) ──────────────────────────────── */

#define CONFIG_AP_PASSWORD          "friendorfoe"
#define CONFIG_AP_CHANNEL           1
#define CONFIG_AP_MAX_CONNECTIONS   4

/* ── HTTP status server ───────────────────────────────────────────────── */

#define CONFIG_HTTP_STATUS_PRIORITY  2
#define CONFIG_HTTP_STATUS_STACK     4096
#define CONFIG_HTTP_STATUS_PORT      80

/* ── Task priorities ───────────────────────────────────────────────────── */

#define CONFIG_UART_RX_PRIORITY     5
#define CONFIG_HTTP_UPLOAD_PRIORITY  4
#define CONFIG_GPS_PRIORITY         3
#define CONFIG_DISPLAY_PRIORITY     2
#define CONFIG_LED_PRIORITY         1

#ifdef __cplusplus
}
#endif
