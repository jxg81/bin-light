#pragma once

#include "esp_err.h"

// The physical reset button (SPEC.md 3.12). One button, two actions chosen by
// how long it is held:
//
//   held  3s ..< 10s  -> restart (nothing is lost)
//   held >= 10s       -> factory reset (everything is lost)
//   held  < 3s        -> nothing, deliberately
//
// The action fires on **release**, and the LEDs show which action is armed
// while the button is held, so neither one has to be timed by feel: nothing
// until 3s, then blue for restart, then red for factory reset. Letting go
// below 3s is always a safe no-op.
//
// Wiring: a momentary push button between the pin and GND. The input is
// active-low with the internal pull-up on, so no external resistor is needed
// - and on a breadboard a bare jumper from the pin to GND is a working stand-
// in for the button (touch to press, pull away to release, which is what
// fires the action).
//
// No-op (and logs) if CONFIG_BINLIGHT_RESET_BUTTON_GPIO is negative.
// Call after led_state_init() and schedule_task_start().
esp_err_t buttons_start(void);
