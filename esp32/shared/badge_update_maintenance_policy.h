#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_UPDATE_SESSION_LENGTH 16U
#define BADGE_UPDATE_SESSION_CAPACITY (BADGE_UPDATE_SESSION_LENGTH + 1U)
#define BADGE_UPDATE_MAINTENANCE_MAGIC 0x464F4655U
#define BADGE_UPDATE_MAINTENANCE_VERSION 1U
#define BADGE_UPDATE_MAINTENANCE_MAX_BOOTS 8U
#define BADGE_UPDATE_MAINTENANCE_INACTIVITY_MS 120000U
#define BADGE_UPDATE_UPLINK_VERSION_CAPACITY 33U
#define BADGE_UPDATE_UPLINK_SHA256_CAPACITY 65U
#define BADGE_UPDATE_UPLINK_PARTITION_CAPACITY 17U

typedef enum {
    BADGE_UPDATE_PHASE_NONE = 0,
    BADGE_UPDATE_PHASE_PREPARING,
    BADGE_UPDATE_PHASE_REBOOT_ARMED,
    BADGE_UPDATE_PHASE_ACTIVE,
} badge_update_maintenance_phase_t;

typedef enum {
    BADGE_UPDATE_BOOT_CLEAR = 0,
    BADGE_UPDATE_BOOT_ENTER,
} badge_update_maintenance_boot_action_t;

typedef enum {
    BADGE_UPDATE_PREPARE_WAITING_FOR_OWNER = 0,
    BADGE_UPDATE_PREPARE_BUSY,
    BADGE_UPDATE_PREPARE_REBOOT_QUIESCED,
    BADGE_UPDATE_PREPARE_REBOOT_SAFE,
} badge_update_prepare_action_t;

typedef enum {
    BADGE_UPDATE_ABORT_CANCEL = 0,
    BADGE_UPDATE_ABORT_WAIT_PREEMPTION,
    BADGE_UPDATE_ABORT_WAIT_REBOOT_OWNER,
    BADGE_UPDATE_ABORT_CLEAR_AND_REBOOT,
} badge_update_abort_action_t;

typedef enum {
    BADGE_UPDATE_OTA_BEGIN_ADMIT = 0,
    BADGE_UPDATE_OTA_BEGIN_REJECT_SESSION_MISMATCH,
    BADGE_UPDATE_OTA_BEGIN_REJECT_UNEXPECTED_SESSION,
} badge_update_ota_begin_admission_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t phase;
    uint8_t boot_count;
    uint8_t uplink_committed;
    uint8_t reserved;
    uint32_t expected_reboot_generation;
    char session[BADGE_UPDATE_SESSION_CAPACITY];
    char uplink_version[BADGE_UPDATE_UPLINK_VERSION_CAPACITY];
    char uplink_sha256[BADGE_UPDATE_UPLINK_SHA256_CAPACITY];
    char uplink_partition[BADGE_UPDATE_UPLINK_PARTITION_CAPACITY];
    uint32_t uplink_size;
    uint32_t uplink_received;
    uint32_t crc32;
} badge_update_maintenance_marker_t;
#pragma pack(pop)

bool badge_update_session_valid(const char *session, size_t byte_len);
void badge_update_maintenance_marker_seal(
    badge_update_maintenance_marker_t *marker);
bool badge_update_maintenance_marker_valid(
    const badge_update_maintenance_marker_t *marker);
bool badge_update_maintenance_marker_prepare(
    badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len);
bool badge_update_maintenance_session_matches(
    const badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len);
bool badge_update_maintenance_marker_abort(
    badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len);
badge_update_abort_action_t badge_update_preparing_abort_decide(
    const badge_update_maintenance_marker_t *marker,
    const char *session,
    size_t session_byte_len,
    bool preemption_safe,
    bool reboot_owner_acquired);
bool badge_update_maintenance_marker_arm_reboot(
    badge_update_maintenance_marker_t *marker,
    uint32_t expected_reboot_generation);
badge_update_maintenance_boot_action_t
badge_update_maintenance_boot_decide(
    const badge_update_maintenance_marker_t *marker,
    bool expected_software_reset,
    uint32_t expected_reboot_generation,
    bool emergency_safe_mode);
bool badge_update_maintenance_marker_activate(
    badge_update_maintenance_marker_t *marker);
bool badge_update_maintenance_marker_commit_uplink(
    badge_update_maintenance_marker_t *marker,
    const char *version,
    const char *sha256,
    uint32_t size,
    const char *partition);
bool badge_update_maintenance_marker_clear_uplink(
    badge_update_maintenance_marker_t *marker);
badge_update_prepare_action_t badge_update_prepare_decide(
    bool firmware_operation_active,
    bool coordinator_suspended,
    bool radio_quiesced);
bool badge_update_maintenance_inactivity_due(
    uint32_t now_ms,
    uint32_t last_activity_ms);
bool badge_update_maintenance_health_satisfied(
    uint32_t uptime_ms,
    bool display_alive,
    bool completed_usb_response,
    bool scanner_uart_zero_alive,
    bool scanner_uart_one_alive,
    bool emergency_safe_mode,
    uint32_t free_internal_heap,
    uint32_t largest_internal_block);
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
