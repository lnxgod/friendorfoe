#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "firmware_image_contract.h"
#include "firmware_operation_token.h"
#include "uplink_ota_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_USB_OTA_MAX_WRITE_BYTES 512U
#define UPLINK_USB_OTA_PARTITION_LABEL_BYTES 17U
#define UPLINK_USB_OTA_ERROR_BYTES 48U
#define UPLINK_USB_OTA_REMAINING_UNKNOWN UINT32_MAX

typedef enum {
    UPLINK_USB_OTA_IDLE = 0,
    UPLINK_USB_OTA_PREPARING,
    UPLINK_USB_OTA_RECEIVING,
    UPLINK_USB_OTA_VERIFYING,
    UPLINK_USB_OTA_COMMITTED,
    UPLINK_USB_OTA_ERROR,
} uplink_usb_ota_state_t;

typedef enum {
    UPLINK_USB_OTA_PHASE_NONE = 0,
    UPLINK_USB_OTA_PHASE_READY,
    UPLINK_USB_OTA_PHASE_PROGRESS,
    UPLINK_USB_OTA_PHASE_CREDIT,
    UPLINK_USB_OTA_PHASE_COMMITTED,
    UPLINK_USB_OTA_PHASE_ABORTED,
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
    UPLINK_USB_OTA_PHASE_ERROR,
#endif
} uplink_usb_ota_phase_t;

typedef struct {
    bool ok;
    bool retryable;
    bool emit_required;
    bool reboot_required;
    uplink_usb_ota_phase_t phase;
    char error[UPLINK_USB_OTA_ERROR_BYTES];
    char partition[UPLINK_USB_OTA_PARTITION_LABEL_BYTES];
    uint32_t received;
    uint32_t total;
    uint32_t credit_bytes;
} uplink_usb_ota_result_t;

typedef struct {
    uplink_usb_ota_state_t state;
    char partition[UPLINK_USB_OTA_PARTITION_LABEL_BYTES];
    char target_version[33];
#if defined(FOF_DC34_GAME_CANARY) || defined(UNIT_TESTING)
    char target_sha256[FOF_FIRMWARE_SHA256_HEX_SIZE];
#endif
    char last_error[UPLINK_USB_OTA_ERROR_BYTES];
    uint32_t received;
    uint32_t total;
} uplink_usb_ota_status_t;

const char *uplink_usb_ota_state_name(uplink_usb_ota_state_t state);
bool uplink_usb_ota_begin(const uplink_ota_manifest_t *manifest,
                          uplink_usb_ota_result_t *out);
bool uplink_usb_ota_write(const uint8_t *bytes, size_t length,
                          uint32_t cumulative_transport_received,
                          uplink_usb_ota_result_t *out);
bool uplink_usb_ota_finish(uint32_t cumulative_transport_received,
                           uplink_usb_ota_result_t *out);
bool uplink_usb_ota_abort(const char *reason,
                          uplink_usb_ota_result_t *out);
uint32_t uplink_usb_ota_remaining(void);
bool uplink_usb_ota_get_status(uplink_usb_ota_status_t *out);

typedef enum {
    UPLINK_USB_OTA_IMAGE_STATE_VALID = 0,
    UPLINK_USB_OTA_IMAGE_STATE_PENDING_VERIFY,
    UPLINK_USB_OTA_IMAGE_STATE_OTHER,
} uplink_usb_ota_image_state_t;

typedef struct {
    uintptr_t native_id;
    char label[UPLINK_USB_OTA_PARTITION_LABEL_BYTES];
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
} uplink_usb_ota_partition_t;

typedef struct {
    void *context;
    bool (*get_running)(void *context, uplink_usb_ota_partition_t *out);
    bool (*get_next)(void *context,
                     const uplink_usb_ota_partition_t *running,
                     uplink_usb_ota_partition_t *out);
    bool (*get_image_state)(void *context,
                            const uplink_usb_ota_partition_t *partition,
                            uplink_usb_ota_image_state_t *out);
    bool (*get_partition_identity)(
        void *context, const uplink_usb_ota_partition_t *partition,
        fof_firmware_image_identity_t *out);
    bool (*ota_begin)(void *context,
                      const uplink_usb_ota_partition_t *partition,
                      uintptr_t *handle);
    bool (*ota_write)(void *context, uintptr_t handle,
                      const uint8_t *bytes, size_t length);
    bool (*ota_end)(void *context, uintptr_t handle);
    bool (*ota_abort)(void *context, uintptr_t handle);
    bool (*set_boot_partition)(
        void *context, const uplink_usb_ota_partition_t *partition);
    bool (*operation_begin)(void *context, fw_operation_token_t *out);
    bool (*operation_end)(void *context, fw_operation_token_t token);
    bool (*pause_http)(void *context, bool *owned);
    bool (*pause_scanner)(void *context, uint8_t slot, bool *owned);
    void (*resume_http)(void *context);
    void (*resume_scanner)(void *context, uint8_t slot);
    bool (*integrity_start)(void *context);
    bool (*integrity_update)(void *context, const uint8_t *bytes,
                             size_t length, uint32_t *crc32);
    bool (*integrity_finish)(void *context,
                             uint8_t digest[FOF_FIRMWARE_SHA256_SIZE]);
    void (*integrity_abort)(void *context);
} uplink_usb_ota_hooks_t;

#ifdef UNIT_TESTING
bool uplink_usb_ota_test_install_hooks(const uplink_usb_ota_hooks_t *hooks);
void uplink_usb_ota_test_reset(void);
void uplink_usb_ota_test_set_mutator_busy(bool busy);
void uplink_usb_ota_test_set_status_writer_busy(bool busy);
#endif

#ifdef __cplusplus
}
#endif
