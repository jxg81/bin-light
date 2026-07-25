// Runs the REAL council-backend parsers in waste_api.c against REAL captured
// payloads (test/host/fixtures/, fetched live from each council's endpoint on
// 2026-07-25). The HTTP layer is stubbed to serve those files by URL match,
// chunked through the same event-handler/append path the device uses - so
// what's tested is the shipping parse code end to end, not a re-derivation.
//
// If a council changes its payload shape, re-capture the fixture with the
// curl commands in SPEC.md 3.13.3/3.13.4 and see what breaks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "nvs.h"

// --- fixture-serving HTTP stub ----------------------------------------------

static const struct { const char *url_substr; const char *path; } FIXTURES[] = {
    {"knox.vic.gov.au/rubbish-collection/autocomplete", "fixtures/knox_search.json"},
    {"knox.vic.gov.au/rubbish-collection/find",         "fixtures/knox_find.json"},
    {"map.whitehorse.vic.gov.au/weave/services/v1/index/search",   "fixtures/whitehorse_search.json"},
    {"map.whitehorse.vic.gov.au/weave/services/v1/feature",        "fixtures/whitehorse_features.json"},
    {"services6.arcgis.com",                            "fixtures/merribek_search.json"},
    {"merri-bek.vic.gov.au/api/AddressDetails",         "fixtures/merribek_details.json"},
    {"monash.vic.gov.au/api/v1/myarea/search",          "fixtures/monash_search.json"},
    {"monash.vic.gov.au/ocapi",                         "fixtures/monash_waste.json"},
};

struct stub_http_client {
    esp_http_client_config_t config;
    int status;
};

