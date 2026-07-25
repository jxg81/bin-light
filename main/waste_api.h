#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "schedule.h"

#define WASTE_API_SUBDOMAIN_MAX_LEN 63
#define WASTE_API_LABEL_MAX_LEN     63
#define WASTE_API_MAX_TYPE_RULES    8

// How one API event_type (e.g. "organic", "recycle", "waste") should be
// treated. Discovered and configured via the web UI's "Test API" page, which
// shows the real event_type strings this council's API actually returns
// rather than guessing at a fixed enum. An empty event_type marks an unused
// slot. Any type with no rule here falls back to a sane default (see
// waste_api.c): "waste" ignored, everything else kept using the API's own
// colour - so a fresh, unconfigured setup still behaves sensibly.
typedef struct {
    char              event_type[24];
    bool              ignored;
    schedule_color_t  color;  // used only when not ignored; overrides the API's own colour for this type
} waste_api_type_rule_t;

// Persisted setup for a single council's "Impact Apps" (waste-info.com.au)
// instance. Any council on this platform works by changing subdomain +
// property_id alone - no code changes.
typedef struct {
    uint8_t                 version;
    bool                    enabled;
    char                    council_subdomain[WASTE_API_SUBDOMAIN_MAX_LEN + 1];  // e.g. "maribyrnong"
    uint32_t                property_id;
    char                    property_label[WASTE_API_LABEL_MAX_LEN + 1];  // e.g. "12 Example St, Footscray" - display only
    waste_api_type_rule_t   type_rules[WASTE_API_MAX_TYPE_RULES];
} waste_api_config_t;

// Loads config from NVS only - does not make a network call.
esp_err_t waste_api_init(void);

waste_api_config_t waste_api_get_config(void);

// Validates, persists to NVS, and wakes the poll task to re-fetch immediately.
esp_err_t waste_api_set_config(const waste_api_config_t *cfg);

// Background task that periodically fetches upcoming collection events for
// the configured property.
esp_err_t waste_api_task_start(void);

// Wakes the poll task immediately instead of waiting for its next interval,
// e.g. right after the property is (re)configured.
void waste_api_task_force_check(void);

// The next known dated collection, from the sticky cache (SPEC.md 3.3).
// `year/month/day` is the day the bins are *collected*; bin night is the
// evening before. has_secondary is true only when a second distinct event
// (e.g. recycling AND glass) shares that same date - see SPEC.md 3.7.
typedef struct {
    uint16_t          year;
    uint8_t           month;
    uint8_t           day;
    schedule_color_t  color;
    bool              has_secondary;
    schedule_color_t  secondary_color;
} waste_api_next_event_t;

// The next known dated collection, or false if there isn't one.
//
// "Sticky": a poll that finds a qualifying event overwrites the cache, but a
// poll that finds *nothing* (or fails outright) leaves it alone rather than
// regressing to "unknown". The only thing that invalidates a cached date is
// **the date itself passing** - there is no freshness timer. A network hiccup
// or a lookahead window that briefly didn't reach far enough can no longer
// discard a perfectly good future date, which is exactly how the light used
// to go dark on a night it should have been lit.
//
// Returns false only when the API is disabled, nothing has ever been cached,
// or the cached collection date is now in the past. The cache survives reboots
// (persisted to NVS), so this is not false merely because we haven't polled
// yet this boot.
bool waste_api_get_next_event(waste_api_next_event_t *out);

// The recurring general-waste collection weekday, in `struct tm.tm_wday`
// convention (0=Sunday..6=Saturday) - note the API reports this in ISO-8601
// (Mon=1..Sun=7); the conversion happens on the way in, so callers never see
// the ISO form. Writes to *out_wday and returns true when known.
//
// Deliberately a *separate, independent* signal from waste_api_get_next_event()
// rather than part of the same cache entry: a weekly recurrence doesn't expire
// the way a specific dated event does, so it is never aged out - once learned,
// it stays until a later poll reports a different one. Used so a plain
// general-waste night can still be identified in weeks with no other event
// (see SPEC.md 3.7).
bool waste_api_get_waste_weekday(uint8_t *out_wday);

// --- Address lookup, for the setup UI. Explicit subdomain param (not the
// persisted config) so the UI can browse before saving. Blocking HTTPS calls -
// call from a task with headroom (e.g. the web server), not a tight loop.
// Returns the number of entries filled into out[], or -1 on failure. ---

typedef struct { uint32_t id; char name[48]; } waste_api_locality_t;
typedef struct { uint32_t id; char name[48]; } waste_api_street_t;
typedef struct { uint32_t id; char name[64]; } waste_api_property_t;

int waste_api_fetch_localities(const char *subdomain, waste_api_locality_t *out, int max_out);
int waste_api_fetch_streets(const char *subdomain, uint32_t locality_id, waste_api_street_t *out, int max_out);
int waste_api_fetch_properties(const char *subdomain, uint32_t street_id, waste_api_property_t *out, int max_out);

// --- Diagnostics: on-demand fetch for the web UI's "Test API" feature.
// Bypasses the poll cache entirely - a fresh, blocking HTTPS call every time
// it's invoked, so the user can verify their setup without waiting for the
// next poll or reading serial logs. ---

typedef struct {
    uint16_t          year;
    uint8_t           month;
    uint8_t           day;
    schedule_color_t  color;
    char              event_type[24];  // e.g. "organic", "recycle" - display only
} waste_api_event_t;

// Fetches every dated event in [today, today+lookahead_days], sorted by date
// ascending, into out[] - deliberately RAW and unfiltered (includes "waste"
// and every other event_type the API returns, using the API's own colour),
// so the web UI can show the user real data to build a type_rules mapping
// against rather than guessing. Returns the count filled, or -1 on failure
// (network/parse error - distinct from a successful fetch that simply found
// nothing, which returns 0).
int waste_api_fetch_upcoming(const char *subdomain, uint32_t property_id,
                              int lookahead_days, waste_api_event_t *out, int max_out);
