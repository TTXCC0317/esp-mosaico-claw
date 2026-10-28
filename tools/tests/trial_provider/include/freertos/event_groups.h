#pragma once
#include "FreeRTOS.h"
typedef struct host_event *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t event);
EventBits_t xEventGroupSetBits(EventGroupHandle_t event, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t event, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t event, EventBits_t bits, int clear, int all, TickType_t ticks);
