#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Brings up Wi-Fi, provisioning the device first if it needs it (SPEC.md 3.4).
//
// Credentials come from NVS (saved by a previous provisioning), falling back
// to the compiled-in Kconfig pair for development. With neither, or when the
// stored network can't be reached at boot, the device enters **AutoAP mode**:
// it raises an open SoftAP named "binlight-XXXX" (last 4 hex digits of its
// station MAC), breathes both LEDs white, and serves a no-JS page at
// http://192.168.4.1/ to pick a network and enter its password. Credentials
// are verified by actually joining before they are persisted, so a typo never
// gets stored.
//
// **Blocks until the device is on a network** - in AutoAP mode that means
// blocking until someone provisions it (or the stored network reappears; it
// is retried in the background throughout). Once connected, later drops are
// retried forever without re-entering AutoAP. Returns ESP_OK when connected,
// or an error only if Wi-Fi itself could not be initialised.
esp_err_t wifi_manager_start(void);

bool wifi_manager_is_connected(void);

// The network the device is currently using, or "" if none. Display only.
const char *wifi_manager_current_ssid(void);

// Erases the stored credentials so the next boot enters AutoAP setup mode.
// Does NOT disconnect or reboot - the caller decides when (the web UI sends
// its confirmation page first, then reboots). Everything else in NVS (the
// schedule, council setup) is untouched.
esp_err_t wifi_manager_forget_credentials(void);
