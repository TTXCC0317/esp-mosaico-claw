#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define BIT(n) (1U << (n))
