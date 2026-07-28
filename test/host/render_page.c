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
#include "ota.h"
#include "esp_http_server.h"

static schedule_t s_sched;
static waste_api_config_t s_api;
static char s_tz[64] = "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
// Status-card state. Defaults to a known collection with the clock synced -
// the ordinary case - so each render mode only overrides what it is testing.
bool stub_clock_ok = true;
bool stub_light_on = false;
schedule_next_t stub_next;

schedule_t schedule_get(void) { return s_sched; }
esp_err_t schedule_set(const schedule_t *n) { s_sched = *n; return ESP_OK; }
void schedule_task_force_check(void) {}
void schedule_test_trigger(void) {}
bool schedule_light_is_on(void) { return stub_light_on; }
schedule_next_t schedule_get_next_collection(void) { return stub_next; }
bool time_sync_is_valid(void) { return stub_clock_ok; }
const char *settings_get_tz(void) { return s_tz; }
esp_err_t settings_set_tz(const char *tz) { snprintf(s_tz, sizeof(s_tz), "%s", tz); return ESP_OK; }
// Mirrors settings.c's table. Stubbed rather than linked because settings.c
// also owns the NVS-backed get/set stubbed above, and linking it would collide.
// The mapping itself is covered by test_settings_tz.c.
const char *settings_tz_for_state(const char *state)
{
    if (state == NULL) return NULL;
    if (strcmp(state, "QLD") == 0) return "AEST-10";
    if (strcmp(state, "WA") == 0) return "AWST-8";
    if (strcmp(state, "NT") == 0) return "ACST-9:30";
    if (strcmp(state, "SA") == 0) return "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3";
    return "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
}
waste_api_config_t waste_api_get_config(void) { return s_api; }
esp_err_t waste_api_set_config(const waste_api_config_t *c) { s_api = *c; return ESP_OK; }
int waste_api_fetch_localities(const char *s, waste_api_locality_t *o, int m) { (void)s;(void)o;(void)m; return -1; }
int waste_api_fetch_streets(const char *s, uint32_t l, waste_api_street_t *o, int m) { (void)s;(void)l;(void)o;(void)m; return -1; }
int waste_api_fetch_properties(const char *s, uint32_t st, waste_api_property_t *o, int m) { (void)s;(void)st;(void)o;(void)m; return -1; }
// Normally fails (no network on the host), which is also the state /api-setup
// and /api-test render on a fresh device. --api-test* plants a full window of
// events instead, so the upcoming table and the mapping form can be measured.
int stub_upcoming_count = -1;
bool stub_upcoming_hostile = false;
int waste_api_fetch_upcoming(const waste_api_config_t *c, int d, waste_api_event_t *o, int m)
{
    (void)c; (void)d;
    if (stub_upcoming_count < 0) {
        return -1;
    }
    int n = stub_upcoming_count < m ? stub_upcoming_count : m;
    static const char *TYPES[] = {"recycle", "organic", "waste", "glass"};
    for (int i = 0; i < n; i++) {
        memset(&o[i], 0, sizeof(o[i]));
        o[i].year = 2026;
        o[i].month = (uint8_t)(8 + i / 28);
        o[i].day = (uint8_t)(1 + (i * 7) % 28);
        if (stub_upcoming_hostile) {
            memset(o[i].event_type, '\'', sizeof(o[i].event_type) - 1);
            o[i].event_type[sizeof(o[i].event_type) - 1] = '\0';
        } else {
            snprintf(o[i].event_type, sizeof(o[i].event_type), "%s", TYPES[i % 4]);
        }
        o[i].color = (schedule_color_t){(uint8_t)(i * 20), 200, 60};
    }
    return n;
}
// Address search normally fails (no network on the host). --setup-escaped
// plants a full page of worst-case matches instead: the longest id the
// bespoke backends produce (Merri-bek's packed ids) and a label made
// entirely of characters html_escape_attr() expands 5:1, which is what
// SETUP_HTML_BUF_SIZE has to survive.
bool stub_search_hostile = false;
int waste_api_search_address(uint8_t b, const char *q, waste_api_search_result_t *o, int m)
{
    (void)b; (void)q;
    if (!stub_search_hostile) {
        return -1;
    }
    for (int i = 0; i < m; i++) {
        memset(o[i].id, 'A', sizeof(o[i].id) - 1);
        o[i].id[sizeof(o[i].id) - 1] = '\0';
        memset(o[i].label, '\'', sizeof(o[i].label) - 1);
        o[i].label[sizeof(o[i].label) - 1] = '\0';
    }
    return m;
}
bool waste_api_config_complete(const waste_api_config_t *c)
{
    if (c->backend == COUNCIL_BACKEND_IMPACT_APPS) return c->council_subdomain[0] != '\0' && c->property_id != 0;
    return c->address_id[0] != '\0';
}

