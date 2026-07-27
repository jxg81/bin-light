#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Over-the-air firmware updates from a static file store (SPEC.md 3.5).
//
// There is no backend: the device fetches a small JSON manifest over HTTPS,
// compares its `version` against the running firmware's, and if they differ
// streams the image named by `url` straight into the spare OTA slot. Both the
// manifest and the image are served by GitHub (raw.githubusercontent.com and
// a Release asset), so the whole update path is a git push and a release -
// nothing to host and nothing to keep running.
//
// Manifest shape:
//   { "version": "1.1.0",
//     "url": "https://github.com/<owner>/<repo>/releases/download/v1.1.0/bin-light.bin",
//     "notes": "one line, shown in the UI" }
//
// Version comparison is deliberately **inequality, not ordering**: any change
// from the running version is an update. That makes deliberate downgrades work
// (publish the old version in the manifest and every device rolls back), which
// matters far more here than refusing to go backwards.

#define OTA_VERSION_MAX_LEN 31
#define OTA_NOTES_MAX_LEN   95

typedef struct {
    bool available;                        // a version different from ours is published
    char version[OTA_VERSION_MAX_LEN + 1];
    char notes[OTA_NOTES_MAX_LEN + 1];
    char url[256];
} ota_manifest_t;

// The running firmware's version, from esp_app_desc_t (set by PROJECT_VER,
// which CMake derives from `git describe`). Never NULL.
const char *ota_running_version(void);

// Fetches and parses the manifest. Blocking HTTPS - call from the web server
// task, not the schedule tick. Returns ESP_OK if the manifest was read (check
// out->available for whether an update is actually published), or an error if
// it could not be fetched or parsed.
esp_err_t ota_check(ota_manifest_t *out);

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING,   // downloading/flashing
    OTA_STATE_SUCCESS,   // flashed, awaiting reboot
    OTA_STATE_FAILED,
} ota_state_t;

// Starts an update in a background task and returns immediately - the image
// is ~1.3MB over TLS, far too slow to hold an HTTP response open for. Poll
// ota_get_state() to follow it. Refuses to start if one is already running.
//
// `restart_when_done` decides what happens after a successful flash, and the
// two callers want opposite things:
//
//   true  - manual install from the web UI. Someone pressed a button and is
//           watching. Flashing and then sitting on the old firmware until they
//           find a second button reads as "nothing happened" - which is
//           precisely how it read the first time this was exercised on
//           hardware, to the point of the update being installed twice.
//   false - the automatic updater, which restarts on its own terms: it waits
//           for schedule_light_is_on() to go false so an update never
//           interrupts a bin-night display. Nobody is watching, so a few
//           hours' delay costs nothing and a badly-timed reboot does.
esp_err_t ota_start(const char *url, bool restart_when_done);

ota_state_t ota_get_state(void);

// Human-readable detail for the current state (progress percentage while
// running, the reason on failure). Never NULL.
const char *ota_get_message(void);

// Confirms the running image is healthy so the bootloader stops treating it
// as pending-verification and won't roll back on the next boot. Call once the
// device has demonstrably worked - after Wi-Fi is up and the web server is
// serving - never straight after boot, or rollback protects nothing.
//
// No-op unless the image is actually pending verification.
void ota_mark_valid(void);

// Arms the rollback watchdog. **Call early in app_main(), before anything that
// can block** - `wifi_manager_start()` above all, which waits indefinitely in
// AutoAP mode. Calling it after that point defeats the entire purpose.
//
// Why this exists: bootloader rollback is decided *at boot*, so it needs a
// reset to happen at all. A new image that **crashes** supplies one and is
// reverted automatically (verified on hardware, SPEC.md 3.5.1). A new image
// that **boots but hangs** - most plausibly one that can no longer reach
// Wi-Fi, so it sits in AutoAP waiting for a human - never reboots, so nothing
// ever triggers the revert. It would wait forever, and that is the failure
// mode that costs a physical visit.
//
// So: if the running image is still PENDING_VERIFY after a generous window,
// assume startup will never complete and force the rollback. No-op on an
// ordinary boot of an already-confirmed image. ota_mark_valid() disarms it.
//
// **Deliberately biased toward firing.** A false positive - rolling back a
// good image because the router happened to be down at the wrong moment -
// costs one wasted update cycle and self-corrects, since the device simply
// installs it again once the network returns. A false negative is a house
// call. The window is sized so ordinary transient outages don't trip it.
void ota_rollback_watchdog_start(void);

// --- automatic updates ---
//
// **On by default.** These devices live in other people's houses; the whole
// reason OTA exists (SPEC.md 1.1) is that a council API breaking otherwise
// means visiting each one with a USB cable. Leaving updates for someone else
// to notice and click would defeat that, so the default is that they simply
// happen.
//
// The updater checks daily, installs anything published, and then waits for
// the light to be off before restarting - never interrupting a bin-night
// display. Rollback (see ota_mark_valid) is what makes automatic installation
// safe: a build that cannot get back onto Wi-Fi is reverted by the bootloader
// without anyone touching the device.

bool ota_auto_update_enabled(void);
esp_err_t ota_set_auto_update(bool enabled);

// Starts the background checker. Safe to call when auto-update is disabled -
// the task still runs but does nothing, so the setting can be toggled at
// runtime without a reboot.
esp_err_t ota_auto_task_start(void);
