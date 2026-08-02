#ifndef BACKEND_NVS_CONFIG_H
#define BACKEND_NVS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_NVS_STORAGE_UNINITIALIZED,
    BACKEND_NVS_STORAGE_READY,
    BACKEND_NVS_STORAGE_FATAL,
} backend_nvs_storage_health_t;

bool backend_config_load(backend_config_record_t *out);
bool backend_config_commit(const backend_config_record_t *record);
bool backend_config_load_or_migrate(backend_config_record_t *out);

backend_nvs_storage_health_t backend_nvs_config_storage_health(void);

typedef enum {
    BACKEND_NVS_IO_OK,
    BACKEND_NVS_IO_NOT_FOUND,
    BACKEND_NVS_IO_NO_FREE_PAGES,
    BACKEND_NVS_IO_NEW_VERSION,
    BACKEND_NVS_IO_ERROR,
} backend_nvs_io_result_t;

#ifdef UNIT_TESTING
typedef struct {
    void *context;
    backend_nvs_io_result_t (*init)(void *context);
    backend_nvs_io_result_t (*read_blob)(
        void *context,
        const char *namespace_name,
        const char *key,
        uint8_t *out,
        size_t capacity,
        size_t *out_length);
    backend_nvs_io_result_t (*write_blob)(
        void *context,
        const char *namespace_name,
        const char *key,
        const uint8_t *bytes,
        size_t length);
    backend_nvs_io_result_t (*commit)(void *context);
    backend_nvs_io_result_t (*read_string)(
        void *context,
        const char *namespace_name,
        const char *key,
        char *out,
        size_t capacity);
    bool (*read_sta_mac)(void *context, uint8_t out[6]);
    void (*erase_storage)(void *context);
    void (*clear_rollback)(void *context);
} backend_nvs_config_hooks_t;

void backend_nvs_config_set_test_hooks(
    const backend_nvs_config_hooks_t *hooks);
void backend_nvs_config_reset_test_hooks(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
