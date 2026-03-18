#pragma once

/**
 * Friend or Foe — Shared Constants
 *
 * Ported from the Android app's Kotlin sources to keep detection logic
 * bit-identical across ESP32 firmware and the mobile client.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Bayesian sensor fusion ──────────────────────────────────────────────── */

#define PRIOR_PROBABILITY           0.1
#define EVIDENCE_HALF_LIFE_SEC      30.0
#define MAX_LOG_ODDS                7.0
#define MIN_LOG_ODDS               -7.0

/* Sensor likelihood ratios (higher = stronger evidence of drone) */
#define LR_BLE_RID                  50.0
#define LR_WIFI_BEACON              50.0
#define LR_WIFI_SSID                3.0
#define LR_WIFI_DJI_IE              30.0
#define LR_WIFI_OUI                 5.0

/* ── RSSI-based distance estimation ──────────────────────────────────────── */

#define RSSI_REF                   -40     /* dBm at 1 metre */
#define PATH_LOSS_EXPONENT          2.5

/* ── Detection lifecycle ─────────────────────────────────────────────────── */

#define DETECTION_STALE_MS          120000  /* 2 minutes */
#define PRUNE_INTERVAL_MS           10000   /* 10 seconds */
#define MAX_TRACKED_DRONES          100

/* ── OpenDroneID / ASTM F3411 message types ──────────────────────────────── */

#define ODID_MSG_TYPE_BASIC_ID      0
#define ODID_MSG_TYPE_LOCATION      1
#define ODID_MSG_TYPE_AUTH          2
#define ODID_MSG_TYPE_SELF_ID       3
#define ODID_MSG_TYPE_SYSTEM        4
#define ODID_MSG_TYPE_OPERATOR_ID   5
#define ODID_MSG_TYPE_MESSAGE_PACK  0xF

/* ODID encoding constants */
#define ODID_LAT_LON_SCALE         1e-7
#define ODID_ALT_SCALE             0.5
#define ODID_ALT_OFFSET           -1000.0
#define ODID_SPEED_SCALE           0.25
#define ODID_MSG_SIZE              25
#define ODID_VSPEED_UNKNOWN        63      /* vertical speed byte value = unknown */

/* ── DJI vendor-specific information element ─────────────────────────────── */

#define DJI_OUI_0                   0x26
#define DJI_OUI_1                   0x37
#define DJI_OUI_2                   0x12
#define DJI_MIN_PAYLOAD_LENGTH      29

/* ── ASTM / WiFi Beacon Remote ID ────────────────────────────────────────── */

#define ASTM_OUI_0                  0xFA
#define ASTM_OUI_1                  0x0B
#define ASTM_OUI_2                  0xBC
#define ASTM_OUI_TYPE               0x0D
#define ASTM_MIN_PAYLOAD_SIZE       30  /* 3 OUI + 1 type + 1 counter + 25 msg */

/* ── IEEE 802.11 Information Element tags ────────────────────────────────── */

#define IE_VENDOR_SPECIFIC          221

#ifdef __cplusplus
}
#endif
