#include "backend_usb_config.h"

#include <string.h>

void backend_usb_config_init(
    backend_usb_config_t *state,
    const backend_config_record_t *current)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    if (current != NULL) {
        state->staged = *current;
    }
}

static bool copy_stage_value(
    char *destination, size_t capacity, const char *value)
{
    if (destination == NULL || capacity == 0 || value == NULL) {
        return false;
    }
    const char *terminator = memchr(value, '\0', capacity);
    if (terminator == NULL) {
        return false;
    }
    const size_t length = (size_t)(terminator - value);
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = (unsigned char)value[index];
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    memset(destination, 0, capacity);
    memcpy(destination, value, length);
    return true;
}

backend_portal_update_result_t backend_usb_config_stage(
    backend_usb_config_t *state,
    const char *key,
    const char *value)
{
    if (state == NULL || key == NULL || value == NULL) {
        return BACKEND_PORTAL_UPDATE_INVALID_ARGUMENT;
    }
    backend_config_record_t candidate = state->staged;
    bool recognized = true;
    bool copied = false;
    if (strcmp(key, "wifi_ssid") == 0) {
        copied = copy_stage_value(
            candidate.networks[0].ssid,
            sizeof(candidate.networks[0].ssid), value);
        if (copied && candidate.networks[0].ssid[0] == '\0') {
            copied = false;
        }
        if (copied && candidate.networks[0].ssid[0] != '\0' &&
            candidate.network_count == 0) {
            candidate.network_count = 1;
        }
    } else if (strcmp(key, "wifi_pass") == 0) {
        copied = copy_stage_value(
            candidate.networks[0].password,
            sizeof(candidate.networks[0].password), value);
    } else if (strcmp(key, "backend_url") == 0) {
        copied = copy_stage_value(
            candidate.backend_url, sizeof(candidate.backend_url), value);
    } else if (strcmp(key, "device_id") == 0) {
        copied = copy_stage_value(
            candidate.device_id, sizeof(candidate.device_id), value);
    } else if (strcmp(key, "ap_pass") == 0) {
        copied = copy_stage_value(
            candidate.ap_password, sizeof(candidate.ap_password), value);
    } else {
        recognized = false;
    }
    if (!recognized) {
        return BACKEND_PORTAL_UPDATE_UNKNOWN_FIELD;
    }
    if (!copied || backend_config_validate(&candidate) !=
                       BACKEND_CONFIG_VALID) {
        return BACKEND_PORTAL_UPDATE_INVALID_CONFIG;
    }
    state->staged = candidate;
    state->dirty = true;
    return BACKEND_PORTAL_UPDATE_OK;
}

backend_portal_update_result_t backend_usb_config_save(
    backend_usb_config_t *state,
    const backend_config_record_t *current,
    backend_config_portal_commit_fn commit,
    backend_config_portal_reconnect_fn reconnect,
    void *context,
    int64_t now_ms,
    uint32_t *out_generation)
{
    if (out_generation != NULL) {
        *out_generation = 0;
    }
    if (state == NULL || current == NULL || commit == NULL ||
        reconnect == NULL) {
        return BACKEND_PORTAL_UPDATE_INVALID_ARGUMENT;
    }
    if (state->staged.generation != current->generation) {
        state->staged = *current;
        state->dirty = false;
        return BACKEND_PORTAL_UPDATE_STALE_GENERATION;
    }
    backend_config_record_t candidate = state->staged;
    ++candidate.generation;
    if (backend_config_validate(&candidate) != BACKEND_CONFIG_VALID ||
        !commit(context, &candidate)) {
        return BACKEND_PORTAL_UPDATE_COMMIT_FAILED;
    }
    state->staged = candidate;
    state->dirty = false;
    if (out_generation != NULL) {
        *out_generation = candidate.generation;
    }
    return reconnect(context, &state->staged, now_ms)
        ? BACKEND_PORTAL_UPDATE_OK
        : BACKEND_PORTAL_UPDATE_RECONNECT_FAILED;
}
