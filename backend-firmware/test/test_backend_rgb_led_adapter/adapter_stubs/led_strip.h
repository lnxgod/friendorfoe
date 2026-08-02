#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0

typedef union {
    struct {
        uint32_t r_pos : 2;
        uint32_t g_pos : 2;
        uint32_t b_pos : 2;
        uint32_t w_pos : 2;
        uint32_t reserved : 19;
        uint32_t bytes_per_color : 2;
        uint32_t num_components : 3;
    } format;
    uint32_t format_id;
} led_color_component_format_t;

#define LED_STRIP_COLOR_COMPONENT_FMT_GRB \
    ((led_color_component_format_t){ \
        .format = {.r_pos = 1, .g_pos = 0, .b_pos = 2, .w_pos = 3, \
                   .reserved = 0, .bytes_per_color = 1, .num_components = 3}})

typedef struct {
    int strip_gpio_num;
    uint32_t max_leds;
    led_color_component_format_t color_component_format;
} led_strip_config_t;

typedef struct led_strip_t *led_strip_handle_t;

esp_err_t led_strip_set_pixel(
    led_strip_handle_t strip,
    uint32_t index,
    uint32_t red,
    uint32_t green,
    uint32_t blue);
esp_err_t led_strip_refresh(led_strip_handle_t strip);
esp_err_t led_strip_clear(led_strip_handle_t strip);
esp_err_t led_strip_del(led_strip_handle_t strip);
