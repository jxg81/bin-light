// Exercises date_parse.c against every real date shape the council backends
// emit (SPEC.md 3.13.4's five-formats finding), plus the traps: postcodes,
// house numbers, markup, and prose.
#include <stdio.h>
#include <string.h>
#include "date_parse.h"

static int g_fail;

static void check(const char *name, bool ok, const char *detail)
{
    printf("%s %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_fail++;
}

static void expect_date(const char *input, int y, int mo, int d)
{
    uint16_t gy = 0;
    uint8_t gm = 0, gd = 0;
    bool ok = date_parse_flex(input, &gy, &gm, &gd);
    char name[80], detail[64];
    snprintf(name, sizeof(name), "\"%.55s\"", input);
    snprintf(detail, sizeof(detail), "got %s %04u-%02u-%02u", ok ? "ok" : "none",
             (unsigned)gy, (unsigned)gm, (unsigned)gd);
    check(name, ok && gy == y && gm == mo && gd == d, detail);
}

static void expect_no_date(const char *input)
{
    char name[80];
    snprintf(name, sizeof(name), "no date in \"%.45s\"", input);
    check(name, !date_parse_flex(input, NULL, NULL, NULL), "");
}

int main(void)
{
    printf("\n== the five real backend formats ==\n");
    expect_date("Next collection is <span>05 August 2026</span>", 2026, 8, 5);   // Knox
    expect_date("29 Jul 2026", 2026, 7, 29);                                      // Whitehorse
    expect_date("5-1-2026", 2026, 1, 5);                                          // Merri-bek array, non-padded
    expect_date("Next collection is on 27 July 2026", 2026, 7, 27);               // Merri-bek summary
    expect_date("Fri 31/7/2026", 2026, 7, 31);                                    // Monash

    printf("\n== variants ==\n");
    expect_date("12-1-2026", 2026, 1, 12);
    expect_date("03 Aug 2026", 2026, 8, 3);
    expect_date("Mon 24/8/2026", 2026, 8, 24);
    expect_date("1/12/2026", 2026, 12, 1);
    expect_date("31 december 2026", 2026, 12, 31);
    expect_date("Collection: 7 September 2026 (Tuesday)", 2026, 9, 7);

    printf("\n== traps ==\n");
    expect_no_date("1053 Burwood Highway, FERNTREE GULLY VIC 3156"); // house number + postcode
    expect_no_date("Weekly collection on Wednesday");
    expect_no_date("no service");
    expect_no_date("");
    expect_no_date("32/13/2026");         // impossible day-month
    expect_no_date("5-1-26");             // 2-digit year: ambiguous, rejected
    expect_no_date("waste-calendar26");
    expect_date("junk 99 then 5-1-2026 works", 2026, 1, 5); // scans past a bad number

    printf("\n== weekdays ==\n");
    char detail[32];
    int w = date_parse_weekday("Weekly collection on Wednesday");
    snprintf(detail, sizeof(detail), "got %d", w);
    check("\"Weekly collection on Wednesday\" -> 3", w == 3, detail);
    w = date_parse_weekday("Monday");
    snprintf(detail, sizeof(detail), "got %d", w);
    check("\"Monday\" -> 1", w == 1, detail);
    w = date_parse_weekday("SUNDAY");
    snprintf(detail, sizeof(detail), "got %d", w);
    check("case-insensitive \"SUNDAY\" -> 0", w == 0, detail);
    w = date_parse_weekday("no day here");
    snprintf(detail, sizeof(detail), "got %d", w);
    check("no weekday -> -1", w == -1, detail);

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
