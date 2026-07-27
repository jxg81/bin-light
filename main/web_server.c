#include "web_server.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "councils.h"
#include "factory_reset.h"
#include "ota.h"
#include "schedule.h"
#include "settings.h"
#include "waste_api.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";

// Sized against the home page, the largest of the three. Measured by
// compiling root_get_handler() on the host against stubs and dumping the
// bytes it emits:
//
//   factory-fresh, nothing configured    8378
//   realistic setup (3 event types)      9909
//   worst case (8 event types, i.e.
//     WASTE_API_MAX_TYPE_RULES, with
//     long type names and address)      12092
//   worst case with every escapable
//     field full of "'" (5:1 expansion) 14592
//
// 16384 leaves ~1.8KB over that last figure - raised from 14336, which the
// escaped worst case overflowed by 256 bytes once the strings printed into
// this page started going through html_escape_attr(). Ordinary content does
// not expand at all (addresses and type names contain nothing escapable), so
// the realistic figures above are unchanged; the ceiling is what moved. Note
// the fresh-device page alone is 8378 bytes: the original 7200 would have
// truncated on any real configuration. `./test/host/run.sh render` reprints
// and now asserts these figures, so re-check them after touching this page.
// Each handler also logs an error if it ever hits the ceiling, because
// safe_append() truncates *silently* and a truncated page drops form fields -
// which then read back as "absent"/unchecked on the next save, quietly
// destroying config.
#define HTML_BUF_SIZE   16384
#define SAVE_BODY_MAX   1024
#define FIELD_BUF_SIZE  16
#define API_TEST_LOOKAHEAD_DAYS 28
#define API_TEST_MAX_EVENTS     12
// TZ strings (up to SETTINGS_TZ_MAX_LEN chars) can triple in size once
// form-urlencoded (every '-', ',', '/', ':' becomes a 3-byte %XX escape), so
// they need their own, much larger, field buffer.
#define TZ_FIELD_BUF_SIZE (SETTINGS_TZ_MAX_LEN * 3 + 1)

static const char *WEEKDAY_LABEL[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *RULE_FIELD_PREFIX[SCHEDULE_MAX_COLOR_RULES] = {"rule1", "rule2", "rule3"};

typedef struct {
    const char *label;
    const char *tz;
} tz_preset_t;

// ESP-IDF's newlib has no IANA tzdata - timezones are POSIX TZ strings
// (offset + DST rule), not zone names. This is a curated list of Australian
// zones; "Custom" lets anyone else type a raw POSIX TZ string.
static const tz_preset_t TZ_PRESETS[] = {
    {"Melbourne / Sydney / Canberra / Hobart (AEST/AEDT)", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"Brisbane (AEST, no DST)", "AEST-10"},
    {"Adelaide (ACST/ACDT)", "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3"},
    {"Darwin (ACST, no DST)", "ACST-9:30"},
    {"Perth (AWST, no DST)", "AWST-8"},
};
#define TZ_PRESET_COUNT (sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]))

typedef struct {
    const char *label;
    uint8_t r, g, b;
} color_preset_t;

// The set of colours requested for the cycle test, with the RGB byte-order
// correction already applied at the driver level (led_state.c) - these are
// plain RGB triples, no further adjustment needed here.
static const color_preset_t COLOR_PRESETS[] = {
    {"Red",    255, 0,   0},
    {"Green",  0,   255, 0},
    // Green is far more luminous than red on a WS2812 at equal duty, so a
    // naive (255,255,0) reads distinctly green - especially diffused through
    // the PLA enclosure. Pulling green down balances it perceptually. Tuned
    // by eye against the real enclosure; see SPEC.md 2.
    {"Yellow", 255, 150, 0},
    {"Purple", 128, 0,   128},
};
#define COLOR_PRESET_COUNT (sizeof(COLOR_PRESETS) / sizeof(COLOR_PRESETS[0]))

// A small wheelie-bin icon, served as-is for /favicon.ico. SVG rather than a
// bitmap ICO format - every browser that requests /favicon.ico honours the
// response's Content-Type over the URL's extension, and an inline shape list
// is far cheaper to keep in source than embedding binary ICO bytes.
static const char FAVICON_SVG[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
    "<rect x='9' y='11' width='14' height='18' rx='2' fill='#3a3a3a'/>"
    "<rect x='11' y='14' width='10' height='2' fill='#5a5a5a'/>"
    "<rect x='11' y='19' width='10' height='2' fill='#5a5a5a'/>"
    "<rect x='11' y='24' width='10' height='2' fill='#5a5a5a'/>"
    "<rect x='8' y='7' width='16' height='4' rx='1' fill='#222'/>"
    "<rect x='13' y='3' width='6' height='4' rx='1' fill='#222'/>"
    "<circle cx='12' cy='30' r='2' fill='#111'/>"
    "<circle cx='20' cy='30' r='2' fill='#111'/>"
    "</svg>";

// True unless this is a request a browser made from a different website.
//
// Browsers attach an Origin header to cross-site requests. Comparing it against
// this request's own Host - rather than a fixed hostname - is what keeps every
// way of reaching the device working: binlight.local, the LAN IP, and
// 192.168.4.1 on the setup AP all compare equal to themselves.
//
// A missing Origin is allowed. Non-browser clients (curl, scripts) do not send
// one, and they are not the threat here: the attack requires a browser to be the
// confused deputy. A header too long to read is rejected rather than allowed, so
// an oversized value cannot be used to slip past the check.
static bool request_is_same_origin(httpd_req_t *req)
{
    char origin[128];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }

    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    // Origin is "scheme://host[:port]" - compare everything after the scheme.
    const char *o = strstr(origin, "://");
    if (o == NULL) {
        return false;
    }
    return strcmp(o + 3, host) == 0;
}

// Returns true if the handler should stop; sends the 403 itself.
static bool reject_cross_origin(httpd_req_t *req)
{
    // GET never fails this check. Only POST handlers act on a body, so a GET
    // has nothing to forge - and /update is registered for both methods on one
    // handler, where a GET only renders status. Checking it there would risk a
    // 403 on the firmware page, which is the one page that must never break,
    // in exchange for no security at all. Safe methods stay out of the way.
    if (req->method != HTTP_POST) {
        return false;
    }
    if (request_is_same_origin(req)) {
        return false;
    }
    ESP_LOGW(TAG, "rejected a cross-site request to %s", req->uri);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                        "This request didn't come from the bin light's own page.");
    return true;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    return httpd_resp_send(req, FAVICON_SVG, HTTPD_RESP_USE_STRLEN);
}

// Decodes a application/x-www-form-urlencoded value in place: "%XX" -> byte,
// "+" -> space. Decoding never increases length, so in-place is safe.
static void url_decode_inplace(char *s)
{
    char *out = s;
    while (*s != '\0') {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *out++ = ' ';
            s += 1;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

// Appends formatted text to buf at offset off, clamping off to buf_size so a
// truncated/overflowing render can never produce a negative or huge "space
// remaining" calculation on the next call.
static int safe_append(char *buf, size_t buf_size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= buf_size) {
        return (int)buf_size;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + off, buf_size - off, fmt, args);
    va_end(args);
    if (n < 0) {
        return off;
    }
    int new_off = off + n;
    return new_off > (int)buf_size ? (int)buf_size : new_off;
}

// Escapes a string for safe embedding in HTML, as text or inside a
// single-quoted attribute. Anything user-supplied or fetched from a council
// API goes through this before being printed into a page - the TZ string, the
// property label, event-type names, and the rest.
static void html_escape_attr(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; *in != '\0' && o + 6 < out_size; in++) {
        switch (*in) {
            case '&':  o += snprintf(out + o, out_size - o, "&amp;");  break;
            case '\'': o += snprintf(out + o, out_size - o, "&#39;");  break;
            case '<':  o += snprintf(out + o, out_size - o, "&lt;");   break;
            case '>':  o += snprintf(out + o, out_size - o, "&gt;");   break;
            case '"':  o += snprintf(out + o, out_size - o, "&quot;"); break;
            default:   out[o++] = *in;
        }
    }
    out[o] = '\0';
}

