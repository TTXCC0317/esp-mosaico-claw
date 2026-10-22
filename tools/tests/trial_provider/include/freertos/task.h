#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *ctx, unsigned priority, void *out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
