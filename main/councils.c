#include "councils.h"

#include <string.h>

// Every subdomain below was probed against the live
// https://<sub>.waste-info.com.au/api/v1/localities.json endpoint - 39 of 39
// returned 200. This list is NOT a copy of any upstream source: both candidate
// sources were wrong on their own (the Home Assistant integration carries dead
// entries such as `bayside` and `burwood-waste`, whose live subdomains are
// `rockdale` and `burwood`; the vendor's own calendars index omits several
// working councils including `maribyrnong`). Re-probe rather than trust if
// this is ever refreshed - see SPEC.md 3.13.1.
//
// Note how little the subdomains resemble the council names - `rockdale` for
// Bayside, `bmcc` for Blue Mountains, `hrcc` for Horsham, `qprc` for
// Queanbeyan-Palerang. That unguessability is the whole reason this table
// exists instead of a free-text field.
//
// `campbelltown` and `wellington` are ambiguous across states (both exist in
// NSW and SA / VIC and NSW respectively). Their states were resolved by
// fetching each one's locality list and reading the suburb names: Airds and
// Ambarvale place `campbelltown` in NSW, Boisdale and Briagolong place
// `wellington` in Gippsland, VIC.
const council_t COUNCILS[] = {
    // --- VIC ---
    // The four bespoke entries (Knox, Merri-bek, City of Monash, Whitehorse)
    // are the critical working group (SPEC.md 1.2) alongside Maribyrnong.
    // Their param is a stable slug for URL/config use, not a subdomain.
    {"Baw Baw Shire Council",                "VIC", COUNCIL_BACKEND_IMPACT_APPS, "baw-baw"},
    {"Benalla Rural City Council",           "VIC", COUNCIL_BACKEND_IMPACT_APPS, "benalla"},
    {"City of Ballarat",                     "VIC", COUNCIL_BACKEND_IMPACT_APPS, "ballarat"},
    {"City of Monash",                       "VIC", COUNCIL_BACKEND_MONASH,      "monash"},
    {"Hobsons Bay City Council",             "VIC", COUNCIL_BACKEND_IMPACT_APPS, "hobsons-bay"},
    {"Horsham Rural City Council",           "VIC", COUNCIL_BACKEND_IMPACT_APPS, "hrcc"},
    {"Knox City Council",                    "VIC", COUNCIL_BACKEND_KNOX,        "knox"},
    {"Maribyrnong City Council",             "VIC", COUNCIL_BACKEND_IMPACT_APPS, "maribyrnong"},
    {"Merri-bek City Council",               "VIC", COUNCIL_BACKEND_MERRI_BEK,   "merri-bek"},
    {"Moira Shire Council",                  "VIC", COUNCIL_BACKEND_IMPACT_APPS, "moira"},
    {"Murrindindi Shire Council",            "VIC", COUNCIL_BACKEND_IMPACT_APPS, "murrindindi"},
    {"Pyrenees Shire Council",               "VIC", COUNCIL_BACKEND_IMPACT_APPS, "pyrenees"},
    {"Wellington Shire Council",             "VIC", COUNCIL_BACKEND_IMPACT_APPS, "wellington"},
    {"Whitehorse City Council",              "VIC", COUNCIL_BACKEND_WHITEHORSE,  "whitehorse"},

    // --- NSW ---
    {"Bayside Council",                      "NSW", COUNCIL_BACKEND_IMPACT_APPS, "rockdale"},
    {"Bega Valley Shire Council",            "NSW", COUNCIL_BACKEND_IMPACT_APPS, "bega"},
    {"Blue Mountains City Council",          "NSW", COUNCIL_BACKEND_IMPACT_APPS, "bmcc"},
    {"Burwood Council",                      "NSW", COUNCIL_BACKEND_IMPACT_APPS, "burwood"},
    {"Campbelltown City Council",            "NSW", COUNCIL_BACKEND_IMPACT_APPS, "campbelltown"},
    {"City of Canada Bay Council",           "NSW", COUNCIL_BACKEND_IMPACT_APPS, "canada-bay"},
    {"Clarence Valley Council",              "NSW", COUNCIL_BACKEND_IMPACT_APPS, "clarence"},
    {"Coffs Coast Waste Services",           "NSW", COUNCIL_BACKEND_IMPACT_APPS, "coffs-coast"},
    {"Cowra Council",                        "NSW", COUNCIL_BACKEND_IMPACT_APPS, "cowra"},
    {"Cumberland City Council",              "NSW", COUNCIL_BACKEND_IMPACT_APPS, "cumberland"},
    {"Forbes Shire Council",                 "NSW", COUNCIL_BACKEND_IMPACT_APPS, "forbes"},
    {"Gwydir Shire Council",                 "NSW", COUNCIL_BACKEND_IMPACT_APPS, "gwydir"},
    {"Kempsey Shire Council",                "NSW", COUNCIL_BACKEND_IMPACT_APPS, "kempsey"},
    {"Ku-ring-gai Council",                  "NSW", COUNCIL_BACKEND_IMPACT_APPS, "ku-ring-gai"},
    {"Lithgow City Council",                 "NSW", COUNCIL_BACKEND_IMPACT_APPS, "lithgow"},
    {"Moree Plains Shire Council",           "NSW", COUNCIL_BACKEND_IMPACT_APPS, "moree"},
    {"Narrabri Shire Council",               "NSW", COUNCIL_BACKEND_IMPACT_APPS, "narrabri"},
    {"Penrith City Council",                 "NSW", COUNCIL_BACKEND_IMPACT_APPS, "penrith"},
    {"Port Macquarie Hastings Council",      "NSW", COUNCIL_BACKEND_IMPACT_APPS, "pmhc"},
    {"Port Stephens Council",                "NSW", COUNCIL_BACKEND_IMPACT_APPS, "port-stephens"},
    {"Queanbeyan-Palerang Regional Council", "NSW", COUNCIL_BACKEND_IMPACT_APPS, "qprc"},
    {"Snowy Valleys Council",                "NSW", COUNCIL_BACKEND_IMPACT_APPS, "snowy-valleys"},
    {"Wollongong City Council",              "NSW", COUNCIL_BACKEND_IMPACT_APPS, "wollongong"},

    // --- QLD ---
    {"Brisbane City Council",                "QLD", COUNCIL_BACKEND_IMPACT_APPS, "brisbane"},
    {"Gympie Regional Council",              "QLD", COUNCIL_BACKEND_IMPACT_APPS, "gympie"},
    {"Livingstone Shire Council",            "QLD", COUNCIL_BACKEND_IMPACT_APPS, "livingstone"},
    {"Redland City Council",                 "QLD", COUNCIL_BACKEND_IMPACT_APPS, "redland"},
    {"South Burnett Regional Council",       "QLD", COUNCIL_BACKEND_IMPACT_APPS, "south-burnett"},

    // --- TAS ---
    {"City of Launceston",                   "TAS", COUNCIL_BACKEND_IMPACT_APPS, "launceston"},
};
const size_t COUNCIL_COUNT = sizeof(COUNCILS) / sizeof(COUNCILS[0]);