// Index of the preset closest to c, by squared distance in RGB space.
static size_t nearest_preset_index(schedule_color_t c)
{
    size_t best_idx = 0;
    long best_dist = -1;
    for (size_t i = 0; i < COLOR_PRESET_COUNT; i++) {
        long dr = (long)c.r - COLOR_PRESETS[i].r;
        long dg = (long)c.g - COLOR_PRESETS[i].g;
        long db = (long)c.b - COLOR_PRESETS[i].b;
        long dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    return best_idx;
}

// Maps an arbitrary RGB colour (e.g. straight from the API's own "color" hex
// field) to whichever of the 4 supported presets is closest. Used to
// auto-populate a sensible colour mapping right after API setup completes,
// without asking the user to pick manually for a council's colours that
// usually already correspond closely to one of our 4 presets.
static schedule_color_t nearest_preset_color(schedule_color_t c)
{
    size_t i = nearest_preset_index(c);
    return (schedule_color_t){COLOR_PRESETS[i].r, COLOR_PRESETS[i].g, COLOR_PRESETS[i].b};
}

// Renders a full <select> of the colour presets for one rotation slot,
// preselecting the *nearest* preset rather than requiring an exact RGB match.
// Stored rules hold literal RGB values, so any tuning of COLOR_PRESETS (e.g.
// warming the yellow, see above) would otherwise leave previously-saved rules
// matching nothing - and an <option>-less <select> silently displays its first
// entry, i.e. a yellow rule would appear to be set to Red. Nearest-match keeps
// the UI truthful across palette changes, and re-saving snaps the stored value
// onto the current palette.
// form_id: NULL for the enclosing form (the usual case), or the id of a
// different form this control belongs to (see append_type_mapping_rows).
static int append_color_select_for(char *buf, size_t buf_size, int off, const char *field_name,
                                    schedule_color_t current, const char *form_id)
{
    size_t selected_idx = nearest_preset_index(current);
    if (form_id != NULL) {
        off = safe_append(buf, buf_size, off, "<select form='%s' name='%s'>", form_id, field_name);
    } else {
        off = safe_append(buf, buf_size, off, "<select name='%s'>", field_name);
    }
    for (size_t c = 0; c < COLOR_PRESET_COUNT; c++) {
        const color_preset_t *cp = &COLOR_PRESETS[c];
        off = safe_append(buf, buf_size, off, "<option value='#%02x%02x%02x' %s>%s</option>",
            cp->r, cp->g, cp->b, (c == selected_idx) ? "selected" : "", cp->label);
    }
    return safe_append(buf, buf_size, off, "</select>");
}

static int append_color_select(char *buf, size_t buf_size, int off, const char *field_name, schedule_color_t current)
{
    return append_color_select_for(buf, buf_size, off, field_name, current, NULL);
}

static schedule_color_t preset_by_label(const char *label)
{
    for (size_t i = 0; i < COLOR_PRESET_COUNT; i++) {
        if (strcmp(COLOR_PRESETS[i].label, label) == 0) {
            return (schedule_color_t){COLOR_PRESETS[i].r, COLOR_PRESETS[i].g, COLOR_PRESETS[i].b};
        }
    }
    return (schedule_color_t){0, 0, 0}; // unreachable - every label below names a real preset
}

// Name-keyed defaults for common Impact-Apps event types (SPEC.md 3.7),
// tried before falling back to nearest_preset_color() when auto-populating a
// mapping: matching by nearest RGB distance to the API's own colour happens
// to get these four right for Maribyrnong's real colours, but only by
// coincidence, and would misfire for a council whose "glass" colour is
// numerically closer to green than purple, say. Anything not in this table
// (paper/food/clean_up/hard_waste, or an unrecognised type) still falls back
// to nearest_preset_color() as before.
static bool default_color_for_type(const char *event_type, schedule_color_t *out)
{
    static const struct { const char *type; const char *preset_label; } DEFAULTS[] = {
        {"waste",      "Red"},
        {"recycle",    "Yellow"},
        {"organic",    "Green"},
        {"greenwaste", "Green"},
        {"glass",      "Purple"},
    };
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        if (strcmp(event_type, DEFAULTS[i].type) == 0) {
            *out = preset_by_label(DEFAULTS[i].preset_label);
            return true;
        }
    }
    return false;
}

// Renders the already-saved type_rules as an editable form (posts to
// POST /api-test, which handles saving for both this page and /api-test's own
// discovery form). redirect_to controls where that POST sends the browser
// back to, so editing from the home page stays on the home page. Renders
// nothing if there's no mapping saved yet (e.g. API never configured, or the
// auto-fetch at setup time found nothing).
static int count_type_rules(const waste_api_config_t *cfg)
{
    int n = 0;
    for (int i = 0; i < WASTE_API_MAX_TYPE_RULES; i++) {
        if (cfg->type_rules[i].event_type[0] != '\0') {
            n++;
        }
    }
    return n;
}

// The colour-mapping controls posted to /api-test, rendered *inside* the home
// page's /save form's DOM but belonging to a different form.
//
// HTML forms can't nest, and since 3.11 the mapping table lives inside the
// collapsible "Bin collection API" block, which is itself inside the /save
// form. The way out is HTML5's `form=` attribute: an empty
// <form id='mapform' action='/api-test'> is emitted *before* the /save form
// opens (see append_type_mapping_anchor), carrying the two hidden fields, and
// every control below claims membership of it by id. No nesting, no
// JavaScript, and the browser submits exactly the same body /api-test already
// parses.
static int append_type_mapping_anchor(char *buf, size_t buf_size, int off,
                                       const waste_api_config_t *cfg, const char *redirect_to)
{
    int type_count = count_type_rules(cfg);
    if (type_count == 0) {
        return off;
    }
    return safe_append(buf, buf_size, off,
        "<form id='mapform' method='POST' action='/api-test'>"
        "<input type='hidden' name='type_count' value='%d'>"
        "<input type='hidden' name='redirect_to' value='%s'>"
        "</form>", type_count, redirect_to);
}

static int append_type_mapping_rows(char *buf, size_t buf_size, int off, const waste_api_config_t *cfg)
{
    if (count_type_rules(cfg) == 0) {
        return off;
    }

    off = safe_append(buf, buf_size, off,
        "<h3>Colour mapping</h3>"
        "<table><tr><th>Type</th><th>Ignore</th><th>Colour</th></tr>");

    int idx = 0;
    for (int i = 0; i < WASTE_API_MAX_TYPE_RULES; i++) {
        if (cfg->type_rules[i].event_type[0] == '\0') {
            continue;
        }
        const waste_api_type_rule_t *r = &cfg->type_rules[i];
        char name_field[16], ignored_field[16], color_field[16];
        snprintf(name_field, sizeof(name_field), "type%d_name", idx);
        snprintf(ignored_field, sizeof(ignored_field), "type%d_ignored", idx);
        snprintf(color_field, sizeof(color_field), "type%d_color", idx);

        // Third-party data (the council API names the types). Escaped once,
        // used as both cell text and attribute value; the browser decodes the
        // entities before submitting, so the stored value round-trips intact.
        char esc_type[sizeof(r->event_type) * 6 + 1];
        html_escape_attr(r->event_type, esc_type, sizeof(esc_type));

        off = safe_append(buf, buf_size, off,
            "<tr><td>%s<input type='hidden' form='mapform' name='%s' value='%s'></td>"
            "<td><input type='checkbox' form='mapform' name='%s' %s></td><td>",
            esc_type, name_field, esc_type, ignored_field, r->ignored ? "checked" : "");
        off = append_color_select_for(buf, buf_size, off, color_field, r->color, "mapform");
        off = safe_append(buf, buf_size, off, "</td></tr>");
        idx++;
    }
    return safe_append(buf, buf_size, off,
        "</table><p><button type='submit' form='mapform'>Save mapping</button></p>");
}

// The configured council's display name, whatever the backend. Bespoke
// backends serve exactly one council each, so a backend lookup suffices;
// Impact Apps resolves by subdomain (falling back to the raw subdomain for
// unlisted councils reached via the free-text escape hatch).
static const char *api_council_name(const waste_api_config_t *cfg)
{
    if (cfg->backend == COUNCIL_BACKEND_IMPACT_APPS) {
        return council_display_name(cfg->council_subdomain);
    }
    const council_t *c = council_find_by_backend(cfg->backend);
    return (c != NULL) ? c->name : "?";
}

// out must be at least HHMM_BUF_SIZE bytes. Sized for uint16_t's full range
// (not just the 0-1439 minutes-since-midnight values we actually pass), so
// the compiler's format-truncation check can prove this never truncates.
#define HHMM_BUF_SIZE 8

