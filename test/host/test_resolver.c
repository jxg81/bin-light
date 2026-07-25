// Host test for schedule_get_next_collection() and is_window_active_for_date()
// (SPEC.md 3.3). Compiles the REAL schedule.c against stubs, with time(NULL)
// redirected to a settable fake clock so the date arithmetic - weekday
// wraparound, the past-midnight window, DST-safe day diffing, and the
// event-vs-waste tie-break - can be driven through concrete scenarios.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

// --- fake clock, installed before schedule.c is pulled in -------------------
static time_t g_now;
static time_t fake_time(time_t *t) { if (t) *t = g_now; return g_now; }
#define time(x) fake_time(x)

#include "schedule.h"
#include "waste_api.h"
#include "led_state.h"
#include "time_sync.h"
#include "nvs.h"

// --- stub state ------------------------------------------------------------
static bool g_time_valid = true;
bool time_sync_is_valid(void) { return g_time_valid; }

static bool g_have_event;
static waste_api_next_event_t g_event;
static bool g_have_waste_dow;
static uint8_t g_waste_dow;

bool waste_api_get_next_event(waste_api_next_event_t *out)
{
    if (!g_have_event) return false;
    // Mirror the real staleness rule: the cached date goes stale once it has
    // actually passed (valid through its own day).
    struct tm ev = {0};
    ev.tm_year = g_event.year - 1900; ev.tm_mon = g_event.month - 1; ev.tm_mday = g_event.day;
    ev.tm_hour = 12; ev.tm_isdst = -1;
    struct tm today; localtime_r(&g_now, &today);
    today.tm_hour = 12; today.tm_min = 0; today.tm_sec = 0; today.tm_isdst = -1;
    if ((mktime(&today) - mktime(&ev)) / 86400 > 0) return false;
    if (out) *out = g_event;
    return true;
}
bool waste_api_get_waste_weekday(uint8_t *o) { if (!g_have_waste_dow) return false; if (o) *o = g_waste_dow; return true; }
waste_api_config_t waste_api_get_config(void) { waste_api_config_t c = {0}; return c; }

static led_color_t g_led_p, g_led_s; static uint8_t g_led_b; static bool g_lit;
esp_err_t led_state_set_dual(led_color_t p, led_color_t s, uint8_t b)
{ g_led_p = p; g_led_s = s; g_led_b = b; g_lit = true; return ESP_OK; }
esp_err_t led_state_off(void) { g_lit = false; return ESP_OK; }

