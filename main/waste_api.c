#include "waste_api.h"

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

static const char *TAG = "waste_api";

#define WASTE_API_NVS_NAMESPACE  "binlight"
#define WASTE_API_NVS_KEY        "waste_api_v2"
#define WASTE_API_STRUCT_VERSION 2

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
#define EVENTS_BUF_SIZE       4096   // real observed 2-week response was ~700 bytes; generous margin
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
    return c;
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
                ESP_LOGI(TAG, "loaded waste API config from NVS (enabled=%d)", s_config.enabled);
                return ESP_OK;
            }
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
static esp_err_t http_get(const char *url, char *buf, size_t buf_size)
{
    http_buf_t hb = { .buf = buf, .capacity = buf_size, .len = 0, .overflow = false };
    buf[0] = '\0';

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

// -------------------------------------------------------- events polling --

static void do_fetch_events(void)
{
    waste_api_config_t cfg = waste_api_get_config();
    if (!cfg.enabled || cfg.council_subdomain[0] == '\0' || cfg.property_id == 0) {
        return;
    }

    waste_api_event_t events[POLL_MAX_EVENTS];
    bool waste_dow_known = false;
    uint8_t waste_weekday = 0;
    int n = fetch_and_parse_events(cfg.council_subdomain, cfg.property_id, EVENTS_LOOKAHEAD_DAYS,
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

int waste_api_fetch_upcoming(const char *subdomain, uint32_t property_id,
                              int lookahead_days, waste_api_event_t *out, int max_out)
{
    return fetch_and_parse_events(subdomain, property_id, lookahead_days, out, max_out, NULL, NULL);
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
