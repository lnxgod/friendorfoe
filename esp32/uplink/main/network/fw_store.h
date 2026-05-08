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
#include "esp_partition.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register firmware store HTTP endpoints on the given server. */
void fw_store_register(httpd_handle_t server);

/** True if a firmware upload or relay operation is currently active.
 *  Used by the watchdog to skip upload-age reboot during relay. */
bool fw_store_is_relay_active(void);

typedef struct {
    bool     stored;
    uint32_t size;
    uint32_t checksum;
    char     version[32];
    char     name[32];
    char     partition[16];
} fw_store_info_t;

/** Read staged scanner firmware metadata from the uplink store. */
bool fw_store_get_info(fw_store_info_t *out);

/**
 * Relay the staged scanner firmware to a scanner UART without HTTP.
 * scanner_id: 0 = BLE scanner slot, 1 = Wi-Fi scanner slot.
 * Writes a compact JSON result into out_json when provided.
 */
bool fw_store_relay_staged_to_scanner(int scanner_id,
                                      char *out_json,
                                      size_t out_json_len);
bool fw_store_relay_staged_to_scanner_ex(int scanner_id,
                                         bool force_probe_skip,
                                         bool allow_same_version,
                                         char *out_json,
                                         size_t out_json_len);

/**
 * Pick the partition where staged scanner firmware is written. Returns the
 * same partition the /api/fw/upload handler uses. NULL if nothing suitable
 * (catastrophic on a sane partition table).
 *
 * Public so fw_auto_check (which downloads scanner firmware via HTTP from
 * the backend) can stage to the same place /api/fw/upload would.
 */
const esp_partition_t *fw_store_get_target_partition(void);

/**
 * Persist scanner firmware metadata to NVS so subsequent fw_store_get_info()
 * / fw_check / fw_offer flows see it.
 *
 * Caller has already written `size` bytes via esp_ota_write to `partition`
 * and called esp_ota_abort (intentionally NOT esp_ota_end — see the comment
 * in fw_upload_handler about why we never make scanner firmware bootable
 * for the uplink itself).
 */
void fw_store_persist_metadata(const char *name, const char *version,
                               const esp_partition_t *partition,
                               uint32_t size, uint32_t crc32);

/**
 * USB serial staging path for badge builds. The caller sends exactly `size`
 * binary bytes after begin; the store computes CRC while writing and persists
 * metadata only when the complete image matches expected_crc32.
 */
bool fw_store_serial_upload_begin(const char *name,
                                  const char *version,
                                  uint32_t size,
                                  uint32_t expected_crc32,
                                  char *out_json,
                                  size_t out_json_len);
bool fw_store_serial_upload_write(const uint8_t *data,
                                  size_t len,
                                  char *out_json,
                                  size_t out_json_len);
bool fw_store_serial_upload_end(char *out_json, size_t out_json_len);
void fw_store_serial_upload_abort(const char *reason);
bool fw_store_serial_upload_active(void);
uint32_t fw_store_serial_upload_remaining(void);

/** Handle scanner-originated firmware negotiation messages. */
void fw_store_handle_scanner_check(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version);
void fw_store_handle_scanner_ready(int scanner_id,
                                   const char *scanner_board,
                                   const char *scanner_version);

#ifdef __cplusplus
}
#endif