// Tiny in-memory NVS.
static struct { char key[32]; unsigned char val[512]; size_t len; bool used; } g_nvs[8];
esp_err_t nvs_open(const char *ns, int m, nvs_handle_t *h) { (void)ns;(void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l)
{
    (void)h;
    for (int i = 0; i < 8; i++) {
        if (g_nvs[i].used && strcmp(g_nvs[i].key, k) == 0) { memcpy(g_nvs[i].val, v, l); g_nvs[i].len = l; return ESP_OK; }
    }
    for (int i = 0; i < 8; i++) {
        if (!g_nvs[i].used) { g_nvs[i].used = true; snprintf(g_nvs[i].key, 32, "%s", k); memcpy(g_nvs[i].val, v, l); g_nvs[i].len = l; return ESP_OK; }
    }
    return ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l)
{
    (void)h;
    for (int i = 0; i < 8; i++) {
        if (g_nvs[i].used && strcmp(g_nvs[i].key, k) == 0) {
            if (o == NULL) { *l = g_nvs[i].len; return ESP_OK; }
            memcpy(o, g_nvs[i].val, g_nvs[i].len); *l = g_nvs[i].len; return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

#include "schedule.c"

// --- harness ---------------------------------------------------------------
static int g_fail;

static void set_now(int y, int mo, int d, int hh, int mm)
{
    struct tm t = {0};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = hh; t.tm_min = mm; t.tm_isdst = -1;
    g_now = mktime(&t);
}

static const char *WD[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *wd_of(int y, int mo, int d)
{
    struct tm t = {0}; t.tm_year = y-1900; t.tm_mon = mo-1; t.tm_mday = d; t.tm_hour = 12; t.tm_isdst = -1;
    time_t tt = mktime(&t); struct tm o; localtime_r(&tt, &o); return WD[o.tm_wday];
}

static void check(const char *name, bool ok, const char *detail)
{
    printf("%s %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_fail++;
}

static void expect_next(const char *name, int ey, int emo, int ed, bool waste_only)
{
    schedule_next_t n = schedule_get_next_collection();
    char detail[160];
    if (!n.known) {
        snprintf(detail, sizeof(detail), "got: unknown, want %04d-%02d-%02d", ey, emo, ed);
        check(name, false, detail);
        return;
    }
    bool ok = (n.year == ey && n.month == emo && n.day == ed && n.waste_only == waste_only);
    snprintf(detail, sizeof(detail), "got %04u-%02u-%02u (%s)%s",
             n.year, n.month, n.day, wd_of(n.year, n.month, n.day), n.waste_only ? " waste-only" : "");
    check(name, ok, detail);
}

static void expect_unknown(const char *name)
{
    schedule_next_t n = schedule_get_next_collection();
    check(name, !n.known, n.known ? "got a date, wanted unknown" : "unknown as expected");
}

static void expect_lit(const char *name, bool want_lit)
{
    g_lit = false;
    // One pass of the live evaluator's decision, mirroring schedule_task_fn.
    struct tm tm_now; localtime_r(&g_now, &tm_now);
    int mod = tm_now.tm_hour * 60 + tm_now.tm_min;
    schedule_t s = schedule_get();
    bool dual = (s.light_mode == LIGHT_MODE_DUAL_COLOUR);
    schedule_next_t n = schedule_get_next_collection();
    bool worth = n.known && !(n.waste_only && !dual);
    bool lit = worth && is_window_active_for_date(&s, n.year, n.month, n.day, tm_now, mod);
    char d[80]; snprintf(d, sizeof(d), "lit=%d", lit);
    check(name, lit == want_lit, d);
}

static schedule_t base_schedule(void)
{
    schedule_t s = {0};
    s.version = SCHEDULE_STRUCT_VERSION;
    s.enabled = false;
    s.bin_night_weekday = 4;   // Thursday night -> Friday collection
    s.start_minute = 18 * 60;  // 18:00
    s.duration_hours = 20;     // ends 14:00 next day
    s.brightness = 128;
    s.light_mode = LIGHT_MODE_SINGLE_COLOUR;
    s.secondary_default_color = (schedule_color_t){255, 0, 0};
    return s;
}

static void reset(void)
{
    memset(g_nvs, 0, sizeof(g_nvs));
    g_have_event = false; g_have_waste_dow = false; g_time_valid = true;
    schedule_init();
    schedule_t s = base_schedule();
    schedule_set(&s);
}

int main(void)
{
    setenv("TZ", "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1);  // Melbourne
    tzset();

    printf("\n== API dated event ==\n");
    reset();
    g_have_event = true;
    g_event = (waste_api_next_event_t){2026, 7, 31, {255,150,0}, false, {0,0,0}};  // Fri 31 Jul
    set_now(2026, 7, 27, 10, 0);   // Mon
    expect_next("future event is the next collection", 2026, 7, 31, false);
    set_now(2026, 7, 30, 19, 0);   // Thu 19:00 - bin night, window open
    expect_lit("lit on the eve, after start_minute", true);
    set_now(2026, 7, 30, 17, 0);   // Thu 17:00 - before start
    expect_lit("not lit on the eve before start_minute", false);
    set_now(2026, 7, 31, 8, 0);    // Fri 08:00 - window wraps past midnight
    expect_lit("still lit next morning inside the wrap", true);
    set_now(2026, 7, 31, 15, 0);   // Fri 15:00 - after 14:00 end
    expect_lit("off once the wrap window closes", false);

    printf("\n== staleness ==\n");
    set_now(2026, 8, 1, 10, 0);    // Sat, event was yesterday
    expect_unknown("passed event with no fallback -> unknown");

    printf("\n== recurring general-waste weekday ==\n");
    reset();
    g_have_waste_dow = true; g_waste_dow = 5;  // Friday collection
    set_now(2026, 7, 27, 10, 0);   // Mon
    expect_next("next Friday, flagged waste-only", 2026, 7, 31, true);
    expect_lit("single-colour mode ignores a waste-only night", false);
    schedule_t s = schedule_get(); s.light_mode = LIGHT_MODE_DUAL_COLOUR; schedule_set(&s);
    set_now(2026, 7, 30, 19, 0);
    expect_lit("dual-colour mode lights a waste-only night", true);
    set_now(2026, 7, 31, 8, 0);    // Fri morning, inside wrap
    expect_next("during the wrap, today is still the answer", 2026, 7, 31, true);
    set_now(2026, 7, 31, 15, 0);   // Fri afternoon, window closed
    expect_next("once closed, it rolls to next week", 2026, 8, 7, true);

    printf("\n== event vs waste: sooner wins, ties to the event ==\n");
    reset();
    g_have_waste_dow = true; g_waste_dow = 5;                                    // Fri 31 Jul
    g_have_event = true;
    g_event = (waste_api_next_event_t){2026, 7, 29, {0,255,0}, false, {0,0,0}};  // Wed 29 Jul
    set_now(2026, 7, 27, 10, 0);
    expect_next("earlier dated event beats the waste weekday", 2026, 7, 29, false);
    g_event = (waste_api_next_event_t){2026, 8, 5, {0,255,0}, false, {0,0,0}};   // Wed 5 Aug
    expect_next("earlier waste weekday beats a later event", 2026, 7, 31, true);
    g_event = (waste_api_next_event_t){2026, 7, 31, {0,255,0}, false, {0,0,0}};  // same day
    expect_next("tie goes to the dated event", 2026, 7, 31, false);

    printf("\n== manual / fallback schedule ==\n");
    reset();
    s = schedule_get();
    s.enabled = true;
    s.rules[0] = (schedule_color_rule_t){true, {255,150,0}, 2026, 7, 31, 2};  // Fri 31 Jul, fortnightly
    schedule_set(&s);
    set_now(2026, 7, 27, 10, 0);   // Mon
    expect_next("bin night Thu -> collection Fri, rule due", 2026, 7, 31, false);
    set_now(2026, 8, 3, 10, 0);    // next Mon - off week
    expect_next("off week is still a date, flagged waste-only", 2026, 8, 7, true);
    set_now(2026, 8, 10, 10, 0);   // Mon of the second cycle
    expect_next("fortnight later, rule due again", 2026, 8, 14, false);

    printf("\n== first-occurrence off-by-one ==\n");
    // The UI labels this field "First collection", so entering the collection
    // date must make the rule due on that very first occurrence.
    reset();
    s = schedule_get();
    s.enabled = true;
    s.rules[0] = (schedule_color_rule_t){true, {128,0,128}, 2026, 7, 31, 1};
    schedule_set(&s);
    set_now(2026, 7, 30, 19, 0);   // bin night before the first collection
    expect_next("due on its own first collection date", 2026, 7, 31, false);
    expect_lit("and the light is actually on that night", true);

    printf("\n== DST boundaries (Melbourne) ==\n");
    // The whole reason the date maths normalises to local noon. DST starts on
    // the first Sunday of October (4 Oct 2026, clocks forward at 2am) and ends
    // on the first Sunday of April (5 Apr 2026, clocks back at 3am). A naive
    // /86400 day count drifts by an hour across these and can land a day out.
    reset();
    s = schedule_get();
    s.enabled = true;
    s.bin_night_weekday = 6;   // Saturday night -> Sunday collection, i.e. the transition day itself
    s.rules[0] = (schedule_color_rule_t){true, {0,255,0}, 2026, 10, 4, 1};  // weekly from the DST-start Sunday
    schedule_set(&s);
    set_now(2026, 10, 1, 10, 0);   // Thu before clocks go forward
    expect_next("across DST start: collection on the transition day", 2026, 10, 4, false);
    set_now(2026, 10, 3, 19, 0);   // Sat night, window open, clocks jump overnight
    expect_lit("lit on the eve of the DST-start collection", true);
    set_now(2026, 10, 4, 8, 0);    // Sun morning, after the 2am jump
    expect_lit("still lit through the wrap despite the clock jump", true);

    reset();
    s = schedule_get();
    s.enabled = true;
    s.bin_night_weekday = 6;
    s.rules[0] = (schedule_color_rule_t){true, {0,255,0}, 2026, 4, 5, 1};   // DST-end Sunday
    schedule_set(&s);
    set_now(2026, 4, 2, 10, 0);
    expect_next("across DST end: collection on the transition day", 2026, 4, 5, false);
    set_now(2026, 4, 5, 8, 0);     // Sun morning, after clocks went back
    expect_lit("still lit through the wrap when an hour repeats", true);

    reset();
    s = schedule_get();
    s.enabled = true;
    s.rules[0] = (schedule_color_rule_t){true, {255,150,0}, 2026, 3, 6, 4};  // 4-weekly, spans DST end
    schedule_set(&s);
    set_now(2026, 4, 1, 10, 0);
    expect_next("4-week cycle counted across a DST change", 2026, 4, 3, false);

    printf("\n== nothing configured ==\n");
    reset();
    expect_unknown("no API, no manual schedule");
    g_time_valid = false;
    g_have_event = true;
    g_event = (waste_api_next_event_t){2026, 7, 31, {255,0,0}, false, {0,0,0}};
    expect_unknown("clock not synced -> unknown even with a cached event");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
