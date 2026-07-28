#pragma once

#include "esp_err.h"

#define SETTINGS_TZ_MAX_LEN 63

// Loads the persisted timezone from NVS (falling back to the Kconfig default
// on first boot) and applies it immediately via setenv("TZ", ...)/tzset().
// Call before time_sync_start() so SNTP-driven local time is correct from the
// first sync onward.
esp_err_t settings_init(void);

// Returns the current POSIX TZ string. Never NULL, always null-terminated.
const char *settings_get_tz(void);

// Validates, persists to NVS, and applies the new TZ immediately - no reboot
// needed for it to take effect.
esp_err_t settings_set_tz(const char *tz);

// The POSIX TZ string for an Australian state code ("VIC", "QLD", ...), or
// NULL if the code isn't one we know.
//
// Exists so the setup wizard can set the timezone from the council the user
// just picked, instead of leaving it as a separate step they have no reason
// to go looking for - an unset timezone is one of the two things that leave a
// freshly-reset light showing nothing at all (SPEC.md 4).
//
// Keyed on the state string rather than on a council, so it lives here with
// the rest of the TZ handling and needs no change to councils.c.
const char *settings_tz_for_state(const char *state);
