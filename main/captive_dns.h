#pragma once

#include "esp_err.h"

// Captive-portal DNS responder for AutoAP setup mode (SPEC.md 3.4).
//
// While the device is running its own SoftAP it answers **every** A query with
// its own address, so any hostname a phone asks for resolves to the setup
// page. That does two things:
//
//   1. `binlight.local` works even on clients with no mDNS (Android's browser
//      being the one that matters). The name resolves through this hijack
//      rather than through multicast DNS, so it no longer depends on the
//      client supporting Bonjour.
//   2. The phone's own connectivity probe - which asks for a Google, Apple or
//      Microsoft hostname the moment it joins - lands on this device instead
//      of failing. Combined with the 302 the provisioning server returns for
//      unknown paths, the OS decides it is behind a captive portal and opens
//      the setup page by itself.
//
// **Only ever run this while the SoftAP is up.** A DNS server that answers
// every query with one address is indistinguishable from an attack if it is
// still listening once the device is on a real network. `run_autoap()` starts
// it with the AP and stops it in the same teardown that stops the
// provisioning server.
//
// Answers carry **TTL 0** deliberately: a phone that caches
// `binlight.local -> 192.168.4.1` and then joins the home network would fail
// to reach the device by name until the entry expired.

// Binds UDP :53 on all interfaces and spawns the responder task. The address
// handed out is read from the SoftAP netif at start, falling back to
// 192.168.4.1 if that lookup fails. Idempotent - a second call while running
// is a no-op returning ESP_OK.
esp_err_t captive_dns_start(void);

// Stops the responder and closes the socket. Blocks briefly (up to one
// receive timeout) so the task is really gone before the AP is torn down.
// Safe to call when not running.
void captive_dns_stop(void);
