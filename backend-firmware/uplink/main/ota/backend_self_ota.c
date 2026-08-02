#include "backend_self_ota.h"

#include <string.h>

#include "backend_identity.h"

#define BACKEND_SELF_OTA_CAPACITY 0x200000U

static bool bounded_terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool lowercase_sha256(const char value[65])
{
    if (!bounded_terminated(value, 65U) || value[64] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        const char byte = value[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool valid_backend_uplink_manifest(
    const backend_ota_manifest_t *manifest)
{
    const backend_firmware_identity_t *identity =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    if (manifest == NULL || identity == NULL ||
        !bounded_terminated(manifest->target, sizeof(manifest->target)) ||
        !bounded_terminated(manifest->project, sizeof(manifest->project)) ||
        !bounded_terminated(manifest->hardware, sizeof(manifest->hardware)) ||
        !bounded_terminated(manifest->version, sizeof(manifest->version)) ||
        manifest->version[0] == '\0' || manifest->image_size == 0U ||
        manifest->image_size > BACKEND_SELF_OTA_CAPACITY ||
        manifest->generation == 0U || !lowercase_sha256(manifest->sha256)) {
        return false;
    }
    return backend_identity_matches(
        identity, manifest->target, manifest->project, manifest->hardware);
}

static bool adapters_valid(const backend_self_ota_adapters_t *adapters)
{
    return adapters != NULL && adapters->begin != NULL &&
           adapters->write != NULL && adapters->end != NULL &&
           adapters->select_boot_partition != NULL &&
           adapters->mark_running_valid != NULL;
}

void backend_self_ota_init(
    backend_self_ota_t *state,
    const backend_self_ota_adapters_t *adapters)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->state = BACKEND_SELF_OTA_IDLE;
    if (adapters_valid(adapters)) {
        state->adapters = *adapters;
    }
}

backend_self_ota_state_t backend_self_ota_begin(
    backend_self_ota_t *state,
    const backend_ota_manifest_t *manifest)
{
    if (state == NULL || !adapters_valid(&state->adapters) ||
        (state->state != BACKEND_SELF_OTA_IDLE &&
         state->state != BACKEND_SELF_OTA_VALID) ||
        !valid_backend_uplink_manifest(manifest)) {
        if (state != NULL && state->state == BACKEND_SELF_OTA_IDLE) {
            state->state = BACKEND_SELF_OTA_REJECTED;
        }
        return BACKEND_SELF_OTA_REJECTED;
    }

    state->image_write_count++;
    if (!state->adapters.begin(
            state->adapters.context, manifest->image_size)) {
        state->state = BACKEND_SELF_OTA_FAILED;
        return state->state;
    }
    state->manifest = *manifest;
    state->next_offset = 0U;
    state->rollback_clear = false;
    state->state = BACKEND_SELF_OTA_READY;
    return state->state;
}

bool backend_self_ota_write(
    backend_self_ota_t *state,
    size_t offset,
    const uint8_t *bytes,
    size_t length)
{
    if (state == NULL || bytes == NULL || length == 0U ||
        (state->state != BACKEND_SELF_OTA_READY &&
         state->state != BACKEND_SELF_OTA_WRITING) ||
        offset != state->next_offset ||
        state->next_offset > state->manifest.image_size ||
        length > state->manifest.image_size - state->next_offset) {
        return false;
    }

    state->image_write_count++;
    if (!state->adapters.write(
            state->adapters.context, offset, bytes, length)) {
        state->state = BACKEND_SELF_OTA_FAILED;
        return false;
    }
    state->next_offset += length;
    state->state = BACKEND_SELF_OTA_WRITING;
    return true;
}

backend_self_ota_state_t backend_self_ota_finish(backend_self_ota_t *state)
{
    if (state == NULL || state->state == BACKEND_SELF_OTA_FAILED) {
        return BACKEND_SELF_OTA_FAILED;
    }
    if ((state->state != BACKEND_SELF_OTA_READY &&
         state->state != BACKEND_SELF_OTA_WRITING) ||
        state->next_offset != state->manifest.image_size) {
        state->state = BACKEND_SELF_OTA_FAILED;
        return state->state;
    }

    state->image_write_count++;
    if (!state->adapters.end(state->adapters.context)) {
        state->state = BACKEND_SELF_OTA_FAILED;
        return state->state;
    }
    state->image_write_count++;
    if (!state->adapters.select_boot_partition(state->adapters.context)) {
        state->state = BACKEND_SELF_OTA_FAILED;
        return state->state;
    }
    state->state = BACKEND_SELF_OTA_READY_TO_REBOOT;
    return state->state;
}

void backend_self_ota_on_boot(
    backend_self_ota_t *state,
    uint32_t boot_id,
    bool pending_verify)
{
    if (state == NULL) {
        return;
    }
    state->boot_id = boot_id;
    state->next_offset = 0U;
    state->rollback_clear = !pending_verify;
    state->state = pending_verify
        ? BACKEND_SELF_OTA_PENDING_VERIFY
        : BACKEND_SELF_OTA_VALID;
}

bool backend_self_ota_mark_valid_if_healthy(
    backend_self_ota_t *state,
    const backend_self_ota_health_t *health)
{
    if (state == NULL || health == NULL ||
        state->state != BACKEND_SELF_OTA_PENDING_VERIFY ||
        !health->config_loaded || !health->led_worker_running ||
        !health->uart_worker_running || !health->coordinator_worker_running ||
        (!health->ap_healthy && !health->sta_healthy)) {
        return false;
    }
    if (!state->adapters.mark_running_valid(state->adapters.context)) {
        state->state = BACKEND_SELF_OTA_FAILED;
        return false;
    }
    state->rollback_clear = true;
    state->state = BACKEND_SELF_OTA_VALID;
    return true;
}

bool backend_self_ota_rollback_clear(const backend_self_ota_t *state)
{
    return state != NULL && state->rollback_clear;
}

uint32_t backend_self_ota_image_write_count(
    const backend_self_ota_t *state)
{
    return state == NULL ? 0U : state->image_write_count;
}
