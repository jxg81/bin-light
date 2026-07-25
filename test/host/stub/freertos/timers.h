#pragma once
#include "freertos/FreeRTOS.h"
static inline TimerHandle_t xTimerCreate(const char *n, TickType_t p, BaseType_t r, void *id, void (*cb)(TimerHandle_t))
{ (void)n;(void)p;(void)r;(void)id;(void)cb; return (TimerHandle_t)1; }
static inline BaseType_t xTimerReset(TimerHandle_t t, TickType_t b) { (void)t;(void)b; return pdPASS; }
