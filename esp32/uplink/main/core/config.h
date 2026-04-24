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

/* ── WiFi credential struct (used by multi-SSID list) ─────────────────── */

typedef struct {
    const char *ssid;
    const char *password;
} wifi_credential_t;

/* ── WiFi credentials — loaded from local-only file (gitignored) ──────── */
#include "wifi_credentials.h"

/* ── Backend URL ───────────────────────────────────────────────────────── */

/* Primary: mDNS hostname (works regardless of DHCP IP changes) */
#define CONFIG_BACKEND_URL          "http://fof-server.local:8000"
/* Fallback: last known static IP */
#define CONFIG_BACKEND_URL_FALLBACK "http://192.168.42.162:8000"
#define CONFIG_UPLOAD_ENDPOINT      "/detections/drones"

/* ── Device identity ───────────────────────────────────────────────────── */

#ifndef CONFIG_DEVICE_ID
#define CONFIG_DEVICE_ID            "auto"   /* MAC-based: uplink_XXYYZZ */
#endif

/* ── Upload settings ───────────────────────────────────────────────────── */

#define CONFIG_BATCH_INTERVAL_MS    80      /* Fast drain to keep queue from filling */
#define CONFIG_BATCH_IDLE_FLUSH_MS  25      /* Quick idle flush */
#define CONFIG_MAX_BATCH_SIZE       6       /* Small batches = less heap per POST */
#define CONFIG_TARGET_BATCH_BYTES   1000    /* Conservative payload size for heap safety */
#if defined(UPLINK_ESP32S3)
/* S3 stores the offline queue in PSRAM — 512 × 4 KB = 2 MB ≈ 10 min of
 * steady uplink traffic. See esp32/shared/psram_policy.md §5. */
#define CONFIG_MAX_OFFLINE_BATCHES  512
#else
/* Legacy ESP32 (no PSRAM) — keep 1 batch to preserve heap stability. */
#define CONFIG_MAX_OFFLINE_BATCHES  1
#endif
#define CONFIG_MAX_RETRY_DELAY_MS   60000
#define CONFIG_HEARTBEAT_INTERVAL_MS 60000

/* ── Display refresh interval ──────────────────────────────────────────── */

#define CONFIG_DISPLAY_UPDATE_MS    500

/* ── Scanner UART inputs ──────────────────────────────────────────────── */

#if defined(UPLINK_ESP32)
/* ideaspark ESP32 OLED — match physical wiring:
 *   UART2: GPIO16(RX)/17(TX) = BLE scanner (S3)
 *   UART1: GPIO25(RX)/26(TX) = WiFi scanner (C5) */
#define CONFIG_BLE_SCANNER_UART     UART_NUM_2
#define CONFIG_BLE_SCANNER_RX_PIN   16
#define CONFIG_BLE_SCANNER_TX_PIN   17
#define CONFIG_WIFI_SCANNER_UART    UART_NUM_1
#define CONFIG_WIFI_SCANNER_RX_PIN  25
#define CONFIG_WIFI_SCANNER_TX_PIN  26
#define CONFIG_DUAL_SCANNER         1

#elif defined(UPLINK_ESP32S3)
/* ESP32-S3 uplink — PRODUCTION wiring. Crossed cables on both slots; no
 * pin-number matches between scanner and uplink sides, so ribbon cables
 * can't be wired "straight-through by label" and end up swapped (what bit
 * us on Pool / FrontYard BLE-slot builds).
 *
 * Scanner-side UART pins are fixed on every scanner board: TX=17, RX=18.
 * Uplink-side RX/TX are the MIRROR:
 *
 *   BLE slot  (UART1):
 *     Scanner TX(17) → Uplink GPIO 18 (RX)
 *     Scanner RX(18) ← Uplink GPIO 17 (TX)
 *   WiFi slot (UART2):
 *     Scanner TX(17) → Uplink GPIO 16 (RX)
 *     Scanner RX(18) ← Uplink GPIO 15 (TX)
 */
#define CONFIG_BLE_SCANNER_UART     UART_NUM_1
#define CONFIG_BLE_SCANNER_RX_PIN   18
#define CONFIG_BLE_SCANNER_TX_PIN   17
#define CONFIG_WIFI_SCANNER_UART    UART_NUM_2
#define CONFIG_WIFI_SCANNER_RX_PIN  16
#define CONFIG_WIFI_SCANNER_TX_PIN  15
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

#if defined(UPLINK_ESP32S3)
#define CONFIG_DETECTION_QUEUE_SIZE 48    /* S3 has headroom; prevents BLE bursts from starving WiFi/RID */
#else
#define CONFIG_DETECTION_QUEUE_SIZE 20
#endif

/* ── Task stack sizes (bytes) ──────────────────────────────────────────── */

#define CONFIG_UART_RX_STACK        5120  /* Needs headroom for cJSON parsing + backpressure logic */
#define CONFIG_HTTP_UPLOAD_STACK   16384  /* Needs full 16KB for raw socket + DNS + error paths */
#define CONFIG_GPS_STACK            3072  /* Reduced from 4096 */
#define CONFIG_DISPLAY_STACK        3072  /* Reduced from 4096 */
#define CONFIG_LED_STACK            2048  /* Can't reduce — RGB LED driver needs this */

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
