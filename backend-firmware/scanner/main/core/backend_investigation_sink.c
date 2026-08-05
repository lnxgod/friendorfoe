#include "backend_investigation_sink.h"

#include <stddef.h>

static backend_investigation_consumer_fn s_consumer;
static void *s_context;

void backend_investigation_sink_register(
    backend_investigation_consumer_fn consumer, void *context)
{
    s_consumer = consumer;
    s_context = consumer != NULL ? context : NULL;
}

bool backend_investigation_sink_emit(const ble_investigation_chunk_t *chunk)
{
    if (chunk == NULL || s_consumer == NULL) {
        return false;
    }

    ble_investigation_chunk_t snapshot = *chunk;
    return s_consumer(s_context, &snapshot);
}