const char *stub_ssid = "Home-WiFi";
const char *wifi_manager_current_ssid(void) { return stub_ssid; }
esp_err_t wifi_manager_forget_credentials(void) { return ESP_OK; }
esp_err_t factory_reset_erase(void) { return ESP_OK; }
void factory_reset_perform(void) { for (;;) {} }
void waste_api_merribek_calendar_url(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "https://www.merri-bek.vic.gov.au/living-in-merri-bek/"
             "waste-and-recycling/bins-and-collection-services/waste-calendar26/");
}

// OTA stubs. The harness can plant a manifest to render each state of the
// update page without a network.
const char *stub_ota_version = "1.0.0";
bool stub_ota_update_available = false;
ota_state_t stub_ota_state = OTA_STATE_IDLE;

const char *ota_running_version(void) { return stub_ota_version; }
esp_err_t ota_check(ota_manifest_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->version, sizeof(out->version), "%s", stub_ota_update_available ? "1.1.0" : "1.0.0");
    snprintf(out->url, sizeof(out->url),
             "https://github.com/jxg81/bin-light/releases/download/v1.1.0/bin-light.bin");
    // Release notes are shown verbatim on /update, so they are user-facing
    // copy and the plain-English check covers them. The fixture models a
    // well-written note rather than a changelog line.
    snprintf(out->notes, sizeof(out->notes), "Adds two more councils.");
    out->available = stub_ota_update_available;
    return ESP_OK;
}
esp_err_t ota_start(const char *url, bool restart_when_done) { (void)url; (void)restart_when_done; return ESP_OK; }
ota_state_t ota_get_state(void) { return stub_ota_state; }
const char *ota_get_message(void) { return "downloading 45%"; }
void ota_mark_valid(void) {}
bool stub_ota_auto = true;
bool ota_auto_update_enabled(void) { return stub_ota_auto; }
esp_err_t ota_set_auto_update(bool e) { stub_ota_auto = e; return ESP_OK; }
esp_err_t ota_auto_task_start(void) { return ESP_OK; }

const char *stub_query_string = NULL;
const char *stub_post_body = NULL;
const char *stub_hdr_origin = NULL;
const char *stub_hdr_host = NULL;
int stub_last_err_code = 0;

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

static int s_origin_failures;
static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) s_origin_failures++;
}

