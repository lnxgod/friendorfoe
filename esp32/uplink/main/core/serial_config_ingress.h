#pragma once

#include "badge_usb_control_schema.h"
#include "firmware_json_schema_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERIAL_CONFIG_INGRESS_REJECTED = 0,
    SERIAL_CONFIG_INGRESS_PING,
    SERIAL_CONFIG_INGRESS_STATUS,
    SERIAL_CONFIG_INGRESS_SAVE,
    SERIAL_CONFIG_INGRESS_REBOOT,
    SERIAL_CONFIG_INGRESS_SET,
    SERIAL_CONFIG_INGRESS_CTL_COMPAT,
    SERIAL_CONFIG_INGRESS_CTL_FIRMWARE,
} serial_config_ingress_kind_t;

typedef struct {
    serial_config_ingress_kind_t kind;
    fof_fw_json_schema_id_t firmware_schema_id;
    badge_usb_control_schema_id_t control_schema_id;
    badge_usb_control_handler_kind_t control_handler_kind;
} serial_config_ingress_result_t;

typedef struct {
    const uint8_t *key;
    size_t key_len;
    const uint8_t *value;
    size_t value_len;
} serial_config_set_parts_t;

bool serial_config_ingress_authorize(
    const uint8_t *line,
    size_t line_byte_len,
    serial_config_ingress_result_t *result);

bool serial_config_ingress_parse_set(
    const uint8_t *line,
    size_t line_byte_len,
    serial_config_set_parts_t *parts);

bool serial_config_ingress_is_uplink_ota_begin(
    const uint8_t *line,
    size_t line_byte_len);

#ifdef __cplusplus
}
#endif
