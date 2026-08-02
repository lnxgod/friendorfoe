#ifndef BACKEND_CONFIG_H
#define BACKEND_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_CONFIG_MAX_NETWORKS 4
#define BACKEND_CONFIG_SCHEMA_VERSION 1

typedef struct {
    char ssid[33];
    char password[65];
} backend_wifi_network_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char wifi_pass[65];
    char backend_url[192];
    char device_id[33];
    char ap_pass[65];
} backend_legacy_config_t;

typedef struct {
    uint16_t schema_version;
    uint32_t generation;
    uint8_t network_count;
    backend_wifi_network_t networks[BACKEND_CONFIG_MAX_NETWORKS];
    char backend_url[192];
    char device_id[33];
    char display_name[65];
    char ap_password[65];
    bool auto_update_enabled;
    bool has_location;
    double latitude;
    double longitude;
    float altitude_m;
} backend_config_record_t;

#define BACKEND_CONFIG_MAGIC UINT32_C(0x47464342)
#define BACKEND_CONFIG_BLOB_MAX 1024
#define BACKEND_CONFIG_PAYLOAD_MAX 1008

typedef struct {
    size_t length;
    uint8_t bytes[BACKEND_CONFIG_BLOB_MAX];
} backend_config_blob_t;

typedef enum {
    BACKEND_CONFIG_VALID,
    BACKEND_CONFIG_INVALID_LENGTH,
    BACKEND_CONFIG_INVALID_CRC,
    BACKEND_CONFIG_INVALID_FIELD,
} backend_config_result_t;

backend_config_result_t backend_config_validate(
    const backend_config_record_t *record);

bool backend_config_encode_canonical(
    const backend_config_record_t *record,
    backend_config_blob_t *out);

backend_config_result_t backend_config_decode_canonical(
    const uint8_t *bytes,
    size_t length,
    backend_config_record_t *out);

bool backend_config_migrate_legacy(
    const backend_legacy_config_t *legacy,
    uint32_t generation,
    backend_config_record_t *out);

#ifdef __cplusplus
}
#endif

#endif
