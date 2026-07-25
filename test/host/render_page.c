// Host harness: compiles the REAL web_server.c against thin stubs and dumps
// the exact bytes root_get_handler() would send, so the rendered preview is
// the firmware's own markup rather than a hand-written approximation.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "schedule.h"
#include "settings.h"
#include "waste_api.h"
#include "web_server.h"
#include "esp_http_server.h"

static schedule_t s_sched;
static waste_api_config_t s_api;
static char s_tz[64] = "AEST-10AEDT,M10.1.0/2,M4.1.0/3";

schedule_t schedule_get(void) { return s_sched; }
esp_err_t schedule_set(const schedule_t *n) { s_sched = *n; return ESP_OK; }
void schedule_task_force_check(void) {}
void schedule_test_trigger(void) {}
const char *settings_get_tz(void) { return s_tz; }
esp_err_t settings_set_tz(const char *tz) { snprintf(s_tz, sizeof(s_tz), "%s", tz); return ESP_OK; }
waste_api_config_t waste_api_get_config(void) { return s_api; }
esp_err_t waste_api_set_config(const waste_api_config_t *c) { s_api = *c; return ESP_OK; }
int waste_api_fetch_localities(const char *s, waste_api_locality_t *o, int m) { (void)s;(void)o;(void)m; return -1; }
int waste_api_fetch_streets(const char *s, uint32_t l, waste_api_street_t *o, int m) { (void)s;(void)l;(void)o;(void)m; return -1; }
int waste_api_fetch_properties(const char *s, uint32_t st, waste_api_property_t *o, int m) { (void)s;(void)st;(void)o;(void)m; return -1; }
int waste_api_fetch_upcoming(const char *s, uint32_t p, int d, waste_api_event_t *o, int m) { (void)s;(void)p;(void)d;(void)o;(void)m; return -1; }

static FILE *s_out;
void stub_capture(const char *buf, int len) { fwrite(buf, 1, (size_t)len, s_out); }

// Pull in the real handlers. They're static, so include the translation unit.
#include "web_server.c"

static void add_type(int i, const char *name, bool ignored, const char *preset)
{
    snprintf(s_api.type_rules[i].event_type, sizeof(s_api.type_rules[i].event_type), "%s", name);
    s_api.type_rules[i].ignored = ignored;
    s_api.type_rules[i].color = preset_by_label(preset);
}

int main(int argc, char **argv)
{
    // A realistic configured device: Maribyrnong via the API, dual colour,
    // and a manual fallback with two rules on different cycles.
    s_sched.version = 5;
    s_sched.enabled = true;
    s_sched.bin_night_weekday = 4;        // Thursday
    s_sched.start_minute = 18 * 60;       // 18:00
    s_sched.duration_hours = 20;
    s_sched.brightness = 128;
    s_sched.light_mode = LIGHT_MODE_DUAL_COLOUR;
    s_sched.secondary_default_color = preset_by_label("Red");

    s_sched.rules[0] = (schedule_color_rule_t){true, preset_by_label("Yellow"), 2026, 7, 9, 2};
    s_sched.rules[1] = (schedule_color_rule_t){true, preset_by_label("Green"), 2026, 7, 16, 2};
    s_sched.rules[2] = (schedule_color_rule_t){false, preset_by_label("Purple"), 2026, 8, 6, 4};

    s_api.version = 1;
    s_api.enabled = true;
    snprintf(s_api.council_subdomain, sizeof(s_api.council_subdomain), "maribyrnong");
    s_api.property_id = 2855360;
    snprintf(s_api.property_label, sizeof(s_api.property_label), "12 Example St, Footscray");
    add_type(0, "waste", true, "Red");
    add_type(1, "recycle", false, "Yellow");
    add_type(2, "organic", false, "Green");

    if (argc > 1 && strcmp(argv[1], "--max") == 0) {
        // Worst case: all WASTE_API_MAX_TYPE_RULES slots filled, longest
        // plausible type names and property label.
        for (int i = 0; i < WASTE_API_MAX_TYPE_RULES; i++) {
            add_type(i, "hard_waste_collection", false, "Purple");
        }
        snprintf(s_api.property_label, sizeof(s_api.property_label),
                 "123 Some Quite Long Street Name Indeed, Suburbville");
        snprintf(s_api.council_subdomain, sizeof(s_api.council_subdomain), "somelongcouncilname");
    }
    if (argc > 1 && strcmp(argv[1], "--empty") == 0) {
        // Fresh out-of-the-box device: nothing configured.
        memset(&s_api, 0, sizeof(s_api));
        s_sched.enabled = false;
        s_sched.light_mode = LIGHT_MODE_SINGLE_COLOUR;
    }

    bool setup_page = (argc > 1 && strcmp(argv[1], "--setup") == 0);

    s_out = fopen(argc > 2 ? argv[2] : "/dev/stdout", "w");
    httpd_req_t req = {0};
    // No query string is stubbed, so /api-setup renders its first step - the
    // state + council pickers, which is the page worth eyeballing.
    if (setup_page) {
        api_setup_get_handler(&req);
    } else {
        root_get_handler(&req);
    }
    fclose(s_out);
    return 0;
}
