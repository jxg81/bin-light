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
