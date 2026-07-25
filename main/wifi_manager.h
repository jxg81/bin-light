#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Blocks until the first connection succeeds or CONFIG_BINLIGHT_WIFI_MAXIMUM_RETRY
// attempts have failed. After the first successful connection, reconnection on
// later drops is retried forever in the background regardless of this return.
esp_err_t wifi_manager_start(void);

bool wifi_manager_is_connected(void);
