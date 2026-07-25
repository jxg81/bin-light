#include "web_server.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"

#include "schedule.h"
#include "settings.h"
#include "waste_api.h"

static const char *TAG = "web_server";

#define HTML_BUF_SIZE   7200
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
    {"Yellow", 255, 255, 0},
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

// Renders a full <select> of the colour presets for one rotation slot,
// preselecting whichever preset matches `current` exactly.
static int append_color_select(char *buf, size_t buf_size, int off, const char *field_name, schedule_color_t current)
{
    off = safe_append(buf, buf_size, off, "<select name='%s'>", field_name);
    for (size_t c = 0; c < COLOR_PRESET_COUNT; c++) {
        const color_preset_t *cp = &COLOR_PRESETS[c];
        bool selected = (cp->r == current.r && cp->g == current.g && cp->b == current.b);
        off = safe_append(buf, buf_size, off, "<option value='#%02x%02x%02x' %s>%s</option>",
            cp->r, cp->g, cp->b, selected ? "selected" : "", cp->label);
    }
    return safe_append(buf, buf_size, off, "</select>");
}

// Maps an arbitrary RGB colour (e.g. straight from the API's own "color" hex
// field) to whichever of the 4 supported presets is closest, by squared
// distance in RGB space. Used to auto-populate a sensible colour mapping
// right after API setup completes, without asking the user to pick manually
// for a council's colours that usually already correspond closely to one of
// our 4 presets (red/green/yellow/purple).
static schedule_color_t nearest_preset_color(schedule_color_t c)
{
    int best_idx = 0;
    long best_dist = -1;
    for (size_t i = 0; i < COLOR_PRESET_COUNT; i++) {
        long dr = (long)c.r - COLOR_PRESETS[i].r;
        long dg = (long)c.g - COLOR_PRESETS[i].g;
        long db = (long)c.b - COLOR_PRESETS[i].b;
        long dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best_idx = (int)i;
        }
    }
    return (schedule_color_t){COLOR_PRESETS[best_idx].r, COLOR_PRESETS[best_idx].g, COLOR_PRESETS[best_idx].b};
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
static int append_type_mapping_form(char *buf, size_t buf_size, int off,
                                     const waste_api_config_t *cfg, const char *redirect_to)
{
    int type_count = 0;
    for (int i = 0; i < WASTE_API_MAX_TYPE_RULES; i++) {
        if (cfg->type_rules[i].event_type[0] != '\0') {
            type_count++;
        }
    }
    if (type_count == 0) {
        return off;
    }

    off = safe_append(buf, buf_size, off,
        "<h3>Colour mapping</h3>"
        "<form method='POST' action='/api-test'>"
        "<input type='hidden' name='type_count' value='%d'>"
        "<input type='hidden' name='redirect_to' value='%s'>"
        "<table><tr><th>Type</th><th>Ignore</th><th>Colour</th></tr>", type_count, redirect_to);

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

        off = safe_append(buf, buf_size, off,
            "<tr><td>%s<input type='hidden' name='%s' value='%s'></td>"
            "<td><input type='checkbox' name='%s' %s></td><td>",
            r->event_type, name_field, r->event_type, ignored_field, r->ignored ? "checked" : "");
        off = append_color_select(buf, buf_size, off, color_field, r->color);
        off = safe_append(buf, buf_size, off, "</td></tr>");
        idx++;
    }
    return safe_append(buf, buf_size, off, "</table><p><button type='submit'>Save mapping</button></p></form>");
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

// Escapes a string for safe embedding inside a single-quoted HTML attribute.
// The stored TZ string is user-supplied (via the "Custom" field) and gets
// reflected back into the page on every load, so this isn't optional.
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

