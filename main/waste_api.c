#include "waste_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"

#include "date_parse.h"

static const char *TAG = "waste_api";

#define WASTE_API_NVS_NAMESPACE  "binlight"
#define WASTE_API_NVS_KEY        "waste_api_v3"
#define WASTE_API_STRUCT_VERSION 3

// v2 (pre-backend-abstraction, Impact Apps only) is migrated on first boot
// rather than discarded - a working Maribyrnong setup survives the upgrade.
#define WASTE_API_NVS_KEY_V2     "waste_api_v2"

// Cache is a separate blob from the config: it's derived data we can refetch,
// not user configuration, and mixing them would mean a cache write rewriting
// the user's settings blob twice a day.
#define WASTE_API_CACHE_NVS_KEY   "waste_cache_v1"
#define WASTE_API_CACHE_VERSION   1

#define POLL_INTERVAL_MS      (12UL * 60 * 60 * 1000)  // 12 hours
// Widened from 13 days: under the sticky-cache model a wider window costs a
// little bandwidth per poll and buys margin against a council publishing
// sparsely, whereas a too-narrow window used to translate directly into the
// light being wrong.
#define EVENTS_LOOKAHEAD_DAYS 30
// Sized for the largest measured schedule response across all backends:
// Merri-bek's AddressDetails is 4397 bytes (would overflow the previous
// 4096), Monash's HTML-in-JSON ~2.5KB, Impact Apps ~700B, Knox 363B,
// Whitehorse ~450B.
#define EVENTS_BUF_SIZE       8192
#define LOOKUP_BUF_SIZE       16384  // locality/street/property lists can be much larger than an events response
#define HTTP_TIMEOUT_MS       8000
#define POLL_MAX_EVENTS       16     // 30-day window across several bin types

// Persisted to NVS (SPEC.md 3.3). The earlier design deliberately kept this in
// RAM only, on the grounds that a reboot cost a few harmless seconds of
// "unknown" before the first poll landed. That's no longer acceptable: the
// device is now required to *always* know the next collection, and a reboot at
// the wrong moment (or with no network) would otherwise leave it blank.
typedef struct {
    uint8_t           version;
    bool              has_event;
    uint16_t          event_year;
    uint8_t           event_month;
    uint8_t           event_day;
    schedule_color_t  color;            // primary (earliest) event's colour
    bool              has_secondary;    // a second distinct event shares the same date
    schedule_color_t  secondary_color;
    bool              waste_dow_known;  // recurring general-waste rule's weekday has been learned
    uint8_t           waste_weekday;    // tm_wday convention (0=Sunday), valid only if waste_dow_known
} waste_api_cache_t;

static SemaphoreHandle_t s_mutex;
static waste_api_config_t s_config;
static waste_api_cache_t s_cache;
static TaskHandle_t s_task_handle;

// Whole days from a calendar date to today, both normalised to local noon so a
// DST transition can't shift the count across a day boundary. Positive when
// the date is in the past. Mirrors days_between() in schedule.c - duplicated
// rather than shared, since exporting a date helper from schedule.h just for
// this would couple the two modules for four lines of arithmetic.
static long days_since(uint16_t y, uint8_t mo, uint8_t d)
{
    struct tm from_tm = {0};
    from_tm.tm_year = (int)y - 1900;
    from_tm.tm_mon = (int)mo - 1;
    from_tm.tm_mday = d;
    from_tm.tm_hour = 12;
    from_tm.tm_isdst = -1;

    time_t now = time(NULL);
    struct tm to_tm;
    localtime_r(&now, &to_tm);
    to_tm.tm_hour = 12;
    to_tm.tm_min = 0;
    to_tm.tm_sec = 0;
    to_tm.tm_isdst = -1;

    time_t from_time = mktime(&from_tm);
    time_t to_time = mktime(&to_tm);
    if (from_time == (time_t)-1 || to_time == (time_t)-1) {
        return 0;
    }
    // Round to nearest day, not truncate - across a DST change two local noons
    // are 23 or 25 hours apart, and truncation would call a 23h gap zero days.
    // Same reasoning as days_between() in schedule.c.
    long secs = (long)(to_time - from_time);
    return (secs >= 0) ? (secs + 43200) / 86400 : -((-secs + 43200) / 86400);
}

// ---------------------------------------------------------------- config --

static waste_api_config_t default_config(void)
{
    waste_api_config_t c = {0};
    c.version = WASTE_API_STRUCT_VERSION;
    c.backend = COUNCIL_BACKEND_IMPACT_APPS;
    return c;
}

// The v2 layout, exactly as compiled before the backend abstraction - no
// backend discriminator, no address_id. Kept only for one-time migration.
typedef struct {
    uint8_t                 version;
    bool                    enabled;
    char                    council_subdomain[WASTE_API_SUBDOMAIN_MAX_LEN + 1];
    uint32_t                property_id;
    char                    property_label[WASTE_API_LABEL_MAX_LEN + 1];
    waste_api_type_rule_t   type_rules[WASTE_API_MAX_TYPE_RULES];
} waste_api_config_v2_t;

// Loads a v2 blob into *out (as a v3 struct), true on success. Everything in
// v2 was Impact Apps by definition.
static bool load_legacy_v2(nvs_handle_t handle, waste_api_config_t *out)
{
    size_t size = 0;
    waste_api_config_v2_t old;
    if (nvs_get_blob(handle, WASTE_API_NVS_KEY_V2, NULL, &size) != ESP_OK ||
        size != sizeof(waste_api_config_v2_t) ||
        nvs_get_blob(handle, WASTE_API_NVS_KEY_V2, &old, &size) != ESP_OK ||
        old.version != 2) {
        return false;
    }
    *out = default_config();
    out->enabled = old.enabled;
    out->backend = COUNCIL_BACKEND_IMPACT_APPS;
    memcpy(out->council_subdomain, old.council_subdomain, sizeof(out->council_subdomain));
    out->property_id = old.property_id;
    memcpy(out->property_label, old.property_label, sizeof(out->property_label));
    memcpy(out->type_rules, old.type_rules, sizeof(out->type_rules));
    return true;
}

bool waste_api_config_complete(const waste_api_config_t *cfg)
{
    switch (cfg->backend) {
    case COUNCIL_BACKEND_IMPACT_APPS:
        return cfg->council_subdomain[0] != '\0' && cfg->property_id != 0;
    case COUNCIL_BACKEND_KNOX:
    case COUNCIL_BACKEND_WHITEHORSE:
    case COUNCIL_BACKEND_MERRI_BEK:
    case COUNCIL_BACKEND_MONASH:
        return cfg->address_id[0] != '\0';
    default:
        return false;
    }
}

static esp_err_t persist_config(const waste_api_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WASTE_API_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, WASTE_API_NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t persist_cache(const waste_api_cache_t *cache)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WASTE_API_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, WASTE_API_CACHE_NVS_KEY, cache, sizeof(*cache));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist next-collection cache: %s", esp_err_to_name(err));
    }
    return err;
}

