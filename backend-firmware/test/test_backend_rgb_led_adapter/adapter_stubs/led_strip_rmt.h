#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "led_strip.h"

typedef struct {
    uint32_t resolution_hz;
    struct {
        unsigned with_dma : 1;
    } flags;
} led_strip_rmt_config_t;

esp_err_t led_strip_new_rmt_device(
    const led_strip_config_t *strip_config,
    const led_strip_rmt_config_t *rmt_config,
    led_strip_handle_t *ret_strip);