static esp_err_t root_get_handler(httpd_req_t *req)
{
    schedule_t s = schedule_get();

    char *html = malloc(HTML_BUF_SIZE);
    if (html == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char start_str[HHMM_BUF_SIZE];
    minutes_to_hhmm(s.start_minute, start_str);

    int off = 0;
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<link rel='icon' href='/favicon.ico' type='image/svg+xml'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Bin Light Schedule</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        "table{width:100%%;border-collapse:collapse;}"
        "td,th{padding:.4em;text-align:left;border-bottom:1px solid #ccc;}"
        "input[type=time],input[type=date]{width:9em;}"
        "input[type=range]{width:8em;}"
        "input[type=number]{width:4em;}"
        "</style></head><body>"
        "<h1>Bin Light Schedule</h1>"
        "<form method='POST' action='/test' style='margin:0 0 1em 0'>"
        "<button type='submit'>Test (light both LEDs for 2 min)</button></form>");

    waste_api_config_t api_cfg = waste_api_get_config();
    off = safe_append(html, HTML_BUF_SIZE, off, "<h2>Bin collection API</h2>");
    if (api_cfg.property_id != 0) {
        off = safe_append(html, HTML_BUF_SIZE, off,
            "<p>Configured: <b>%s</b>, %s (property #%u)</p>",
            api_cfg.council_subdomain, api_cfg.property_label, (unsigned)api_cfg.property_id);
    } else {
        off = safe_append(html, HTML_BUF_SIZE, off, "<p>No council/address configured yet.</p>");
    }

    // Its own <form> (posts to /api-test) since it can't nest inside the
    // /save form below - rendered from the saved config directly, no live
    // fetch, so the home page stays fast. /api-setup auto-populates this at
    // setup time; /api-test can (re)discover new types via a live fetch.
    off = append_type_mapping_form(html, HTML_BUF_SIZE, off, &api_cfg, "/");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p><a href='/api-setup'>Change council / address</a>"
        " &middot; <a href='/api-test'>Test API (show upcoming weeks)</a></p>");

    off = safe_append(html, HTML_BUF_SIZE, off, "<form method='POST' action='/save'>");
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p><label><input type='checkbox' name='api_enabled' %s> Use automatic bin collection API</label></p>"
        "<p style='color:#888'>When enabled and reachable, the API overrides the manual "
        "schedule below entirely, including turning the light off on weeks it reports "
        "nothing due. The manual schedule below is used only when the API is disabled, "
        "not yet configured, or unreachable.</p>",
        api_cfg.enabled ? "checked" : "");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<h2>Manual schedule</h2>"
        "<table>"
        "<tr><td>Enabled</td><td><input type='checkbox' name='enabled' %s></td></tr>",
        s.enabled ? "checked" : "");

    off = safe_append(html, HTML_BUF_SIZE, off, "<tr><td>Bin night</td><td><select name='bin_night_weekday'>");
    for (int i = 0; i < 7; i++) {
        off = safe_append(html, HTML_BUF_SIZE, off, "<option value='%d' %s>%s</option>",
            i, (i == s.bin_night_weekday) ? "selected" : "", WEEKDAY_LABEL[i]);
    }
    off = safe_append(html, HTML_BUF_SIZE, off, "</select></td></tr>");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<tr><td>On from</td><td><input type='time' name='start' value='%s'></td></tr>"
        "<tr><td>Turn off after</td><td><input type='number' name='duration_hours' min='1' max='23' value='%u'> hours</td></tr>"
        "<tr><td>Brightness</td><td><input type='range' name='brightness' min='10' max='255' value='%u'></td></tr>",
        start_str, (unsigned)s.duration_hours, (unsigned)s.brightness);

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
        "<tr><td colspan='2' style='color:#888'>In dual colour mode, the second LED shows this "
        "colour (general waste, by default red) unless the schedule finds two distinct bin "
        "types due the same night, in which case it shows the second one instead. Only used "
        "when \"Light mode\" above is set to dual colour.</td></tr>");

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
        "<p style='color:#888'>The light turns on at the chosen time on bin night, and stays on for "
        "the given number of hours (carrying past midnight if needed).</p>");

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<h2>How the colour rules work</h2>"
        "<p>Each colour has its own independent cycle: its first collection date "
        "and how often it repeats (every 1-4 weeks) &mdash; these don't need to "
        "match between colours. If more than one colour is due the same week, "
        "Colour 1 takes priority, then Colour 2, then Colour 3.</p>"
        "<p style='color:#888'>Example: Yellow first collected 6 Jul 2026, every "
        "2 weeks. Green first collected 13 Jul 2026, every 3 weeks:</p>"
        "<ul style='color:#888'>"
        "<li>Week of 6 Jul &rarr; <b>Yellow</b> (Yellow's first week)</li>"
        "<li>Week of 13 Jul &rarr; <b>Green</b> (Green's first week)</li>"
        "<li>Week of 20 Jul &rarr; <b>Yellow</b> (2 weeks after its first)</li>"
        "<li>Week of 27 Jul &rarr; nothing due</li>"
        "<li>Week of 3 Aug &rarr; <b>Yellow</b>, and Green is also due this week "
        "&mdash; Yellow wins since it's Colour 1</li>"
        "</ul>"
        "<p style='color:#888'>To disable a colour entirely, leave its \"Enabled\" "
        "box unchecked.</p>");

    const char *current_tz = settings_get_tz();
    const tz_preset_t *current_preset = find_tz_preset(current_tz);
    char escaped_tz[SETTINGS_TZ_MAX_LEN * 6 + 1];
    html_escape_attr(current_tz, escaped_tz, sizeof(escaped_tz));

    off = safe_append(html, HTML_BUF_SIZE, off, "<h2>Timezone</h2><p><select name='tz'>");
    for (size_t i = 0; i < TZ_PRESET_COUNT; i++) {
        off = safe_append(html, HTML_BUF_SIZE, off, "<option value='%s' %s>%s</option>",
            TZ_PRESETS[i].tz, (&TZ_PRESETS[i] == current_preset) ? "selected" : "", TZ_PRESETS[i].label);
    }
    off = safe_append(html, HTML_BUF_SIZE, off, "<option value='custom' %s>Custom&hellip;</option></select></p>",
        current_preset == NULL ? "selected" : "");
    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p>Custom POSIX TZ string (used only when \"Custom&hellip;\" is selected above):<br>"
        "<input type='text' name='tz_custom' size='40' value='%s'></p>", escaped_tz);

    off = safe_append(html, HTML_BUF_SIZE, off,
        "<p style='margin-top:1em'><button type='submit'>Save</button></p>"
        "</form></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(html);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
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

// Previews whatever the light would show at its next scheduled occurrence
// (SPEC.md 3.8) for a fixed 2 minutes, then hands back to the real evaluator.
// No body to read - the button always previews "next", nothing to configure.
static esp_err_t test_post_handler(httpd_req_t *req)
{
    schedule_test_trigger();

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

#define SETUP_LOOKUP_MAX    150
#define SETUP_HTML_BUF_SIZE 20000

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

// No-JS, server-rendered locality -> street -> property wizard for configuring
// the external bin-collection API. Every step is a plain GET link carrying
// accumulated state in the query string; the final "save" step persists via
// waste_api_set_config() and redirects home. Saving via a GET link rather than
// a POST is a deliberate simplification - no CSRF-relevant risk on a
// single-user LAN device, consistent with how simple the rest of this UI is.
static esp_err_t api_setup_get_handler(httpd_req_t *req)
{
    char step[16] = "";
    char subdomain[WASTE_API_SUBDOMAIN_MAX_LEN + 1] = "maribyrnong";
    char locality_id_str[16] = "";
    char street_id_str[16] = "";
    char property_id_str[16] = "";
    char label[WASTE_API_LABEL_MAX_LEN + 1] = "";

    get_query_param(req, "step", step, sizeof(step));
    get_query_param(req, "subdomain", subdomain, sizeof(subdomain));
    get_query_param(req, "locality", locality_id_str, sizeof(locality_id_str));
    get_query_param(req, "street", street_id_str, sizeof(street_id_str));
    get_query_param(req, "property", property_id_str, sizeof(property_id_str));
    get_query_param(req, "label", label, sizeof(label));

    if (strcmp(step, "save") == 0 && property_id_str[0] != '\0') {
        waste_api_config_t cfg = waste_api_get_config();
        cfg.enabled = true;
        snprintf(cfg.council_subdomain, sizeof(cfg.council_subdomain), "%s", subdomain);
        cfg.property_id = (uint32_t)strtoul(property_id_str, NULL, 10);
        snprintf(cfg.property_label, sizeof(cfg.property_label), "%s", label);

        // Auto-populate a starting colour mapping: fetch what's actually
        // coming up for this property and build one rule per distinct type
        // seen, defaulting "waste" to ignored and everything else to the
        // nearest of our 4 presets to the API's own colour - so the mapping
        // on the home page usually needs no manual editing at all.
        memset(cfg.type_rules, 0, sizeof(cfg.type_rules));
        waste_api_event_t events[API_TEST_MAX_EVENTS];
        int n = waste_api_fetch_upcoming(cfg.council_subdomain, cfg.property_id,
                                          API_TEST_LOOKAHEAD_DAYS, events, API_TEST_MAX_EVENTS);
        int rule_count = 0;
        for (int i = 0; i < n && rule_count < WASTE_API_MAX_TYPE_RULES; i++) {
            bool already = false;
            for (int r = 0; r < rule_count; r++) {
                if (strcmp(cfg.type_rules[r].event_type, events[i].event_type) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            waste_api_type_rule_t *rule = &cfg.type_rules[rule_count];
            snprintf(rule->event_type, sizeof(rule->event_type), "%s", events[i].event_type);
            rule->ignored = (strcmp(events[i].event_type, "waste") == 0);
            schedule_color_t named_default;
            rule->color = default_color_for_type(events[i].event_type, &named_default)
                              ? named_default
                              : nearest_preset_color(events[i].color);
            rule_count++;
        }

        // Persists and wakes the poll task to fetch immediately, so the real
        // evaluator has fresh, mapped data right away rather than waiting for
        // the next 12h interval.
        waste_api_set_config(&cfg);

        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char *html = malloc(SETUP_HTML_BUF_SIZE);
    if (html == NULL) {
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
        "</style></head><body>"
        "<h1>Bin Collection API Setup</h1>"
        "<p><a href='/'>&larr; Back to schedule</a></p>");

    char enc_subdomain[WASTE_API_SUBDOMAIN_MAX_LEN * 3 + 1];
    url_encode_component(subdomain, enc_subdomain, sizeof(enc_subdomain));

    if (strcmp(step, "property") == 0 && street_id_str[0] != '\0') {
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
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=save&subdomain=%s&property=%u&label=%s'>%s</a>",
                    enc_subdomain, (unsigned)props[i].id, enc_label, props[i].name);
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
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=property&subdomain=%s&street=%u'>%s</a>",
                    enc_subdomain, (unsigned)streets[i].id, streets[i].name);
            }
        }
        if (streets != NULL) {
            free(streets);
        }
    } else if (strcmp(step, "locality") == 0 && subdomain[0] != '\0') {
        waste_api_locality_t *localities = malloc(sizeof(waste_api_locality_t) * SETUP_LOOKUP_MAX);
        int n = (localities != NULL) ? waste_api_fetch_localities(subdomain, localities, SETUP_LOOKUP_MAX) : -1;
        if (n < 0) {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Couldn't reach \"%s.waste-info.com.au\" &mdash; check the council subdomain and try again.</p>",
                subdomain);
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<h2>Select your suburb</h2>");
            for (int i = 0; i < n; i++) {
                off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                    "<a class='item' href='/api-setup?step=street&subdomain=%s&locality=%u'>%s</a>",
                    enc_subdomain, (unsigned)localities[i].id, localities[i].name);
            }
        }
        if (localities != NULL) {
            free(localities);
        }
    } else {
        waste_api_config_t cfg = waste_api_get_config();
        if (cfg.property_id != 0) {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
                "<p>Currently configured: <b>%s</b>, %s (property #%u)</p>",
                cfg.council_subdomain, cfg.property_label, (unsigned)cfg.property_id);
        } else {
            off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "<p>No council/address configured yet.</p>");
        }
        off = safe_append(html, SETUP_HTML_BUF_SIZE, off,
            "<h2>Set up a new council / address</h2>"
            "<p>This works for any council running the same \"waste-info.com.au\" "
            "platform &mdash; just enter their subdomain (the part before "
            "\".waste-info.com.au\" in their bin-day lookup URL).</p>"
            "<form method='GET' action='/api-setup'>"
            "<input type='hidden' name='step' value='locality'>"
            "<label>Council subdomain: <input type='text' name='subdomain' value='%s'></label> "
            "<button type='submit'>Find my suburb</button>"
            "</form>", subdomain);
    }

    off = safe_append(html, SETUP_HTML_BUF_SIZE, off, "</body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
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
    if (html == NULL) {
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

    if (cfg.property_id == 0 || cfg.council_subdomain[0] == '\0') {
        off = safe_append(html, HTML_BUF_SIZE, off,
            "<p>No council/address configured yet &mdash; <a href='/api-setup'>set one up first</a>.</p>");
    } else {
        off = safe_append(html, HTML_BUF_SIZE, off,
            "<p>Testing <b>%s</b>, %s (property #%u) &mdash; raw data for the next %d days "
            "(nothing filtered out yet):</p>",
            cfg.council_subdomain, cfg.property_label, (unsigned)cfg.property_id, API_TEST_LOOKAHEAD_DAYS);

        waste_api_event_t events[API_TEST_MAX_EVENTS];
        int n = waste_api_fetch_upcoming(cfg.council_subdomain, cfg.property_id,
                                          API_TEST_LOOKAHEAD_DAYS, events, API_TEST_MAX_EVENTS);
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
                off = safe_append(html, HTML_BUF_SIZE, off,
                    "<tr><td>%04u-%02u-%02u</td><td>%s</td>"
                    "<td><span class='swatch' style='background:#%02x%02x%02x'></span>#%02x%02x%02x</td></tr>",
                    (unsigned)e->year, (unsigned)e->month, (unsigned)e->day, e->event_type,
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

                off = safe_append(html, HTML_BUF_SIZE, off,
                    "<tr><td>%s<input type='hidden' name='%s' value='%s'></td>"
                    "<td><input type='checkbox' name='%s' %s></td><td>",
                    summaries[i].event_type, name_field, summaries[i].event_type,
                    ignored_field, ignored_default ? "checked" : "");
                off = append_color_select(html, HTML_BUF_SIZE, off, color_field, color_default);
                off = safe_append(html, HTML_BUF_SIZE, off, "</td></tr>");
            }
            off = safe_append(html, HTML_BUF_SIZE, off,
                "</table><p><button type='submit'>Save mapping</button></p></form>");
        }
    }

    off = safe_append(html, HTML_BUF_SIZE, off, "</body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(html);
    return ESP_OK;
}

// Saves the colour-mapping form from api_test_get_handler(): replaces
// cfg.type_rules wholesale with whatever rows the form submitted (row count
// declared explicitly via the "type_count" hidden field, since a plain
// urlencoded body has no arrays).
static esp_err_t api_test_post_handler(httpd_req_t *req)
{
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
    config.max_uri_handlers = 8; // 7 in use, small headroom for future additions
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

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &api_setup_uri);
    httpd_register_uri_handler(server, &api_test_get_uri);
    httpd_register_uri_handler(server, &api_test_post_uri);
    httpd_register_uri_handler(server, &favicon_uri);
    httpd_register_uri_handler(server, &test_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
