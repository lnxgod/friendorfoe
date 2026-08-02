#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool backend_ingest_ack_validate(
    const char *json,
    size_t length,
    const char *expected_device_id,
    uint16_t expected_item_count);

#ifdef __cplusplus
}
#endif
