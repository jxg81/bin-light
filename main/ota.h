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
esp_err_t ota_start(const char *url);

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
