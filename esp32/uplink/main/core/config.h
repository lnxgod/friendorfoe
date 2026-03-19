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

#define CONFIG_WIFI_SSID            "YourSSID"
#define CONFIG_WIFI_PASSWORD        "YourPassword"

/* ── Backend URL ───────────────────────────────────────────────────────── */

#define CONFIG_BACKEND_URL          "http://192.168.1.100:8000"
#define CONFIG_UPLOAD_ENDPOINT      "/detections/drones"

/* ── Device identity ───────────────────────────────────────────────────── */

#define CONFIG_DEVICE_ID            "fof_esp32_001"

/* ── Upload settings ───────────────────────────────────────────────────── */

#define CONFIG_BATCH_INTERVAL_MS    5000
#define CONFIG_MAX_BATCH_SIZE       10
#define CONFIG_MAX_OFFLINE_BATCHES  100
#define CONFIG_MAX_RETRY_DELAY_MS   60000

/* ── Display refresh interval ──────────────────────────────────────────── */

#define CONFIG_DISPLAY_UPDATE_MS    500

/* ── GPS UART ──────────────────────────────────────────────────────────── */

/* GPS uses UART0 — console is redirected to USB-JTAG (see sdkconfig.defaults) */
#define CONFIG_GPS_UART_NUM         UART_NUM_0
#define CONFIG_GPS_TX_PIN           6
#define CONFIG_GPS_RX_PIN           7
#define CONFIG_GPS_BAUD             9600

/* ── Detection queue ───────────────────────────────────────────────────── */

#define CONFIG_DETECTION_QUEUE_SIZE 50

/* ── Task stack sizes (bytes) ──────────────────────────────────────────── */

#define CONFIG_UART_RX_STACK        4096
#define CONFIG_HTTP_UPLOAD_STACK    8192
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
