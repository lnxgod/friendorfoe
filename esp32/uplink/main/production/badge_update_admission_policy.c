#include "badge_update_admission_policy.h"

badge_update_ota_begin_admission_t
badge_update_uplink_ota_begin_admission_decide(
    bool canary_build,
    bool maintenance_active,
    size_t member_count,
    bool session_present,
    bool session_is_string,
    bool session_matches)
{
    if (!canary_build || !maintenance_active) {
        return member_count == 10U && !session_present
            ? BADGE_UPDATE_OTA_BEGIN_ADMIT
            : BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION;
    }
    return member_count == 11U &&
           session_present &&
           session_is_string &&
           session_matches
        ? BADGE_UPDATE_OTA_BEGIN_ADMIT
        : BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH;
}

badge_update_ota_begin_admission_t
badge_update_scanner_stage_begin_admission_decide(
    bool canary_build,
    bool maintenance_active,
    size_t member_count,
    bool session_present,
    bool session_is_string,
    bool session_matches)
{
    if (!canary_build || !maintenance_active) {
        return member_count == 11U && !session_present
            ? BADGE_UPDATE_OTA_BEGIN_ADMIT
            : BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION;
    }
    return member_count == 12U &&
           session_present &&
           session_is_string &&
           session_matches
        ? BADGE_UPDATE_OTA_BEGIN_ADMIT
        : BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH;
}
