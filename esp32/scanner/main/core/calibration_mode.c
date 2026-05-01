#include "calibration_mode.h"

#include "detection_policy.h"
#include "wifi_scanner.h"
#include "ble_remote_id.h"
#include "uart_tx.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "scanner_cal_mode";

static bool s_active = false;
static char s_session_id[24] = {0};
static char s_uuid[48] = {0};
static char s_scan_profile[24] = "hybrid_failover";

static bool profile_is_ble_primary(void)
{
    return strcmp(s_scan_profile, "ble_primary") == 0;
}

static void apply_normal_profile_radios(void)
{
    if (s_active) {
        return;
    }
    if (profile_is_ble_primary()) {
        wifi_scanner_pause();
    } else {
        wifi_scanner_resume();
    }
}

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
    ble_rid_lockon_cancel();
    uart_tx_flush_detection_queue();
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
    apply_normal_profile_radios();
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

void scanner_scan_profile_set(const char *profile)
{
    if (!profile || profile[0] == '\0') {
        profile = "hybrid_failover";
    }
    if (strcmp(profile, "ble_primary") != 0 &&
        strcmp(profile, "wifi_primary") != 0 &&
        strcmp(profile, "hybrid_failover") != 0) {
        profile = "hybrid_failover";
    }
    strncpy(s_scan_profile, profile, sizeof(s_scan_profile) - 1);
    s_scan_profile[sizeof(s_scan_profile) - 1] = '\0';
    apply_normal_profile_radios();
    ESP_LOGI(TAG, "Scan profile set: %s", s_scan_profile);
}

const char *scanner_scan_profile_label(void)
{
    return s_active ? "calibration" : s_scan_profile;
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
    if (!detection) {
        return false;
    }
    if (!s_active) {
        return fof_policy_scan_profile_allows_source(s_scan_profile,
                                                     detection->source);
    }
    if (detection->source != DETECTION_SRC_BLE_FINGERPRINT) {
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