static char g_last_url[1024];

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct stub_http_client *c = calloc(1, sizeof(*c));
    c->config = *config;
    snprintf(g_last_url, sizeof(g_last_url), "%s", config->url);
    return c;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    const char *path = NULL;
    for (size_t i = 0; i < sizeof(FIXTURES) / sizeof(FIXTURES[0]); i++) {
        if (strstr(client->config.url, FIXTURES[i].url_substr) != NULL) {
            path = FIXTURES[i].path;
            break;
        }
    }
    if (path == NULL) {
        fprintf(stderr, "stub_http: no fixture for %s\n", client->config.url);
        client->status = 404;
        return ESP_OK;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "stub_http: cannot open %s\n", path);
        client->status = 404;
        return ESP_OK;
    }
    // Deliver in small chunks so the append/overflow path gets exercised the
    // way real socket reads exercise it.
    char chunk[512];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        esp_http_client_event_t evt = {
            .event_id = HTTP_EVENT_ON_DATA,
            .user_data = client->config.user_data,
            .data = chunk,
            .data_len = (int)got,
        };
        client->config.event_handler(&evt);
    }
    fclose(f);
    client->status = 200;
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { free(client); return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

// --- tiny in-memory NVS (same as test_resolver.c) ----------------------------

static struct { char key[32]; unsigned char val[1024]; size_t len; bool used; } g_nvs[8];
esp_err_t nvs_open(const char *ns, int m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
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

// Pull in the real module (static functions become directly callable).
#include "waste_api.c"

// --- harness -----------------------------------------------------------------

static int g_fail;

static void check(const char *name, bool ok, const char *detail)
{
    printf("%s %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_fail++;
}

static void expect_event(const char *name, const waste_api_event_t *ev,
                          const char *type, int y, int mo, int d)
{
    char detail[128];
    snprintf(detail, sizeof(detail), "got %s %04u-%02u-%02u", ev->event_type,
             (unsigned)ev->year, (unsigned)ev->month, (unsigned)ev->day);
    check(name, strcmp(ev->event_type, type) == 0 && ev->year == y && ev->month == mo && ev->day == d, detail);
}

int main(void)
{
    char detail[160];
    waste_api_event_t ev[POLL_MAX_EVENTS];
    waste_api_search_result_t res[12];

    printf("\n== Knox ==\n");
    int n = knox_search("1053 Burwood Highway", res, 12);
    snprintf(detail, sizeof(detail), "n=%d id=%s label=%.40s", n, n > 0 ? res[0].id : "-", n > 0 ? res[0].label : "-");
    check("search finds the property id", n == 1 && strcmp(res[0].id, "69454") == 0, detail);

    n = knox_fetch_events("69454", ev, POLL_MAX_EVENTS);
    snprintf(detail, sizeof(detail), "n=%d", n);
    check("fetch returns all three streams", n == 3, detail);
    if (n == 3) {
        // Sorted ascending: recycle + organic tie on 29 Jul, waste 5 Aug.
        expect_event("  recycle first (29 Jul)", &ev[0], "recycle", 2026, 7, 29);
        expect_event("  organic ties (29 Jul)", &ev[1], "organic", 2026, 7, 29);
        expect_event("  waste last (05 Aug)", &ev[2], "waste", 2026, 8, 5);
    }

    printf("\n== Whitehorse ==\n");
    n = whitehorse_search("1 Main Street", res, 12);
    snprintf(detail, sizeof(detail), "n=%d id=%s label=%.40s", n, n > 0 ? res[0].id : "-", n > 0 ? res[0].label : "-");
    check("search returns matches with ids", n > 0 && strcmp(res[0].id, "3060205") == 0, detail);

    bool dow_known = false;
    uint8_t dow = 99;
    n = whitehorse_fetch_events("3060205", ev, POLL_MAX_EVENTS, &dow_known, &dow);
    snprintf(detail, sizeof(detail), "n=%d dow_known=%d dow=%u", n, dow_known, (unsigned)dow);
    check("fetch returns recycle + organic", n == 2, detail);
    if (n == 2) {
        expect_event("  organic first (27 Jul)", &ev[0], "organic", 2026, 7, 27);
        expect_event("  recycle second (03 Aug)", &ev[1], "recycle", 2026, 8, 3);
    }
    check("weekly waste day parsed (Monday=1)", dow_known && dow == 1, detail);

    printf("\n== Merri-bek ==\n");
    n = merribek_search("1 Vincent Street Oak Park", res, 12);
    snprintf(detail, sizeof(detail), "n=%d id=%s", n, n > 0 ? res[0].id : "-");
    check("search packs the full id", n == 1 &&
          strcmp(res[0].id, "101|142|160|170|Monday|B|3|1 VINCENT STREET OAK PARK 3046") == 0, detail);
    check("search label is the address",
          n == 1 && strcmp(res[0].label, "1 VINCENT STREET OAK PARK 3046") == 0,
          n == 1 ? res[0].label : "-");

    n = merribek_fetch_events("101|142|160|170|Monday|B|3|1 VINCENT STREET OAK PARK 3046", ev, POLL_MAX_EVENTS);
    snprintf(detail, sizeof(detail), "n=%d", n);
    check("fetch returns all four streams", n == 4, detail);
    if (n == 4) {
        // waste/recycle/organic all 27 Jul (in stream order within the tie),
        // glass 3 Aug.
        expect_event("  waste (27 Jul)", &ev[0], "waste", 2026, 7, 27);
        expect_event("  recycle (27 Jul)", &ev[1], "recycle", 2026, 7, 27);
        expect_event("  organic (27 Jul)", &ev[2], "organic", 2026, 7, 27);
        expect_event("  glass (03 Aug)", &ev[3], "glass", 2026, 8, 3);
    }
    check("malformed id fails cleanly", merribek_fetch_events("101|142", ev, POLL_MAX_EVENTS) == -1, "-1");

    printf("\n== Monash ==\n");
    n = monash_search("4 Carson Street, Mulgrave", res, 12);
    snprintf(detail, sizeof(detail), "n=%d id=%.40s", n, n > 0 ? res[0].id : "-");
    check("search returns GUID ids", n > 0 && strcmp(res[0].id, "e1c469c8-6565-401a-9b9e-f5440daffa82") == 0, detail);

    n = monash_fetch_events("e1c469c8-6565-401a-9b9e-f5440daffa82", ev, POLL_MAX_EVENTS);
    snprintf(detail, sizeof(detail), "n=%d", n);
    check("fetch parses the HTML: three streams, hard-waste skipped", n == 3, detail);
    if (n == 3) {
        expect_event("  waste (31 Jul)", &ev[0], "waste", 2026, 7, 31);
        expect_event("  organic ties (31 Jul)", &ev[1], "organic", 2026, 7, 31);
        expect_event("  recycle (07 Aug)", &ev[2], "recycle", 2026, 8, 7);
    }

    printf("\n== type rules across a bespoke backend ==\n");
    // The whole point of normalising to the waste/recycle/organic/glass
    // vocabulary: the existing default ("waste" ignored when unmapped)
    // applies to every backend with zero backend-specific code.
    waste_api_config_t cfg = {0};
    n = merribek_fetch_events("101|142|160|170|Monday|B|3|1 VINCENT STREET OAK PARK 3046", ev, POLL_MAX_EVENTS);
    n = apply_type_rules(&cfg, ev, n);
    snprintf(detail, sizeof(detail), "n=%d first=%s", n, n > 0 ? ev[0].event_type : "-");
    check("unmapped 'waste' is dropped by default", n == 3 && strcmp(ev[0].event_type, "recycle") == 0, detail);

    printf("\n== config migration v2 -> v3 ==\n");
    memset(g_nvs, 0, sizeof(g_nvs));
    waste_api_config_v2_t old = {0};
    old.version = 2;
    old.enabled = true;
    snprintf(old.council_subdomain, sizeof(old.council_subdomain), "maribyrnong");
    old.property_id = 2855360;
    snprintf(old.property_label, sizeof(old.property_label), "12 Example St, Footscray");
    snprintf(old.type_rules[0].event_type, sizeof(old.type_rules[0].event_type), "waste");
    old.type_rules[0].ignored = true;
    nvs_handle_t h;
    nvs_open("binlight", NVS_READWRITE, &h);
    nvs_set_blob(h, WASTE_API_NVS_KEY_V2, &old, sizeof(old));
    nvs_close(h);

    waste_api_init();
    waste_api_config_t migrated = waste_api_get_config();
    snprintf(detail, sizeof(detail), "backend=%u subdomain=%s prop=%u label=%.30s rule0=%s/%d",
             (unsigned)migrated.backend, migrated.council_subdomain, (unsigned)migrated.property_id,
             migrated.property_label, migrated.type_rules[0].event_type, migrated.type_rules[0].ignored);
    check("v2 blob migrates with everything intact",
          migrated.version == 3 && migrated.enabled &&
          migrated.backend == COUNCIL_BACKEND_IMPACT_APPS &&
          strcmp(migrated.council_subdomain, "maribyrnong") == 0 &&
          migrated.property_id == 2855360 &&
          strcmp(migrated.property_label, "12 Example St, Footscray") == 0 &&
          migrated.type_rules[0].ignored &&
          strcmp(migrated.type_rules[0].event_type, "waste") == 0,
          detail);
    check("migrated config is complete", waste_api_config_complete(&migrated), "");

    // And the migrated blob persisted under the new key, so next boot loads
    // v3 directly.
    waste_api_config_t reloaded;
    size_t len = sizeof(reloaded);
    nvs_open("binlight", NVS_READWRITE, &h);
    bool has_v3 = nvs_get_blob(h, WASTE_API_NVS_KEY, &reloaded, &len) == ESP_OK && reloaded.version == 3;
    nvs_close(h);
    check("migration persisted under the v3 key", has_v3, "");

    printf("\n== config completeness ==\n");
    waste_api_config_t c2 = {0};
    c2.backend = COUNCIL_BACKEND_KNOX;
    check("knox without an address id is incomplete", !waste_api_config_complete(&c2), "");
    snprintf(c2.address_id, sizeof(c2.address_id), "69454");
    check("knox with an address id is complete", waste_api_config_complete(&c2), "");
    c2.backend = 99;
    check("unknown backend is never complete", !waste_api_config_complete(&c2), "");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
