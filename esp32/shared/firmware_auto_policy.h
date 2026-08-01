#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "firmware_version_order.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOF_AUTO_SLOT_EXCLUDED = 0,
    FOF_AUTO_SLOT_AWAITING_CHECK = 1,
    FOF_AUTO_SLOT_OFFERED = 2,
    FOF_AUTO_SLOT_READY_QUEUED = 3,
    FOF_AUTO_SLOT_RELAYING = 4,
    FOF_AUTO_SLOT_CONVERGED = 5,
    FOF_AUTO_SLOT_CURRENT = 6,
    FOF_AUTO_SLOT_REFUSED = 7,
    FOF_AUTO_SLOT_FAILED = 8,
    FOF_AUTO_SLOT_NEWER_SKIPPED = 9,
    FOF_AUTO_SLOT_RECOVERING = 10,
} fof_auto_slot_state_t;

typedef struct {
    bool complete;
    uint32_t identity_generation;
    int64_t received_ms;
} fof_auto_identity_view_t;

typedef struct {
    uint32_t generation;
    uint32_t manifest_crc32;
    uint8_t slot;
    uint32_t identity_generation;
    const char *hardware_id;
    int64_t captured_ms;
} fof_auto_offer_binding_t;

typedef enum {
    FOF_AUTO_PROBE_WAIT = 0,
    FOF_AUTO_PROBE_SEND = 1,
    FOF_AUTO_PROBE_EXHAUSTED = 2,
} fof_auto_probe_decision_t;

typedef struct {
    bool manual_probe;
    bool identity_fresh;
    bool same_hardware_id;
    bool target_contract_matches;
    bool rollback_clear;
    bool recovery_normal;
    bool command_healthy;
    bool profile_healthy;
    bool radio_healthy;
    fof_firmware_version_relation_t version_relation;
    const char *source_version;
} fof_auto_recovery_view_t;

typedef enum {
    FOF_AUTO_RECOVERY_HOLD = 0,
    FOF_AUTO_RECOVERY_CONVERGED = 1,
    FOF_AUTO_RECOVERY_REOFFER = 2,
    FOF_AUTO_RECOVERY_REFUSED = 3,
} fof_auto_recovery_decision_t;

bool fof_auto_identity_is_fresh(const fof_auto_identity_view_t *identity,
                                uint32_t generation_floor,
                                int64_t now_ms,
                                int64_t max_age_ms);

bool fof_auto_offer_binding_matches(
    const fof_auto_offer_binding_t *binding,
    uint32_t generation,
    uint32_t manifest_crc32,
    uint8_t slot,
    uint32_t identity_generation,
    const char *hardware_id,
    int64_t now_ms,
    int64_t max_age_ms);

bool fof_auto_queue_state_allows(fof_auto_slot_state_t state);

bool fof_auto_slot_is_terminal(fof_auto_slot_state_t state);

bool fof_auto_wifi_gate_open(bool ble_requested,
                             fof_auto_slot_state_t ble_state);

fof_auto_probe_decision_t fof_auto_readiness_probe_decide(
    bool identity_fresh,
    uint8_t probes_used,
    uint8_t max_probes);

/**
 * Decide whether a gate-open scanner identity-acquisition window should wait,
 * proceed, or fail terminally. The durable wait marker is separate from the
 * readiness-probe budget. A fresh identity always wins at the deadline.
 */
fof_auto_probe_decision_t fof_auto_identity_acquisition_decide(
    bool identity_fresh,
    bool wait_started,
    int64_t now_ms,
    int64_t deadline_ms);

bool fof_auto_terminal_reopen_allowed(
    fof_auto_slot_state_t state,
    bool identity_exhausted,
    uint8_t attempts_used,
    uint8_t max_attempts);

fof_auto_probe_decision_t fof_auto_recovery_probe_decide(
    int64_t now_ms,
    int64_t not_before_ms,
    uint8_t probes_used,
    uint8_t max_probes);

fof_auto_recovery_decision_t fof_auto_recovery_decide(
    const fof_auto_recovery_view_t *recovery);

#ifdef __cplusplus
}
#endif
