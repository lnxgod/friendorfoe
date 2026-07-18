#include "firmware_auto_policy.h"

#include "firmware_legacy_ready.h"

#include <stddef.h>
#include <string.h>

static bool hardware_id_is_canonical(const char *hardware_id)
{
    if (!hardware_id || strlen(hardware_id) != 17U) {
        return false;
    }
    for (size_t i = 0; i < 17U; ++i) {
        if ((i + 1U) % 3U == 0U) {
            if (hardware_id[i] != ':') {
                return false;
            }
            continue;
        }
        char ch = hardware_id[i];
        bool hex = (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f') ||
                   (ch >= 'A' && ch <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

static bool timestamp_is_recent(int64_t timestamp_ms,
                                int64_t now_ms,
                                int64_t max_age_ms)
{
    return timestamp_ms > 0 && max_age_ms > 0 && now_ms >= timestamp_ms &&
           now_ms - timestamp_ms <= max_age_ms;
}

bool fof_auto_identity_is_fresh(const fof_auto_identity_view_t *identity,
                                uint32_t generation_floor,
                                int64_t now_ms,
                                int64_t max_age_ms)
{
    return identity && identity->complete &&
           identity->identity_generation > generation_floor &&
           timestamp_is_recent(identity->received_ms, now_ms, max_age_ms);
}

bool fof_auto_offer_binding_matches(
    const fof_auto_offer_binding_t *binding,
    uint32_t generation,
    uint32_t manifest_crc32,
    uint8_t slot,
    uint32_t identity_generation,
    const char *hardware_id,
    int64_t now_ms,
    int64_t max_age_ms)
{
    return binding && binding->generation != 0 &&
           binding->generation == generation &&
           binding->manifest_crc32 == manifest_crc32 &&
           binding->slot == slot &&
           binding->identity_generation == identity_generation &&
           hardware_id_is_canonical(binding->hardware_id) &&
           hardware_id_is_canonical(hardware_id) &&
           strcmp(binding->hardware_id, hardware_id) == 0 &&
           timestamp_is_recent(binding->captured_ms, now_ms, max_age_ms);
}

bool fof_auto_queue_state_allows(fof_auto_slot_state_t state)
{
    return state == FOF_AUTO_SLOT_OFFERED;
}

bool fof_auto_wifi_gate_open(bool ble_requested,
                             fof_auto_slot_state_t ble_state)
{
    return !ble_requested || ble_state == FOF_AUTO_SLOT_CONVERGED ||
           ble_state == FOF_AUTO_SLOT_CURRENT;
}

fof_auto_probe_decision_t fof_auto_recovery_probe_decide(
    int64_t now_ms,
    int64_t not_before_ms,
    uint8_t probes_used,
    uint8_t max_probes)
{
    if (not_before_ms <= 0 || now_ms < not_before_ms) {
        return FOF_AUTO_PROBE_WAIT;
    }
    if (max_probes == 0 || probes_used >= max_probes) {
        return FOF_AUTO_PROBE_EXHAUSTED;
    }
    return FOF_AUTO_PROBE_SEND;
}

fof_auto_recovery_decision_t fof_auto_recovery_decide(
    const fof_auto_recovery_view_t *recovery)
{
    if (!recovery) {
        return FOF_AUTO_RECOVERY_REFUSED;
    }
    bool base_proof = recovery->manual_probe && recovery->identity_fresh &&
        recovery->same_hardware_id && recovery->target_contract_matches &&
        recovery->rollback_clear && recovery->recovery_normal &&
        recovery->command_healthy;
    if (!base_proof) {
        return FOF_AUTO_RECOVERY_HOLD;
    }

    if (recovery->version_relation == FOF_VERSION_EQUAL) {
        return recovery->profile_healthy && recovery->radio_healthy
            ? FOF_AUTO_RECOVERY_CONVERGED
            : FOF_AUTO_RECOVERY_HOLD;
    }
    if (recovery->version_relation == FOF_VERSION_NEWER) {
        return recovery->source_version &&
               strcmp(recovery->source_version,
                      FOF_LEGACY_READY_BOOTSTRAP_VERSION) == 0
            ? FOF_AUTO_RECOVERY_REOFFER
            : FOF_AUTO_RECOVERY_REFUSED;
    }
    return FOF_AUTO_RECOVERY_REFUSED;
}
