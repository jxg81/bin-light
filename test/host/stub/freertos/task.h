#pragma once
#include "freertos/FreeRTOS.h"
static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t d, void *p, unsigned pri, TaskHandle_t *h)
{ (void)fn; (void)n; (void)d; (void)p; (void)pri; if (h) *h = (TaskHandle_t)1; return pdPASS; }
static inline void xTaskNotifyGive(TaskHandle_t h) { (void)h; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; return 0; }
static inline void vTaskDelay(TickType_t t) { (void)t; }
