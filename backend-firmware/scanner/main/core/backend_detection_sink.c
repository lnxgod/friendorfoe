#include "backend_detection_sink.h"

#include <stddef.h>

static backend_detection_consumer_fn s_consumer;
static void *s_context;

void backend_detection_sink_register(
    backend_detection_consumer_fn consumer, void *context)
{
    s_consumer = consumer;
    s_context = consumer != NULL ? context : NULL;
}

bool backend_detection_sink_emit(
    const drone_detection_t *detection, int64_t observed_monotonic_ms)
{
    if (detection == NULL || s_consumer == NULL) {
        return false;
    }

    drone_detection_t snapshot = *detection;
    return s_consumer(s_context, &snapshot, observed_monotonic_ms);
}
