#include "schedule.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "esp_log.h"

#include "led_state.h"
#include "time_sync.h"
#include "waste_api.h"

static const char *TAG = "schedule";

#define SCHEDULE_NVS_NAMESPACE  "binlight"
#define SCHEDULE_NVS_KEY        "schedule_v5"
#define SCHEDULE_STRUCT_VERSION 5
#define SCHEDULE_POLL_MS        30000
// SPEC.md 3.8: long enough to check the colours by eye, short enough that an
// accidental press isn't an effective always-on override.
#define TEST_PREVIEW_DURATION_MS (30UL * 1000)

// Brightness is a multiplier applied to every colour channel in
// led_state_set_dual(), so 0 renders the light black no matter which colour
// the schedule resolved - indistinguishable from broken hardware. Never let
// it reach 0: "off" is expressed by the schedule not being due, not by a
// zero multiplier.
#define SCHEDULE_DEFAULT_BRIGHTNESS 128  // 50%
#define SCHEDULE_MIN_BRIGHTNESS     10   // ~4%: dim, but unambiguously lit

static schedule_t s_schedule;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task_handle;
static TimerHandle_t s_test_timer;

// What the evaluator last decided, so the action button can tell "the light
// is on for a collection" from "the light is off". Written only by
// schedule_task_fn.
static bool s_light_on;
static uint16_t s_light_year;
static uint8_t s_light_month;
static uint8_t s_light_day;

// The collection currently dismissed, if any (SPEC.md 3.12). Self-clearing:
// once the resolver's answer moves to a different date, this is dropped.
static bool s_suppress_active;
static uint16_t s_suppress_year;
static uint8_t s_suppress_month;
static uint8_t s_suppress_day;

static schedule_t default_schedule(void)
{
    schedule_t s = {0};
    s.version = SCHEDULE_STRUCT_VERSION;
    s.start_minute = 15 * 60;   // 3:00pm
    s.duration_hours = 20;
    s.brightness = SCHEDULE_DEFAULT_BRIGHTNESS;
    s.light_mode = LIGHT_MODE_SINGLE_COLOUR;
    s.secondary_default_color = (schedule_color_t){255, 0, 0}; // red, "general waste"
    return s;
}

static esp_err_t persist_schedule(const schedule_t *sched)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, SCHEDULE_NVS_KEY, sched, sizeof(*sched));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist schedule: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t schedule_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s), using defaults", esp_err_to_name(err));
        s_schedule = default_schedule();
        return persist_schedule(&s_schedule);
    }

    size_t required_size = 0;
    schedule_t loaded;
    err = nvs_get_blob(handle, SCHEDULE_NVS_KEY, NULL, &required_size);
    if (err == ESP_OK && required_size == sizeof(schedule_t)) {
        err = nvs_get_blob(handle, SCHEDULE_NVS_KEY, &loaded, &required_size);
        if (err == ESP_OK && loaded.version == SCHEDULE_STRUCT_VERSION) {
            s_schedule = loaded;
            nvs_close(handle);

            // Self-heal, not a schema change: devices flashed before the
            // brightness default existed stored a valid v5 blob with
            // brightness 0, which renders the light permanently black. Repair
            // it in place rather than bumping SCHEDULE_STRUCT_VERSION, which
            // would needlessly discard the rest of a working configuration.
            if (s_schedule.brightness < SCHEDULE_MIN_BRIGHTNESS) {
                ESP_LOGW(TAG, "stored brightness was %u, raising to default %u",
                         (unsigned)s_schedule.brightness, SCHEDULE_DEFAULT_BRIGHTNESS);
                s_schedule.brightness = SCHEDULE_DEFAULT_BRIGHTNESS;
                persist_schedule(&s_schedule);
            }

            ESP_LOGI(TAG, "loaded schedule from NVS");
            return ESP_OK;
        }
    }
    nvs_close(handle);

    ESP_LOGW(TAG, "no valid stored schedule found, seeding defaults");
    s_schedule = default_schedule();
    return persist_schedule(&s_schedule);
}

schedule_t schedule_get(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    schedule_t copy = s_schedule;
    xSemaphoreGive(s_mutex);
    return copy;
}

esp_err_t schedule_set(const schedule_t *new_schedule)
{
    schedule_t to_store = *new_schedule;
    to_store.version = SCHEDULE_STRUCT_VERSION;
    if (to_store.bin_night_weekday > 6) {
        to_store.bin_night_weekday = 6;
    }
    if (to_store.duration_hours < 1) {
        to_store.duration_hours = 1;
    } else if (to_store.duration_hours > 23) {
        to_store.duration_hours = 23;
    }
    if (to_store.light_mode != LIGHT_MODE_DUAL_COLOUR) {
        to_store.light_mode = LIGHT_MODE_SINGLE_COLOUR;
    }
    if (to_store.brightness < SCHEDULE_MIN_BRIGHTNESS) {
        to_store.brightness = SCHEDULE_MIN_BRIGHTNESS;
    }
    for (int i = 0; i < SCHEDULE_MAX_COLOR_RULES; i++) {
        schedule_color_rule_t *r = &to_store.rules[i];
        if (r->frequency_weeks < 1) {
            r->frequency_weeks = 1;
        } else if (r->frequency_weeks > 4) {
            r->frequency_weeks = 4;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_schedule = to_store;
    xSemaphoreGive(s_mutex);

    return persist_schedule(&to_store);
}

// (The old weekday-keyed window checks lived here. Both paths now resolve to a
// concrete collection date first - see schedule_get_next_collection() - so
// is_window_active_for_date() below is the only window check left.)

// Whole days between two noon-normalized calendar dates via mktime(), so DST
// transitions can't shift the count by an hour across a day boundary.
static long days_between(int from_year, int from_month, int from_day, struct tm to_tm)
{
    struct tm from_tm = {0};
    from_tm.tm_year = from_year - 1900;
    from_tm.tm_mon = from_month - 1;
    from_tm.tm_mday = from_day;
    from_tm.tm_hour = 12;
    from_tm.tm_isdst = -1;

    to_tm.tm_hour = 12;
    to_tm.tm_min = 0;
    to_tm.tm_sec = 0;
    to_tm.tm_isdst = -1;

    time_t from_time = mktime(&from_tm);
    time_t to_time = mktime(&to_tm);
    if (from_time == (time_t)-1 || to_time == (time_t)-1) {
        return 0;
    }

    // Round to the nearest whole day rather than dividing. Normalising both
    // ends to local noon keeps the *date* from flipping, but it does not make
    // the gap an exact multiple of 86400: across a DST change the two noons
    // are 23 or 25 hours apart. C's integer division truncates toward zero, so
    // a -23h gap would come out as 0 days instead of -1 - i.e. on the eve of a
    // collection that falls on the DST-start Sunday, the light would not come
    // on. Adding half a day before dividing (with the sign handled explicitly,
    // since truncation is asymmetric for negatives) absorbs that +/-1h.
    long secs = (long)(to_time - from_time);
    return (secs >= 0) ? (secs + 43200) / 86400 : -((-secs + 43200) / 86400);
}

// True if this rule's colour applies during the calendar week containing
// tm_now, i.e. a whole number of frequency_weeks have elapsed since its own
// first occurrence.
static bool rule_due(const schedule_color_rule_t *r, struct tm tm_now)
{
    if (!r->enabled || r->frequency_weeks == 0) {
        return false;
    }
    long days = days_between(r->first_year, r->first_month, r->first_day, tm_now);
    if (days < 0) {
        return false; // first occurrence hasn't happened yet
    }
    return (days / 7) % r->frequency_weeks == 0;
}

// Up to two rules (in array order) due on tm_at - single-colour mode only
// ever uses the first; dual-colour mode uses both, so two rules landing the
// same week (e.g. a 2-week and a 3-week cycle) can each get their own LED
// instead of one silently winning.
static void find_due_rules(const schedule_t *s, struct tm tm_at,
                            const schedule_color_rule_t **out_primary, const schedule_color_rule_t **out_secondary)
{
    *out_primary = NULL;
    *out_secondary = NULL;
    for (int i = 0; i < SCHEDULE_MAX_COLOR_RULES; i++) {
        if (!rule_due(&s->rules[i], tm_at)) {
            continue;
        }
        if (*out_primary == NULL) {
            *out_primary = &s->rules[i];
        } else {
            *out_secondary = &s->rules[i];
            break;
        }
    }
}

// tm_now shifted forward by `days` calendar days, noon-normalized to dodge
// DST-boundary edge cases (same technique as days_between()).
static struct tm date_plus_days(struct tm tm_now, int days)
{
    struct tm t = tm_now;
    t.tm_hour = 12;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    tt += (time_t)days * 86400;
    struct tm result;
    localtime_r(&tt, &result);
    return result;
}

// True if the light's on-window is active right now for a collection falling
// on the given date. Bin night is the evening *before* event_date: the light
// turns on at start_minute that evening and stays on for duration_hours, which
// may wrap past midnight into event_date itself.
static bool is_window_active_for_date(const schedule_t *s, uint16_t event_year, uint8_t event_month, uint8_t event_day,
                                       struct tm tm_now, int minute_of_day)
{
    long day_diff = days_between(event_year, event_month, event_day, tm_now); // today - event_date, in days
    int end_minute = (int)s->start_minute + (int)s->duration_hours * 60;      // may exceed 1440

    if (day_diff == -1) {
        if (end_minute <= 1440) {
            return minute_of_day >= s->start_minute && minute_of_day < end_minute;
        }
        return minute_of_day >= s->start_minute;
    }
    if (day_diff == 0 && end_minute > 1440) {
        return minute_of_day < (end_minute - 1440);
    }
    return false;
}

// Days from today to the next occurrence of target_wday as a collection day
// (0..7). The only subtle case is off == 0, i.e. today *is* a collection day:
// its window opened last night, so it is either still running (when the window
// wraps past midnight) or already finished. While it's still running today is
// genuinely the current answer; once it closes, the honest answer is next
// week's, so this returns 7 rather than pointing at a collection that's been
// and gone. Getting this wrong strands the answer on a past date for a whole
// week, which is what the host test "once closed, it rolls to next week"
// pins down.
static int days_to_next_collection(const schedule_t *s, int today_wday, int target_wday, int minute_of_day)
{
    int off = (target_wday - today_wday + 7) % 7;
    if (off == 0) {
        int end_minute = (int)s->start_minute + (int)s->duration_hours * 60;
        bool wrap_still_open = (end_minute > 1440) && (minute_of_day < end_minute - 1440);
        if (!wrap_still_open) {
            return 7;
        }
    }
    return off;
}

schedule_next_t schedule_get_next_collection(void)
{
    schedule_next_t result = {0};

    // Every branch below needs today's date to answer "next".
    if (!time_sync_is_valid()) {
        return result;
    }

    schedule_t s = schedule_get();
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int minute_of_day = tm_now.tm_hour * 60 + tm_now.tm_min;

    // (1) The API's next known dated event.
    waste_api_next_event_t ev;
    bool have_event = waste_api_get_next_event(&ev);

    // (2) The next plain general-waste collection, from the recurring weekday.
    // Note this weekday is the *collection* day, exactly like a dated event's
    // date - bin night is its eve in both cases. Treating it as bin night (as
    // the old evaluator did) lit the light a day late.
    uint8_t waste_wday = 0;
    struct tm waste_tm = tm_now;
    bool have_waste = waste_api_get_waste_weekday(&waste_wday);
    if (have_waste) {
        waste_tm = date_plus_days(tm_now, days_to_next_collection(&s, tm_now.tm_wday, waste_wday, minute_of_day));
    }

    if (have_event || have_waste) {
        bool use_event = have_event;
        if (have_event && have_waste) {
            // days_between(date, today) is today-minus-date, so the *smaller*
            // value is the later date. Negate to get "days from today", where
            // smaller means sooner. Ties go to the dated event: it's the more
            // specific answer, and it carries a real colour.
            long ev_days = -days_between(ev.year, ev.month, ev.day, tm_now);
            long waste_days = -days_between(waste_tm.tm_year + 1900, waste_tm.tm_mon + 1, waste_tm.tm_mday, tm_now);
            use_event = (ev_days <= waste_days);
        }

        result.known = true;
        if (use_event) {
            result.year = ev.year;
            result.month = ev.month;
            result.day = ev.day;
            result.primary = ev.color;
            result.secondary = ev.has_secondary ? ev.secondary_color : s.secondary_default_color;
            result.waste_only = false;
        } else {
            result.year = (uint16_t)(waste_tm.tm_year + 1900);
            result.month = (uint8_t)(waste_tm.tm_mon + 1);
            result.day = (uint8_t)waste_tm.tm_mday;
            result.primary = s.secondary_default_color;
            result.secondary = s.secondary_default_color;
            result.waste_only = true;
        }
        return result;
    }

    // (3) Manual / fallback schedule. bin_night_weekday is the night the bins
    // go out, so the collection is the following day - converting here is what
    // lets one date-keyed window check serve both this and the API paths.
    if (s.enabled) {
        int collection_wday = (s.bin_night_weekday + 1) % 7;
        struct tm coll = date_plus_days(tm_now,
            days_to_next_collection(&s, tm_now.tm_wday, collection_wday, minute_of_day));

        const schedule_color_rule_t *due_primary = NULL;
        const schedule_color_rule_t *due_secondary = NULL;
        find_due_rules(&s, coll, &due_primary, &due_secondary);

        result.known = true;
        result.year = (uint16_t)(coll.tm_year + 1900);
        result.month = (uint8_t)(coll.tm_mon + 1);
        result.day = (uint8_t)coll.tm_mday;
        if (due_primary != NULL) {
            result.primary = due_primary->color;
            result.secondary = (due_secondary != NULL) ? due_secondary->color : s.secondary_default_color;
            result.waste_only = false;
        } else {
            result.primary = s.secondary_default_color;
            result.secondary = s.secondary_default_color;
            result.waste_only = true;
        }
        return result;
    }

    // (4) API stale/disabled and the manual fallback switched off: the one
    // genuinely-unknown case.
    return result;
}

// One pass of the evaluator: work out what the light should be doing right
// now and drive it. Split out of the task loop so the host tests can step it
// deterministically (test/host/test_resolver.c) instead of re-deriving the
// decision and drifting from it.
static void schedule_evaluate_once(void)
{
    {
        if (time_sync_is_valid()) {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            int minute_of_day = tm_now.tm_hour * 60 + tm_now.tm_min;

            schedule_t snapshot = schedule_get();
            bool dual = (snapshot.light_mode == LIGHT_MODE_DUAL_COLOUR);
            schedule_next_t next = schedule_get_next_collection();

            // A dismissal applies to one specific collection; as soon as the
            // resolver's answer moves on, it has served its purpose and is
            // dropped. Doing this here (rather than on a timer) is what makes
            // the feature self-clearing.
            if (s_suppress_active && next.known &&
                !(next.year == s_suppress_year && next.month == s_suppress_month &&
                  next.day == s_suppress_day)) {
                ESP_LOGI(TAG, "dismissal of %04u-%02u-%02u cleared - next collection is now %04u-%02u-%02u",
                         (unsigned)s_suppress_year, (unsigned)s_suppress_month, (unsigned)s_suppress_day,
                         (unsigned)next.year, (unsigned)next.month, (unsigned)next.day);
                s_suppress_active = false;
            }
            bool suppressed = s_suppress_active && next.known &&
                              next.year == s_suppress_year && next.month == s_suppress_month &&
                              next.day == s_suppress_day;

            // General waste is weekly and needs no reminder of its own, so a
            // waste-only night lights up in dual-colour mode only, where LED2
            // *is* the general-waste indicator (SPEC.md 3.7).
            bool worth_lighting = next.known && !(next.waste_only && !dual) && !suppressed;
            bool light_on = worth_lighting &&
                is_window_active_for_date(&snapshot, next.year, next.month, next.day, tm_now, minute_of_day);

            s_light_on = light_on;
            if (light_on) {
                s_light_year = next.year;
                s_light_month = next.month;
                s_light_day = next.day;
            }

            if (light_on) {
                led_color_t primary_led = {next.primary.r, next.primary.g, next.primary.b};
                led_color_t secondary_led = dual
                    ? (led_color_t){next.secondary.r, next.secondary.g, next.secondary.b}
                    : primary_led;
                led_state_set_dual(primary_led, secondary_led, snapshot.brightness);
            } else {
                led_state_off();
            }
        }
    }
}

static void schedule_task_fn(void *arg)
{
    for (;;) {
        if (time_sync_is_valid()) {
            schedule_evaluate_once();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SCHEDULE_POLL_MS));
    }
}

esp_err_t schedule_task_start(void)
{
    BaseType_t ok = xTaskCreate(schedule_task_fn, "schedule_task", 4096, NULL, tskIDLE_PRIORITY + 3, &s_task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void schedule_task_force_check(void)
{
    if (s_task_handle != NULL) {
        xTaskNotifyGive(s_task_handle);
    }
}

bool schedule_light_is_on(void)
{
    return s_light_on;
}

void schedule_suppress_current(void)
{
    if (!s_light_on) {
        ESP_LOGI(TAG, "dismiss requested but the light is already off - ignoring");
        return;
    }
    s_suppress_active = true;
    s_suppress_year = s_light_year;
    s_suppress_month = s_light_month;
    s_suppress_day = s_light_day;
    ESP_LOGI(TAG, "dismissed the light for the %04u-%02u-%02u collection",
             (unsigned)s_suppress_year, (unsigned)s_suppress_month, (unsigned)s_suppress_day);

    led_state_off();
    s_light_on = false;
    schedule_task_force_check(); // re-evaluate now, so the state is consistent immediately
}

void schedule_action_press(void)
{
    if (s_light_on) {
        schedule_suppress_current();
    } else {
        schedule_test_trigger();
    }
}

static void test_timer_callback(TimerHandle_t timer)
{
    schedule_task_force_check();
}

void schedule_test_trigger(void)
{
    schedule_t snapshot = schedule_get();
    schedule_next_t next = schedule_get_next_collection();
    // Unlike the live evaluator, this deliberately does NOT skip waste-only
    // nights in single-colour mode - it answers "what's the next collection?",
    // not "should the light be on right now?". The only no-op case is a
    // genuinely unknown next collection.
    if (!next.known) {
        ESP_LOGW(TAG, "display-next requested but the next collection is unknown");
        return;
    }

    ESP_LOGI(TAG, "displaying next collection: %04u-%02u-%02u%s",
             (unsigned)next.year, (unsigned)next.month, (unsigned)next.day,
             next.waste_only ? " (general waste)" : "");

    led_color_t primary = (led_color_t){next.primary.r, next.primary.g, next.primary.b};
    led_color_t secondary = primary;
    if (snapshot.light_mode == LIGHT_MODE_DUAL_COLOUR) {
        secondary = (led_color_t){next.secondary.r, next.secondary.g, next.secondary.b};
    }
    led_state_set_dual(primary, secondary, snapshot.brightness);

    if (s_test_timer == NULL) {
        s_test_timer = xTimerCreate("led_test", pdMS_TO_TICKS(TEST_PREVIEW_DURATION_MS), pdFALSE, NULL, test_timer_callback);
    }
    if (s_test_timer != NULL) {
        xTimerReset(s_test_timer, 0);
    }
}
