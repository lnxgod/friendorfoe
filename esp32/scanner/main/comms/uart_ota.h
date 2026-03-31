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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start a UART OTA session.
 * Called when the scanner receives an "ota_begin" JSON command.
 *
 * @param total_size  Expected firmware size in bytes
 * @param uart_num    UART port to use for communication
 * @return true if OTA partition is ready
 */
bool uart_ota_begin(uint32_t total_size, uart_port_t uart_num);

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

#ifdef __cplusplus
}
#endif
