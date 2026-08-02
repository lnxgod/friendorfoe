#pragma once

#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void *argument);
typedef void *TaskHandle_t;

TaskHandle_t xTaskCreateStatic(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    StackType_t *stack_buffer,
    StaticTask_t *task_buffer);

void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t task);
