#pragma once

#include "esp_err.h"

// Registers the schedule UI (GET /) and save handler (POST /save), and
// starts the HTTP server. Call after schedule_init().
esp_err_t web_server_start(void);
