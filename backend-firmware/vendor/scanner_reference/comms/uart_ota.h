#pragma once

/**
 * Friend or Foe — Scanner UART OTA Receiver
 *
 * Receives firmware updates from the uplink via UART relay.
 * Protocol: JSON control messages + binary data chunks.
 */

#include <stdbool.h>
#include <stdint.h>
#include "driver/uart.h"
#include "firmware_image_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char target[32];
    char project[33];
    char hardware[33];
    char version[32];
    char sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
    uint32_t generation;
    uint32_t size;
    uint32_t crc32;
    bool allow_same_version;
} uart_ota_manifest_t;

/**
 * Start a UART OTA session.
 * Called when the scanner receives an "ota_begin" JSON command.
 *
 * @param total_size    Expected firmware size in bytes
 * @param expected_crc  Expected CRC32 of the complete firmware image (0 if unknown)
 * @param has_crc       true if uplink provided a CRC32 for verification
 * @param uart_num      UART port to use for communication
 * @return true if OTA partition is ready
 */
bool uart_ota_begin(uint32_t total_size, uint32_t expected_crc,
                    bool has_crc, uart_port_t uart_num,
                    const char *session_id,
                    const uart_ota_manifest_t *manifest);

/**
 * Process incoming UART data during an active OTA session.
 * Handles binary chunks with the [0xF0] header format.
 *
 * @param data    Raw UART bytes
 * @param len     Number of bytes
 * @return true if OTA is still in progress, false if done or error
 */
bool uart_ota_process_data(const uint8_t *data, int len);

/**
 * True only while the receiver expects binary chunk frames. Once the full
 * image has been verified, this becomes false while uart_ota_is_active()
 * remains true so the command listener can parse the manifest-bound ota_end.
 */
bool uart_ota_is_receiving_binary(void);

/**
 * Finalize OTA: validate, set boot partition, reboot.
 * Called when "ota_end" is received.
 *
 * @return true if OTA finalized successfully (will reboot)
 */
bool uart_ota_finalize(void);

/** Abort an in-progress OTA session. */
void uart_ota_abort(void);

/** Check if a UART OTA session is active. */
bool uart_ota_is_active(void);

/** Lock-free read-only guard for non-owner tasks; never runs watchdog cleanup. */
bool uart_ota_is_active_snapshot(void);

/** Human-readable OTA state for status/debug telemetry. */
const char *uart_ota_state_label(void);

/** Active OTA relay session id, or an empty string when idle/no id. */
const char *uart_ota_session_id(void);

/** True only when every immutable manifest field matches the active session. */
bool uart_ota_manifest_matches_active(const uart_ota_manifest_t *manifest);

/** Current staged byte count for status/debug telemetry. */
uint32_t uart_ota_received(void);

/** Expected OTA image size for status/debug telemetry. */
uint32_t uart_ota_total_size(void);

/**
 * Safe recovery mode can keep radios uninitialized. Disable scan halt/resume
 * hooks so UART OTA remains available in that mode.
 */
void uart_ota_set_radio_control_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