static void minutes_to_hhmm(uint16_t minutes, char *out)
{
    snprintf(out, HHMM_BUF_SIZE, "%02u:%02u", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
}

static uint16_t parse_hhmm(const char *s)
{
    int h = 0, m = 0;
    sscanf(s, "%d:%d", &h, &m);
    if (h < 0) h = 0;
    if (h > 23) h = 23;
    if (m < 0) m = 0;
    if (m > 59) m = 59;
    return (uint16_t)(h * 60 + m);
}

// value must already be url_decode_inplace()'d, so '#' is a literal here.
static void parse_hex_color(const char *value, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const char *hex = (value[0] == '#') ? value + 1 : value;
    long rgb = strtol(hex, NULL, 16);
    *r = (rgb >> 16) & 0xFF;
    *g = (rgb >> 8) & 0xFF;
    *b = rgb & 0xFF;
}

// Finds a TZ_PRESETS entry matching tz exactly, or NULL if it's a custom string.
static const tz_preset_t *find_tz_preset(const char *tz)
{
    for (size_t i = 0; i < TZ_PRESET_COUNT; i++) {
        if (strcmp(TZ_PRESETS[i].tz, tz) == 0) {
            return &TZ_PRESETS[i];
        }
    }
    return NULL;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    schedule_t s = schedule_get();

    char *html = malloc(HTML_BUF_SIZE);
    if (html == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char start_str[HHMM_BUF_SIZE];
    minutes_to_hhmm(s.start_minute, start_str);

    waste_api_config_t api_cfg = waste_api_get_config();

    int off = 0;
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Bin Light</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        "table{width:100%%;border-collapse:collapse;}"
        "td,th{padding:.4em;text-align:left;border-bottom:1px solid #ccc;}"
        "input[type=time],input[type=date]{width:9em;}"
        "input[type=range]{width:8em;}"
        "input[type=number]{width:4em;}"
        ".note{color:#888;}"
        // Progressive disclosure without JavaScript (SPEC.md 3.11): the
        // section's own enable checkbox reveals its detail block via the
        // sibling combinator. The .sect wrapper is load-bearing - a bare
        // "input:checked ~ .details" would match *every* later .details on the
        // page, so ticking the API box would also expand the manual schedule.
        ".sect{margin:1.5em 0;}"
        ".details{display:none;}"
        ".sect input:checked ~ .details{display:block;}"
        "</style></head><body>"
        "<h1>Bin Light</h1>"
        "<form method='POST' action='/test' style='margin:0 0 1em 0'>"
        "<button type='submit'>Display Next Collection (30 seconds)</button></form>");

    // Emitted before the /save form opens, because forms can't nest - see the
    // comment on append_type_mapping_anchor().
    off = append_type_mapping_anchor(html, HTML_BUF_SIZE, off, &api_cfg, "/");

    off = safe_append(html, HTML_BUF_SIZE, off, "<form method='POST' action='/save'>");

    // ---- Preferences: settings that apply whichever schedule is driving the
    // light, so they sit above (and outside) both schedule sections.
    const char *current_tz = settings_get_tz();
    const tz_preset_t *current_preset = find_tz_preset(current_tz);
    char escaped_tz[SETTINGS_TZ_MAX_LEN * 6 + 1];
    html_escape_attr(current_tz, escaped_tz, sizeof(escaped_tz));

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<h2>Preferences</h2>"
        "<table>"
        "<tr><td>Brightness</td><td><input type='range' name='brightness' min='10' max='255' value='%u'></td></tr>"
        "<tr><td>On from</td><td><input type='time' name='start' value='%s'></td></tr>"
        "<tr><td>Turn off after</td><td><input type='number' name='duration_hours' min='1' max='23' value='%u'> hours</td></tr>"
        "<tr><td colspan='2' class='note'>The light turns on at the chosen time on the night "
        "before a collection, and stays on for the given number of hours (carrying past "
        "midnight if needed).</td></tr>",
        (unsigned)s.brightness, start_str, (unsigned)s.duration_hours);

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<tr><td>Light mode</td><td><select name='light_mode'>"
        "<option value='%d' %s>Single colour (both LEDs match)</option>"
        "<option value='%d' %s>Dual colour (second LED shows its own colour)</option>"
        "</select></td></tr>",
        LIGHT_MODE_SINGLE_COLOUR, (s.light_mode == LIGHT_MODE_SINGLE_COLOUR) ? "selected" : "",
        LIGHT_MODE_DUAL_COLOUR, (s.light_mode == LIGHT_MODE_DUAL_COLOUR) ? "selected" : "");

    off = safe_append(html, HTML_BUF_SIZE, off, "<tr><td>Second LED default colour</td><td>");
    off = append_color_select(html, HTML_BUF_SIZE, off, "secondary_default_color", s.secondary_default_color);
    off = safe_append(html, HTML_BUF_SIZE, off,
        "</td></tr>"
        "<tr><td colspan='2' class='note'>In dual colour mode, the second LED shows this "
        "colour (general waste, by default red) unless the schedule finds two distinct bin "
        "types due the same night, in which case it shows the second one instead.</td></tr>");

    off = safe_append(html, HTML_BUF_SIZE, off, "<tr><td>Timezone</td><td><select name='tz'>");
    for (size_t i = 0; i < TZ_PRESET_COUNT; i++) {
        off = safe_append(html, HTML_BUF_SIZE, off, "<option value='%s' %s>%s</option>",
            TZ_PRESETS[i].tz, (&TZ_PRESETS[i] == current_preset) ? "selected" : "", TZ_PRESETS[i].label);
    }
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<option value='custom' %s>Custom&hellip;</option></select></td></tr>"
        "<tr><td>Custom TZ</td><td><input type='text' name='tz_custom' size='24' value='%s'></td></tr>"
        "<tr><td colspan='2' class='note'>The custom POSIX TZ string is used only when "
        "\"Custom&hellip;\" is selected above.</td></tr>"
        "</table>",
        current_preset == NULL ? "selected" : "", escaped_tz);

    // ---- Bin collection API (collapsed until enabled)
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<div class='sect'><h2>Bin collection API</h2>"
        "<input type='checkbox' id='api_en' name='api_enabled' %s>"
        "<label for='api_en'> Use automatic bin collection API</label>"
        "<div class='details'>"
        "<p class='note'>When enabled and reachable, the API is authoritative: it overrides "
        "the manual / fallback schedule entirely, including turning the light off on weeks "
        "it reports nothing due.</p>",
        api_cfg.enabled ? "checked" : "");

    if (waste_api_config_complete(&api_cfg)) {
        // Both strings are attacker-influenceable: the label arrives via the
        // setup wizard's ?label= query parameter, and the council name falls
        // back to the raw typed subdomain for unlisted councils.
        char esc_council[WASTE_API_SUBDOMAIN_MAX_LEN * 6 + 1];
        char esc_label[WASTE_API_LABEL_MAX_LEN * 6 + 1];
        html_escape_attr(api_council_name(&api_cfg), esc_council, sizeof(esc_council));
        html_escape_attr(api_cfg.property_label, esc_label, sizeof(esc_label));
        off = safe_append(html, HTML_BUF_SIZE, off,
            "<p>Configured: <b>%s</b><br>%s</p>", esc_council, esc_label);
    } else {
        off = safe_append(html, HTML_BUF_SIZE, off, "<p>No council/address configured yet.</p>");
    }

    // Rendered from the saved config directly, no live fetch, so the home page
    // stays fast. /api-setup auto-populates this at setup time; /api-test can
    // (re)discover new types via a live fetch.
    off = append_type_mapping_rows(html, HTML_BUF_SIZE, off, &api_cfg);

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p><a href='/api-setup'>Change council / address</a>"
        " &middot; <a href='/api-test'>Test API (show upcoming weeks)</a></p>"
        "</div></div>");

    // ---- Manual / fallback schedule (collapsed until enabled)
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<div class='sect'><h2>Manual / Fallback Schedule</h2>"
        "<input type='checkbox' id='man_en' name='enabled' %s>"
        "<label for='man_en'> Use manual / fallback schedule</label>"
        "<div class='details'>"
        "<p class='note'>Optional. This is only used when the API above is switched off or "
        "not configured, or when it can't be reached and the last collection date it gave "
        "us has already passed. If the API is working, nothing here has any effect.</p>"
        "<table>",
        s.enabled ? "checked" : "");

    off = safe_append(html, HTML_BUF_SIZE, off, "<tr><td>Bin night</td><td><select name='bin_night_weekday'>");
    for (int i = 0; i < 7; i++) {
        off = safe_append(html, HTML_BUF_SIZE, off, "<option value='%d' %s>%s</option>",
            i, (i == s.bin_night_weekday) ? "selected" : "", WEEKDAY_LABEL[i]);
    }
    off = safe_append(html, HTML_BUF_SIZE, off, "</select></td></tr>");

    for (int i = 0; i < SCHEDULE_MAX_COLOR_RULES; i++) {
        const schedule_color_rule_t *r = &s.rules[i];
        const char *prefix = RULE_FIELD_PREFIX[i];
        char color_field[24];
        snprintf(color_field, sizeof(color_field), "%s_color", prefix);

        off = safe_append(html, HTML_BUF_SIZE, off,
            "<tr><td colspan='2'><b>Colour %d</b></td></tr>"
            "<tr><td>Enabled</td><td><input type='checkbox' name='%s_enabled' %s></td></tr>"
            "<tr><td>Colour</td><td>",
            i + 1, prefix, r->enabled ? "checked" : "");
        off = append_color_select(html, HTML_BUF_SIZE, off, color_field, r->color);
        off = safe_append(html, HTML_BUF_SIZE, off,
            "</td></tr>"
            "<tr><td>First collection</td><td><input type='date' name='%s_first_date' value='%04u-%02u-%02u'></td></tr>"
            "<tr><td>Every</td><td><select name='%s_frequency'>",
            prefix, (unsigned)r->first_year, (unsigned)r->first_month, (unsigned)r->first_day, prefix);
        for (int f = 1; f <= 4; f++) {
            off = safe_append(html, HTML_BUF_SIZE, off, "<option value='%d' %s>%d week%s</option>",
                f, (f == r->frequency_weeks) ? "selected" : "", f, f == 1 ? "" : "s");
        }
        off = safe_append(html, HTML_BUF_SIZE, off, "</select></td></tr>");
    }

    off = safe_append(html, HTML_BUF_SIZE, off,
        "</table>"
        "<h3>How the colour rules work</h3>"
        "<p>Each colour has its own independent cycle: its first collection date "
        "and how often it repeats (every 1-4 weeks) &mdash; these don't need to "
        "match between colours. If more than one colour is due the same week, "
        "Colour 1 takes priority, then Colour 2, then Colour 3.</p>"
        "<p class='note'>Example: Yellow first collected 6 Jul 2026, every "
        "2 weeks. Green first collected 13 Jul 2026, every 3 weeks:</p>"
        "<ul class='note'>"
        "<li>Week of 6 Jul &rarr; <b>Yellow</b> (Yellow's first week)</li>"
        "<li>Week of 13 Jul &rarr; <b>Green</b> (Green's first week)</li>"
        "<li>Week of 20 Jul &rarr; <b>Yellow</b> (2 weeks after its first)</li>"
        "<li>Week of 27 Jul &rarr; nothing due</li>"
        "<li>Week of 3 Aug &rarr; <b>Yellow</b>, and Green is also due this week "
        "&mdash; Yellow wins since it's Colour 1</li>"
        "</ul>"
        "<p class='note'>To disable a colour entirely, leave its \"Enabled\" "
        "box unchecked.</p>"
        "</div></div>");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p style='margin-top:1em'><button type='submit'>Save</button></p>"
        "</form>");

    // Outside the /save form (these post elsewhere), last on the page because
    // they're the destructive actions. The two are deliberately separate and
    // described by what they *keep*: the difference between them is the whole
    // reason to have both.
    //
    // The SSID is escaped: it is at most 32 bytes but its content is whatever
    // the joined network calls itself.
    char esc_ssid[33 * 6 + 1];
    html_escape_attr(wifi_manager_current_ssid(), esc_ssid, sizeof(esc_ssid));
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<div class='sect'><h2>Wi-Fi</h2>"
        "<p>Connected to <b>%s</b>.</p>"
        "<form method='POST' action='/wifi-forget'>"
        "<button type='submit'>Forget this network and restart setup</button></form>"
        "<p class='note'>Moves the light to a different Wi-Fi network. It restarts into "
        "setup mode. <b>Everything else is kept</b> &mdash; schedule, council and colours.</p>"
        "</div>",
        esc_ssid);

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<div class='sect'><h2>Firmware</h2>"
        "<p>Version <b>%s</b>.</p>"
        "<form method='POST' action='/update'>"
        "<button type='submit'>Check for updates</button></form>"
        "<p class='note'>Automatic updates are <b>%s</b>. Your settings are kept across an "
        "update either way.</p>"
        "</div>", ota_running_version(), ota_auto_update_enabled() ? "on" : "off");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<div class='sect'><h2>Restart</h2>"
        "<form method='POST' action='/reboot'>"
        "<button type='submit'>Restart the light</button></form>"
        "<p class='note'><b>Nothing is lost</b> &mdash; the light just reboots. Same as "
        "holding the reset button for 3 seconds.</p>"
        "</div>");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<div class='sect'><h2>Factory reset</h2>"
        "<form method='POST' action='/factory-reset'>"
        "<button type='submit'>Factory reset&hellip;</button></form>"
        "<p class='note'>Erases <b>everything</b>, including the Wi-Fi network, and "
        "returns the light to how it left the workbench. You'll be asked to confirm "
        "first. Same as holding the reset button for 10 seconds.</p>"
        "</div>");

    off = safe_append(html, HTML_BUF_SIZE, off, "</body></html>");

    if (off >= HTML_BUF_SIZE) {
        ESP_LOGE(TAG, "home page truncated at %d bytes - raise HTML_BUF_SIZE", HTML_BUF_SIZE);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(html);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len >= SAVE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    schedule_t new_schedule = {0};
    char value[FIELD_BUF_SIZE];

    bool api_enabled = (httpd_query_key_value(body, "api_enabled", value, sizeof(value)) == ESP_OK);

    new_schedule.enabled = (httpd_query_key_value(body, "enabled", value, sizeof(value)) == ESP_OK);

    if (httpd_query_key_value(body, "bin_night_weekday", value, sizeof(value)) == ESP_OK) {
        new_schedule.bin_night_weekday = (uint8_t)atoi(value);
    }

    if (httpd_query_key_value(body, "start", value, sizeof(value)) == ESP_OK) {
        url_decode_inplace(value);
        new_schedule.start_minute = parse_hhmm(value);
    }
    if (httpd_query_key_value(body, "duration_hours", value, sizeof(value)) == ESP_OK) {
        new_schedule.duration_hours = (uint8_t)atoi(value);
    }
    if (httpd_query_key_value(body, "brightness", value, sizeof(value)) == ESP_OK) {
        new_schedule.brightness = (uint8_t)atoi(value);
    }
    if (httpd_query_key_value(body, "light_mode", value, sizeof(value)) == ESP_OK) {
        new_schedule.light_mode = (uint8_t)atoi(value);
    }
    if (httpd_query_key_value(body, "secondary_default_color", value, sizeof(value)) == ESP_OK) {
        url_decode_inplace(value);
        parse_hex_color(value, &new_schedule.secondary_default_color.r,
                         &new_schedule.secondary_default_color.g, &new_schedule.secondary_default_color.b);
    }
    char key[24];
    for (int i = 0; i < SCHEDULE_MAX_COLOR_RULES; i++) {
        schedule_color_rule_t *r = &new_schedule.rules[i];
        const char *prefix = RULE_FIELD_PREFIX[i];

        snprintf(key, sizeof(key), "%s_enabled", prefix);
        r->enabled = (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK);

        snprintf(key, sizeof(key), "%s_color", prefix);
        if (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK) {
            url_decode_inplace(value);
            parse_hex_color(value, &r->color.r, &r->color.g, &r->color.b);
        }

        snprintf(key, sizeof(key), "%s_first_date", prefix);
        if (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK) {
            url_decode_inplace(value);
            int y = 0, mo = 0, d = 0;
            sscanf(value, "%d-%d-%d", &y, &mo, &d);
            r->first_year = (uint16_t)y;
            r->first_month = (uint8_t)mo;
            r->first_day = (uint8_t)d;
        }

        snprintf(key, sizeof(key), "%s_frequency", prefix);
        if (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK) {
            r->frequency_weeks = (uint8_t)atoi(value);
        }
    }

    char tz_value[TZ_FIELD_BUF_SIZE];
    char tz_custom_value[TZ_FIELD_BUF_SIZE];
    bool have_tz = httpd_query_key_value(body, "tz", tz_value, sizeof(tz_value)) == ESP_OK;
    bool have_tz_custom = httpd_query_key_value(body, "tz_custom", tz_custom_value, sizeof(tz_custom_value)) == ESP_OK;
    if (have_tz) {
        url_decode_inplace(tz_value);
    }
    if (have_tz_custom) {
        url_decode_inplace(tz_custom_value);
    }

    free(body);

    esp_err_t err = schedule_set(&new_schedule);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save schedule");
        return ESP_FAIL;
    }
    schedule_task_force_check();

    if (have_tz) {
        const char *new_tz = (strcmp(tz_value, "custom") == 0 && have_tz_custom) ? tz_custom_value : tz_value;
        if (settings_set_tz(new_tz) != ESP_OK) {
            ESP_LOGW(TAG, "rejected timezone value, keeping previous setting");
        }
    }

    waste_api_config_t api_cfg = waste_api_get_config();
    if (api_cfg.enabled != api_enabled) {
        api_cfg.enabled = api_enabled;
        waste_api_set_config(&api_cfg);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Erases the stored Wi-Fi credentials and reboots into AutoAP setup mode
// (SPEC.md 3.4) - the deliberate path for moving a device to a new network.
// The device normally falls into AutoAP by itself when the stored network is
// unreachable, so this is for the case where the old network still exists and
// works: handing the device to someone else, or changing which network it
// should be on.
//
// Renders a confirmation page and only then reboots, so the response reaches
// the browser before the connection drops.
static esp_err_t wifi_forget_post_handler(httpd_req_t *req)
{
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    esp_err_t err = wifi_manager_forget_credentials();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to erase credentials");
        return ESP_FAIL;
    }

    static const char PAGE[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Wi-Fi Reset</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}</style>"
        "</head><body><h1>Wi-Fi reset</h1>"
        "<p>The saved network has been forgotten and the light is restarting.</p>"
        "<p>In a few seconds both LEDs will start breathing white, and a Wi-Fi network "
        "named <b>binlight-XXXX</b> will appear. Join it, then open "
        "<b>http://binlight.local/</b> &mdash; or <b>http://192.168.4.1/</b> if your "
        "phone or browser can't find that name.</p>"
        "<p>Your schedule and council settings have been kept.</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);

    // Let the response actually flush before the reset.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK; // not reached
}

