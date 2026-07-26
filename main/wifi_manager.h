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
// http://binlight.local/ (or http://192.168.4.1/ where mDNS doesn't resolve -
// notably Android) to pick a network and enter its password. Credentials are
// verified by actually joining before they are persisted, so a typo never
// gets stored.
//
// mDNS answers on the setup AP only because main() starts it *before* this
// function, which blocks for the whole AutoAP session - see the note there.
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

// Disarms the auto-reconnect logic and stops the Wi-Fi driver, in that order.
//
// Call before anything that tears down the Wi-Fi configuration and then
// restarts - specifically factory_reset_erase(), which calls
// esp_wifi_restore(). Without this, the STA_DISCONNECTED handler is still
// armed when the config is pulled out from under it and immediately calls
// esp_wifi_connect() against credentials that no longer exist, on the event
// task, while the caller is racing toward esp_restart() on another. That race
// is what left a factory reset hung and needing a power cycle.
//
// Idempotent, and safe to call whatever state Wi-Fi is in (including never
// started). After this the device has no network until it reboots - which is
// the only thing callers do next.
void wifi_manager_shutdown(void);
