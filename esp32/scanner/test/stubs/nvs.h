#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef unsigned int nvs_handle_t;

#define ESP_OK                        0
#define ESP_ERR_NVS_NOT_FOUND         0x1102
#define ESP_ERR_NVS_INVALID_STATE     0x110b

#define NVS_READONLY                  0
#define NVS_READWRITE                 1

esp_err_t nvs_open(const char *namespace_name, int open_mode,
                   nvs_handle_t *out_handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key,
                      char *out_value, size_t *length);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

static inline const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test_error";
}
