#pragma once

/**
 * Friend or Foe — Unified Firmware Version
 *
 * Single source of truth for current ESP32-S3 firmware variants.
 * Update FOF_VERSION here; all boards pick it up automatically.
 */

/*
 * Production and FoF Badge are intentionally on separate version tracks.
 * Production firmware is shared by uplink-s3, scanner-s3-combo, and
 * scanner-s3-combo-seed; the FoF Badge build is XIAO-only and ships a
 * different feature set (Waveshare ST7735 display, Triforce splash).
 *
 * NEVER collapse these — flashing a production node with a "-badge-*"
 * version string is misleading and was caught by the user once already.
 *
 * Both string literals also live as fixed names so the per-target CMake
 * (uplink/CMakeLists.txt, scanner/CMakeLists.txt) can pick the right one
 * for ESP-IDF's PROJECT_VER metadata based on the PIOENV env var.
 */
#define FOF_VERSION_PROD  "0.63.0-svc156"
#define FOF_VERSION_BADGE "0.64.40-badge-ble-theme"

#if defined(FOF_BADGE_VARIANT)
#define FOF_VERSION FOF_VERSION_BADGE
#else
#define FOF_VERSION FOF_VERSION_PROD
#endif

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
