#include "calibration_mode.h"

#include "detection_policy.h"
#include "wifi_scanner.h"
#include "ble_remote_id.h"
#include "uart_tx.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "scanner_cal_mode";

static atomic_bool s_active = false;
static char s_session_id[24] = {0};
static char s_uuid[48] = {0};
static char s_scan_profile[24] = "hybrid_failover";
static bool s_ble_radio_enabled = true;
static atomic_bool s_quiet_mode = false;
static atomic_bool s_wake_pending = false;
static atomic_uint_least32_t s_quiet_generation = 0;
static atomic_bool s_quiet_restore_tx_enabled = true;
typedef enum {
    SCANNER_QUIET_ERROR_NONE = 0,
    SCANNER_QUIET_ERROR_TRANSITION_PENDING,
    SCANNER_QUIET_ERROR_LOCK_UNAVAILABLE,
} scanner_quiet_error_t;
static atomic_int s_quiet_last_error = SCANNER_QUIET_ERROR_NONE;
static StaticSemaphore_t s_transition_mutex_storage;
static SemaphoreHandle_t s_transition_mutex;

void scanner_calibration_mode_init(void)
{
    if (!s_transition_mutex) {
        s_transition_mutex = xSemaphoreCreateMutexStatic(
            &s_transition_mutex_storage);
    }
}

static bool scanner_transition_lock(void)
{
    scanner_calibration_mode_init();
    return s_transition_mutex &&
           xSemaphoreTake(s_transition_mutex, portMAX_DELAY) == pdTRUE;
}

static void scanner_transition_unlock(void)
{
    if (s_transition_mutex) {
        (void)xSemaphoreGive(s_transition_mutex);
    }
}

static bool profile_is_ble_primary(void)
{
    return strcmp(s_scan_profile, "ble_primary") == 0;
}

static bool profile_is_wifi_primary(void)
{
    return strcmp(s_scan_profile, "wifi_primary") == 0;
}

static void ensure_ble_radio_enabled(bool enabled)
{
    if (enabled && s_ble_radio_enabled && !ble_remote_id_is_scanning()) {
        ble_remote_id_start();
        return;
    }
    if (s_ble_radio_enabled == enabled &&
        (enabled || ble_remote_id_is_quiesced())) {
        return;
    }
    if (enabled) {
        ble_remote_id_start();
    } else {
        ble_rid_lockon_cancel();
        ble_remote_id_stop();
    }
    s_ble_radio_enabled = enabled;
}

static void apply_quiet_radios(void)
{
    wifi_scanner_lockon_cancel();
    wifi_scanner_pause();
    ble_rid_lockon_cancel();
    ensure_ble_radio_enabled(false);
}

static void apply_normal_profile_radios_locked(void)
{
    if (atomic_load_explicit(&s_quiet_mode, memory_order_acquire)) {
        apply_quiet_radios();
        return;
    }
    if (atomic_load_explicit(&s_active, memory_order_acquire)) {
        return;
    }
    if (profile_is_ble_primary()) {
        wifi_scanner_pause();
        ensure_ble_radio_enabled(true);
    } else if (profile_is_wifi_primary()) {
        ensure_ble_radio_enabled(false);
        wifi_scanner_resume();
    } else {
        ensure_ble_radio_enabled(true);
        wifi_scanner_resume();
    }
}

static bool normal_profile_radios_ready_locked(void)
{
    if (atomic_load_explicit(&s_active, memory_order_acquire) ||
        profile_is_ble_primary()) {
        return wifi_scanner_is_quiesced() && ble_remote_id_is_active();
    }
    if (profile_is_wifi_primary()) {
        return wifi_scanner_is_active() && ble_remote_id_is_quiesced();
    }
    return wifi_scanner_is_active() && ble_remote_id_is_active();
}

void scanner_scan_profile_apply(void)
{
    if (!scanner_transition_lock()) {
        ESP_LOGE(TAG, "Scan profile apply skipped: transition lock unavailable");
        return;
    }
    apply_normal_profile_radios_locked();
    scanner_transition_unlock();
}