// Firmware updates (SPEC.md 3.5), one handler covering three steps:
//   no action      -> check the manifest and report what's published
//   action=install -> kick off the download, then show progress
//   action=status  -> progress only (the page auto-refreshes into this)
// POST throughout: checking is a network call and installing is obviously not
// something a link should trigger.
static esp_err_t update_post_handler(httpd_req_t *req)
{
    // POST only - reject_cross_origin() lets every GET through, which is what
    // keeps the dual-registered GET /update (see the registration below) unable
    // to answer 403.
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    char body[320] = "";
    if (req->content_len > 0 && req->content_len < (int)sizeof(body)) {
        int received = 0;
        while (received < req->content_len) {
            int ret = httpd_req_recv(req, body + received, req->content_len - received);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    continue;
                }
                return ESP_FAIL;
            }
            received += ret;
        }
        body[received] = '\0';
    }
    char action[16] = "";
    httpd_query_key_value(body, "action", action, sizeof(action));

    char *html = malloc(HTML_BUF_SIZE);
    if (html == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int off = 0;

    if (strcmp(action, "setauto") == 0) {
        char value[8];
        // An unchecked checkbox submits nothing, so absence means "off".
        ota_set_auto_update(httpd_query_key_value(body, "auto", value, sizeof(value)) == ESP_OK);
    }

    bool installing = (strcmp(action, "install") == 0);
    if (installing) {
        char url[256] = "";
        if (httpd_query_key_value(body, "url", url, sizeof(url)) == ESP_OK) {
            url_decode_inplace(url);
            ota_start(url, true); // manual install: the user is here, restart when done
        }
    }
    bool show_progress = installing || (strcmp(action, "status") == 0) ||
                         ota_get_state() == OTA_STATE_RUNNING;

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Firmware Update</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        ".note{color:#888;}code{background:#eee;padding:.1em .3em;}</style>"
        "</head><body><h1>Firmware update</h1>"
        "<p>Running version: <b>%s</b></p>", ota_running_version());

    if (show_progress) {
        ota_state_t state = ota_get_state();
        if (state == OTA_STATE_RUNNING) {
            // No JavaScript, so progress is a meta-refresh poll. 3s is often
            // enough to see the percentage move without hammering the device
            // while it is busy pulling ~1.3MB over TLS.
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<meta http-equiv='refresh' content='3'>"
                "<p><b>Updating: %s</b></p>"
                "<p class='note'>Leave this page open. Don't power the light off until it "
                "finishes &mdash; if it is interrupted the light keeps running its current "
                "firmware, so a failed update is recoverable, but a half-written one wastes "
                "the download.</p>", ota_get_message());
        } else if (state == OTA_STATE_SUCCESS) {
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<meta http-equiv='refresh' content='15'>"
                "<p><b>Update installed &mdash; the light is restarting now.</b></p>"
                "<p class='note'>It will be back in a few seconds. If the new firmware "
                "can't get onto Wi-Fi, the light rolls itself back to the current version "
                "on the reboot after that.</p>"
                "<p class='note'>Nothing happening? Use the button &mdash; the restart is "
                "automatic, but this is here in case it doesn't take.</p>"
                "<form method='POST' action='/reboot'>"
                "<button type='submit'>Restart now</button></form>");
        } else {
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<p><b>Update failed:</b> %s</p>"
                "<p class='note'>Nothing has changed &mdash; the light is still running its "
                "current firmware.</p>", ota_get_message());
        }
    } else {
        ota_manifest_t m;
        // The manifest is owner-published, but it is still remote input - the
        // version is escaped like the url and notes already are.
        char esc_version[sizeof(m.version) * 6 + 1];
        if (ota_check(&m) != ESP_OK) {
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<p><b>Couldn't check for updates.</b></p>"
                "<p class='note'>The light couldn't reach the update manifest. Check its "
                "internet connection and try again.</p>");
        } else if (!m.available) {
            html_escape_attr(m.version, esc_version, sizeof(esc_version));
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<p><b>Up to date.</b> The published version is <b>%s</b>, which is what "
                "this light is running.</p>", esc_version);
        } else {
            char esc_url[sizeof(m.url) * 6 + 1];
            char esc_notes[sizeof(m.notes) * 6 + 1];
            html_escape_attr(m.url, esc_url, sizeof(esc_url));
            html_escape_attr(m.notes, esc_notes, sizeof(esc_notes));
            html_escape_attr(m.version, esc_version, sizeof(esc_version));
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<p><b>Version %s is available.</b></p>", esc_version);
            if (m.notes[0] != '\0') {
                off = safe_append(html, HTML_BUF_SIZE, off, "<p>%s</p>", esc_notes);
            }
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<form method='POST' action='/update'>"
                "<input type='hidden' name='action' value='install'>"
                "<input type='hidden' name='url' value='%s'>"
                "<button type='submit'>Download and install</button></form>"
                "<p class='note'>Takes a couple of minutes. Your settings are kept &mdash; "
                "an update replaces the firmware, not the configuration.</p>", esc_url);
        }
    }

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<hr style='margin-top:1.5em'>"
        "<form method='POST' action='/update'>"
        "<input type='hidden' name='action' value='setauto'>"
        "<p><label><input type='checkbox' name='auto' %s> Install updates automatically</label></p>"
        "<button type='submit'>Save</button></form>"
        "<p class='note'>Checks once a day and installs anything new on its own. It waits "
        "until the light is off before restarting, so an update never interrupts a bin-night "
        "reminder. If an update ever stopped the light getting back onto Wi-Fi, it reverts to "
        "the previous version by itself.</p>",
        ota_auto_update_enabled() ? "checked" : "");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p style='margin-top:1em'><a href='/'>&larr; Back to schedule</a></p></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(html);
    return ESP_OK;
}

