#ifndef BACKEND_TIME_SYNC_H
#define BACKEND_TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_TIME_SOURCE_NONE = 0,
    BACKEND_TIME_SOURCE_SNTP,
    BACKEND_TIME_SOURCE_BACKEND,
} backend_time_source_t;

bool backend_time_parse_response(
    const char *json,
    size_t length,
    int64_t *out_epoch_ms);

backend_time_source_t backend_time_select_source(
    bool sntp_synced,
    int64_t sntp_epoch_ms,
    bool backend_response_valid,
    int64_t backend_epoch_ms,
    int64_t *out_epoch_ms);

#ifdef __cplusplus
}
#endif

#endif