static void load_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open(WASTE_API_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    size_t size = 0;
    waste_api_cache_t loaded;
    if (nvs_get_blob(handle, WASTE_API_CACHE_NVS_KEY, NULL, &size) == ESP_OK &&
        size == sizeof(waste_api_cache_t) &&
        nvs_get_blob(handle, WASTE_API_CACHE_NVS_KEY, &loaded, &size) == ESP_OK &&
        loaded.version == WASTE_API_CACHE_VERSION) {
        s_cache = loaded;
        ESP_LOGI(TAG, "restored next-collection cache (event=%d %04u-%02u-%02u, waste_dow_known=%d)",
                 s_cache.has_event, (unsigned)s_cache.event_year, (unsigned)s_cache.event_month,
                 (unsigned)s_cache.event_day, s_cache.waste_dow_known);
    } else {
        s_cache = (waste_api_cache_t){ .version = WASTE_API_CACHE_VERSION };
    }
    nvs_close(handle);
}

esp_err_t waste_api_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_cache = (waste_api_cache_t){ .version = WASTE_API_CACHE_VERSION };
    load_cache();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WASTE_API_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        size_t required_size = 0;
        waste_api_config_t loaded;
        err = nvs_get_blob(handle, WASTE_API_NVS_KEY, NULL, &required_size);
        if (err == ESP_OK && required_size == sizeof(waste_api_config_t)) {
            err = nvs_get_blob(handle, WASTE_API_NVS_KEY, &loaded, &required_size);
            if (err == ESP_OK && loaded.version == WASTE_API_STRUCT_VERSION) {
                s_config = loaded;
                nvs_close(handle);
                ESP_LOGI(TAG, "loaded waste API config from NVS (enabled=%d, backend=%u)",
                         s_config.enabled, (unsigned)s_config.backend);
                return ESP_OK;
            }
        }

        // No v3 blob: migrate a v2 one instead of discarding a working setup.
        if (load_legacy_v2(handle, &s_config)) {
            nvs_close(handle);
            ESP_LOGI(TAG, "migrated waste API config v2 -> v3 (Impact Apps, enabled=%d)", s_config.enabled);
            persist_config(&s_config);
            return ESP_OK;
        }
        nvs_close(handle);
    }

    s_config = default_config();
    ESP_LOGI(TAG, "no stored waste API config, defaulting to disabled");
    return ESP_OK;
}

waste_api_config_t waste_api_get_config(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    waste_api_config_t copy = s_config;
    xSemaphoreGive(s_mutex);
    return copy;
}

void waste_api_task_force_check(void)
{
    if (s_task_handle != NULL) {
        xTaskNotifyGive(s_task_handle);
    }
}

esp_err_t waste_api_set_config(const waste_api_config_t *cfg)
{
    waste_api_config_t to_store = *cfg;
    to_store.version = WASTE_API_STRUCT_VERSION;
    to_store.council_subdomain[WASTE_API_SUBDOMAIN_MAX_LEN] = '\0';
    to_store.address_id[WASTE_API_ADDRESS_ID_MAX_LEN] = '\0';
    to_store.property_label[WASTE_API_LABEL_MAX_LEN] = '\0';
    for (int i = 0; i < WASTE_API_MAX_TYPE_RULES; i++) {
        to_store.type_rules[i].event_type[sizeof(to_store.type_rules[i].event_type) - 1] = '\0';
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = to_store;
    xSemaphoreGive(s_mutex);

    esp_err_t err = persist_config(&to_store);
    waste_api_task_force_check();
    return err;
}

// ------------------------------------------------------------- HTTP/JSON --

typedef struct {
    char   *buf;
    size_t  capacity;
    size_t  len;
    bool    overflow;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_buf_t *hb = (http_buf_t *)evt->user_data;
        if (hb != NULL && hb->buf != NULL && evt->data_len > 0) {
            if (hb->len + (size_t)evt->data_len + 1 > hb->capacity) {
                hb->overflow = true;
            } else {
                memcpy(hb->buf + hb->len, evt->data, evt->data_len);
                hb->len += (size_t)evt->data_len;
                hb->buf[hb->len] = '\0';
            }
        }
    }
    return ESP_OK;
}