// Restart the device (SPEC.md 3.12). Unlike the two resets below this loses
// nothing - it's the "have you tried turning it off and on again" action for
// a device that's misbehaving but still serving pages - so it goes straight
// through with no confirmation step.
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    static const char PAGE[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<meta http-equiv='refresh' content='12; url=/'>"
        "<title>Restarting</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        ".note{color:#888;}</style></head><body>"
        "<h1>Restarting</h1>"
        "<p>The light is restarting. Nothing has been changed &mdash; your Wi-Fi, "
        "council setup and schedule are all still there.</p>"
        "<p class='note'>You'll see the LEDs run their startup colour test. This page "
        "will try to come back on its own in a few seconds; if it doesn't, browse to "
        "<b>http://binlight.local</b> again.</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);

    ESP_LOGW(TAG, "reboot requested from the web UI");
    vTaskDelay(pdMS_TO_TICKS(1500)); // let the response reach the browser
    esp_restart();
    return ESP_OK; // not reached
}

// Factory reset (SPEC.md 3.12), in two steps through one handler: the first
// POST renders an "are you sure?" page spelling out exactly what is lost, and
// only a second POST carrying confirm=yes actually wipes. Deliberately not a
// GET at any stage - nothing destructive should be reachable by following a
// link or replaying a URL from history.
static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    char body[64] = "";
    bool confirmed = false;
    if (req->content_len > 0 && req->content_len < (int)sizeof(body)) {
        int received = 0;
        while (received < req->content_len) {
            int ret = httpd_req_recv(req, body + received, req->content_len - received);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    continue;
                }
                return ESP_FAIL;
            }
            received += ret;
        }
        body[received] = '\0';
        char value[8];
        confirmed = httpd_query_key_value(body, "confirm", value, sizeof(value)) == ESP_OK &&
                    strcmp(value, "yes") == 0;
    }

    static const char HEAD[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Factory Reset</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        ".warn{border:2px solid #a00;padding:.8em 1em;border-radius:4px;}"
        ".note{color:#888;}</style></head><body>";

    if (!confirmed) {
        static const char CONFIRM_PAGE[] =
            "<h1>Factory reset</h1>"
            "<div class='warn'>"
            "<p><b>Are you sure you want to reset this bin light?</b></p>"
            "<p>All configuration will be lost, including:</p>"
            "<ul>"
            "<li>the Wi-Fi network and password</li>"
            "<li>the council and address setup, and the bin colour mapping</li>"
            "<li>the manual / fallback schedule and its colour rules</li>"
            "<li>brightness, on-time, light mode and timezone</li>"
            "</ul>"
            "<p>The light will restart into setup mode with both LEDs breathing white, "
            "and will need to be set up again from scratch &mdash; starting with joining "
            "it to your Wi-Fi.</p>"
            "<p><b>This cannot be undone.</b></p>"
            "</div>"
            "<form method='POST' action='/factory-reset' style='margin-top:1em'>"
            "<input type='hidden' name='confirm' value='yes'>"
            "<button type='submit'>Yes, erase everything and restart</button></form>"
            "<p style='margin-top:1em'><a href='/'>&larr; No, take me back</a></p>"
            "</body></html>";

        char *page = malloc(sizeof(HEAD) + sizeof(CONFIRM_PAGE));
        if (page == NULL) {
            return ESP_ERR_NO_MEM;
        }
        int off = 0;
        memcpy(page, HEAD, sizeof(HEAD) - 1);
        off += sizeof(HEAD) - 1;
        memcpy(page + off, CONFIRM_PAGE, sizeof(CONFIRM_PAGE) - 1);
        off += sizeof(CONFIRM_PAGE) - 1;

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, page, off);
        free(page);
        return ESP_OK;
    }

    static const char DONE_PAGE[] =
        "<h1>Reset</h1>"
        "<p>Everything has been erased and the light is restarting.</p>"
        "<p>In a few seconds both LEDs will start breathing white and a Wi-Fi network "
        "named <b>binlight-XXXX</b> will appear. Join it, then open "
        "<b>http://binlight.local/</b> &mdash; or <b>http://192.168.4.1/</b> if your "
        "phone or browser can't find that name.</p>"
        "</body></html>";

    char *page = malloc(sizeof(HEAD) + sizeof(DONE_PAGE));
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int off = 0;
    memcpy(page, HEAD, sizeof(HEAD) - 1);
    off += sizeof(HEAD) - 1;
    memcpy(page + off, DONE_PAGE, sizeof(DONE_PAGE) - 1);
    off += sizeof(DONE_PAGE) - 1;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, off);
    free(page);

    // Send first, wipe second: the browser must have the page before the
    // device disappears off the network.
    vTaskDelay(pdMS_TO_TICKS(1500));
    factory_reset_perform(); // does not return
}

// Previews whatever the light would show at its next scheduled occurrence
// (SPEC.md 3.8) for a fixed 30 seconds, then hands back to the real evaluator.
// No body to read - the button always previews "next", nothing to configure.
static esp_err_t test_post_handler(httpd_req_t *req)
{
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    schedule_test_trigger();

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

#define SETUP_LOOKUP_MAX    150
// Bespoke address search renders each match as a link carrying the (encoded)
// opaque id - Merri-bek's packed ids make those links ~800 bytes each, so
// this cap is what keeps the page inside SETUP_HTML_BUF_SIZE.
#define SETUP_SEARCH_MAX    12
#define SETUP_HTML_BUF_SIZE 20000
// One scratch for html_escape_attr() at the setup and API-test pages' print
// sinks. Heap rather than stack because those handlers also run blocking TLS
// fetches on their own task stack (see web_server_start), which is where the
// headroom matters. Sized for the largest escapable input, a bespoke
// search-result label.
#define PAGE_ESC_BUF_SIZE  (sizeof(((waste_api_search_result_t *)0)->label) * 6 + 1)

// Percent-encodes a string for safe use as one application/x-www-form-urlencoded
// query value (the mirror of url_decode_inplace() - needed here because this
// handler builds its own <a href='...'> links carrying council/street/property
// names forward between wizard steps).
static void url_encode_component(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; *in != '\0' && o + 4 < out_size; in++) {
        unsigned char c = (unsigned char)*in;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out[o++] = (char)c;
        } else {
            o += snprintf(out + o, out_size - o, "%%%02X", (unsigned)c);
        }
    }
    out[o] = '\0';
}

