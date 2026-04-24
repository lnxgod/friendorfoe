#include "calibration_mode.h"

#include "detection_policy.h"
#include "wifi_scanner.h"

#ifndef WIFI_SCANNER_ONLY
#include "ble_remote_id.h"
#endif

#include "esp_log.h"

#include <string.h>

static const char *TAG = "scanner_cal_mode";

static bool s_active = false;
static char s_session_id[24] = {0};
static char s_uuid[48] = {0};

bool scanner_calibration_mode_start(const char *session_id,
                                    const char *advertise_uuid)
{
    if (!session_id || session_id[0] == '\0' ||
        !advertise_uuid || advertise_uuid[0] == '\0') {
        return false;
    }

    strncpy(s_session_id, session_id, sizeof(s_session_id) - 1);
    s_session_id[sizeof(s_session_id) - 1] = '\0';
    strncpy(s_uuid, advertise_uuid, sizeof(s_uuid) - 1);
    s_uuid[sizeof(s_uuid) - 1] = '\0';
    s_active = true;

    wifi_scanner_lockon_cancel();
    wifi_scanner_pause();
#ifndef WIFI_SCANNER_ONLY
    ble_rid_lockon_cancel();
#endif
    ESP_LOGW(TAG, "Calibration mode ACTIVE: session=%s uuid=%s", s_session_id, s_uuid);
    return true;
}

void scanner_calibration_mode_stop(const char *reason)
{
    if (!s_active && s_uuid[0] == '\0' && s_session_id[0] == '\0') {
        return;
    }

    s_active = false;
    s_session_id[0] = '\0';
    s_uuid[0] = '\0';
    wifi_scanner_resume();
    ESP_LOGW(TAG, "Calibration mode STOPPED: %s", reason ? reason : "unspecified");
}

bool scanner_calibration_mode_is_active(void)
{
    return s_active;
}

const char *scanner_calibration_mode_uuid(void)
{
    return s_uuid;
}

const char *scanner_calibration_mode_session_id(void)
{
    return s_session_id;
}

const char *scanner_calibration_mode_label(void)
{
    return s_active ? "calibration" : "normal";
}

bool scanner_calibration_mode_allows_ble_uuid128(const uint8_t uuids[][16],
                                                 uint8_t count)
{
    if (!s_active) {
        return true;
    }
    return fof_policy_ble_has_exact_uuid128_le(uuids, count, s_uuid);
}

bool scanner_calibration_mode_allows_detection(const drone_detection_t *detection)
{
    if (!s_active) {
        return true;
    }
    if (!detection || detection->source != DETECTION_SRC_BLE_FINGERPRINT) {
        return false;
    }
    if (fof_policy_ble_svc_raw_contains_uuid(detection->ble_svc_uuids_raw, s_uuid)) {
        return true;
    }
    return fof_policy_ble_has_exact_uuid128_le(
        detection->ble_service_uuids_128,
        detection->ble_svc_uuid_128_count,
        s_uuid
    );
}
