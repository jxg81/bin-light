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

// Starts a background task making both LEDs breathe the given colour - a
// slow, calm brightness ramp up and down (SPEC.md 3.4's AutoAP "waiting for
// setup" indicator; a device-state signal, so both LEDs always move together
// and light_mode does not apply). No-op if already breathing. The caller must
// ensure nothing else is driving the LEDs while breathing runs - in practice
// this is only used during boot-time provisioning, before the schedule task
// exists.
void led_state_breathe_start(led_color_t color);

// Stops the breathing task (blocking briefly until it has exited) and turns
// both LEDs off. Safe to call when not breathing.
void led_state_breathe_stop(void);
