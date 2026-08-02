#ifndef BACKEND_SELF_OTA_H
#define BACKEND_SELF_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_ota_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_SELF_OTA_IDLE = 0,
    BACKEND_SELF_OTA_REJECTED,
    BACKEND_SELF_OTA_READY,
    BACKEND_SELF_OTA_WRITING,
    BACKEND_SELF_OTA_READY_TO_REBOOT,
    BACKEND_SELF_OTA_PENDING_VERIFY,
    BACKEND_SELF_OTA_VALID,
    BACKEND_SELF_OTA_FAILED,
} backend_self_ota_state_t;

typedef struct {
    void *context;
    bool (*begin)(void *context, size_t image_size);
    bool (*write)(
        void *context, size_t offset, const uint8_t *bytes, size_t length);
    bool (*end)(void *context);
    bool (*select_boot_partition)(void *context);
    bool (*mark_running_valid)(void *context);
} backend_self_ota_adapters_t;

typedef struct {
    bool config_loaded;
    bool led_worker_running;
    bool uart_worker_running;
    bool coordinator_worker_running;
    bool ap_healthy;
    bool sta_healthy;
    /* Deliberately diagnostic only: trusted-LAN backend availability must not
     * strand a locally healthy pending image in the rollback window. */
    bool backend_reachable;
} backend_self_ota_health_t;

typedef struct {
    backend_self_ota_adapters_t adapters;
    backend_ota_manifest_t manifest;
    backend_self_ota_state_t state;
    size_t next_offset;
    uint32_t image_write_count;
    uint32_t boot_id;
    bool rollback_clear;
} backend_self_ota_t;

void backend_self_ota_init(
    backend_self_ota_t *state,
    const backend_self_ota_adapters_t *adapters);

backend_self_ota_state_t backend_self_ota_begin(
    backend_self_ota_t *state,
    const backend_ota_manifest_t *manifest);

bool backend_self_ota_write(
    backend_self_ota_t *state,
    size_t offset,
    const uint8_t *bytes,
    size_t length);

backend_self_ota_state_t backend_self_ota_finish(
    backend_self_ota_t *state);

void backend_self_ota_on_boot(
    backend_self_ota_t *state,
    uint32_t boot_id,
    bool pending_verify);

bool backend_self_ota_mark_valid_if_healthy(
    backend_self_ota_t *state,
    const backend_self_ota_health_t *health);

bool backend_self_ota_rollback_clear(const backend_self_ota_t *state);
uint32_t backend_self_ota_image_write_count(
    const backend_self_ota_t *state);

#ifdef __cplusplus
}
#endif

#endif
