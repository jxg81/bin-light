#pragma once
#include <stdint.h>
#include "esp_err.h"
#define GPIO_MODE_INPUT        1
#define GPIO_PULLUP_ENABLE     1
#define GPIO_PULLUP_DISABLE    0
#define GPIO_PULLDOWN_ENABLE   1
#define GPIO_PULLDOWN_DISABLE  0
#define GPIO_INTR_DISABLE      0
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
// The harness drives this to simulate presses.
extern int stub_gpio_level;
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline int gpio_get_level(int pin) { (void)pin; return stub_gpio_level; }
