#ifndef BACKEND_USB_CONFIG_H
#define BACKEND_USB_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "backend_config.h"
#include "backend_config_portal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    backend_config_record_t staged;
    bool dirty;
} backend_usb_config_t;

void backend_usb_config_init(
    backend_usb_config_t *state,
    const backend_config_record_t *current);
backend_portal_update_result_t backend_usb_config_stage(
    backend_usb_config_t *state,
    const char *key,
    const char *value);
backend_portal_update_result_t backend_usb_config_save(
    backend_usb_config_t *state,
    backend_config_portal_commit_fn commit,
    backend_config_portal_reconnect_fn reconnect,
    void *context,
    int64_t now_ms,
    uint32_t *out_generation);

#ifdef __cplusplus
}
#endif

#endif