// Reads one decoded query-string parameter from a GET request. Returns false
// if there's no query string or the key isn't present (out is left untouched
// in that case, so callers can pre-fill defaults).
static bool get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) {
        return false;
    }
    char *query = malloc(qlen + 1);
    if (query == NULL) {
        return false;
    }
    bool found = false;
    if (httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK) {
        found = httpd_query_key_value(query, key, out, out_size) == ESP_OK;
    }
    free(query);
    if (found) {
        url_decode_inplace(out);
    }
    return found;
}

// Auto-populate a starting colour mapping: fetch what's actually coming up
// for this property and build one rule per distinct type seen, defaulting
// "waste" to ignored (weekly, no reminder value) and everything else to the
// name-keyed default, falling back to the nearest preset to the API's own
// colour - so the mapping on the home page usually needs no manual editing.
static void auto_map_type_rules(waste_api_config_t *cfg)
{
    memset(cfg->type_rules, 0, sizeof(cfg->type_rules));
    waste_api_event_t events[API_TEST_MAX_EVENTS];
    int n = waste_api_fetch_upcoming(cfg, API_TEST_LOOKAHEAD_DAYS, events, API_TEST_MAX_EVENTS);
    int rule_count = 0;
    for (int i = 0; i < n && rule_count < WASTE_API_MAX_TYPE_RULES; i++) {
        bool already = false;
        for (int r = 0; r < rule_count; r++) {
            if (strcmp(cfg->type_rules[r].event_type, events[i].event_type) == 0) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        waste_api_type_rule_t *rule = &cfg->type_rules[rule_count];
        snprintf(rule->event_type, sizeof(rule->event_type), "%s", events[i].event_type);
        rule->ignored = (strcmp(events[i].event_type, "waste") == 0);
        schedule_color_t named_default;
        rule->color = default_color_for_type(events[i].event_type, &named_default)
                          ? named_default
                          : nearest_preset_color(events[i].color);
        rule_count++;
    }
}

// No-JS, server-rendered setup wizard for the external bin-collection API.
// Every step is a plain GET link carrying accumulated state in the query
// string; the final save step persists via waste_api_set_config() and
// redirects home. Saving via a GET link rather than a POST is a deliberate
// simplification - no CSRF-relevant risk on a single-user LAN device.
//
// Two flows, chosen by the selected council's backend (SPEC.md 3.13.5):
//   Impact Apps:  council -> locality -> street -> property -> save
//   bespoke:      council -> address search -> pick a match -> bsave
static esp_err_t api_setup_get_handler(httpd_req_t *req)
{
    char step[16] = "";
    // No hardcoded default - the council now comes from the dropdown, and the
    // free-text field below it prefills from whatever is already configured.
    char subdomain[WASTE_API_SUBDOMAIN_MAX_LEN + 1] = "";
    char locality_id_str[16] = "";
    char street_id_str[16] = "";
    char property_id_str[16] = "";
    char label[WASTE_API_LABEL_MAX_LEN + 1] = "";
    char council_param[40] = "";
    char query[80] = "";
    char address_id[WASTE_API_ADDRESS_ID_MAX_LEN + 1] = "";

    get_query_param(req, "step", step, sizeof(step));
    get_query_param(req, "subdomain", subdomain, sizeof(subdomain));
    get_query_param(req, "locality", locality_id_str, sizeof(locality_id_str));
    get_query_param(req, "street", street_id_str, sizeof(street_id_str));
    get_query_param(req, "property", property_id_str, sizeof(property_id_str));
    get_query_param(req, "label", label, sizeof(label));
    get_query_param(req, "council", council_param, sizeof(council_param));
    get_query_param(req, "q", query, sizeof(query));
    get_query_param(req, "id", address_id, sizeof(address_id));

    const council_t *council = council_find_by_param(council_param);

    // A council picked from the dropdown routes by its backend: Impact Apps
    // continues into the existing locality wizard; a bespoke backend goes to
    // the single search-and-pick flow.
    if (strcmp(step, "council") == 0 && council != NULL &&
        council->backend == COUNCIL_BACKEND_IMPACT_APPS) {
        snprintf(subdomain, sizeof(subdomain), "%s", council->param);
        snprintf(step, sizeof(step), "locality");
    }

    if (strcmp(step, "save") == 0 && property_id_str[0] != '\0') {
        waste_api_config_t cfg = waste_api_get_config();
        cfg.enabled = true;
        cfg.backend = COUNCIL_BACKEND_IMPACT_APPS;
        snprintf(cfg.council_subdomain, sizeof(cfg.council_subdomain), "%s", subdomain);
        cfg.property_id = (uint32_t)strtoul(property_id_str, NULL, 10);
        cfg.address_id[0] = '\0';
        snprintf(cfg.property_label, sizeof(cfg.property_label), "%s", label);
        auto_map_type_rules(&cfg);

        // Persists and wakes the poll task to fetch immediately, so the real
        // evaluator has fresh, mapped data right away rather than waiting for
        // the next 12h interval.
        waste_api_set_config(&cfg);

        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strcmp(step, "bsave") == 0 && council != NULL && address_id[0] != '\0') {
        waste_api_config_t cfg = waste_api_get_config();
        cfg.enabled = true;
        cfg.backend = (uint8_t)council->backend;
        cfg.council_subdomain[0] = '\0';
        cfg.property_id = 0;
        snprintf(cfg.address_id, sizeof(cfg.address_id), "%s", address_id);
        snprintf(cfg.property_label, sizeof(cfg.property_label), "%s", label);
        auto_map_type_rules(&cfg);
        waste_api_set_config(&cfg);

        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char *html = malloc(SETUP_HTML_BUF_SIZE);
    char *esc = malloc(PAGE_ESC_BUF_SIZE);
    if (html == NULL || esc == NULL) {
        free(html);
        free(esc);
        return ESP_ERR_NO_MEM;
    }

    int off = 0;
    off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Bin Collection API Setup</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        "a.item{display:block;padding:.35em 0;border-bottom:1px solid #eee;text-decoration:none;}"
        ".note{color:#888;}"
        "select{max-width:100%%;}"
        "</style></head><body>"
        "<h1>Bin Collection API Setup</h1>"
        "<p><a href='/'>&larr; Back to schedule</a></p>");

    char enc_subdomain[WASTE_API_SUBDOMAIN_MAX_LEN * 3 + 1];
    url_encode_component(subdomain, enc_subdomain, sizeof(enc_subdomain));

    if (strcmp(step, "council") == 0 && council != NULL) {
        // Bespoke backend: one search box instead of the three-level wizard.
        char enc_param[sizeof(council_param) * 3 + 1];
        url_encode_component(council->param, enc_param, sizeof(enc_param));
        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "<h2>%s</h2>"
            "<p>Start typing your street address, then pick your property from "
            "the matches.</p>"
            "<form method='GET' action='/api-setup'>"
            "<input type='hidden' name='step' value='bsearch'>"
            "<input type='hidden' name='council' value='%s'>"
            "<label>Address: <input type='text' name='q' size='28'></label> "
            "<button type='submit'>Search</button>"
            "</form>",
            council->name, enc_param);

        if (council->backend == COUNCIL_BACKEND_MERRI_BEK) {
            // Merri-bek's address layer stores addresses in one exact
            // canonical form, and this search matches from the start of that
            // string - so the most reliable way to get a hit is to copy the
            // exact spelling from the council's own autocomplete. The link is
            // year-derived (the council bakes the year into the URL).
            char cal_url[192];
            waste_api_merribek_calendar_url(cal_url, sizeof(cal_url));
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p class='note'>Tip: Merri-bek needs your address spelled exactly the way "
                "the council records it (all caps, street type written out in full, no "
                "commas). The easy way to get it right: open "
                "<a href='%s' target='_blank'>the council's waste calendar page</a>, start "
                "typing your address there and let it autocomplete, then copy the completed "
                "address and paste it into the search box above.</p>"
                // A format template, not a real address. The council's dataset
                // is residential-only, so any example that actually returned a
                // match would be someone's home - published in a public repo
                // and shown on every device. The shape is what the user needs
                // anyway; the autocomplete above supplies the content.
                "<p class='note'>It will look like: "
                "<b>3/85 EXAMPLE STREET BRUNSWICK 3056</b> &mdash; unit number, "
                "street type spelled out, suburb and postcode, all in capitals.</p>",
                cal_url);
        }
    } else if (strcmp(step, "bsearch") == 0 && council != NULL && query[0] != '\0') {
        char enc_param[sizeof(council_param) * 3 + 1];
        url_encode_component(council->param, enc_param, sizeof(enc_param));

        waste_api_search_result_t *results = malloc(sizeof(waste_api_search_result_t) * SETUP_SEARCH_MAX);
        int n = (results != NULL)
                    ? waste_api_search_address((uint8_t)council->backend, query, results, SETUP_SEARCH_MAX)
                    : -1;
        if (n < 0) {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Couldn't search %s's address lookup just now &mdash; check the "
                "device's connection and try again.</p>", council->name);
        } else if (n == 0) {
            html_escape_attr(query, esc, PAGE_ESC_BUF_SIZE);
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>No matches for \"%s\". Try just the house number and street name.</p>", esc);
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<h2>Select your address</h2>");
            for (int i = 0; i < n; i++) {
                char enc_id[sizeof(results[i].id) * 3 + 1];
                char enc_label[sizeof(results[i].label) * 3 + 1];
                url_encode_component(results[i].id, enc_id, sizeof(enc_id));
                url_encode_component(results[i].label, enc_label, sizeof(enc_label));
                html_escape_attr(results[i].label, esc, PAGE_ESC_BUF_SIZE);
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=bsave&council=%s&id=%s&label=%s'>%s</a>",
                    enc_param, enc_id, enc_label, esc);
            }
        }
        if (results != NULL) {
            free(results);
        }
    } else if (strcmp(step, "property") == 0 && street_id_str[0] != '\0') {
        uint32_t street_id = (uint32_t)strtoul(street_id_str, NULL, 10);
        waste_api_property_t *props = malloc(sizeof(waste_api_property_t) * SETUP_LOOKUP_MAX);
        int n = (props != NULL) ? waste_api_fetch_properties(subdomain, street_id, props, SETUP_LOOKUP_MAX) : -1;
        if (n < 0) {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Couldn't fetch properties &mdash; check the council subdomain and try again.</p>");
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<h2>Select your address</h2>");
            for (int i = 0; i < n; i++) {
                char enc_label[sizeof(props[i].name) * 3 + 1];
                url_encode_component(props[i].name, enc_label, sizeof(enc_label));
                html_escape_attr(props[i].name, esc, PAGE_ESC_BUF_SIZE);
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=save&subdomain=%s&property=%u&label=%s'>%s</a>",
                    enc_subdomain, (unsigned)props[i].id, enc_label, esc);
            }
        }
        if (props != NULL) {
            free(props);
        }
    } else if (strcmp(step, "street") == 0 && locality_id_str[0] != '\0') {
        uint32_t locality_id = (uint32_t)strtoul(locality_id_str, NULL, 10);
        waste_api_street_t *streets = malloc(sizeof(waste_api_street_t) * SETUP_LOOKUP_MAX);
        int n = (streets != NULL) ? waste_api_fetch_streets(subdomain, locality_id, streets, SETUP_LOOKUP_MAX) : -1;
        if (n < 0) {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Couldn't fetch streets &mdash; check the council subdomain and try again.</p>");
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<h2>Select your street</h2>");
            for (int i = 0; i < n; i++) {
                html_escape_attr(streets[i].name, esc, PAGE_ESC_BUF_SIZE);
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=property&subdomain=%s&street=%u'>%s</a>",
                    enc_subdomain, (unsigned)streets[i].id, esc);
            }
        }
        if (streets != NULL) {
            free(streets);
        }
    } else if (strcmp(step, "locality") == 0 && subdomain[0] != '\0') {
        waste_api_locality_t *localities = malloc(sizeof(waste_api_locality_t) * SETUP_LOOKUP_MAX);
        int n = (localities != NULL) ? waste_api_fetch_localities(subdomain, localities, SETUP_LOOKUP_MAX) : -1;
        if (n < 0) {
            html_escape_attr(subdomain, esc, PAGE_ESC_BUF_SIZE);
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Couldn't reach \"%s.waste-info.com.au\" &mdash; check the council subdomain and try again.</p>",
                esc);
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<h2>Select your suburb</h2>");
            for (int i = 0; i < n; i++) {
                html_escape_attr(localities[i].name, esc, PAGE_ESC_BUF_SIZE);
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=street&subdomain=%s&locality=%u'>%s</a>",
                    enc_subdomain, (unsigned)localities[i].id, esc);
            }
        }
        if (localities != NULL) {
            free(localities);
        }
    } else {
        waste_api_config_t cfg = waste_api_get_config();
        // The dropdown entry corresponding to the current config, so the list
        // reopens showing what's actually selected.
        const council_t *current = (cfg.backend == COUNCIL_BACKEND_IMPACT_APPS)
                                       ? council_find_impact_apps(cfg.council_subdomain)
                                       : council_find_by_backend(cfg.backend);

        if (waste_api_config_complete(&cfg)) {
            // Split so both values can share the one escape scratch (council
            // name falls back to the raw typed subdomain; the label came in
            // via the wizard's query string).
            html_escape_attr(api_council_name(&cfg), esc, PAGE_ESC_BUF_SIZE);
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Currently configured: <b>%s</b><br>", esc);
            html_escape_attr(cfg.property_label, esc, PAGE_ESC_BUF_SIZE);
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "%s</p>", esc);
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<p>No council/address configured yet.</p>");
        }

        // Which state's councils to list. Explicit ?state= wins; otherwise the
        // configured council's own state; otherwise VIC.
        char state[8] = "";
        if (!get_query_param(req, "state", state, sizeof(state)) || state[0] == '\0') {
            snprintf(state, sizeof(state), "%s", current ? current->state : COUNCIL_DEFAULT_STATE);
        }
        bool state_known = false;
        for (size_t i = 0; i < STATE_ORDER_COUNT; i++) {
            if (strcmp(STATE_ORDER[i], state) == 0) {
                state_known = true;
                break;
            }
        }
        if (!state_known) {
            snprintf(state, sizeof(state), "%s", COUNCIL_DEFAULT_STATE);
        }

        off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<h2>Set up a new council / address</h2>");

        // Two separate forms rather than one with two buttons: changing state
        // is a page reload that re-renders the council list (no JavaScript, so
        // the filtering has to happen server-side), while choosing a council
        // moves on to the address wizard. Keeping them apart makes which
        // button does what obvious.
        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "<form method='GET' action='/api-setup' style='margin-bottom:1em'>"
            "<label>State: <select name='state'>");
        for (size_t i = 0; i < STATE_ORDER_COUNT; i++) {
            int n = 0;
            for (size_t c = 0; c < COUNCIL_COUNT; c++) {
                if (strcmp(COUNCILS[c].state, STATE_ORDER[i]) == 0) {
                    n++;
                }
            }
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<option value='%s' %s>%s (%d)</option>",
                STATE_ORDER[i], strcmp(STATE_ORDER[i], state) == 0 ? "selected" : "",
                council_state_label(STATE_ORDER[i]), n);
        }
        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "</select></label> <button type='submit'>Show councils</button></form>");

        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "<form method='GET' action='/api-setup'>"
            "<input type='hidden' name='step' value='council'>"
            "<label>Council: <select name='council'>");
        for (size_t c = 0; c < COUNCIL_COUNT; c++) {
            if (strcmp(COUNCILS[c].state, state) != 0) {
                continue;
            }
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<option value='%s' %s>%s</option>",
                COUNCILS[c].param,
                (current != NULL && current == &COUNCILS[c]) ? "selected" : "",
                COUNCILS[c].name);
        }
        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "</select></label> <button type='submit'>Find my address</button></form>");

        // The escape hatch that keeps SPEC.md 3.3's deliberate flexibility:
        // any council on this platform works without a firmware change, listed
        // or not. Demoted below the dropdown rather than removed.
        html_escape_attr(subdomain[0] != '\0' ? subdomain : cfg.council_subdomain,
                         esc, PAGE_ESC_BUF_SIZE);
        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "<h3>Council not listed?</h3>"
            "<p class='note'>Any council running the same \"waste-info.com.au\" platform will "
            "work &mdash; enter their subdomain (the part before \".waste-info.com.au\" in "
            "their bin-day lookup URL).</p>"
            "<form method='GET' action='/api-setup'>"
            "<input type='hidden' name='step' value='locality'>"
            "<label>Subdomain: <input type='text' name='subdomain' value='%s'></label> "
            "<button type='submit'>Find my suburb</button>"
            "</form>",
            esc);
    }

    off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "</body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(esc);
    free(html);
    return ESP_OK;
}

