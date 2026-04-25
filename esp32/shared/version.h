#pragma once

/**
 * Friend or Foe — Unified Firmware Version
 *
 * Single source of truth for current ESP32-S3 firmware variants.
 * Update FOF_VERSION here; all boards pick it up automatically.
 */

#define FOF_VERSION "0.63.0-svc138"

/*
 * FIRMWARE_NAME is set per build target:
 *   - "scanner"       (S3 combo/seed: WiFi + BLE)
 *   - "uplink"        (S3 uplink relay)
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
