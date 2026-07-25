#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// Must be called once before any other led_state_* function.
esp_err_t led_state_init(void);

// Sets both LEDs to the same colour. Thin wrapper over led_state_set_dual().
// Safe to call from any task.
esp_err_t led_state_set(led_color_t color, uint8_t brightness);

// The two LEDs are wired as a 2-pixel WS2812 chain (LED1's data-out feeds
// LED2) and always illuminate together - this is the only path that can show
// them independent colours, for "dual colour" light mode (see schedule.h).
esp_err_t led_state_set_dual(led_color_t primary, led_color_t secondary, uint8_t brightness);

esp_err_t led_state_off(void);

// Last primary (LED1) colour applied, for UI display only.
led_color_t led_state_get_current(void);

// Runs once at boot, before normal operation: LED1 cycles through the 4
// preset colours (0.5s each), then LED2 does the same, then both light the
// same colour simultaneously - a hardware self-test confirming both LEDs and
// their wiring work. Blocks for ~6 seconds total; leaves both LEDs off when
// done.
void led_state_run_self_test(void);
