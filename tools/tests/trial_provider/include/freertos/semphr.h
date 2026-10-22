#pragma once
#include "FreeRTOS.h"
typedef struct host_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks);
int xSemaphoreGive(SemaphoreHandle_t lock);
void vSemaphoreDelete(SemaphoreHandle_t lock);