bool scanner_quiet_mode_set(bool enabled, uint32_t generation)
{
    if (!scanner_transition_lock()) {
        atomic_store_explicit(&s_quiet_last_error,
                              SCANNER_QUIET_ERROR_LOCK_UNAVAILABLE,
                              memory_order_release);
        ESP_LOGE(TAG, "Scanner quiet transition lock unavailable");
        return false;
    }
    atomic_store_explicit(&s_quiet_last_error, SCANNER_QUIET_ERROR_NONE,
                          memory_order_release);
    atomic_store_explicit(&s_quiet_generation, generation,
                          memory_order_release);

    bool result = false;
    if (enabled) {
        if (!atomic_load_explicit(&s_quiet_mode, memory_order_acquire)) {
            bool restore_tx_enabled = uart_tx_is_enabled();

            /* Publish the authoritative guard before touching TX or radios. */
            atomic_store_explicit(&s_quiet_mode, true, memory_order_release);
            atomic_store_explicit(&s_quiet_restore_tx_enabled,
                                  restore_tx_enabled,
                                  memory_order_release);

            /* A calibration session cannot remain meaningful with both radios
             * intentionally stopped.  Keep the configured normal profile so
             * it can be restored when quiet mode exits. */
            atomic_store_explicit(&s_active, false, memory_order_release);
            s_session_id[0] = '\0';
            s_uuid[0] = '\0';
        }
        atomic_store_explicit(&s_wake_pending, false, memory_order_release);

        uart_tx_set_enabled(false);
        uart_tx_flush_detection_queue();
        apply_quiet_radios();

        bool radios_quiesced = wifi_scanner_is_quiesced() &&
                               ble_remote_id_is_quiesced();
        if (radios_quiesced) {
            /* Producers are now physically idle. Flush again so nothing that
             * completed after the initial TX fence survives the quiet edge. */
            uart_tx_flush_detection_queue();
        }
        result = !uart_tx_is_enabled() && radios_quiesced;
        if (!result) {
            atomic_store_explicit(&s_quiet_last_error,
                                  SCANNER_QUIET_ERROR_TRANSITION_PENDING,
                                  memory_order_release);
        }
        scanner_transition_unlock();
        return result;
    }

    atomic_store_explicit(&s_quiet_mode, false, memory_order_release);
    atomic_store_explicit(&s_wake_pending, true, memory_order_release);
    apply_normal_profile_radios_locked();
    /* TX remained fenced while radios resumed. Drop any stale quiet-period
     * work immediately before restoring the saved TX state. */
    uart_tx_flush_detection_queue();
    bool restore_tx_enabled = atomic_load_explicit(
        &s_quiet_restore_tx_enabled, memory_order_acquire);
    if (uart_tx_is_enabled() != restore_tx_enabled) {
        uart_tx_set_enabled(restore_tx_enabled);
    }
    bool radios_ready = normal_profile_radios_ready_locked();
    bool tx_restored = uart_tx_is_enabled() == restore_tx_enabled;
    result = radios_ready && tx_restored;
    atomic_store_explicit(&s_wake_pending, !result, memory_order_release);
    if (!result) {
        atomic_store_explicit(&s_quiet_last_error,
                              SCANNER_QUIET_ERROR_TRANSITION_PENDING,
                              memory_order_release);
    }
    scanner_transition_unlock();
    return result;
}

bool scanner_quiet_mode_is_active(void)
{
    return atomic_load_explicit(&s_quiet_mode, memory_order_acquire);
}

uint32_t scanner_quiet_mode_generation(void)
{
    return atomic_load_explicit(&s_quiet_generation, memory_order_acquire);
}

bool scanner_quiet_mode_radios_ready(void)
{
    if (!scanner_transition_lock()) {
        return false;
    }
    bool ready;
    if (atomic_load_explicit(&s_quiet_mode, memory_order_acquire)) {
        ready = wifi_scanner_is_quiesced() && ble_remote_id_is_quiesced();
    } else {
        ready = normal_profile_radios_ready_locked();
    }
    scanner_transition_unlock();
    return ready;
}

bool scanner_quiet_mode_tx_restored(void)
{
    if (atomic_load_explicit(&s_quiet_mode, memory_order_acquire)) {
        return false;
    }
    bool restore_tx_enabled = atomic_load_explicit(
        &s_quiet_restore_tx_enabled, memory_order_acquire);
    return uart_tx_is_enabled() == restore_tx_enabled;
}

