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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enter serial configuration mode.
 *
 * Waits up to `timeout_ms` for the first serial command. If a command
 * arrives, extends the window until FOF_SAVE or an idle timeout.
 * Writes received values to NVS.
 *
 * @param timeout_ms  Initial wait time for first command (e.g., 3000)
 * @return true if any configuration was saved, false if timed out
 */
bool serial_config_listen(int timeout_ms);

/**
 * Start a low-priority USB serial control listener for runtime maintenance.
 *
 * Supported commands:
 *   FOF_PING        -> FOF_PONG:<version>
 *   FOF_REBOOT      -> restart the app
 *   FOF_BOOTLOADER  -> restart into ESP32 ROM download mode for esptool
 *
 * This is intentionally separate from the startup config window so flashing
 * tools can request bootloader mode from a running badge without physical
 * BOOT/RESET access.
 */
void serial_config_start_control_task(void);

/**
 * Emit a machine-readable badge detection line over USB serial.
 *
 * Android and desktop tools can listen for lines beginning with FOF_DET:
 * while ordinary ESP-IDF logs continue to flow on the same console.
 */
void serial_config_emit_badge_detection(const char *detection_id,
                                        const char *manufacturer,
                                        uint8_t source,
                                        float confidence,
                                        int rssi);

#ifdef __cplusplus
}
#endif
