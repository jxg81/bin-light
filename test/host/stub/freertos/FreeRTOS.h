#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *TimerHandle_t;

#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdPASS; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdPASS; }
