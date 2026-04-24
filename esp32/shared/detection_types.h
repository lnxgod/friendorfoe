#pragma once

/**
 * Friend or Foe — Shared Detection Types
 *
 * Drone detection structs and enums shared between the Scanner (ESP32-S3)
 * and Uplink (ESP32-C3) firmwares.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Detection source identifiers ────────────────────────────────────────── */

#define DETECTION_SRC_BLE_RID       0   /* BLE Remote ID (ASTM F3411) */
#define DETECTION_SRC_WIFI_SSID     1   /* Drone-like SSID pattern match */
#define DETECTION_SRC_WIFI_DJI_IE   2   /* DJI vendor-specific IE in beacon */
#define DETECTION_SRC_WIFI_BEACON   3   /* WiFi Beacon Remote ID (NAN/Action) */
#define DETECTION_SRC_WIFI_OUI      4   /* Known drone manufacturer OUI */
#define DETECTION_SRC_WIFI_PROBE_REQUEST 5  /* WiFi probe request (device searching for SSID) */
#define DETECTION_SRC_BLE_FINGERPRINT 6  /* Generic BLE fingerprint / device-class traffic */
#define DETECTION_SRC_WIFI_ASSOC    7   /* AP<->STA association traffic */

/* ── Full drone detection state ──────────────────────────────────────────── */

typedef struct {
    /* Identity */
    char        drone_id[64];           /* Serial number or generated ID */
    uint8_t     source;                 /* DETECTION_SRC_* */
    float       confidence;             /* 0.0–1.0  raw source confidence */

    /* Position (WGS84) */
    double      latitude;               /* degrees */
    double      longitude;              /* degrees */
    double      altitude_m;             /* meters MSL */

    /* Kinematics */
    float       heading_deg;            /* 0–360 degrees true north */
    float       speed_mps;              /* horizontal speed, m/s */
    float       vertical_speed_mps;     /* m/s, positive = ascending */

    /* RF measurements */
    int8_t      rssi;                   /* dBm */
    double      estimated_distance_m;   /* distance estimated from RSSI */

    /* Aircraft metadata */
    char        manufacturer[32];
    char        model[32];

    /* Operator / system info (from ASTM System & Operator ID msgs) */
    double      operator_lat;           /* operator position, degrees */
    double      operator_lon;
    char        operator_id[24];

    /* ASTM / OpenDroneID fields */
    uint8_t     ua_type;                /* ASTM UA type classification */
    uint8_t     id_type;                /* ASTM ID type */
    uint8_t     self_id_desc_type;      /* Self-ID description type */
    char        self_id_text[24];       /* free-text self-ID */
    double      height_agl_m;           /* height above ground level */
    double      geodetic_alt_m;         /* geodetic altitude */
    float       h_accuracy_m;           /* horizontal accuracy */
    float       v_accuracy_m;           /* vertical accuracy */

    /* ASTM System message area info */
    uint16_t    area_count;             /* number of aircraft in area */
    uint16_t    area_radius;            /* area radius (x 10m) */
    double      area_ceiling;           /* area ceiling altitude (m) */
    double      area_floor;             /* area floor altitude (m) */
    uint8_t     classification_type;    /* EU category or other */

    /* WiFi-specific */
    char        ssid[33];               /* null-terminated SSID (max 32 chars) */
    char        bssid[18];              /* "AA:BB:CC:DD:EE:FF" */
    int32_t     freq_mhz;              /* centre frequency */
    int32_t     channel_width_mhz;     /* channel width */

    /* BLE-specific (for device fingerprinting / JA3-style grouping) */
    uint16_t    ble_company_id;         /* BT SIG company ID from mfr-specific data */
    uint8_t     ble_apple_type;         /* Apple Continuity sub-type (0x07=AirPods, 0x10=NearbyInfo, 0x12=FindMy) */
    uint16_t    ble_service_uuids[4];   /* Up to 4 16-bit service UUIDs */
    uint8_t     ble_svc_uuid_count;     /* Number of service UUIDs captured */
    /* v0.63: 128-bit service UUIDs (AD 0x06/0x07). Stored LE as transmitted;
     * uart_tx reverses to big-endian hyphenated string on emit. Covers
     * custom service UUIDs that scanners previously dropped silently —
     * including the phone-driven calibration walk beacon (cafe...-...). */
    uint8_t     ble_service_uuids_128[2][16];
    uint8_t     ble_svc_uuid_128_count;
    /* Pass-through raw ble_svc string so the full formatted list
     * survives the scanner→uplink→backend hop intact (uplink uart_rx
     * previously had a 36-char buffer + uint16 re-parse that truncated
     * 128-bit UUIDs to their first 4 hex chars). */
    char        ble_svc_uuids_raw[160]; /* "aaaa,bbbb,xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx,..." + NUL */
    uint8_t     ble_ad_type_count;      /* Number of distinct AD types */
    uint8_t     ble_payload_len;        /* Raw advertisement payload length */
    uint8_t     ble_addr_type;          /* 0=public, 1=random static, 2=RPA, 3=non-resolvable */
    uint32_t    ble_ja3_hash;           /* BLE-JA3 structural profile hash (same for all devices of same model) */

    /* Apple Continuity deep fields */
    uint8_t     ble_apple_auth[3];      /* Auth tag (rotates slower than MAC — entity resolution key) */
    uint8_t     ble_apple_activity;     /* Activity: 0=idle, 1=audio, 2=phone, 3=video, 4=driving */
    uint8_t     ble_apple_flags;        /* Nearby-Info/Action data-flags byte — see
                                           uart_protocol.h JSON_KEY_BLE_APPLE_FLAGS
                                           for bit layout. Renamed from ble_apple_info
                                           in v0.58 (cold switch; dropped non-zero guard). */
    uint8_t     ble_raw_mfr[20];       /* First 20 bytes of manufacturer-specific data */
    uint8_t     ble_raw_mfr_len;       /* Actual length captured (0-20) */

    /* Advertisement timing */
    int64_t     ble_adv_interval_us;   /* Microseconds since last ad from same MAC (0 = first sighting) */

    /* Timestamps */
    int64_t     first_seen_ms;          /* epoch milliseconds */
    int64_t     last_updated_ms;        /* epoch milliseconds */

    /* Post-fusion */
    float       fused_confidence;       /* after Bayesian sensor fusion */

    /* WiFi probe request fingerprinting */
    char        probed_ssids[128];      /* Comma-separated SSIDs being probed for */
    uint32_t    probe_ie_hash;          /* FNV1a hash of IE type sequence (device fingerprint) */
    uint8_t     wifi_generation;        /* 0=legacy, 4=802.11n, 5=802.11ac, 6=802.11ax */

    /* WiFi AP encryption (v0.61+, Marauder feature parity).
     * Maps esp-idf wifi_auth_mode_t: 0=open, 1=WEP, 2=WPA-PSK, 3=WPA2-PSK,
     * 4=WPA/WPA2-PSK, 5=WPA2-Enterprise, 6=WPA3-PSK, 7=WPA2/WPA3-PSK,
     * 8=WAPI, 9=OWE, 10=WPA3-Ent-192. 0xFF = unknown / not a WiFi AP. */
    uint8_t     wifi_auth_mode;
} drone_detection_t;

#ifdef __cplusplus
}
#endif