// On-demand diagnostic: fetches fresh (bypassing the poll cache) and shows
// every upcoming qualifying collection in the next few weeks, so the API
// setup can be verified immediately instead of waiting for the next poll or
// reading serial logs.
static esp_err_t api_test_get_handler(httpd_req_t *req)
{
    waste_api_config_t cfg = waste_api_get_config();

    char *html = malloc(HTML_BUF_SIZE);
    char *esc = malloc(PAGE_ESC_BUF_SIZE);
    if (html == NULL || esc == NULL) {
        free(html);
        free(esc);
        return ESP_ERR_NO_MEM;
    }

    int off = 0;
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Bin Collection API Test</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        "table{width:100%%;border-collapse:collapse;}"
        "td,th{padding:.4em;text-align:left;border-bottom:1px solid #ccc;}"
        ".swatch{display:inline-block;width:1em;height:1em;border:1px solid #999;"
        "vertical-align:middle;margin-right:.4em;}"
        "</style></head><body>"
        "<h1>Bin Collection API Test</h1>"
        "<p><a href='/'>&larr; Back to schedule</a></p>");

    if (!waste_api_config_complete(&cfg)) {
        off = safe_append(html, HTML_BUF_SIZE, off,
            "<p>No council/address configured yet &mdash; <a href='/api-setup'>set one up first</a>.</p>");
    } else {
        // Split so both values can share the one escape scratch.
        html_escape_attr(api_council_name(&cfg), esc, PAGE_ESC_BUF_SIZE);
        off = safe_append(html, HTML_BUF_SIZE, off, "<p>Testing <b>%s</b>, ", esc);
        html_escape_attr(cfg.property_label, esc, PAGE_ESC_BUF_SIZE);
        off = safe_append(html, HTML_BUF_SIZE, off,
            "%s &mdash; raw data for the next %d days "
            "(nothing filtered out yet):</p>", esc, API_TEST_LOOKAHEAD_DAYS);

        waste_api_event_t events[API_TEST_MAX_EVENTS];
        int n = waste_api_fetch_upcoming(&cfg, API_TEST_LOOKAHEAD_DAYS, events, API_TEST_MAX_EVENTS);
        if (n < 0) {
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<p style='color:#a00'>Couldn't reach the API just now &mdash; check the device's Wi-Fi "
                "connection and the council subdomain, then try again.</p>");
        } else if (n == 0) {
            off = safe_append(html, HTML_BUF_SIZE, off, "<p>Nothing scheduled in that window.</p>");
        } else {
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<table><tr><th>Date</th><th>Type</th><th>API's colour</th></tr>");
            for (int i = 0; i < n; i++) {
                const waste_api_event_t *e = &events[i];
                html_escape_attr(e->event_type, esc, PAGE_ESC_BUF_SIZE);
                off = safe_append(html, HTML_BUF_SIZE, off,
                    "<tr><td>%04u-%02u-%02u</td><td>%s</td>"
                    "<td><span class='swatch' style='background:#%02x%02x%02x'></span>#%02x%02x%02x</td></tr>",
                    (unsigned)e->year, (unsigned)e->month, (unsigned)e->day, esc,
                    e->color.r, e->color.g, e->color.b, e->color.r, e->color.g, e->color.b);
            }
            off = safe_append(html, HTML_BUF_SIZE, off, "</table>");
        }

        // Build the distinct set of event_types to map: every type seen just
        // now, unioned with any type already configured (even if it didn't
        // happen to appear in this particular window) so a saved mapping
        // never silently loses an entry.
        struct { char event_type[24]; bool has_rule; waste_api_type_rule_t rule; } summaries[WASTE_API_MAX_TYPE_RULES];
        int type_count = 0;
        memset(summaries, 0, sizeof(summaries));

        for (int i = 0; i < n && type_count < WASTE_API_MAX_TYPE_RULES; i++) {
            bool found = false;
            for (int t = 0; t < type_count; t++) {
                if (strcmp(summaries[t].event_type, events[i].event_type) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                snprintf(summaries[type_count].event_type, sizeof(summaries[type_count].event_type),
                    "%s", events[i].event_type);
                type_count++;
            }
        }
        for (int r = 0; r < WASTE_API_MAX_TYPE_RULES; r++) {
            if (cfg.type_rules[r].event_type[0] == '\0') {
                continue;
            }
            int idx = -1;
            for (int t = 0; t < type_count; t++) {
                if (strcmp(summaries[t].event_type, cfg.type_rules[r].event_type) == 0) {
                    idx = t;
                    break;
                }
            }
            if (idx < 0 && type_count < WASTE_API_MAX_TYPE_RULES) {
                idx = type_count++;
                snprintf(summaries[idx].event_type, sizeof(summaries[idx].event_type),
                    "%s", cfg.type_rules[r].event_type);
            }
            if (idx >= 0) {
                summaries[idx].has_rule = true;
                summaries[idx].rule = cfg.type_rules[r];
            }
        }

        if (type_count > 0) {
            off = safe_append(html, HTML_BUF_SIZE, off,
                "<h2>Colour mapping</h2>"
                "<p>Choose which of these event types should light the bin light, and which of "
                "the 4 supported colours each one uses. \"waste\" (general rubbish, collected "
                "every week) defaults to ignored until you say otherwise; every other type "
                "defaults to shown until you tick \"Ignore\".</p>"
                "<form method='POST' action='/api-test'>"
                "<input type='hidden' name='type_count' value='%d'>"
                "<input type='hidden' name='redirect_to' value='/api-test'>"
                "<table><tr><th>Type</th><th>Ignore</th><th>Colour</th></tr>", type_count);

            for (int i = 0; i < type_count; i++) {
                bool ignored_default = summaries[i].has_rule
                    ? summaries[i].rule.ignored
                    : (strcmp(summaries[i].event_type, "waste") == 0);
                schedule_color_t color_default = summaries[i].has_rule ? summaries[i].rule.color : (schedule_color_t){255, 0, 0};

                char name_field[16], ignored_field[16], color_field[16];
                snprintf(name_field, sizeof(name_field), "type%d_name", i);
                snprintf(ignored_field, sizeof(ignored_field), "type%d_ignored", i);
                snprintf(color_field, sizeof(color_field), "type%d_color", i);

                // As on the home page's mapping table: escaped once, used as
                // both cell text and attribute value.
                html_escape_attr(summaries[i].event_type, esc, PAGE_ESC_BUF_SIZE);
                off = safe_append(html, HTML_BUF_SIZE, off,
                    "<tr><td>%s<input type='hidden' name='%s' value='%s'></td>"
                    "<td><input type='checkbox' name='%s' %s></td><td>",
                    esc, name_field, esc,
                    ignored_field, ignored_default ? "checked" : "");
                off = append_color_select(html, HTML_BUF_SIZE, off, color_field, color_default);
                off = safe_append(html, HTML_BUF_SIZE, off, "</td></tr>");
            }
            off = safe_append(html, HTML_BUF_SIZE, off,
                "</table><p><button type='submit'>Save mapping</button></p></form>");
        }
    }

    off = safe_append(html, HTML_BUF_SIZE, off, "</body></html>");

    if (off >= HTML_BUF_SIZE) {
        ESP_LOGE(TAG, "API test page truncated at %d bytes - raise HTML_BUF_SIZE", HTML_BUF_SIZE);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(esc);
    free(html);
    return ESP_OK;
}

