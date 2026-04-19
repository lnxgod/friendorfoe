#pragma once

/**
 * Friend or Foe — Unified Firmware Version
 *
 * Single source of truth for all ESP32 firmware variants.
 * Update FOF_VERSION here; all boards pick it up automatically.
 */

#define FOF_VERSION "0.60.0"

/*
 * FIRMWARE_NAME is set per build target:
 *   - "scanner"       (S3 combo: WiFi + BLE)
 *   - "wifi-scanner"  (WiFi-only, any chip)
 *   - "ble-scanner"   (BLE-only)
 *   - "uplink"        (C3 or ESP32 uplink relay)
 *   - "rid-simulator" (Remote ID simulator)
 *
 * Each main.c #defines FIRMWARE_NAME before including this header,
 * or derives it from build flags.
 */

/**
 * Machine-readable identification line.
 * Printed as the very first log line in app_main().
 * Format: FOF_IDENT:<name>:<version>:<chip>
 *
 * Auto-flash tools can match on "^FOF_IDENT:" to identify the board.
 */
#define FOF_PRINT_IDENT(tag, name) \
    ESP_LOGI(tag, "FOF_IDENT:%s:%s:%s", name, FOF_VERSION, CONFIG_IDF_TARGET)
