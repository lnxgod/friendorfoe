#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_CON_RADIO_SCANNER_LANES 2
#define BADGE_CON_SELF_ACK_RETRY_MS 5000U
#define BADGE_CON_RADIO_INTERNAL_HEAP_MIN 24576U
#define BADGE_CON_RADIO_INTERNAL_BLOCK_MIN 16384U
#define BADGE_CON_RADIO_PSRAM_TOTAL_MIN 8388608U
#define BADGE_CON_RADIO_PSRAM_FREE_MIN 5242880U

typedef enum {
    BADGE_CON_RADIO_MEMORY_OK = 0,
    BADGE_CON_RADIO_MEMORY_PSRAM,
    BADGE_CON_RADIO_MEMORY_INTERNAL,
} badge_con_radio_memory_gate_t;

typedef struct {
    uint32_t scanner_boot_id[BADGE_CON_RADIO_SCANNER_LANES];
    uint32_t self_ack_deadline_ms;
    bool self_sent[BADGE_CON_RADIO_SCANNER_LANES];
    bool self_ack_waiting;
} badge_con_radio_runtime_policy_t;

bool badge_con_radio_runtime_controller_init_allowed(
    bool firmware_operation_allows_radio,
    bool identity_valid,
    bool game_snapshot_valid,
    bool game_active,
    bool ota_pending_verify);
badge_con_radio_memory_gate_t badge_con_radio_runtime_memory_gate(
    uint32_t internal_free,
    uint32_t internal_largest,
    bool psram_initialized,
    uint32_t psram_total,
    uint32_t psram_free);
void badge_con_radio_runtime_policy_init(
    badge_con_radio_runtime_policy_t *policy);
bool badge_con_radio_runtime_observe_boot_id(
    badge_con_radio_runtime_policy_t *policy,
    int scanner_lane,
    uint32_t boot_id);
void badge_con_radio_runtime_clear_self_delivery(
    badge_con_radio_runtime_policy_t *policy);
int badge_con_radio_runtime_next_unsent_lane(
    const badge_con_radio_runtime_policy_t *policy);
void badge_con_radio_runtime_note_self_sent(
    badge_con_radio_runtime_policy_t *policy,
    int scanner_lane,
    uint32_t now_ms);
bool badge_con_radio_runtime_all_self_sent(
    const badge_con_radio_runtime_policy_t *policy);
bool badge_con_radio_runtime_retry_self_due(
    badge_con_radio_runtime_policy_t *policy,
    bool exact_self_ack_matches,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif
