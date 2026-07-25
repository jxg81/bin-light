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
#define TEST_PREVIEW_DURATION_MS (2UL * 60 * 1000)

static schedule_t s_schedule;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task_handle;
static TimerHandle_t s_test_timer;

static schedule_t default_schedule(void)
{
    schedule_t s = {0};
    s.version = SCHEDULE_STRUCT_VERSION;
    s.start_minute = 15 * 60;   // 3:00pm
    s.duration_hours = 20;
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

// True if the on-window is active right now, given today's weekday and
// minute-of-day, anchored on an explicit target weekday: it turns on at
// start_minute that evening and stays on for duration_hours, which may carry
// past midnight into the following day.
static bool is_window_active_for_weekday(const schedule_t *s, int target_weekday, int wday, int minute_of_day)
{
    int end_minute = (int)s->start_minute + (int)s->duration_hours * 60; // may exceed 1440
    int day_after = (target_weekday + 1) % 7;

    if (wday == target_weekday) {
        if (end_minute <= 1440) {
            return minute_of_day >= s->start_minute && minute_of_day < end_minute;
        }
        return minute_of_day >= s->start_minute;
    }
    if (wday == day_after && end_minute > 1440) {
        return minute_of_day < (end_minute - 1440);
    }
    return false;
}

// Same as is_window_active_for_weekday(), anchored on the manual schedule's
// configured bin_night_weekday.
static bool is_window_active(const schedule_t *s, int wday, int minute_of_day)
{
    return is_window_active_for_weekday(s, s->bin_night_weekday, wday, minute_of_day);
}

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
    return (long)((to_time - from_time) / 86400);
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

// Like is_window_active(), but anchored on a specific calendar date (from the
// external API) instead of a repeating weekday. Bin night is the evening
// before event_date; the window may still wrap past midnight into event_date
// itself, same as the weekday-keyed version.
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

static void schedule_task_fn(void *arg)
{
    for (;;) {
        if (time_sync_is_valid()) {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            int minute_of_day = tm_now.tm_hour * 60 + tm_now.tm_min;

            schedule_t snapshot = schedule_get();
            bool dual = (snapshot.light_mode == LIGHT_MODE_DUAL_COLOUR);

            waste_api_slot_t api_primary, api_secondary;
            uint16_t event_year;
            uint8_t event_month, event_day;
            waste_api_result_t api_result =
                waste_api_get_current(&api_primary, &api_secondary, &event_year, &event_month, &event_day);

            bool light_on = false;
            schedule_color_t primary_color = {0};
            schedule_color_t secondary_color = {0};

            if (api_result == WASTE_API_RESULT_EVENT) {
                if (is_window_active_for_date(&snapshot, event_year, event_month, event_day, tm_now, minute_of_day)) {
                    light_on = true;
                    primary_color = api_primary.color;
                    secondary_color = api_secondary.due ? api_secondary.color : snapshot.secondary_default_color;
                }
            } else if (api_result == WASTE_API_RESULT_NO_EVENT) {
                // API is reachable and authoritative for "other" events: no
                // rotating item is due. In dual-colour mode a plain
                // general-waste night still lights both LEDs as a reminder,
                // using the recurring waste rule's own weekday (independent
                // of the polled "other events" cache) - see SPEC.md 3.7.
                uint8_t waste_wday;
                if (dual && waste_api_get_waste_weekday(&waste_wday) &&
                    is_window_active_for_weekday(&snapshot, waste_wday, tm_now.tm_wday, minute_of_day)) {
                    light_on = true;
                    primary_color = snapshot.secondary_default_color;
                    secondary_color = snapshot.secondary_default_color;
                }
            } else { // WASTE_API_RESULT_UNAVAILABLE: fall back to the manual schedule
                if (snapshot.enabled && is_window_active(&snapshot, tm_now.tm_wday, minute_of_day)) {
                    const schedule_color_rule_t *due_primary = NULL;
                    const schedule_color_rule_t *due_secondary = NULL;
                    find_due_rules(&snapshot, tm_now, &due_primary, &due_secondary);

                    if (due_primary != NULL) {
                        light_on = true;
                        primary_color = due_primary->color;
                        secondary_color = (due_secondary != NULL) ? due_secondary->color : snapshot.secondary_default_color;
                    } else if (dual) {
                        // Nothing rotating due, but it's still bin night for
                        // general waste in dual-colour mode - the light turns
                        // on every bin night when dual, unlike single mode.
                        light_on = true;
                        primary_color = snapshot.secondary_default_color;
                        secondary_color = snapshot.secondary_default_color;
                    }
                }
            }

            led_color_t primary_led = (led_color_t){primary_color.r, primary_color.g, primary_color.b};
            led_color_t secondary_led = dual ? (led_color_t){secondary_color.r, secondary_color.g, secondary_color.b}
                                              : primary_led;

            if (light_on) {
                led_state_set_dual(primary_led, secondary_led, snapshot.brightness);
            } else {
                led_state_off();
            }
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

schedule_preview_t schedule_preview_next(void)
{
    schedule_preview_t preview = {0};
    schedule_t snapshot = schedule_get();
    waste_api_config_t api_cfg = waste_api_get_config();

    waste_api_slot_t api_primary, api_secondary;
    uint16_t event_year;
    uint8_t event_month, event_day;
    waste_api_result_t api_result =
        waste_api_get_current(&api_primary, &api_secondary, &event_year, &event_month, &event_day);

    if (api_result == WASTE_API_RESULT_EVENT) {
        preview.has_primary = true;
        preview.primary = api_primary.color;
        preview.has_secondary = true;
        preview.secondary = api_secondary.due ? api_secondary.color : snapshot.secondary_default_color;
        return preview;
    }
    if (api_result == WASTE_API_RESULT_NO_EVENT) {
        // API is configured and reachable, just nothing rotating due right
        // now - Test's promise is "once configured, pressing this lights
        // both LEDs" (not a strict preview of tonight's real outcome), so
        // this doesn't depend on whether the general-waste weekday happens
        // to be known too. Real operation is stricter about this - see
        // schedule_task_fn()'s dual-colour NO_EVENT branch.
        preview.has_primary = true;
        preview.has_secondary = true;
        preview.primary = snapshot.secondary_default_color;
        preview.secondary = snapshot.secondary_default_color;
        return preview;
    }

    // WASTE_API_RESULT_UNAVAILABLE: preview the manual schedule's next
    // upcoming bin night, not necessarily tonight.
    if (snapshot.enabled) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int days_ahead = ((int)snapshot.bin_night_weekday - tm_now.tm_wday + 7) % 7;
        struct tm target = date_plus_days(tm_now, days_ahead);

        const schedule_color_rule_t *due_primary = NULL;
        const schedule_color_rule_t *due_secondary = NULL;
        find_due_rules(&snapshot, target, &due_primary, &due_secondary);

        preview.has_primary = true;
        preview.has_secondary = true;
        if (due_primary != NULL) {
            preview.primary = due_primary->color;
            preview.secondary = (due_secondary != NULL) ? due_secondary->color : snapshot.secondary_default_color;
        } else {
            // Nothing rotating due on the next bin night - still a
            // general-waste night, matching the live evaluator's
            // dual-colour-mode behaviour.
            preview.primary = snapshot.secondary_default_color;
            preview.secondary = snapshot.secondary_default_color;
        }
        return preview;
    }

    // Neither a specific API event nor a manual fallback to preview - but if
    // the API is at least configured (just momentarily unavailable, e.g. not
    // yet polled since boot), still guarantee something rather than a silent
    // no-op, since "configured" is the only promise Test makes.
    if (api_cfg.enabled) {
        preview.has_primary = true;
        preview.has_secondary = true;
        preview.primary = snapshot.secondary_default_color;
        preview.secondary = snapshot.secondary_default_color;
    }
    return preview;
}

static void test_timer_callback(TimerHandle_t timer)
{
    schedule_task_force_check();
}

void schedule_test_trigger(void)
{
    schedule_t snapshot = schedule_get();
    schedule_preview_t preview = schedule_preview_next();
    if (!preview.has_primary) {
        return;
    }

    led_color_t primary = (led_color_t){preview.primary.r, preview.primary.g, preview.primary.b};
    led_color_t secondary = primary;
    if (snapshot.light_mode == LIGHT_MODE_DUAL_COLOUR && preview.has_secondary) {
        secondary = (led_color_t){preview.secondary.r, preview.secondary.g, preview.secondary.b};
    }
    led_state_set_dual(primary, secondary, snapshot.brightness);

    if (s_test_timer == NULL) {
        s_test_timer = xTimerCreate("led_test", pdMS_TO_TICKS(TEST_PREVIEW_DURATION_MS), pdFALSE, NULL, test_timer_callback);
    }
    if (s_test_timer != NULL) {
        xTimerReset(s_test_timer, 0);
    }
}
