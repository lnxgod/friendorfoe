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
#define FOF_VERSION_PROD  "0.64.68-live-follow"
#define FOF_VERSION_BADGE "0.67.2-badge-defcon34"
#define FOF_VERSION_BADGE_CANARY "0.67.2-badge-defcon34"

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
#define FOF_VERSION FOF_VERSION_BADGE_CANARY
#elif defined(FOF_BADGE_VARIANT)
#define FOF_VERSION FOF_VERSION_BADGE
#else
#define FOF_VERSION FOF_VERSION_PROD
#endif

/*
 * Compile-selected release identity. These values are the runtime side of the
 * same five target -> ESP app project map in scanner/uplink CMakeLists.txt.
 * Keep the project names under ESP_APP_DESC_PROJECT_NAME_SIZE (32 bytes).
 */
#if defined(UPLINK_BOARD)
# if defined(FOF_BADGE_VARIANT)
#  define FOF_FIRMWARE_TARGET "uplink-s3-fof_badge"
#  define FOF_APP_PROJECT "fof_badge_uplink"
#  define FOF_HARDWARE_TYPE "seeed_xiao_esp32s3"
# else
#  define FOF_FIRMWARE_TARGET "uplink-s3"
#  define FOF_APP_PROJECT "fof_uplink"
#  define FOF_HARDWARE_TYPE "esp32-s3-devkitc-1"
# endif
#elif defined(SCANNER_BOARD)
# if defined(FOF_BADGE_VARIANT)
#  define FOF_FIRMWARE_TARGET "scanner-s3-combo-fof_badge"
#  define FOF_APP_PROJECT "fof_badge_scanner"
#  define FOF_HARDWARE_TYPE "seeed_xiao_esp32s3"
# elif defined(SEED_SCANNER_PINS)
#  define FOF_FIRMWARE_TARGET "scanner-s3-combo-seed"
#  define FOF_APP_PROJECT "fof_scanner_seed"
#  define FOF_HARDWARE_TYPE "esp32-s3-devkitc-1"
# else
#  define FOF_FIRMWARE_TARGET "scanner-s3-combo"
#  define FOF_APP_PROJECT "fof_scanner"
#  define FOF_HARDWARE_TYPE "esp32-s3-devkitc-1"
# endif
#elif defined(BLE_SCANNER_BOARD)
# define FOF_FIRMWARE_TARGET "ble-scanner"
# define FOF_APP_PROJECT "fof_ble_scanner"
# define FOF_HARDWARE_TYPE "esp32"
#elif defined(RID_SIMULATOR_BOARD)
# define FOF_FIRMWARE_TARGET "rid-simulator"
# define FOF_APP_PROJECT "fof_rid_simulator"
# define FOF_HARDWARE_TYPE "esp32"
#else
/* Host-only native tests compile shared headers without a firmware board. */
# define FOF_FIRMWARE_TARGET "host-test"
# define FOF_APP_PROJECT "fof_host_test"
# define FOF_HARDWARE_TYPE "native"
#endif

/**
 * Machine-readable identification line.
 * Printed as the very first log line in app_main().
 * Format: FOF_IDENT:<target>:<project>:<hardware>:<version>:<chip>
 *
 * Auto-flash tools can match on "^FOF_IDENT:" to identify the board.
 */
#define FOF_PRINT_IDENT(tag, target) \
    ESP_LOGI(tag, "FOF_IDENT:%s:%s:%s:%s:%s", target, FOF_APP_PROJECT, \
             FOF_HARDWARE_TYPE, FOF_VERSION, CONFIG_IDF_TARGET)
