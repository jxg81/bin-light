// Structural checks on the hand-maintained council table (SPEC.md 3.13.1/5).
// The table is data, not logic, so the risk isn't a wrong algorithm - it's a
// duplicate subdomain, a typo'd state that silently hides a council from every
// dropdown, or an entry that drifts out of sort order and renders in the wrong
// group. None of those would fail to compile.
#include <stdio.h>
#include <string.h>
#include "councils.h"

static int g_fail;

static void check(const char *what, bool ok, const char *detail)
{
    printf("%s %-52s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) g_fail++;
}

int main(void)
{
    char detail[256];

    snprintf(detail, sizeof(detail), "%zu councils, %zu states", COUNCIL_COUNT, STATE_ORDER_COUNT);
    check("table is populated", COUNCIL_COUNT > 0 && STATE_ORDER_COUNT > 0, detail);

    // Every council's state must appear in STATE_ORDER, or it renders in no
    // group at all and becomes unreachable from the UI.
    int orphans = 0;
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        bool found = false;
        for (size_t s = 0; s < STATE_ORDER_COUNT; s++) {
            if (strcmp(COUNCILS[i].state, STATE_ORDER[s]) == 0) { found = true; break; }
        }
        if (!found) {
            printf("     orphaned: %s (state '%s')\n", COUNCILS[i].name, COUNCILS[i].state);
            orphans++;
        }
    }
    snprintf(detail, sizeof(detail), "%d orphaned", orphans);
    check("every council's state is in STATE_ORDER", orphans == 0, detail);

    // Duplicate params would make two councils indistinguishable once saved.
    int dupes = 0;
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        for (size_t j = i + 1; j < COUNCIL_COUNT; j++) {
            if (strcmp(COUNCILS[i].param, COUNCILS[j].param) == 0) {
                printf("     duplicate param '%s': %s / %s\n", COUNCILS[i].param, COUNCILS[i].name, COUNCILS[j].name);
                dupes++;
            }
            if (strcmp(COUNCILS[i].name, COUNCILS[j].name) == 0) {
                printf("     duplicate name '%s'\n", COUNCILS[i].name);
                dupes++;
            }
        }
    }
    snprintf(detail, sizeof(detail), "%d duplicate(s)", dupes);
    check("no duplicate subdomains or display names", dupes == 0, detail);

    // No empty fields - an empty param would build a URL to
    // "https://.waste-info.com.au/..." and fail confusingly at runtime.
    int empties = 0;
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        if (COUNCILS[i].name[0] == '\0' || COUNCILS[i].state[0] == '\0' || COUNCILS[i].param[0] == '\0') {
            empties++;
        }
    }
    snprintf(detail, sizeof(detail), "%d empty field(s)", empties);
    check("no empty name/state/param", empties == 0, detail);

    // The table must be grouped in STATE_ORDER sequence and alphabetical
    // within each group, because the dropdown renders it in one pass with no
    // runtime sort.
    int last_state_idx = -1, order_breaks = 0, sort_breaks = 0;
    const char *prev_name = NULL;
    const char *prev_state = NULL;
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        int idx = -1;
        for (size_t s = 0; s < STATE_ORDER_COUNT; s++) {
            if (strcmp(COUNCILS[i].state, STATE_ORDER[s]) == 0) { idx = (int)s; break; }
        }
        if (idx < last_state_idx) {
            printf("     out of state order at '%s' (%s)\n", COUNCILS[i].name, COUNCILS[i].state);
            order_breaks++;
        }
        if (prev_state != NULL && strcmp(prev_state, COUNCILS[i].state) == 0 &&
            strcmp(prev_name, COUNCILS[i].name) > 0) {
            printf("     out of alphabetical order: '%s' after '%s'\n", COUNCILS[i].name, prev_name);
            sort_breaks++;
        }
        last_state_idx = idx;
        prev_name = COUNCILS[i].name;
        prev_state = COUNCILS[i].state;
    }
    snprintf(detail, sizeof(detail), "%d break(s)", order_breaks);
    check("grouped in STATE_ORDER sequence", order_breaks == 0, detail);
    snprintf(detail, sizeof(detail), "%d break(s)", sort_breaks);
    check("alphabetical within each state", sort_breaks == 0, detail);

    // Lookup helpers.
    const council_t *c = council_find_impact_apps("maribyrnong");
    check("lookup finds a known subdomain",
          c != NULL && strcmp(c->name, "Maribyrnong City Council") == 0,
          c ? c->name : "not found");
    check("lookup misses an unknown subdomain", council_find_impact_apps("nope-not-real") == NULL, "NULL as expected");
    check("display name falls back to the subdomain itself",
          strcmp(council_display_name("nope-not-real"), "nope-not-real") == 0,
          council_display_name("nope-not-real"));
    check("display name is safe on empty input", council_display_name("")[0] == '\0', "empty");
    check("state label resolves", strcmp(council_state_label("VIC"), "Victoria") == 0, council_state_label("VIC"));
    check("state label falls back to the code", strcmp(council_state_label("ZZZ"), "ZZZ") == 0, "ZZZ");

    // Per-state counts, printed so a table edit shows up as a visible diff.
    printf("\n  councils per state:");
    for (size_t s = 0; s < STATE_ORDER_COUNT; s++) {
        int n = 0;
        for (size_t i = 0; i < COUNCIL_COUNT; i++) {
            if (strcmp(COUNCILS[i].state, STATE_ORDER[s]) == 0) n++;
        }
        printf(" %s=%d", STATE_ORDER[s], n);
    }
    printf(" (total %zu)\n", COUNCIL_COUNT);

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
