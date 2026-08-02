#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef unsigned UBaseType_t;

typedef struct {
    uint32_t opaque;
} StaticTask_t;

#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
