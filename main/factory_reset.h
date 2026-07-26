#pragma once

#include "esp_err.h"

// Erases every persisted setting this device holds and returns it to the
// out-of-the-box state (SPEC.md 3.12): the schedule and colour rules, the
// council/API setup and its cached next-collection, the timezone, and the
// Wi-Fi credentials. After a reset the device comes up in AutoAP setup mode
// (SPEC.md 3.4) with both LEDs breathing white.
//
// This is the single implementation behind both entry points - the web UI's
// confirmed Factory Reset action, and the physical reset button. Any
// "are you sure?" step, or the button's long-press requirement, belongs to
// the caller; by the time this is called the decision has been made.

// Wipes the persisted state without rebooting. Returns the first error
// encountered, but always attempts every step - a failure to clear one store
// must not leave the others behind, since a half-reset device is worse than
// either outcome.
esp_err_t factory_reset_erase(void);

// factory_reset_erase() followed by a reboot. Does not return. Callers that
// need to get a response out first (the web UI) should send it, wait briefly,
// then call this.
void factory_reset_perform(void) __attribute__((noreturn));
