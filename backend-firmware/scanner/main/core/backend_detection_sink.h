#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*backend_detection_consumer_fn)(
    void *context,
    const drone_detection_t *detection,
    int64_t observed_monotonic_ms);

void backend_detection_sink_register(
    backend_detection_consumer_fn consumer, void *context);
bool backend_detection_sink_emit(
    const drone_detection_t *detection, int64_t observed_monotonic_ms);

#ifdef __cplusplus
}
#endif