// Saves the colour-mapping form from api_test_get_handler(): replaces
// cfg.type_rules wholesale with whatever rows the form submitted (row count
// declared explicitly via the "type_count" hidden field, since a plain
// urlencoded body has no arrays).
static esp_err_t api_test_post_handler(httpd_req_t *req)
{
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len >= SAVE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    waste_api_config_t cfg = waste_api_get_config();
    memset(cfg.type_rules, 0, sizeof(cfg.type_rules));

    char value[FIELD_BUF_SIZE];
    int type_count = 0;
    if (httpd_query_key_value(body, "type_count", value, sizeof(value)) == ESP_OK) {
        type_count = atoi(value);
    }
    if (type_count > WASTE_API_MAX_TYPE_RULES) {
        type_count = WASTE_API_MAX_TYPE_RULES;
    }

    char key[24];
    for (int i = 0; i < type_count; i++) {
        waste_api_type_rule_t *rule = &cfg.type_rules[i];

        snprintf(key, sizeof(key), "type%d_name", i);
        if (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK) {
            url_decode_inplace(value);
            snprintf(rule->event_type, sizeof(rule->event_type), "%s", value);
        }

        snprintf(key, sizeof(key), "type%d_ignored", i);
        rule->ignored = (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK);

        snprintf(key, sizeof(key), "type%d_color", i);
        if (httpd_query_key_value(body, key, value, sizeof(value)) == ESP_OK) {
            url_decode_inplace(value);
            parse_hex_color(value, &rule->color.r, &rule->color.g, &rule->color.b);
        }
    }

    char redirect_to[16] = "/api-test";
    if (httpd_query_key_value(body, "redirect_to", value, sizeof(value)) == ESP_OK) {
        url_decode_inplace(value);
        // Only ever redirect to one of our own pages - not attacker-controlled
        // in practice on a single-user LAN device, but cheap to constrain.
        if (strcmp(value, "/") == 0 || strcmp(value, "/api-test") == 0) {
            snprintf(redirect_to, sizeof(redirect_to), "%s", value);
        }
    }

    free(body);
    waste_api_set_config(&cfg);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", redirect_to);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.max_uri_handlers = 14; // 12 in use, small headroom for future additions
    // /api-setup performs blocking HTTPS/TLS calls (via waste_api_fetch_*) on
    // this same task - TLS handshakes are stack-hungry, matching the 8192-byte
    // stack already given to the dedicated waste_api polling task.
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    static const httpd_uri_t api_setup_uri = {
        .uri = "/api-setup",
        .method = HTTP_GET,
        .handler = api_setup_get_handler,
    };
    static const httpd_uri_t api_test_get_uri = {
        .uri = "/api-test",
        .method = HTTP_GET,
        .handler = api_test_get_handler,
    };
    static const httpd_uri_t api_test_post_uri = {
        .uri = "/api-test",
        .method = HTTP_POST,
        .handler = api_test_post_handler,
    };
    static const httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_get_handler,
    };
    static const httpd_uri_t test_uri = {
        .uri = "/test",
        .method = HTTP_POST,
        .handler = test_post_handler,
    };
    static const httpd_uri_t wifi_forget_uri = {
        .uri = "/wifi-forget",
        .method = HTTP_POST,
        .handler = wifi_forget_post_handler,
    };
    static const httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post_handler,
    };
    // GET too, on the same handler. The handler already treats "no body" as
    // "just check and report", so a GET is exactly the no-action case. Without
    // this, reloading the firmware page - the obvious thing to do while
    // waiting on a download, and the first thing anyone does after a failed
    // one - answers 405 Method Not Allowed. Observed on real hardware while
    // the OTA of bug 23 was failing, which made a confusing situation worse.
    static const httpd_uri_t update_get_uri = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = update_post_handler,
    };
    static const httpd_uri_t reboot_uri = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_post_handler,
    };
    static const httpd_uri_t factory_reset_uri = {
        .uri = "/factory-reset",
        .method = HTTP_POST,
        .handler = factory_reset_post_handler,
    };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &api_setup_uri);
    httpd_register_uri_handler(server, &api_test_get_uri);
    httpd_register_uri_handler(server, &api_test_post_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &test_uri);
    httpd_register_uri_handler(server, &wifi_forget_uri);
    httpd_register_uri_handler(server, &update_uri);
    httpd_register_uri_handler(server, &update_get_uri);
    httpd_register_uri_handler(server, &reboot_uri);
    httpd_register_uri_handler(server, &factory_reset_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