// Blocking GET into buf (already allocated by the caller, buf_size bytes).
// out_status (optional) receives the HTTP status when the request completed
// at the transport level, or -1 when it never got that far - letting callers
// tell "the server rejected this" from "the network is down", which matters
// for Merri-bek's cpage self-discovery below.
static esp_err_t http_get_status(const char *url, char *buf, size_t buf_size, int *out_status)
{
    http_buf_t hb = { .buf = buf, .capacity = buf_size, .len = 0, .overflow = false };
    buf[0] = '\0';
    if (out_status != NULL) {
        *out_status = -1;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (out_status != NULL) {
        *out_status = status;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "http request returned status %d", status);
        return ESP_FAIL;
    }
    if (hb.overflow) {
        ESP_LOGW(TAG, "http response exceeded buffer (%u bytes), discarding", (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url, char *buf, size_t buf_size)
{
    return http_get_status(url, buf, buf_size, NULL);
}

static void parse_hex_color(const char *hex, schedule_color_t *out)
{
    if (hex[0] == '#') {
        hex++;
    }
    long rgb = strtol(hex, NULL, 16);
    out->r = (rgb >> 16) & 0xFF;
    out->g = (rgb >> 8) & 0xFF;
    out->b = rgb & 0xFF;
}

static void format_date(char *out, size_t out_size, time_t t)
{
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    strftime(out, out_size, "%Y-%m-%d", &tm_val);
}

static time_t event_mktime(uint16_t y, uint8_t mo, uint8_t d)
{
    struct tm tm_ev = {0};
    tm_ev.tm_year = (int)y - 1900;
    tm_ev.tm_mon = (int)mo - 1;
    tm_ev.tm_mday = d;
    tm_ev.tm_hour = 12;
    tm_ev.tm_isdst = -1;
    return mktime(&tm_ev);
}

// Fetches every dated event in the given window, sorted by date ascending,
// into out[] (capped at max_out) - deliberately RAW, no type filtering at all
// (not even "waste"), so both the poll path and the diagnostics UI see the
// same real data; type_rules filtering/recolouring happens as a separate step
// (see apply_type_rules()) so the raw fetch stays reusable for both. Returns
// the count filled, or -1 on a network/parse failure (0 is a valid,
// successful "nothing scheduled" result).
//
// out_waste_dow_known/out_waste_weekday are optional (NULL is fine - only the
// real poll path in do_fetch_events() needs them): the response's recurring
// general-waste rule entry (no "start" field, a "dow" array instead) is
// otherwise skipped entirely by this function, so this is the only place that
// ever sees it. Confirmed live against the real Maribyrnong API: "dow" is a
// JSON array (e.g. [5]) - only its first element is used, since a single
// weekly collection day is what's been observed and what this feature needs.
static int fetch_and_parse_events(const char *subdomain, uint32_t property_id, int lookahead_days,
                                   waste_api_event_t *out, int max_out,
                                   bool *out_waste_dow_known, uint8_t *out_waste_weekday)
{
    if (out_waste_dow_known != NULL) {
        *out_waste_dow_known = false;
    }
    time_t now = time(NULL);
    char start_str[11];
    char end_str[11];
    format_date(start_str, sizeof(start_str), now);
    format_date(end_str, sizeof(end_str), now + (time_t)lookahead_days * 86400);

    char url[192];
    snprintf(url, sizeof(url), "https://%s.waste-info.com.au/api/v1/properties/%u.json?start=%s&end=%s",
             subdomain, (unsigned)property_id, start_str, end_str);

    char *buf = malloc(EVENTS_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for events fetch buffer");
        return -1;
    }

    esp_err_t err = http_get(url, buf, EVENTS_BUF_SIZE);
    if (err != ESP_OK) {
        free(buf);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL || !cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "unexpected events response shape (expected a JSON array)");
        cJSON_Delete(root);
        return -1;
    }

    int n = 0;
    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count && n < max_out; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        cJSON *event_type = cJSON_GetObjectItem(item, "event_type");
        cJSON *start = cJSON_GetObjectItem(item, "start");
        cJSON *color = cJSON_GetObjectItem(item, "color");

        // The recurring general-waste rule entry has no plain "start" field -
        // it carries a "dow" array (weekly collection weekday) instead. Grab
        // that here, before skipping the entry below, since this is the only
        // place that ever sees it.
        if (out_waste_dow_known != NULL && !*out_waste_dow_known) {
            cJSON *dow = cJSON_GetObjectItem(item, "dow");
            if (cJSON_IsArray(dow) && cJSON_GetArraySize(dow) > 0) {
                cJSON *first = cJSON_GetArrayItem(dow, 0);
                if (cJSON_IsNumber(first)) {
                    int iso = (int)first->valuedouble;
                    // The API reports ISO-8601 weekdays (Mon=1..Sun=7); the
                    // rest of this project uses struct tm's (Sun=0..Sat=6).
                    // They agree for Mon-Sat and differ *only* on Sunday,
                    // which is why Maribyrnong's Friday dow:[5] has always
                    // worked and hid this: a Sunday-collection council would
                    // have produced an out-of-range weekday of 7.
                    if (iso >= 1 && iso <= 7) {
                        *out_waste_dow_known = true;
                        if (out_waste_weekday != NULL) {
                            *out_waste_weekday = (uint8_t)(iso == 7 ? 0 : iso);
                        }
                    }
                }
            }
        }

        // The recurring general-waste rule entry has no plain "start" field -
        // skip anything that isn't a concrete dated instance. This is a data
        // shape filter, not a type-mapping decision - type_rules (applied
        // separately) is where "ignore this type" actually gets decided.
        if (!cJSON_IsString(event_type) || !cJSON_IsString(start)) {
            continue;
        }

        int y = 0, mo = 0, d = 0;
        sscanf(start->valuestring, "%d-%d-%d", &y, &mo, &d);
        time_t ev_time = event_mktime((uint16_t)y, (uint8_t)mo, (uint8_t)d);
        if (ev_time == (time_t)-1) {
            continue;
        }

        waste_api_event_t ev = {0};
        ev.year = (uint16_t)y;
        ev.month = (uint8_t)mo;
        ev.day = (uint8_t)d;
        if (cJSON_IsString(color)) {
            parse_hex_color(color->valuestring, &ev.color);
        }
        snprintf(ev.event_type, sizeof(ev.event_type), "%s", event_type->valuestring);

        // Insertion sort by date ascending - n is always small (a few events
        // per multi-week window), so this is simpler than pulling in qsort.
        int pos = n;
        while (pos > 0 && event_mktime(out[pos - 1].year, out[pos - 1].month, out[pos - 1].day) > ev_time) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = ev;
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// Applies the configured type_rules to a raw event list: drops ignored types
// and overrides colour for types with an explicit (non-ignored) rule. A type
// with no rule at all falls back to a sane default so a fresh, unconfigured
// setup still behaves sensibly: "waste" is dropped (general collection,
// weekly, no special reminder needed - matches the previously-proven-correct
// manual shortcut logic this replaces), everything else is kept using the
// API's own colour. Filters events[] in place; returns the new count.
static int apply_type_rules(const waste_api_config_t *cfg, waste_api_event_t *events, int n)
{
    int out_n = 0;
    for (int i = 0; i < n; i++) {
        const waste_api_type_rule_t *rule = NULL;
        for (int r = 0; r < WASTE_API_MAX_TYPE_RULES; r++) {
            if (cfg->type_rules[r].event_type[0] != '\0' &&
                strcmp(cfg->type_rules[r].event_type, events[i].event_type) == 0) {
                rule = &cfg->type_rules[r];
                break;
            }
        }

        if (rule != NULL) {
            if (rule->ignored) {
                continue;
            }
            events[i].color = rule->color;
        } else if (strcmp(events[i].event_type, "waste") == 0) {
            continue; // no explicit rule yet: default-ignore general waste
        }
        // else: no rule configured for this type yet - keep it, API's own colour as-is

        if (out_n != i) {
            events[out_n] = events[i];
        }
        out_n++;
    }
    return out_n;
}

// ------------------------------------------------- bespoke council backends --
//
// Knox, Whitehorse, Merri-bek and Monash (SPEC.md 3.13.3/3.13.4) - the
// critical working group (SPEC.md 1.2) minus Maribyrnong, which rides the
// Impact Apps path above. All four share one shape: an opaque address id
// (produced by the search functions below, stored in config.address_id) and
// a fetch that returns the next collection date per waste stream. Each
// backend normalises its streams onto the Impact Apps event_type vocabulary
// (waste / recycle / organic / glass) so everything downstream - type rules,
// ignore defaults, the name-keyed colour table, the sticky cache, the
// resolver - is completely backend-agnostic.
//
// None of these backends return colour data, so events are coloured here
// from the same defaults the web UI's name-keyed table uses (Victorian lid
// colours; the user can still remap via type rules).

static schedule_color_t bespoke_default_color(const char *event_type)
{
    if (strcmp(event_type, "recycle") == 0) return (schedule_color_t){255, 150, 0}; // tuned yellow, see web_server.c
    if (strcmp(event_type, "organic") == 0) return (schedule_color_t){0, 255, 0};
    if (strcmp(event_type, "glass") == 0)   return (schedule_color_t){128, 0, 128};
    return (schedule_color_t){255, 0, 0};   // waste / anything unrecognised
}

// Percent-encodes src into dst (RFC 3986 unreserved set kept literal).
static void url_encode(char *dst, size_t dst_size, const char *src)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 4 < dst_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

static void insert_event_sorted(waste_api_event_t *out, int *n, int max_out, const waste_api_event_t *ev)
{
    if (*n >= max_out) {
        return;
    }
    time_t ev_time = event_mktime(ev->year, ev->month, ev->day);
    int pos = *n;
    while (pos > 0 && event_mktime(out[pos - 1].year, out[pos - 1].month, out[pos - 1].day) > ev_time) {
        out[pos] = out[pos - 1];
        pos--;
    }
    out[pos] = *ev;
    (*n)++;
}

// Builds one event from a prose/markup string carrying a date ("Next
// collection is <span>05 August 2026</span>") and inserts it. Quietly skips
// strings with no parseable date - a missing stream is data, not an error.
static void add_stream_event(waste_api_event_t *out, int *n, int max_out,
                              const char *event_type, const char *text)
{
    if (text == NULL) {
        return;
    }
    waste_api_event_t ev = {0};
    if (!date_parse_flex(text, &ev.year, &ev.month, &ev.day)) {
        return;
    }
    snprintf(ev.event_type, sizeof(ev.event_type), "%s", event_type);
    ev.color = bespoke_default_color(event_type);
    insert_event_sorted(out, n, max_out, &ev);
}

// --- Knox (SPEC.md 3.13.4): two plain JSON calls on knox.vic.gov.au. ---
//
// The recurring-weekday strings ("Weekly collection on Wednesday" /
// "Fortnightly collection on Wednesday") are deliberately NOT used as a waste
// dow signal: which of the two describes general waste is ambiguous in the
// payload, and every stream arrives as an explicit dated event anyway.

static int knox_fetch_events(const char *address_id, waste_api_event_t *out, int max_out)
{
    char enc[64];
    url_encode(enc, sizeof(enc), address_id);
    char url[160];
    snprintf(url, sizeof(url), "https://www.knox.vic.gov.au/rubbish-collection/find?address=%s", enc);

    char *buf = malloc(EVENTS_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, EVENTS_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "knox: unexpected response shape");
        cJSON_Delete(root);
        return -1;
    }

    static const struct { const char *field; const char *type; } STREAMS[] = {
        {"rubbish_date",   "waste"},
        {"recycling_date", "recycle"},
        {"green_date",     "organic"},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof(STREAMS) / sizeof(STREAMS[0]); i++) {
        cJSON *item = cJSON_GetObjectItem(root, STREAMS[i].field);
        add_stream_event(out, &n, max_out, STREAMS[i].type,
                          cJSON_IsString(item) ? item->valuestring : NULL);
    }
    cJSON_Delete(root);
    return n;
}

static int knox_search(const char *query, waste_api_search_result_t *out, int max_out)
{
    char enc[256];
    url_encode(enc, sizeof(enc), query);
    char url[384];
    snprintf(url, sizeof(url), "https://www.knox.vic.gov.au/rubbish-collection/autocomplete/find?q=%s", enc);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, LOOKUP_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1;
    }

    int n = 0;
    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count && n < max_out; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        cJSON *value = cJSON_GetObjectItem(item, "value");
        cJSON *label = cJSON_GetObjectItem(item, "label");
        if (!cJSON_IsString(value) || !cJSON_IsString(label)) {
            continue;
        }
        snprintf(out[n].id, sizeof(out[n].id), "%s", value->valuestring);
        snprintf(out[n].label, sizeof(out[n].label), "%s", label->valuestring);
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// --- Whitehorse (SPEC.md 3.13.4): public "Weave" GIS on
// map.whitehorse.vic.gov.au. Recycling and organics arrive as explicit next
// dates; household waste is weekly on collectionDay, which becomes the
// recurring waste-dow signal (same mechanism Impact Apps' recurring rule
// feeds). ---

static int whitehorse_fetch_events(const char *address_id, waste_api_event_t *out, int max_out,
                                    bool *out_dow_known, uint8_t *out_dow)
{
    char enc[64];
    url_encode(enc, sizeof(enc), address_id);
    char url[256];
    snprintf(url, sizeof(url),
             "https://map.whitehorse.vic.gov.au/weave/services/v1/feature/getFeaturesByIds"
             "?entityId=lyr_vicmap_property&datadefinition=dd_whm_property_waste&ids=%s", enc);

    char *buf = malloc(EVENTS_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, EVENTS_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *features = cJSON_GetObjectItem(root, "features");
    cJSON *feature = cJSON_IsArray(features) ? cJSON_GetArrayItem(features, 0) : NULL;
    cJSON *props = cJSON_GetObjectItem(feature, "properties");
    cJSON *dd = cJSON_GetObjectItem(props, "dd_whm_property_waste");
    cJSON *rec = cJSON_IsArray(dd) ? cJSON_GetArrayItem(dd, 0) : NULL;
    if (rec != NULL) {
        n = 0;
        cJSON *next_recycle = cJSON_GetObjectItem(rec, "nextRecycle");
        cJSON *next_gobs = cJSON_GetObjectItem(rec, "nextGOBS");
        add_stream_event(out, &n, max_out, "recycle",
                          cJSON_IsString(next_recycle) ? next_recycle->valuestring : NULL);
        add_stream_event(out, &n, max_out, "organic",
                          cJSON_IsString(next_gobs) ? next_gobs->valuestring : NULL);

        cJSON *day = cJSON_GetObjectItem(rec, "collectionDay");
        if (out_dow_known != NULL && cJSON_IsString(day)) {
            int wd = date_parse_weekday(day->valuestring);
            if (wd >= 0) {
                *out_dow_known = true;
                if (out_dow != NULL) {
                    *out_dow = (uint8_t)wd;
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "whitehorse: unexpected response shape");
    }
    cJSON_Delete(root);
    return n;
}

static int whitehorse_search(const char *query, waste_api_search_result_t *out, int max_out)
{
    char enc[256];
    url_encode(enc, sizeof(enc), query);
    char url[448];
    // Small limit on purpose: the reference client asks for 1000, which would
    // be unusable here (SPEC.md 3.13.4).
    snprintf(url, sizeof(url),
             "https://map.whitehorse.vic.gov.au/weave/services/v1/index/search"
             "?query=%s&indexes=index.property&type=EXACT&crs=EPSG:3857&limit=%d", enc, max_out);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, LOOKUP_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (cJSON_IsArray(results)) {
        n = 0;
        int count = cJSON_GetArraySize(results);
        for (int i = 0; i < count && n < max_out; i++) {
            cJSON *item = cJSON_GetArrayItem(results, i);
            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *label = cJSON_GetObjectItem(item, "display1");
            if (!cJSON_IsString(id) || !cJSON_IsString(label)) {
                continue;
            }
            snprintf(out[n].id, sizeof(out[n].id), "%s", id->valuestring);
            snprintf(out[n].label, sizeof(out[n].label), "%s", label->valuestring);
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

// --- Merri-bek (SPEC.md 3.13.3): ArcGIS address search + the council's own
// CMS API. The "address id" packs everything AddressDetails needs,
// '|'-separated: rate codes for waste/recycling/FOGO/glass, day, zone, glass
// week, then the address itself. Coordinates are NOT included - the endpoint
// ignores them entirely (verified by probing with garbage values), so zeros
// are sent and the EPSG:28355 projection the council's own JS performs is
// skipped. ---

// A bare CMS content-page id, mandatory. It is a real page id, validated by
// the server (a made-up value gets HTTP 500), and it rotates when the council
// publishes a new year's calendar page (86612 -> 183782 between research and
// implementation; old ids keep working only for as long as the old page
// exists). Rather than being a hardcoded annual-maintenance item, the current
// value is DISCOVERED at runtime when the compiled-in fallback stops working:
// the calendar page's own HTML carries the id (in its AJAX call), so the
// device POSTs its saved address fields to the year-derived calendar URL and
// scans the response. SPEC.md 3.13.3.
#define MERRIBEK_CPAGE_FALLBACK "183782"

// Runtime-discovered cpage (empty until a discovery has succeeded). Guarded
// by s_mutex - fetches can run on both the poll task and the httpd task.
static char s_merribek_cpage[12];

static void merribek_calendar_url_for_year(char *buf, size_t buf_size, int year)
{
    snprintf(buf, buf_size,
             "https://www.merri-bek.vic.gov.au/living-in-merri-bek/waste-and-recycling/"
             "bins-and-collection-services/waste-calendar%02d/", year % 100);
}

// The current year per the device clock, with a sane floor for the window
// between boot and SNTP sync (when the clock reads 1970).
static int merribek_current_year(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int year = tm_now.tm_year + 1900;
    return (year < 2026) ? 2026 : year;
}

void waste_api_merribek_calendar_url(char *buf, size_t buf_size)
{
    merribek_calendar_url_for_year(buf, buf_size, merribek_current_year());
}

// Streaming scanner for "cpage" followed by a digit run, fed byte-by-byte so
// chunk boundaries can't split the match. The calendar page is ~135KB -
// far beyond any buffer here - so it is never accumulated, only scanned.
typedef struct {
    size_t match_pos;   // progress through the literal "cpage"
    int    seps_seen;   // separators consumed after the literal (: ' " = space)
    char   digits[8];
    size_t digits_len;
    bool   done;
} cpage_scan_t;

static void cpage_scan_feed(cpage_scan_t *s, const char *data, size_t len)
{
    static const char LIT[] = "cpage";
    for (size_t i = 0; i < len && !s->done; i++) {
        char c = data[i];
        if (s->match_pos < sizeof(LIT) - 1) {
            s->match_pos = (c == LIT[s->match_pos]) ? s->match_pos + 1 : (c == LIT[0] ? 1 : 0);
            s->seps_seen = 0;
            s->digits_len = 0;
            continue;
        }
        // Literal matched; allow a few separator chars, then collect digits.
        if (c >= '0' && c <= '9') {
            if (s->digits_len + 1 < sizeof(s->digits)) {
                s->digits[s->digits_len++] = c;
            }
        } else if (s->digits_len > 0) {
            if (s->digits_len >= 4) {
                s->digits[s->digits_len] = '\0';
                s->done = true; // a plausible page id (>=4 digits) ended
            } else {
                s->match_pos = 0; // too short to be a page id - keep looking
            }
        } else if ((c == ':' || c == '\'' || c == '"' || c == '=' || c == ' ') && ++s->seps_seen <= 4) {
            // still between the literal and its value
        } else {
            s->match_pos = 0; // "cpagex" or similar - not our token
        }
    }
}

static esp_err_t cpage_scan_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != NULL && evt->data_len > 0) {
        cpage_scan_feed((cpage_scan_t *)evt->user_data, (const char *)evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

// POSTs the saved address fields to the given calendar-page URL (the same
// form submission a browser makes) and scans the response for the cpage id.
static bool merribek_scan_calendar_page(const char *url, const char *body, char *out, size_t out_size)
{
    cpage_scan_t scan = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = cpage_scan_handler,
        .user_data = &scan,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || !scan.done) {
        ESP_LOGW(TAG, "merri-bek: cpage scan of %s failed (status %d, found=%d)", url, status, scan.done);
        return false;
    }
    snprintf(out, out_size, "%s", scan.digits);
    return true;
}

// Tries the calendar page for this year, then next year, then last year -
// covering both edges of the annual rollover (new page not yet published /
// old page already retired).
static bool merribek_discover_cpage(const char *body, char *out, size_t out_size)
{
    int year = merribek_current_year();
    const int years[] = {year, year + 1, year - 1};
    for (size_t i = 0; i < sizeof(years) / sizeof(years[0]); i++) {
        char url[192];
        merribek_calendar_url_for_year(url, sizeof(url), years[i]);
        if (merribek_scan_calendar_page(url, body, out, out_size)) {
            ESP_LOGI(TAG, "merri-bek: discovered cpage %s from the %d calendar page", out, years[i]);
            return true;
        }
    }
    return false;
}

// One attempt against /api/AddressDetails with a specific cpage.
// Returns event count >= 0 on success; -1 on transport failure (network down
// - don't bother with discovery, it would fail too); -2 when the server
// actively rejected the request (HTTP 500, or the all-null "no service"
// response) - the signature of a stale cpage, worth a discovery+retry.
static int merribek_attempt_fetch(const char *fields[8], const char *cpage,
                                   waste_api_event_t *out, int max_out)
{
    char enc_addr[256];
    url_encode(enc_addr, sizeof(enc_addr), fields[7]);
    char url[640];
    snprintf(url, sizeof(url),
             "https://www.merri-bek.vic.gov.au/api/AddressDetails"
             "?xPoint=0&yPoint=0&wasteDay=%s&wasteRateCode=%s&recycleRateCode=%s"
             "&fogoRateCode=%s&glassRateCode=%s&zone=%s&glassWeekNumber=%s"
             "&address=%s&cpage=%s",
             fields[4], fields[0], fields[1], fields[2], fields[3], fields[5], fields[6],
             enc_addr, cpage);

    char *buf = malloc(EVENTS_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    int status = -1;
    esp_err_t err = http_get_status(url, buf, EVENTS_BUF_SIZE, &status);
    if (err != ESP_OK) {
        free(buf);
        // A response that reached us proves the network path works, so the
        // rejection is about the request - i.e. very likely the cpage.
        return (status >= 400) ? -2 : -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    cJSON *rec = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : root;
    cJSON *no_service = cJSON_GetObjectItem(rec, "noService");
    if (cJSON_IsString(no_service) && strcmp(no_service->valuestring, "no service") == 0) {
        cJSON_Delete(root);
        return -2;
    }

    static const struct { const char *field; const char *type; } STREAMS[] = {
        {"wasteNext",   "waste"},
        {"recycleNext", "recycle"},
        {"fogoNext",    "organic"},
        {"glassNext",   "glass"},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof(STREAMS) / sizeof(STREAMS[0]); i++) {
        cJSON *item = cJSON_GetObjectItem(rec, STREAMS[i].field);
        add_stream_event(out, &n, max_out, STREAMS[i].type,
                          cJSON_IsString(item) ? item->valuestring : NULL);
    }
    cJSON_Delete(root);
    return n;
}

static int merribek_fetch_events(const char *address_id, waste_api_event_t *out, int max_out)
{
    // Unpack "waste|recycle|fogo|glass|Day|Zone|Week|ADDRESS".
    char id_copy[WASTE_API_ADDRESS_ID_MAX_LEN + 1];
    snprintf(id_copy, sizeof(id_copy), "%s", address_id);
    const char *fields[8] = {0};
    char *p = id_copy;
    for (int i = 0; i < 8; i++) {
        fields[i] = p;
        if (i == 7) {
            break; // the address is the tail; it may itself never contain '|'
        }
        char *sep = strchr(p, '|');
        if (sep == NULL) {
            ESP_LOGW(TAG, "merri-bek: malformed address id");
            return -1;
        }
        *sep = '\0';
        p = sep + 1;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char cpage[sizeof(s_merribek_cpage)];
    snprintf(cpage, sizeof(cpage), "%s", s_merribek_cpage[0] ? s_merribek_cpage : MERRIBEK_CPAGE_FALLBACK);
    xSemaphoreGive(s_mutex);

    int n = merribek_attempt_fetch(fields, cpage, out, max_out);
    if (n != -2) {
        return n;
    }

    // The server rejected this cpage - it has likely rotated with a new
    // calendar year. Rediscover it from the calendar page and retry once.
    ESP_LOGW(TAG, "merri-bek: cpage %s rejected, attempting rediscovery", cpage);
    // Sized for the compiler's worst case: the encoded address appears twice
    // (sAddress + address) at up to 255 bytes each, plus every other field.
    char body[832];
    char enc_addr[256];
    url_encode(enc_addr, sizeof(enc_addr), fields[7]);
    snprintf(body, sizeof(body),
             "sAddress=%s&address=%s&xPoint=0&yPoint=0&wasteDay=%s&zone=%s"
             "&wasteRateCode=%s&recycleRateCode=%s&fogoRateCode=%s&glassRateCode=%s"
             "&glassWeekNumber=%s",
             enc_addr, enc_addr, fields[4], fields[5], fields[0], fields[1], fields[2], fields[3], fields[6]);

    char fresh[sizeof(s_merribek_cpage)];
    if (!merribek_discover_cpage(body, fresh, sizeof(fresh)) || strcmp(fresh, cpage) == 0) {
        return -1;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_merribek_cpage, sizeof(s_merribek_cpage), "%s", fresh);
    xSemaphoreGive(s_mutex);

    n = merribek_attempt_fetch(fields, fresh, out, max_out);
    return (n < 0) ? -1 : n;
}

static int merribek_search(const char *query, waste_api_search_result_t *out, int max_out)
{
    // The layer stores addresses UPPERCASE and unabbreviated; matching is
    // case-sensitive, so uppercase the query on the way in (SPEC.md 3.13.3).
    // Single quotes are doubled for the SQL-ish LIKE - Merri-bek has real
    // apostrophe streets (O'Hea Street, Coburg).
    char upper[96];
    size_t o = 0;
    for (const char *p = query; *p != '\0' && o + 2 < sizeof(upper); p++) {
        if (*p == '\'') {
            upper[o++] = '\'';
            upper[o++] = '\'';
        } else {
            upper[o++] = (char)toupper((unsigned char)*p);
        }
    }
    upper[o] = '\0';

    char where[160];
    snprintf(where, sizeof(where), "UPPER(EZI_Address) LIKE '%s%%'", upper);
    char enc_where[480];
    url_encode(enc_where, sizeof(enc_where), where);

    char url[768];
    snprintf(url, sizeof(url),
             "https://services6.arcgis.com/8L5sOwfzTAvcvQur/ArcGIS/rest/services/WasteServices4Bin/FeatureServer/0/query"
             "?where=%s&outFields=EZI_Address,Waste_Rate_Code,Recycling_Rate_Code,FOGO_Rate_Code,"
             "Glass_Rate_Code,Day,Zone,GlassWeek&returnGeometry=false&resultRecordCount=%d&f=json",
             enc_where, max_out);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, LOOKUP_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsArray(features)) {
        n = 0;
        int count = cJSON_GetArraySize(features);
        for (int i = 0; i < count && n < max_out; i++) {
            cJSON *attrs = cJSON_GetObjectItem(cJSON_GetArrayItem(features, i), "attributes");
            cJSON *addr  = cJSON_GetObjectItem(attrs, "EZI_Address");
            cJSON *waste = cJSON_GetObjectItem(attrs, "Waste_Rate_Code");
            cJSON *recy  = cJSON_GetObjectItem(attrs, "Recycling_Rate_Code");
            cJSON *fogo  = cJSON_GetObjectItem(attrs, "FOGO_Rate_Code");
            cJSON *glass = cJSON_GetObjectItem(attrs, "Glass_Rate_Code");
            cJSON *day   = cJSON_GetObjectItem(attrs, "Day");
            cJSON *zone  = cJSON_GetObjectItem(attrs, "Zone");
            cJSON *week  = cJSON_GetObjectItem(attrs, "GlassWeek");
            if (!cJSON_IsString(addr) || !cJSON_IsString(waste) || !cJSON_IsString(recy) ||
                !cJSON_IsString(fogo) || !cJSON_IsString(glass) || !cJSON_IsString(day) ||
                !cJSON_IsString(zone) || !cJSON_IsNumber(week)) {
                continue;
            }
            snprintf(out[n].id, sizeof(out[n].id), "%s|%s|%s|%s|%s|%s|%d|%s",
                     waste->valuestring, recy->valuestring, fogo->valuestring, glass->valuestring,
                     day->valuestring, zone->valuestring, (int)week->valuedouble, addr->valuestring);
            snprintf(out[n].label, sizeof(out[n].label), "%s", addr->valuestring);
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

// --- Monash (SPEC.md 3.13.4): OpenCities "MyArea". The schedule arrives as
// an HTML fragment inside a JSON envelope; the CSS class tokens
// (general-waste / recycling / green-waste) are the stable machine
// vocabulary, NOT the <h3> display labels a council can rename at will.
// Akamai-fronted: served fine to honest clients, but a browser User-Agent
// that doesn't match the TLS fingerprint gets 403 - so no UA games, ever
// (verified live: plain curl 200, spoofed-Chrome curl 403). ---

// Finds needle inside the first len bytes of hay (a bounded strstr - the
// block being scanned is a slice of a larger HTML string, not its own
// NUL-terminated buffer).
static const char *find_in(const char *hay, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) {
        return NULL;
    }
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static int monash_fetch_events(const char *address_id, waste_api_event_t *out, int max_out)
{
    char enc[128];
    url_encode(enc, sizeof(enc), address_id);
    char url[256];
    // ocsvclang is mandatory: without it the endpoint returns
    // {"success":false} with HTTP 200 and no message (SPEC.md 3.13.4).
    snprintf(url, sizeof(url),
             "https://www.monash.vic.gov.au/ocapi/Public/myarea/wasteservices"
             "?geolocationid=%s&ocsvclang=en-AU", enc);

    char *buf = malloc(EVENTS_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, EVENTS_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    cJSON *success = cJSON_GetObjectItem(root, "success");
    cJSON *content = cJSON_GetObjectItem(root, "responseContent");
    if (!cJSON_IsTrue(success) || !cJSON_IsString(content)) {
        // success:false means a malformed request (bad/missing ocsvclang) at
        // least as often as an unknown address id.
        ESP_LOGW(TAG, "monash: success=false or missing content");
        cJSON_Delete(root);
        return -1;
    }

    static const struct { const char *token; const char *type; } TOKENS[] = {
        {"general-waste", "waste"},
        {"recycling",     "recycle"},
        {"green-waste",   "organic"},
    };

    int n = 0;
    const char *html = content->valuestring; // cJSON already unescaped it
    const char *p = html;
    while ((p = strstr(p, "waste-services-result")) != NULL) {
        const char *after = p + strlen("waste-services-result");
        if (*after == 's') {
            // The container div's class is "waste-services-results" (plural) -
            // same prefix, not a service block.
            p = after;
            continue;
        }
        const char *class_end = strchr(p, '"');
        const char *next_block = strstr(after, "waste-services-result");
        size_t block_len = next_block ? (size_t)(next_block - p) : strlen(p);
        size_t class_len = (class_end && (size_t)(class_end - p) < block_len)
                               ? (size_t)(class_end - p) : block_len;

        const char *type = NULL;
        for (size_t t = 0; t < sizeof(TOKENS) / sizeof(TOKENS[0]); t++) {
            if (find_in(p, class_len, TOKENS[t].token) != NULL) {
                type = TOKENS[t].type;
                break;
            }
        }
        // date-precise guards against deployments that return a week number
        // instead of a real date; one-off services (hard waste) match no
        // token and are skipped.
        if (type != NULL && find_in(p, class_len, "date-precise") != NULL) {
            const char *ns = find_in(p, block_len, "next-service");
            if (ns != NULL) {
                size_t rest = block_len - (size_t)(ns - p);
                const char *gt = find_in(ns, rest, ">");
                if (gt != NULL) {
                    char text[48];
                    size_t o = 0;
                    for (const char *c = gt + 1; c < p + block_len && *c != '<' && o + 1 < sizeof(text); c++) {
                        text[o++] = *c;
                    }
                    text[o] = '\0';
                    add_stream_event(out, &n, max_out, type, text);
                }
            }
        }
        p = after;
    }
    cJSON_Delete(root);
    return n;
}

static int monash_search(const char *query, waste_api_search_result_t *out, int max_out)
{
    char enc[256];
    url_encode(enc, sizeof(enc), query);
    char url[384];
    snprintf(url, sizeof(url), "https://www.monash.vic.gov.au/api/v1/myarea/search?keywords=%s", enc);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    if (http_get(url, buf, LOOKUP_BUF_SIZE) != ESP_OK) {
        free(buf);
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *items = cJSON_GetObjectItem(root, "Items");
    if (cJSON_IsArray(items)) {
        n = 0;
        int count = cJSON_GetArraySize(items);
        for (int i = 0; i < count && n < max_out; i++) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            cJSON *id = cJSON_GetObjectItem(item, "Id");
            cJSON *label = cJSON_GetObjectItem(item, "AddressSingleLine");
            if (!cJSON_IsString(id) || !cJSON_IsString(label)) {
                continue;
            }
            snprintf(out[n].id, sizeof(out[n].id), "%s", id->valuestring);
            snprintf(out[n].label, sizeof(out[n].label), "%s", label->valuestring);
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

// --- dispatch ---

int waste_api_search_address(uint8_t backend, const char *query,
                              waste_api_search_result_t *out, int max_out)
{
    switch (backend) {
    case COUNCIL_BACKEND_KNOX:       return knox_search(query, out, max_out);
    case COUNCIL_BACKEND_WHITEHORSE: return whitehorse_search(query, out, max_out);
    case COUNCIL_BACKEND_MERRI_BEK:  return merribek_search(query, out, max_out);
    case COUNCIL_BACKEND_MONASH:     return monash_search(query, out, max_out);
    default:
        return -1; // Impact Apps uses the locality/street/property wizard instead
    }
}

// One raw fetch for whatever backend cfg selects, normalised to sorted dated
// events + the optional recurring waste weekday. Everything above this layer
// is backend-agnostic.
static int fetch_events_for_config(const waste_api_config_t *cfg, int lookahead_days,
                                    waste_api_event_t *out, int max_out,
                                    bool *out_dow_known, uint8_t *out_dow)
{
    if (out_dow_known != NULL) {
        *out_dow_known = false;
    }
    switch (cfg->backend) {
    case COUNCIL_BACKEND_IMPACT_APPS:
        return fetch_and_parse_events(cfg->council_subdomain, cfg->property_id, lookahead_days,
                                       out, max_out, out_dow_known, out_dow);
    case COUNCIL_BACKEND_KNOX:
        return knox_fetch_events(cfg->address_id, out, max_out);
    case COUNCIL_BACKEND_WHITEHORSE:
        return whitehorse_fetch_events(cfg->address_id, out, max_out, out_dow_known, out_dow);
    case COUNCIL_BACKEND_MERRI_BEK:
        return merribek_fetch_events(cfg->address_id, out, max_out);
    case COUNCIL_BACKEND_MONASH:
        return monash_fetch_events(cfg->address_id, out, max_out);
    default:
        ESP_LOGW(TAG, "unknown backend %u", (unsigned)cfg->backend);
        return -1;
    }
}

// -------------------------------------------------------- events polling --

static void do_fetch_events(void)
{
    waste_api_config_t cfg = waste_api_get_config();
    if (!cfg.enabled || !waste_api_config_complete(&cfg)) {
        return;
    }

    waste_api_event_t events[POLL_MAX_EVENTS];
    bool waste_dow_known = false;
    uint8_t waste_weekday = 0;
    int n = fetch_events_for_config(&cfg, EVENTS_LOOKAHEAD_DAYS,
                                     events, sizeof(events) / sizeof(events[0]),
                                     &waste_dow_known, &waste_weekday);
    if (n < 0) {
        // Network/parse failure. Leave the cache entirely alone - a failed
        // poll is not evidence that the collection we already know about
        // isn't happening.
        ESP_LOGW(TAG, "poll failed, keeping existing cache");
        return;
    }
    n = apply_type_rules(&cfg, events, n);

    // Drop anything already in the past, so a stale-but-still-listed event
    // can't be picked as "next". events[] is sorted ascending.
    int first = 0;
    while (first < n && days_since(events[first].year, events[first].month, events[first].day) > 0) {
        first++;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = false;
    if (first < n) {
        // A qualifying event is authoritative: always overwrite.
        s_cache.has_event = true;
        s_cache.event_year = events[first].year;
        s_cache.event_month = events[first].month;
        s_cache.event_day = events[first].day;
        s_cache.color = events[first].color;
        // A second distinct type sharing the same earliest date (e.g.
        // recycling AND glass together) - see SPEC.md 3.7. events[] is
        // sorted by date, so only the next index can tie with this one.
        s_cache.has_secondary = (first + 1 < n &&
                                  events[first + 1].year == events[first].year &&
                                  events[first + 1].month == events[first].month &&
                                  events[first + 1].day == events[first].day);
        if (s_cache.has_secondary) {
            s_cache.secondary_color = events[first + 1].color;
        }
        changed = true;
    }
    // Note the absent `else`: a poll that found nothing does NOT clear the
    // cache. Staleness is decided by the cached date passing (see
    // waste_api_get_next_event), not by any given poll coming up empty.

    // The recurring waste weekday is a separate signal with no expiry - only
    // ever updated when a poll actually learns one, never cleared by one that
    // didn't happen to see the recurring rule entry.
    if (waste_dow_known && (!s_cache.waste_dow_known || s_cache.waste_weekday != waste_weekday)) {
        s_cache.waste_dow_known = true;
        s_cache.waste_weekday = waste_weekday;
        changed = true;
    }
    waste_api_cache_t to_persist = s_cache;
    xSemaphoreGive(s_mutex);

    if (changed) {
        persist_cache(&to_persist);
    }

    ESP_LOGI(TAG, "poll complete: %d event(s) in window, next=%s", n,
             to_persist.has_event ? "known" : "unknown");
}

int waste_api_fetch_upcoming(const waste_api_config_t *cfg,
                              int lookahead_days, waste_api_event_t *out, int max_out)
{
    return fetch_events_for_config(cfg, lookahead_days, out, max_out, NULL, NULL);
}

bool waste_api_get_next_event(waste_api_next_event_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool enabled = s_config.enabled;
    waste_api_cache_t cache = s_cache;
    xSemaphoreGive(s_mutex);

    if (!enabled || !cache.has_event) {
        return false;
    }
    // The one and only staleness rule: has the collection date itself passed?
    // Note `> 0`, not `>= 0` - on the collection day the light's window may
    // still be wrapping past midnight from the night before, so the date stays
    // valid through its own day.
    if (days_since(cache.event_year, cache.event_month, cache.event_day) > 0) {
        return false;
    }

    if (out != NULL) {
        out->year = cache.event_year;
        out->month = cache.event_month;
        out->day = cache.event_day;
        out->color = cache.color;
        out->has_secondary = cache.has_secondary;
        out->secondary_color = cache.secondary_color;
    }
    return true;
}

bool waste_api_get_waste_weekday(uint8_t *out_wday)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool enabled = s_config.enabled;
    waste_api_cache_t cache = s_cache;
    xSemaphoreGive(s_mutex);

    if (!enabled || !cache.waste_dow_known) {
        return false;
    }
    if (out_wday) *out_wday = cache.waste_weekday;
    return true;
}

static void waste_api_task_fn(void *arg)
{
    for (;;) {
        do_fetch_events();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t waste_api_task_start(void)
{
    // Generous stack: TLS handshakes are stack-hungry compared to the rest of
    // this project's small tasks.
    BaseType_t ok = xTaskCreate(waste_api_task_fn, "waste_api_task", 8192, NULL, tskIDLE_PRIORITY + 2, &s_task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

// -------------------------------------------------------- address lookup --

int waste_api_fetch_localities(const char *subdomain, waste_api_locality_t *out, int max_out)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s.waste-info.com.au/api/v1/localities.json", subdomain);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    esp_err_t err = http_get(url, buf, LOOKUP_BUF_SIZE);
    if (err != ESP_OK) {
        free(buf);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf); // free the HTTP buffer (and, below, the cJSON tree) before the caller builds any HTML
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *array = cJSON_GetObjectItem(root, "localities");
    if (cJSON_IsArray(array)) {
        int count = cJSON_GetArraySize(array);
        n = count < max_out ? count : max_out;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(array, i);
            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *name = cJSON_GetObjectItem(item, "name");
            out[i].id = cJSON_IsNumber(id) ? (uint32_t)id->valuedouble : 0;
            snprintf(out[i].name, sizeof(out[i].name), "%s", cJSON_IsString(name) ? name->valuestring : "");
        }
    }
    cJSON_Delete(root);
    return n;
}

int waste_api_fetch_streets(const char *subdomain, uint32_t locality_id, waste_api_street_t *out, int max_out)
{
    char url[160];
    snprintf(url, sizeof(url), "https://%s.waste-info.com.au/api/v1/streets.json?locality=%u",
             subdomain, (unsigned)locality_id);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    esp_err_t err = http_get(url, buf, LOOKUP_BUF_SIZE);
    if (err != ESP_OK) {
        free(buf);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *array = cJSON_GetObjectItem(root, "streets");
    if (cJSON_IsArray(array)) {
        int count = cJSON_GetArraySize(array);
        n = count < max_out ? count : max_out;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(array, i);
            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *name = cJSON_GetObjectItem(item, "name");
            out[i].id = cJSON_IsNumber(id) ? (uint32_t)id->valuedouble : 0;
            snprintf(out[i].name, sizeof(out[i].name), "%s", cJSON_IsString(name) ? name->valuestring : "");
        }
    }
    cJSON_Delete(root);
    return n;
}

int waste_api_fetch_properties(const char *subdomain, uint32_t street_id, waste_api_property_t *out, int max_out)
{
    char url[160];
    snprintf(url, sizeof(url), "https://%s.waste-info.com.au/api/v1/properties.json?street=%u",
             subdomain, (unsigned)street_id);

    char *buf = malloc(LOOKUP_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    esp_err_t err = http_get(url, buf, LOOKUP_BUF_SIZE);
    if (err != ESP_OK) {
        free(buf);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return -1;
    }

    int n = -1;
    cJSON *array = cJSON_GetObjectItem(root, "properties");
    if (cJSON_IsArray(array)) {
        int count = cJSON_GetArraySize(array);
        n = count < max_out ? count : max_out;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(array, i);
            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *name = cJSON_GetObjectItem(item, "name");
            out[i].id = cJSON_IsNumber(id) ? (uint32_t)id->valuedouble : 0;
            snprintf(out[i].name, sizeof(out[i].name), "%s", cJSON_IsString(name) ? name->valuestring : "");
        }
    }
    cJSON_Delete(root);
    return n;
}
