#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BADGE_UPDATE_OTA_BEGIN_ADMIT = 0,
    BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH,
    BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION,
} badge_update_ota_begin_admission_t;

badge_update_ota_begin_admission_t
badge_update_uplink_ota_begin_admission_decide(
    bool canary_build,
    bool maintenance_active,
    size_t member_count,
    bool session_present,
    bool session_is_string,
    bool session_matches);
badge_update_ota_begin_admission_t
badge_update_scanner_stage_begin_admission_decide(
    bool canary_build,
    bool maintenance_active,
    size_t member_count,
    bool session_present,
    bool session_is_string,
    bool session_matches);

#ifdef __cplusplus
}
#endif
