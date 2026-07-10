#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "badge_ble_investigation_state.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_BLE_INVESTIGATION_CHUNK_JSON_MAX UART_JSON_MAX_SIZE

void badge_ble_investigation_init(void);
bool badge_ble_investigation_start(const char *request_id,
                                   const char *mode,
                                   const char *target_mac,
                                   const char *transport,
                                   char *err,
                                   size_t err_len);
bool badge_ble_investigation_start_local(const char *request_id,
                                         const char *mode,
                                         const char *target_mac,
                                         char *err,
                                         size_t err_len);
bool badge_ble_investigation_accept_scanner_json(const cJSON *root);
void badge_ble_investigation_get(ble_investigation_result_t *out);
size_t badge_ble_investigation_status_json(char *out, size_t out_len);
bool badge_ble_investigation_chunk_available(const char *request_id, int seq);
size_t badge_ble_investigation_chunk_json(const char *request_id,
                                          int seq,
                                          char *out,
                                          size_t out_len);

#ifdef __cplusplus
}
#endif
