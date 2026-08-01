#pragma once

/**
 * Friend or Foe -- Serial Configuration Handler
 *
 * Listens on USB console for configuration commands during a brief
 * startup window. This enables the web flasher to write NVS config
 * (WiFi credentials, backend URL, device ID) immediately after flashing.
 *
 * Protocol:
 *   FOF_SET:wifi_ssid=MyNetwork\n
 *   FOF_SET:wifi_pass=MyPassword\n
 *   FOF_SET:backend_url=http://192.168.1.100:8000\n
 *   FOF_SET:device_id=fof_esp32_001\n
 *   FOF_SAVE\n
 *
 * Responses:
 *   FOF_OK:key           — value saved
 *   FOF_SAVED            — all values committed to NVS
 *   FOF_ERROR:message    — error
 *   FOF_READY            — config mode entered, waiting for commands
 *   FOF_TIMEOUT          — config window expired, continuing boot
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERIAL_CONFIG_RECOVERY_DENIED = 0,
    SERIAL_CONFIG_RECOVERY_PING,
    SERIAL_CONFIG_RECOVERY_STATUS,
    SERIAL_CONFIG_RECOVERY_APP_REBOOT,
    SERIAL_CONFIG_RECOVERY_ROM_BOOT,
    SERIAL_CONFIG_RECOVERY_UPLINK_OTA_BEGIN,
} serial_config_recovery_command_t;

/**
 * Dispatch one complete line already framed by badge_usb_transport.
 *
 * Supported runtime recovery commands:
 *   FOF_PING        -> FOF_PONG:<version>
 *   FOF_STATUS      -> machine-readable badge/scanner status JSON
 *   FOF_REBOOT      -> expected software restart back into the app
 *   FOF_BOOTLOADER  -> expected restart into ESP32 ROM download mode
 *   FOF_DOWNLOAD    -> alias for FOF_BOOTLOADER
 *   FOF_FLASH       -> alias for FOF_BOOTLOADER
 *   FOF_CTL:{...}   -> JSON control, including safe_mode/network/fw relay
 *
 * These are also accepted by the badge startup config window so no-button
 * recovery tools can catch a freshly power-cycled badge before display/scanner
 * work starts.
 */
bool serial_config_dispatch_line(
    const uint8_t *line, size_t line_byte_len);

/** Classify the dependency-free commands allowed by startup recovery. */
serial_config_recovery_command_t serial_config_recovery_command_classify(
    const uint8_t *line, size_t line_byte_len);

/** Reauthorize and dispatch one startup-recovery command from its raw span. */
bool serial_config_dispatch_recovery_command(
    const uint8_t *line, size_t line_byte_len);

/** Parse and dispatch the same exact uplink OTA line in normal or recovery. */
bool serial_config_dispatch_uplink_ota_begin(
    const uint8_t *line, size_t line_byte_len);

/** Parse and classify a complete command without invoking any handler. */
bool serial_config_line_is_recognized(
    const uint8_t *line, size_t line_byte_len);

#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
/**
 * Advance an orphaned PREPARING update session without blocking USB command
 * handling. Safe to call from the USB transport task on every loop.
 */
void serial_config_poll_update_preparation(uint32_t now_ms);
#endif

/** Emit one already-bounded FOF_INV frame without interleaving USB control. */
bool serial_config_emit_investigation_frame(const char *frame);

/**
 * Emit a machine-readable badge detection line over USB serial.
 *
 * Android and desktop tools can listen for lines beginning with FOF_DET:
 * while ordinary ESP-IDF logs continue to flow on the same console.
 */
void serial_config_emit_badge_detection(const char *detection_id,
                                        const char *manufacturer,
                                        const char *badge_label,
                                        const char *badge_class,
                                        const char *badge_entity_key,
                                        uint8_t source,
                                        float confidence,
                                        float threat_score,
                                        int rssi);

#ifdef __cplusplus
}
#endif
