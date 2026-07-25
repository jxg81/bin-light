#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SCHEDULE_MAX_COLOR_RULES 3

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} schedule_color_t;

// One independently-periodic colour rule: this colour is "due" every
// frequency_weeks weeks, counted from its own first occurrence - councils
// often run bin types on genuinely different, unrelated cycles (e.g.
// recycling every 2 weeks from one date, organics every 3 weeks from
// another), which a single shared rotation length can't express.
typedef struct {
    bool              enabled;
    schedule_color_t  color;
    uint16_t          first_year;
    uint8_t           first_month;      // 1-12
    uint8_t           first_day;        // 1-31
    uint8_t           frequency_weeks;  // 1-4
} schedule_color_rule_t;

// Both LEDs (a 2-pixel WS2812 chain, see SPEC.md 3.7) always illuminate
// together - this only decides whether they show the same colour or two
// independent ones. Not an "enable second LED" toggle: the second LED is
// always physically lit, in either mode.
#define LIGHT_MODE_SINGLE_COLOUR 0  // both LEDs show the same colour
#define LIGHT_MODE_DUAL_COLOUR   1  // LED2 shows a second, independent colour

typedef struct {
    uint8_t                version;
    bool                   enabled;
    uint8_t                bin_night_weekday;   // 0=Sunday..6=Saturday, struct tm.tm_wday convention -
                                                 // the evening the bins go out; the schedule is anchored
                                                 // on this day, not the (following) collection day
    uint16_t               start_minute;        // minutes since midnight, when the light turns on
                                                 // on bin_night_weekday
    uint8_t                duration_hours;      // how many hours the light stays on; wraps past
                                                 // midnight into the following day if start + duration > 24h
    uint8_t                brightness;          // 0-255
    uint8_t                light_mode;          // LIGHT_MODE_SINGLE_COLOUR or LIGHT_MODE_DUAL_COLOUR
    schedule_color_t       secondary_default_color;  // LED2's colour in dual mode when nothing else
                                                       // distinguishes it from a plain general-waste
                                                       // night (default red) - see SPEC.md 3.7
    schedule_color_rule_t  rules[SCHEDULE_MAX_COLOR_RULES];  // first due rule (in array order) wins
} schedule_t;

// Loads the schedule from NVS, or seeds+persists an all-disabled default if
// absent, the wrong size, or an unrecognised version.
esp_err_t schedule_init(void);

schedule_t schedule_get(void);

// Validates, persists to NVS, and updates the in-RAM copy.
esp_err_t schedule_set(const schedule_t *new_schedule);

// Spawns the background task that evaluates the schedule (and, when
// configured and reachable, the external bin-collection API) against the
// current time and drives led_state_set()/led_state_off() accordingly.
esp_err_t schedule_task_start(void);

// Wakes the background task immediately instead of waiting for its next poll,
// for snappy feedback right after a schedule edit is saved.
void schedule_task_force_check(void);

typedef struct {
    bool              known;
    uint16_t          year;       // the day the bins are COLLECTED; bin night is the evening before
    uint8_t           month;
    uint8_t           day;
    schedule_color_t  primary;
    schedule_color_t  secondary;  // dual-colour mode only; equals primary when nothing distinguishes them
    // True when nothing rotating is due and this is a plain general-waste
    // collection. General waste is weekly and needs no reminder of its own, so
    // the live evaluator lights these nights only in dual-colour mode (where
    // LED2 *is* the general-waste indicator) - but "Display Next Collection"
    // shows them regardless, since it answers "what's next?", not "should the
    // light be on?".
    bool              waste_only;
} schedule_next_t;

// The single resolver for "what is the next collection, and what colour(s)
// does it show" - used by both the live evaluator and the "Display Next
// Collection" button (SPEC.md 3.3/3.8) so the two can't drift apart, which is
// exactly how they broke once before.
//
// Resolution order:
//   1. The API's next known dated event (sticky cache, not stale).
//   2. The next plain general-waste date, from the API's recurring weekday.
//      Whichever of (1)/(2) is sooner wins; on a tie (1) wins, being the more
//      specific answer.
//   3. Otherwise the manual / fallback schedule's next bin night, if enabled.
//   4. Otherwise genuinely unknown (`known == false`) - the *only* acceptable
//      unknown state, reached only when the API is stale/disabled AND the
//      manual fallback is disabled too.
//
// Read-only: touches no persisted or in-RAM state. Returns known == false if
// the clock hasn't synced yet, since every branch needs today's date.
schedule_next_t schedule_get_next_collection(void);

// Lights both LEDs with schedule_get_next_collection()'s colours, respecting
// light_mode, for a fixed 30 seconds, then hands control back to the real
// evaluator via schedule_task_force_check(). Safe to call again while a
// preview is already running - restarts the timer with a fresh result. No-op
// only when the next collection is genuinely unknown.
void schedule_test_trigger(void);