// Drives the cross-origin gate through the real handlers with planted
// Origin/Host headers. Uses /update for the allow cases (it renders without
// side effects) and /factory-reset for a reject case (rejection happens before
// the body is read, so nothing is wiped - and the harness's
// factory_reset_perform() would loop forever if the gate ever let a confirmed
// POST through here, which is caught by the confirm field being absent).
static int run_origin_checks(void)
{
    httpd_req_t req = {0};
    req.uri = "/update";
    req.method = HTTP_POST;

    stub_hdr_host = "binlight.local";

    stub_hdr_origin = NULL;
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 0, "no Origin header is allowed (curl, old browsers)");

    stub_hdr_origin = "http://binlight.local";
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 0, "matching Origin is allowed (binlight.local)");

    stub_hdr_origin = "http://192.168.1.7";
    stub_hdr_host = "192.168.1.7";
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 0, "matching Origin is allowed (LAN IP)");

    stub_hdr_host = "binlight.local";

    stub_hdr_origin = "http://evil.example";
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 403, "cross-site Origin answers 403");

    stub_hdr_origin = "http://binlight.local.evil.example";
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 403, "prefix-spoofed Origin answers 403");

    stub_hdr_origin = "null";
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 403, "Origin: null (sandboxed page) answers 403");

    // The firmware page must never 403. GET /update shares this handler and
    // only renders status, so it is deliberately exempt - if this ever starts
    // failing, the update page can break for anyone whose browser sends an
    // Origin on a navigation, which is the one regression that matters most.
    req.method = HTTP_GET;
    stub_hdr_origin = "http://evil.example";
    stub_last_err_code = 0;
    update_post_handler(&req);
    expect(stub_last_err_code == 0, "GET /update is never rejected (status render only)");
    req.method = HTTP_POST;

    req.uri = "/factory-reset";
    stub_hdr_origin = "http://evil.example";
    stub_last_err_code = 0;
    factory_reset_post_handler(&req);
    expect(stub_last_err_code == 403, "cross-site factory reset answers 403");

    stub_hdr_origin = NULL;
    stub_hdr_host = NULL;
    return s_origin_failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    // A realistic configured device: Maribyrnong via the API, dual colour,
    // and a manual fallback with two rules on different cycles.
    s_sched.version = 5;
    s_sched.enabled = true;
    s_sched.bin_night_weekday = 4;        // Thursday
    s_sched.start_minute = 15 * 60;       // 3:00pm - matches schedule.c's shipped default
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
    if (argc > 1 && strcmp(argv[1], "--max-escaped") == 0) {
        // --max, but every string that now goes through html_escape_attr()
        // filled with the character it expands most (' -> &#39;, 5:1). Nothing
        // a council would really send, but it is the ceiling the buffer has to
        // clear, and the fixtures above contain no escapable characters at all
        // so they measure the escaping at zero cost.
        for (int i = 0; i < WASTE_API_MAX_TYPE_RULES; i++) {
            char *t = s_api.type_rules[i].event_type;
            memset(t, '\'', sizeof(s_api.type_rules[i].event_type) - 1);
            t[sizeof(s_api.type_rules[i].event_type) - 1] = '\0';
            s_api.type_rules[i].ignored = false;
            s_api.type_rules[i].color = preset_by_label("Purple");
        }
        memset(s_api.property_label, '\'', sizeof(s_api.property_label) - 1);
        s_api.property_label[sizeof(s_api.property_label) - 1] = '\0';
        // Unlisted council: api_council_name() falls back to the raw subdomain.
        memset(s_api.council_subdomain, '\'', sizeof(s_api.council_subdomain) - 1);
        s_api.council_subdomain[sizeof(s_api.council_subdomain) - 1] = '\0';
        static char hostile_ssid[33];
        memset(hostile_ssid, '\'', sizeof(hostile_ssid) - 1);
        stub_ssid = hostile_ssid;
        // A custom TZ is reflected too, and was already escaped before this work.
        memset(s_tz, '\'', sizeof(s_tz) - 1);
        s_tz[sizeof(s_tz) - 1] = '\0';
    }
    if (argc > 1 && strcmp(argv[1], "--empty") == 0) {
        // Fresh out-of-the-box device: nothing configured.
        memset(&s_api, 0, sizeof(s_api));
        s_sched.enabled = false;
        s_sched.light_mode = LIGHT_MODE_SINGLE_COLOUR;
    }

    bool update_page = false;
    if (argc > 1 && strncmp(argv[1], "--update", 8) == 0) {
        update_page = true;
        if (strcmp(argv[1], "--update-available") == 0) stub_ota_update_available = true;
        if (strcmp(argv[1], "--update-progress") == 0)  stub_ota_state = OTA_STATE_RUNNING;
    }
    stub_next.known = true;
    stub_next.year = 2026; stub_next.month = 8; stub_next.day = 4;
    stub_next.primary = preset_by_label("Yellow");
    stub_next.secondary = preset_by_label("Green");

    bool settings_page = (argc > 1 && strcmp(argv[1], "--settings") == 0);
    if (argc > 1 && strcmp(argv[1], "--no-clock") == 0) stub_clock_ok = false;
    if (argc > 1 && strcmp(argv[1], "--no-data") == 0) stub_next.known = false;
    bool reset_confirm = (argc > 1 && strcmp(argv[1], "--reset-confirm") == 0);
    bool api_test_page = false;
    if (argc > 1 && strncmp(argv[1], "--api-test", 10) == 0) {
        api_test_page = true;
        // A full window of events AND every type-rule slot filled is what
        // /api-test's mapping form has to survive now that it is the only
        // place the mapping lives.
        stub_upcoming_count = API_TEST_MAX_EVENTS;
        if (strcmp(argv[1], "--api-test-escaped") == 0) {
            stub_upcoming_hostile = true;
        }
    }
    bool setup_page = (argc > 1 && strcmp(argv[1], "--setup") == 0);
    if (argc > 1 && strcmp(argv[1], "--merribek") == 0) {
        setup_page = true;
        stub_query_string = "step=council&council=merri-bek";
    }
    if (argc > 1 && strcmp(argv[1], "--setup-escaped") == 0) {
        // A full page of worst-case address matches - see waste_api_search_address
        // above. This is what SETUP_HTML_BUF_SIZE is sized against.
        setup_page = true;
        stub_search_hostile = true;
        stub_query_string = "step=bsearch&council=merri-bek&q=x";
    }

    if (argc > 1 && strcmp(argv[1], "--origin-check") == 0) {
        s_out = fopen("/dev/null", "w"); // the pages themselves aren't the point here
        int rc = run_origin_checks();
        fclose(s_out);
        return rc;
    }

    s_out = fopen(argc > 2 ? argv[2] : "/dev/stdout", "w");
    httpd_req_t req = {0};
    // No query string is stubbed, so /api-setup renders its first step - the
    // state + council pickers, which is the page worth eyeballing.
    if (update_page) {
        req.content_len = 0;
        update_post_handler(&req);
    } else if (reset_confirm) {
        req.content_len = 0;         // first POST: no confirm field
        factory_reset_post_handler(&req);
    } else if (settings_page) {
        settings_get_handler(&req);
    } else if (setup_page) {
        api_setup_get_handler(&req);
    } else if (api_test_page) {
        api_test_get_handler(&req);
    } else {
        root_get_handler(&req);
    }
    fclose(s_out);
    return 0;
}
