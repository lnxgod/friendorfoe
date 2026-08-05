#pragma once

#include <stdbool.h>

#include "ble_investigation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*backend_investigation_consumer_fn)(
    void *context, const ble_investigation_chunk_t *chunk);

void backend_investigation_sink_register(
    backend_investigation_consumer_fn consumer, void *context);
bool backend_investigation_sink_emit(
    const ble_investigation_chunk_t *chunk);

#ifdef __cplusplus
}
#endif