const char *scanner_quiet_mode_last_error(void)
{
    switch ((scanner_quiet_error_t)atomic_load_explicit(
                &s_quiet_last_error, memory_order_acquire)) {
    case SCANNER_QUIET_ERROR_TRANSITION_PENDING:
        return "transition_pending";
    case SCANNER_QUIET_ERROR_LOCK_UNAVAILABLE:
        return "transition_lock_unavailable";
    default:
        return "";
    }
}

bool scanner_calibration_mode_start(const char *session_id,
                                    const char *advertise_uuid)
{
    if (!session_id || session_id[0] == '\0' ||
        !advertise_uuid || advertise_uuid[0] == '\0') {
        return false;
    }
    if (!scanner_transition_lock()) {
        return false;
    }
    if (atomic_load_explicit(&s_quiet_mode, memory_order_acquire)) {
        ESP_LOGW(TAG, "Calibration mode rejected while scanner quiet is active");
        scanner_transition_unlock();
        return false;
    }

    strncpy(s_session_id, session_id, sizeof(s_session_id) - 1);
    s_session_id[sizeof(s_session_id) - 1] = '\0';
    strncpy(s_uuid, advertise_uuid, sizeof(s_uuid) - 1);
    s_uuid[sizeof(s_uuid) - 1] = '\0';
    atomic_store_explicit(&s_active, true, memory_order_release);

    wifi_scanner_lockon_cancel();
    wifi_scanner_pause();
    ble_rid_lockon_cancel();
    ensure_ble_radio_enabled(true);
    uart_tx_flush_detection_queue();
    ESP_LOGW(TAG, "Calibration mode ACTIVE: session=%s uuid=%s", s_session_id, s_uuid);
    scanner_transition_unlock();
    return true;
}

void scanner_calibration_mode_stop(const char *reason)
{
    if (!scanner_transition_lock()) {
        return;
    }
    if (!atomic_load_explicit(&s_active, memory_order_acquire) &&
        s_uuid[0] == '\0' && s_session_id[0] == '\0') {
        scanner_transition_unlock();
        return;
    }

    atomic_store_explicit(&s_active, false, memory_order_release);
    s_session_id[0] = '\0';
    s_uuid[0] = '\0';
    apply_normal_profile_radios_locked();
    ESP_LOGW(TAG, "Calibration mode STOPPED: %s", reason ? reason : "unspecified");
    scanner_transition_unlock();
}

bool scanner_calibration_mode_is_active(void)
{
    return atomic_load_explicit(&s_active, memory_order_acquire);
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
    return atomic_load_explicit(&s_active, memory_order_acquire)
        ? "calibration" : "normal";
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
    if (!scanner_transition_lock()) {
        return;
    }
    bool changed = strcmp(s_scan_profile, profile) != 0;
    strncpy(s_scan_profile, profile, sizeof(s_scan_profile) - 1);
    s_scan_profile[sizeof(s_scan_profile) - 1] = '\0';
    if (changed) {
        uart_tx_reset_counts();
        ble_remote_id_reset_profile_counters();
        wifi_scanner_reset_attack_counters();
        wifi_scanner_reset_fc_histogram();
    }
    apply_normal_profile_radios_locked();
    ESP_LOGI(TAG, "Scan profile set: %s", s_scan_profile);
    scanner_transition_unlock();
}

const char *scanner_scan_profile_label(void)
{
    return atomic_load_explicit(&s_active, memory_order_acquire)
        ? "calibration" : s_scan_profile;
}

bool scanner_calibration_mode_allows_ble_uuid128(const uint8_t uuids[][16],
                                                 uint8_t count)
{
    if (!atomic_load_explicit(&s_active, memory_order_acquire)) {
        return true;
    }
    return fof_policy_ble_has_exact_uuid128_le(uuids, count, s_uuid);
}

bool scanner_calibration_mode_allows_detection(const drone_detection_t *detection)
{
    if (!detection) {
        return false;
    }
    if (!atomic_load_explicit(&s_active, memory_order_acquire)) {
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
