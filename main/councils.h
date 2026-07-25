#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The supported-council table (SPEC.md 3.13.5). Users pick their council by
// name from a dropdown; they should never have to know, choose, or understand
// which backend serves it.
//
// Only councils whose backend is actually implemented belong in here. Listing
// a council we can't yet serve would be worse than omitting it: the user picks
// their own council by name, gets an address wizard, and only then discovers
// nothing works. The 46 South Australian councils are researched (SPEC.md
// 3.13.2) but their backend is not built, so they are deliberately absent.
//
// The enum values are persisted in NVS (waste_api_config_t.backend), so they
// must never be renumbered - append only.

typedef enum {
    // Every council on the white-label "Impact Apps" platform, identified by
    // its waste-info.com.au subdomain. See SPEC.md 3.3.
    COUNCIL_BACKEND_IMPACT_APPS = 0,
    // Bespoke single-council backends, SPEC.md 3.13.3/3.13.4. Each serves
    // exactly one LGA of the critical working group (SPEC.md 1.2).
    COUNCIL_BACKEND_KNOX        = 1,
    COUNCIL_BACKEND_WHITEHORSE  = 2,
    COUNCIL_BACKEND_MERRI_BEK   = 3,
    COUNCIL_BACKEND_MONASH      = 4,
} council_backend_t;

typedef struct {
    const char        *name;   // display name, e.g. "Maribyrnong City Council"
    const char        *state;  // "VIC", "NSW", ...
    council_backend_t  backend;
    // Whatever the backend needs to identify this council. For Impact Apps
    // that's the subdomain; other backends will carry a domain or an opaque
    // id, which is why this is a bare string rather than a subdomain field.
    const char        *param;
} council_t;

// Sorted by state (in STATE_ORDER below), then by name within each state, so
// a grouped <select> renders in one pass with no sorting at runtime.
extern const council_t COUNCILS[];
extern const size_t COUNCIL_COUNT;

// The order states are presented in. VIC first: this project's own councils
// (SPEC.md 1.2) are Victorian, and it's the default selection.
extern const char *const STATE_ORDER[];
extern const size_t STATE_ORDER_COUNT;

#define COUNCIL_DEFAULT_STATE "VIC"

// The council with this Impact Apps subdomain, or NULL if it isn't a listed
// one (which is legitimate - the free-text escape hatch exists precisely so
// unlisted councils on the same platform still work).
const council_t *council_find_impact_apps(const char *subdomain);

// The council with this param, any backend, or NULL. Params are unique across
// the whole table (enforced by test/host/test_councils.c), so no backend
// qualifier is needed.
const council_t *council_find_by_param(const char *param);

// The first council served by this backend, or NULL. Meaningful for the
// bespoke single-council backends; for shared platforms (Impact Apps) it
// returns an arbitrary member, so look up by param instead there.
const council_t *council_find_by_backend(uint8_t backend);

// A human-readable name for an Impact Apps subdomain: the council's display
// name when listed, otherwise the subdomain itself. Never NULL, so it can be
// dropped straight into a format string.
const char *council_display_name(const char *subdomain);

// Full state name for a code ("VIC" -> "Victoria"), or the code itself if
// unrecognised.
const char *council_state_label(const char *state);
