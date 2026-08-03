#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_detection_codec.h"
#include "backend_led_pattern.h"
#include "backend_scanner_status_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_UPLOAD_MAX_JSON 5120U
#define BACKEND_UPLOAD_FIFO_CAPACITY 512U

typedef struct {
    uint32_t sequence;
    uint16_t item_count;
    uint16_t json_len;
    uint32_t json_crc32;
    char json[BACKEND_UPLOAD_MAX_JSON + 1U];
} backend_upload_batch_t;

typedef enum {
    BACKEND_ENCODE_OK = 0,
    BACKEND_ENCODE_NEEDS_FLUSH,
    BACKEND_ENCODE_ITEM_TOO_LARGE,
    BACKEND_ENCODE_INVALID,
} backend_encode_result_t;

typedef struct {
    uint16_t depth_batches;
    uint16_t capacity_batches;
    uint32_t overflow_dropped_batches;
    uint32_t quarantined_batches;
} backend_upload_queue_telemetry_t;

typedef struct {
    uint32_t ok;
    uint32_t failed;
    uint32_t retry_count;
    bool has_last_success_age;
    uint32_t last_success_age_s;
} backend_upload_telemetry_t;

typedef struct {
    char device_id[33];
    uint32_t boot_id;
    uint32_t topology_generation;
    char product_family[24];
    char firmware_line[24];
    char component[16];
    char firmware_version[32];
    char firmware_target[40];
    char app_project[40];
    char hardware_type[40];
    char hardware_mac[18];
    char node_name[65];
    char capabilities[16][41];
    uint8_t capability_count;
    bool has_device_location;
    double device_lat;
    double device_lon;
    double device_alt;
    backend_scanner_status_t scanners[2];
    bool scanner_present[2];
    bool clock_valid;
    int64_t epoch_ms;
    char wifi_ssid[33];
    int8_t wifi_rssi;
    bool ap_active;
    uint32_t config_generation;
    uint32_t command_success_count;
    uint32_t command_failure_count;
    uint64_t uptime_ms;
    backend_led_state_t led_state;
    backend_upload_queue_telemetry_t upload_queue;
    backend_upload_telemetry_t upload;
    uint32_t sequence;
} backend_batch_context_t;

typedef struct {
    backend_batch_context_t context;
    char json[BACKEND_UPLOAD_MAX_JSON + 1U];
    /* Builder-owned transaction space; never automatic or shared mutable. */
    char scratch[BACKEND_UPLOAD_MAX_JSON + 1U];
    size_t json_len;
    uint16_t item_count;
    int64_t opened_ms;
    int64_t last_item_ms;
    bool active;
    bool failed;
} backend_upload_builder_t;

void backend_upload_builder_init(
    backend_upload_builder_t *builder,
    const backend_batch_context_t *context,
    int64_t now_ms);

backend_encode_result_t backend_upload_builder_add(
    backend_upload_builder_t *builder,
    const backend_detection_observation_t *observation,
    int64_t now_ms);

bool backend_upload_builder_tick(
    backend_upload_builder_t *builder,
    int64_t now_ms,
    backend_upload_batch_t *out);

bool backend_upload_builder_finish(
    backend_upload_builder_t *builder,
    backend_upload_batch_t *out);

#ifdef __cplusplus
}
#endif