const char *const STATE_ORDER[] = {"VIC", "NSW", "QLD", "TAS"};
const size_t STATE_ORDER_COUNT = sizeof(STATE_ORDER) / sizeof(STATE_ORDER[0]);

static const struct { const char *code; const char *label; } STATE_LABELS[] = {
    {"VIC", "Victoria"},
    {"NSW", "New South Wales"},
    {"QLD", "Queensland"},
    {"SA",  "South Australia"},
    {"TAS", "Tasmania"},
    {"WA",  "Western Australia"},
    {"NT",  "Northern Territory"},
    {"ACT", "Australian Capital Territory"},
};

const council_t *council_find_by_param(const char *param)
{
    if (param == NULL || param[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        if (strcmp(COUNCILS[i].param, param) == 0) {
            return &COUNCILS[i];
        }
    }
    return NULL;
}

const council_t *council_find_by_backend(uint8_t backend)
{
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        if ((uint8_t)COUNCILS[i].backend == backend) {
            return &COUNCILS[i];
        }
    }
    return NULL;
}

const council_t *council_find_impact_apps(const char *subdomain)
{
    const council_t *c = council_find_by_param(subdomain);
    return (c != NULL && c->backend == COUNCIL_BACKEND_IMPACT_APPS) ? c : NULL;
}

const char *council_display_name(const char *subdomain)
{
    const council_t *c = council_find_impact_apps(subdomain);
    return (c != NULL) ? c->name : (subdomain != NULL ? subdomain : "");
}

const char *council_state_label(const char *state)
{
    if (state == NULL) {
        return "";
    }
    for (size_t i = 0; i < sizeof(STATE_LABELS) / sizeof(STATE_LABELS[0]); i++) {
        if (strcmp(STATE_LABELS[i].code, state) == 0) {
            return STATE_LABELS[i].label;
        }
    }
    return state;
}
