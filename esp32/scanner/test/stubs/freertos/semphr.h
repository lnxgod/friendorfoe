#pragma once

#include "FreeRTOS.h"

typedef struct {
    int locked;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *storage)
{
    if (storage) {
        storage->locked = 0;
    }
    return storage;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                                        uint32_t wait_ticks)
{
    (void)wait_ticks;
    if (!semaphore || semaphore->locked) {
        return pdFALSE;
    }
    semaphore->locked = 1;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (!semaphore || !semaphore->locked) {
        return pdFALSE;
    }
    semaphore->locked = 0;
    return pdTRUE;
}
