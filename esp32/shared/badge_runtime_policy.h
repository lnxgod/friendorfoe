#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_RUNTIME_EXPECTED_REBOOT_MAGIC 0xF0F0B007U
#define BADGE_RUNTIME_USB_RECOVERY_ONCE_MAGIC 0xF0F05542U

typedef enum {
    BADGE_RUNTIME_NETWORK_OFF = 0,
    BADGE_RUNTIME_NETWORK_LOCAL_AP,
    BADGE_RUNTIME_NETWORK_BACKEND,
} badge_runtime_network_mode_t;

typedef enum {
    BADGE_RUNTIME_RESET_CLEAN = 0,
    BADGE_RUNTIME_RESET_EXPECTED_SW,
    BADGE_RUNTIME_RESET_CRASH,
} badge_runtime_reset_class_t;

typedef enum {
    BADGE_RUNTIME_RECOVERY_TOKEN_CLEAR = 0,
    BADGE_RUNTIME_RECOVERY_TOKEN_CONSUME_SAFE_USB,
} badge_runtime_recovery_token_action_t;

typedef struct {
    bool enter_safe_mode;
    bool force_ota_rollback;
    uint32_t new_crash_count;
} badge_runtime_boot_decision_t;

typedef struct {
    bool expected_software_reset;
    bool legacy_v078;
    bool clear_generation;
    uint32_t consumed_generation;
} badge_runtime_expected_reboot_decision_t;

typedef enum {
    BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT = 0,
    BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_LEGACY_V078_ROLLBACK,
} badge_runtime_expected_reboot_target_t;

typedef enum {
    BADGE_RUNTIME_EXPECTED_REBOOT_ARM_IDLE = 0,
    BADGE_RUNTIME_EXPECTED_REBOOT_ARM_PREPARING,
    BADGE_RUNTIME_EXPECTED_REBOOT_ARM_OWNED,
} badge_runtime_expected_reboot_arm_phase_t;

typedef struct {
    uint32_t owner_id;
    uint32_t generation;
} badge_runtime_expected_reboot_lease_t;

typedef struct {
    badge_runtime_expected_reboot_arm_phase_t phase;
    uint32_t last_owner_id;
    uint32_t active_owner_id;
    uint32_t active_generation;
} badge_runtime_expected_reboot_arm_state_t;

badge_runtime_network_mode_t badge_runtime_default_network_mode(bool badge_variant);
bool badge_runtime_parse_network_mode(const char *value,
                                      badge_runtime_network_mode_t *out);
const char *badge_runtime_network_mode_name(badge_runtime_network_mode_t mode);
bool badge_runtime_badge_allows_network_mode(badge_runtime_network_mode_t mode);
int badge_runtime_network_ttl_s(badge_runtime_network_mode_t mode,
                                int requested_ttl_s);
int badge_runtime_post_ota_hold_ttl_s(badge_runtime_network_mode_t mode,
                                      int requested_ttl_s);
badge_runtime_boot_decision_t badge_runtime_boot_decide(
    badge_runtime_reset_class_t reset_class,
    bool pending_verify,
    uint32_t prior_crash_count,
    uint32_t crash_loop_threshold
);
uint32_t badge_runtime_expected_reboot_next_generation(
    uint32_t prior_generation);
uint32_t badge_runtime_expected_reboot_generation_for_target(
    badge_runtime_expected_reboot_target_t target,
    uint32_t prior_generation);
void badge_runtime_expected_reboot_arm_state_init(
    badge_runtime_expected_reboot_arm_state_t *state);
bool badge_runtime_expected_reboot_arm_reserve(
    badge_runtime_expected_reboot_arm_state_t *state,
    uint32_t generation,
    badge_runtime_expected_reboot_lease_t *out_lease);
bool badge_runtime_expected_reboot_arm_is_preparing(
    const badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease);
bool badge_runtime_expected_reboot_arm_publish(
    badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease);
bool badge_runtime_expected_reboot_arm_is_owned(
    const badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease);
bool badge_runtime_expected_reboot_arm_cancel(
    badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease);
bool badge_runtime_expected_reboot_arm_release(
    badge_runtime_expected_reboot_arm_state_t *state,
    const badge_runtime_expected_reboot_lease_t *lease);
badge_runtime_expected_reboot_decision_t
badge_runtime_expected_reboot_decide(
    bool software_reset,
    uint32_t generation_word,
    uint32_t magic_word);
bool badge_runtime_normal_stability_satisfied(
    bool safe_mode,
    bool display_alive,
    bool ble_uart_worker_heartbeat,
    bool wifi_uart_worker_heartbeat,
    uint32_t free_heap_bytes,
    int64_t uptime_s,
    int64_t stable_after_s);
bool badge_runtime_uart_heartbeat_fresh(int64_t heartbeat_ms,
                                        int64_t now_ms,
                                        int64_t stale_after_ms);
badge_runtime_recovery_token_action_t badge_runtime_recovery_token_decide(
    bool token_armed,
    badge_runtime_reset_class_t reset_class);
bool badge_runtime_rollback_health_satisfied(
    bool safe_usb,
    bool display_alive,
    bool completed_usb_response,
    bool scanner_uart_worker_heartbeat,
    uint32_t free_heap_bytes,
    int64_t uptime_s,
    int64_t stable_after_s);
bool badge_runtime_usb_recovery_due(bool safe_mode,
                                    bool usb_control_alive,
                                    int64_t usb_control_age_s,
                                    int64_t uptime_s,
                                    int64_t stale_after_s,
                                    int64_t boot_grace_s);

#ifdef __cplusplus
}
#endif
