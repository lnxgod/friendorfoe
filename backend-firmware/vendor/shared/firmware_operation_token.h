#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_OPERATION_OWNER_NONE = 0,
    FW_OPERATION_OWNER_SCANNER_STAGING,
    FW_OPERATION_OWNER_SCANNER_RELAY,
    FW_OPERATION_OWNER_UPLINK_OTA,
    FW_OPERATION_OWNER_RUNTIME_STARTUP,
} fw_operation_owner_t;

typedef struct {
    fw_operation_owner_t owner;
    uint32_t generation;
    bool valid;
} fw_operation_token_t;

/** Lock-protected firmware-operation state. Callers provide synchronization. */
typedef struct {
    bool active;
    fw_operation_owner_t owner;
    uint32_t generation;
    uint32_t operation_epoch;
    bool uart_lease;
    bool recovery_restart_reserved;
    bool radio_inhibited;
    bool preemption_requested;
} fw_operation_state_t;

typedef struct {
    bool active;
    fw_operation_owner_t owner;
    uint32_t operation_epoch;
    bool radio_inhibited;
    bool preemption_requested;
    bool recovery_restart_reserved;
} fw_operation_snapshot_t;

void fw_operation_state_init(fw_operation_state_t *state);
bool fw_operation_state_try_begin(fw_operation_state_t *state,
                                  fw_operation_owner_t owner,
                                  fw_operation_token_t *out_token);
bool fw_operation_state_try_begin_quiesced(
    fw_operation_state_t *state,
    fw_operation_owner_t owner,
    uint32_t acknowledged_inhibit_epoch,
    fw_operation_token_t *out_token);
bool fw_operation_state_attach_uart_lease(
    fw_operation_state_t *state, fw_operation_token_t token);
bool fw_operation_state_end(fw_operation_state_t *state,
                            fw_operation_token_t token,
                            bool *release_uart_lease);
bool fw_operation_state_try_reserve_recovery_restart(
    fw_operation_state_t *state);
void fw_operation_state_snapshot(const fw_operation_state_t *state,
                                 fw_operation_snapshot_t *out);
bool fw_operation_state_request_radio_inhibit(
    fw_operation_state_t *state);
bool fw_operation_state_request_preemption(
    fw_operation_state_t *state);
bool fw_operation_state_clear_radio_inhibit(
    fw_operation_state_t *state);

#ifdef __cplusplus
}
#endif
