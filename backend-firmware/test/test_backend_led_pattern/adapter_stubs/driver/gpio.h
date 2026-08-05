#pragma once

#include <stdint.h>

typedef int esp_err_t;
typedef int gpio_num_t;

#define ESP_OK 0
#define GPIO_NUM_21 21
#define GPIO_INTR_DISABLE 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLUP_DISABLE 0

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
