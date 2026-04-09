#pragma once

/**
 * Friend or Foe — Scanner Firmware Store
 *
 * Stores scanner firmware on the uplink's fw_store flash partition.
 * The backend uploads firmware in chunks, then triggers a UART relay
 * to flash the scanner at the uplink's own pace — no HTTP timeout pressure.
 *
 * Flow:
 *   1. POST /api/fw/upload   — receive firmware, store to flash
 *   2. POST /api/fw/relay    — read from flash, send to scanner UART
 *   3. GET  /api/fw/info     — check stored firmware status
 */

#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register firmware store HTTP endpoints on the given server. */
void fw_store_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
