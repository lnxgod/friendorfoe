#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend_dashboard_event.h"
#include "backend_event_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_DASHBOARD_DEFAULT_LIMIT 25U
#define BACKEND_DASHBOARD_MAX_LIMIT 50U

typedef struct {
    uint64_t after;
    size_t limit;
} backend_dashboard_query_t;

bool backend_dashboard_query_parse(
    const char *query,
    backend_dashboard_query_t *out);
size_t backend_dashboard_snapshot_encode_prefix(
    const backend_event_ring_snapshot_t *snapshot,
    char *output,
    size_t capacity);
const char *backend_dashboard_snapshot_suffix(void);
bool backend_dashboard_status_is_redacted(
    const char *json,
    size_t length);
const char *backend_dashboard_page_html(void);

#ifdef __cplusplus
}
#endif
