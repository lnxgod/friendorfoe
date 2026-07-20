#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "calibration_mode.h"
#include "nvs.h"

static bool g_tx_enabled;
static bool g_wifi_paused;
static bool g_wifi_quiesced;
static bool g_wifi_active;
static bool g_wifi_resume_converges = true;
static bool g_ble_scanning;
static bool g_ble_quiesced;
static bool g_ble_active;
static bool g_ble_start_converges = true;
static bool g_ble_stop_converges = true;
static int g_order_violations;
static int g_tx_disable_calls;
static int g_tx_enable_calls;
static int g_queue_flush_calls;
static int g_flush_calls_at_tx_enable;
static bool g_last_flush_radios_quiesced;
static int g_wifi_pause_calls;
static int g_wifi_resume_calls;
static int g_wifi_lockon_cancel_calls;
static int g_ble_start_calls;
static int g_ble_stop_calls;
static int g_ble_lockon_cancel_calls;
static int g_nvs_commit_calls;
static bool g_nvs_role_persisted;
static uint8_t g_nvs_schema;
static char g_nvs_role[24];

esp_err_t nvs_open(const char *namespace_name, int open_mode,
                   nvs_handle_t *out_handle)
{
    (void)namespace_name;
    if (open_mode == NVS_READONLY && !g_nvs_role_persisted) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    (void)handle;
    (void)key;
    if (!g_nvs_role_persisted) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = g_nvs_schema;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key,
                      char *out_value, size_t *length)
{
    (void)handle;
    (void)key;
    size_t required = strlen(g_nvs_role) + 1;
    if (!g_nvs_role_persisted || !out_value || !length || *length < required) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(out_value, g_nvs_role, required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    g_nvs_schema = value;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *value)
{
    (void)handle;
    (void)key;
    strncpy(g_nvs_role, value, sizeof(g_nvs_role) - 1);
    g_nvs_role[sizeof(g_nvs_role) - 1] = '\0';
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    g_nvs_commit_calls++;
    g_nvs_role_persisted = true;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

static void require_quiet_before_halt(void)
{
    if (!scanner_quiet_mode_is_active()) {
        g_order_violations++;
    }
}

bool uart_tx_is_enabled(void)
{
    return g_tx_enabled;
}

void uart_tx_set_enabled(bool enabled)
{
    if (!enabled) {
        require_quiet_before_halt();
        g_tx_disable_calls++;
    } else {
        g_tx_enable_calls++;
        g_flush_calls_at_tx_enable = g_queue_flush_calls;
    }
    g_tx_enabled = enabled;
}

void uart_tx_flush_detection_queue(void)
{
    require_quiet_before_halt();
    g_queue_flush_calls++;
    g_last_flush_radios_quiesced = g_wifi_quiesced && g_ble_quiesced;
}

void uart_tx_reset_counts(void) {}

void wifi_scanner_lockon_cancel(void)
{
    require_quiet_before_halt();
    g_wifi_lockon_cancel_calls++;
}

void wifi_scanner_pause(void)
{
    require_quiet_before_halt();
    g_wifi_pause_calls++;
    g_wifi_paused = true;
    g_wifi_active = false;
}

bool wifi_scanner_resume(void)
{
    g_wifi_resume_calls++;
    g_wifi_paused = false;
    g_wifi_quiesced = false;
    g_wifi_active = g_wifi_resume_converges;
    return g_wifi_active;
}

bool wifi_scanner_is_paused(void)
{
    return g_wifi_paused;
}

bool wifi_scanner_is_quiesced(void)
{
    return g_wifi_quiesced;
}

bool wifi_scanner_is_active(void)
{
    return g_wifi_active;
}

void wifi_scanner_reset_attack_counters(void) {}
void wifi_scanner_reset_fc_histogram(void) {}

bool ble_remote_id_is_scanning(void)
{
    return g_ble_scanning;
}

bool ble_remote_id_is_quiesced(void)
{
    return g_ble_quiesced;
}

bool ble_remote_id_is_active(void)
{
    return g_ble_active;
}

void ble_remote_id_start(void)
{
    g_ble_start_calls++;
    g_ble_scanning = g_ble_start_converges;
    g_ble_active = g_ble_start_converges;
    g_ble_quiesced = false;
}

void ble_remote_id_stop(void)
{
    require_quiet_before_halt();
    g_ble_stop_calls++;
    g_ble_scanning = false;
    g_ble_active = false;
    if (g_ble_stop_converges) {
        g_ble_quiesced = true;
    }
}

void ble_remote_id_reset_profile_counters(void) {}

void ble_rid_lockon_cancel(void)
{
    require_quiet_before_halt();
    g_ble_lockon_cancel_calls++;
}

bool fof_policy_ble_has_exact_uuid128_le(const uint8_t uuids[][16],
                                         uint8_t count,
                                         const char *uuid_token)
{
    (void)uuids;
    (void)count;
    (void)uuid_token;
    return false;
}

bool fof_policy_ble_svc_raw_contains_uuid(const char *svc_raw,
                                          const char *uuid_token)
{
    (void)svc_raw;
    (void)uuid_token;
    return false;
}

bool fof_policy_scan_profile_allows_source(const char *scan_profile,
                                           uint8_t source)
{
    (void)scan_profile;
    (void)source;
    return true;
}

static void reset_call_counts(void)
{
    g_tx_disable_calls = 0;
    g_tx_enable_calls = 0;
    g_queue_flush_calls = 0;
    g_flush_calls_at_tx_enable = 0;
    g_last_flush_radios_quiesced = false;
    g_wifi_pause_calls = 0;
    g_wifi_resume_calls = 0;
    g_wifi_lockon_cancel_calls = 0;
    g_ble_start_calls = 0;
    g_ble_stop_calls = 0;
    g_ble_lockon_cancel_calls = 0;
}

static void close_unassigned_role_boot_window(void)
{
    scanner_scan_profile_runtime_ready_set(true);
    assert(!scanner_slot_role_boot_window_close());
    reset_call_counts();
}

static void test_invalid_late_role_is_rejected_without_mutation(void)
{
    bool late_recovery_required = true;

    assert(!scanner_slot_role_command_apply(NULL, "ble_primary",
                                            &late_recovery_required));
    assert(!late_recovery_required);
    assert(!scanner_slot_role_assignment_seen());
    assert(!scanner_slot_role_command_seen());
    assert(!scanner_scan_profile_assignment_seen());
    assert(g_nvs_commit_calls == 0);
}

static void test_mismatched_late_profile_cannot_allocate_hybrid(void)
{
    bool late_recovery_required = true;

    assert(!scanner_slot_role_command_apply("ble_primary", "hybrid_failover",
                                            &late_recovery_required));
    assert(!late_recovery_required);
    assert(!scanner_slot_role_assignment_seen());
    assert(strcmp(scanner_scan_profile_label(), "hybrid_failover") == 0);
    assert(g_nvs_commit_calls == 0);
    assert(g_ble_start_calls == 0);
    assert(g_wifi_resume_calls == 0);
}

static void test_valid_late_role_claims_one_exact_recovery(void)
{
    bool late_recovery_required = false;

    assert(scanner_slot_role_command_apply("ble_primary", "ble_primary",
                                           &late_recovery_required));
    assert(late_recovery_required);
    assert(scanner_slot_role_assignment_seen());
    assert(scanner_slot_role_command_seen());
    assert(scanner_scan_profile_assignment_seen());
    assert(strcmp(scanner_slot_role_label(), "ble_primary") == 0);
    assert(strcmp(scanner_scan_profile_label(), "ble_primary") == 0);
    assert(g_nvs_commit_calls == 1);
    assert(g_ble_start_calls == 0);
    assert(g_wifi_resume_calls == 0);
}

static void test_replayed_late_role_is_idempotent(void)
{
    bool late_recovery_required = true;

    assert(scanner_slot_role_command_apply("ble_primary", "ble_primary",
                                           &late_recovery_required));
    assert(!late_recovery_required);
    assert(g_nvs_commit_calls == 1);
    assert(g_ble_start_calls == 0);
    assert(g_wifi_resume_calls == 0);
}

static void test_durable_role_change_is_rejected_entirely(void)
{
    bool late_recovery_required = true;

    assert(!scanner_slot_role_command_apply("wifi_primary", "wifi_primary",
                                            &late_recovery_required));
    assert(!late_recovery_required);
    assert(strcmp(scanner_slot_role_label(), "ble_primary") == 0);
    assert(strcmp(scanner_scan_profile_label(), "ble_primary") == 0);
    assert(g_nvs_commit_calls == 1);
}

static void test_quiet_halts_both_radios_after_publishing_state(void)
{
    g_tx_enabled = true;
    g_wifi_paused = false;
    g_wifi_quiesced = true;
    g_ble_scanning = true;
    g_ble_quiesced = true;
    g_order_violations = 0;
    reset_call_counts();

    assert(scanner_quiet_mode_set(true, 41));
    assert(scanner_quiet_mode_is_active());
    assert(scanner_quiet_mode_generation() == 41);
    assert(!g_tx_enabled);
    assert(g_wifi_paused);
    assert(!g_ble_scanning);
    assert(g_order_violations == 0);
    assert(g_tx_disable_calls == 1);
    assert(g_queue_flush_calls >= 2);
    assert(g_last_flush_radios_quiesced);
    assert(g_wifi_pause_calls >= 1);
    assert(g_wifi_lockon_cancel_calls >= 1);
    assert(g_ble_stop_calls >= 1);
    assert(g_ble_lockon_cancel_calls >= 1);
}

static void test_quiet_guards_profile_and_calibration_restart(void)
{
    reset_call_counts();

    scanner_scan_profile_apply();
    assert(g_wifi_resume_calls == 0);
    assert(g_ble_start_calls == 0);
    assert(!scanner_calibration_mode_start("quiet-cal", "00112233"));

    scanner_scan_profile_set("wifi_primary");
    assert(strcmp(scanner_scan_profile_label(), "wifi_primary") == 0);
    assert(g_wifi_resume_calls == 0);
    assert(g_ble_start_calls == 0);
}

static void test_quiet_exit_restores_saved_profile_and_tx(void)
{
    reset_call_counts();

    assert(scanner_quiet_mode_set(false, 42));
    assert(!scanner_quiet_mode_is_active());
    assert(scanner_quiet_mode_generation() == 42);
    assert(g_tx_enabled);
    assert(!g_wifi_paused);
    assert(!g_ble_scanning);
    assert(g_wifi_resume_calls == 1);
    assert(g_tx_enable_calls == 1);
    assert(g_queue_flush_calls >= 1);
    assert(g_flush_calls_at_tx_enable == g_queue_flush_calls);
}

static void test_quiet_replay_does_not_overwrite_saved_tx_state(void)
{
    scanner_scan_profile_set("hybrid_failover");
    g_tx_enabled = false;
    g_wifi_paused = false;
    g_wifi_quiesced = true;
    g_ble_scanning = true;
    g_ble_quiesced = false;
    reset_call_counts();

    assert(scanner_quiet_mode_set(true, 100));
    assert(scanner_quiet_mode_set(true, 101));
    assert(scanner_quiet_mode_set(false, 102));
    assert(!g_tx_enabled);
    assert(g_tx_enable_calls == 0);
}

static void test_quiet_requires_confirmed_physical_radio_idle(void)
{
    if (scanner_quiet_mode_is_active()) {
        g_wifi_quiesced = true;
        g_ble_quiesced = true;
        assert(scanner_quiet_mode_set(false, 200));
    }

    g_tx_enabled = true;
    g_wifi_paused = false;
    g_wifi_quiesced = false;
    g_ble_scanning = true;
    g_ble_quiesced = false;
    g_ble_stop_converges = false;
    assert(!scanner_quiet_mode_set(true, 201));
    assert(scanner_quiet_mode_is_active());
    assert(g_wifi_paused);
    assert(!g_ble_scanning);

    g_wifi_quiesced = true;
    g_ble_quiesced = false;
    assert(!scanner_quiet_mode_set(true, 201));

    g_ble_quiesced = true;
    assert(scanner_quiet_mode_set(true, 201));
    g_ble_stop_converges = true;
    assert(scanner_quiet_mode_set(false, 202));
}

static void test_wake_ack_waits_for_configured_radios_and_saved_tx(void)
{
    scanner_scan_profile_set("hybrid_failover");
    g_tx_enabled = true;
    g_wifi_quiesced = true;
    g_ble_quiesced = true;
    g_ble_stop_converges = true;
    assert(scanner_quiet_mode_set(true, 300));

    g_wifi_resume_converges = false;
    g_ble_start_converges = false;
    assert(!scanner_quiet_mode_set(false, 301));
    assert(!scanner_quiet_mode_is_active());
    assert(!g_wifi_active);
    assert(!g_ble_active);

    g_wifi_resume_converges = true;
    g_ble_start_converges = true;
    assert(scanner_quiet_mode_set(false, 301));
    assert(g_wifi_active);
    assert(g_ble_active);
    assert(g_tx_enabled);
}

int main(void)
{
    close_unassigned_role_boot_window();
    test_invalid_late_role_is_rejected_without_mutation();
    test_mismatched_late_profile_cannot_allocate_hybrid();
    test_valid_late_role_claims_one_exact_recovery();
    test_replayed_late_role_is_idempotent();
    test_durable_role_change_is_rejected_entirely();
    scanner_scan_profile_runtime_ready_set(true);
    test_quiet_halts_both_radios_after_publishing_state();
    test_quiet_guards_profile_and_calibration_restart();
    test_quiet_exit_restores_saved_profile_and_tx();
    test_quiet_replay_does_not_overwrite_saved_tx_state();
    test_quiet_requires_confirmed_physical_radio_idle();
    test_wake_ack_waits_for_configured_radios_and_saved_tx();
    puts("scanner quiet mode native tests passed");
    return 0;
}
