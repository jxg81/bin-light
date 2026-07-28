// The state -> POSIX TZ mapping the setup wizard uses to set the timezone
// from the council the user picked. Compiles the real table out of
// settings.c... except settings.c is NVS-bound, so the function under test is
// carved out by including it directly. If that ever stops working, the right
// fix is to move settings_tz_for_state() into its own file, not to weaken the
// test: an unset or wrong timezone is one of the two things that leave a
// freshly-reset light showing nothing (SPEC.md 4).
#include <stdio.h>
#include <string.h>
#include "councils.h"

const char *settings_tz_for_state(const char *state);

static int failures;
static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    const char *AEDT = "AEST-10AEDT,M10.1.0/2,M4.1.0/3";

    printf("== state -> timezone ==\n");
    expect(strcmp(settings_tz_for_state("VIC"), AEDT) == 0, "VIC observes DST");
    expect(strcmp(settings_tz_for_state("NSW"), AEDT) == 0, "NSW observes DST");
    expect(strcmp(settings_tz_for_state("TAS"), AEDT) == 0, "TAS observes DST");
    expect(strcmp(settings_tz_for_state("QLD"), "AEST-10") == 0, "QLD does not");
    expect(settings_tz_for_state("XX") == NULL, "an unknown code returns NULL, not a guess");
    expect(settings_tz_for_state(NULL) == NULL, "NULL is handled");

    printf("\n== every listed council resolves to a zone ==\n");
    // The wizard sets the timezone from council->state, so a council whose
    // state has no mapping would silently leave the timezone unset - exactly
    // the failure this feature exists to remove.
    int unmapped = 0;
    for (size_t i = 0; i < COUNCIL_COUNT; i++) {
        if (settings_tz_for_state(COUNCILS[i].state) == NULL) {
            printf("  unmapped: %s (%s)\n", COUNCILS[i].name, COUNCILS[i].state);
            unmapped++;
        }
    }
    expect(unmapped == 0, "no council in COUNCILS[] has an unmapped state");

    printf("\n%s (%d failures)\n", failures ? "FAILURES" : "all passed", failures);
    return failures ? 1 : 0;
}
