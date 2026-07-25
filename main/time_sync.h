#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Sets the configured timezone, starts SNTP, and waits (bounded) for the
// first sync. Non-fatal on timeout: logs a warning and returns ESP_ERR_TIMEOUT,
// but the schedule task safely treats pre-sync time as "no window active".
esp_err_t time_sync_start(void);

bool time_sync_is_valid(void);
