#pragma once

#include <stdbool.h>
#include <stdint.h>

// One tolerant date scanner instead of a parser per council (SPEC.md 3.13.4).
// The bespoke backends return five distinct date shapes, most wrapped in
// prose or markup:
//
//   "Next collection is <span>05 August 2026</span>"   Knox
//   "29 Jul 2026"                                      Whitehorse
//   "5-1-2026"                                         Merri-bek arrays (NOT zero-padded)
//   "Next collection is on 27 July 2026"               Merri-bek summaries
//   "Fri 31/7/2026"                                    Monash
//
// Scans s for the first thing that reads as a date: either numeric
// D<sep>M<sep>YYYY (sep '-' or '/', no padding required) or "D MonthName
// YYYY" (full or 3-letter month, case-insensitive). Leading prose, HTML tags
// and weekday names are skipped naturally because they aren't digits.
// Requires a 4-digit year (2000-2099) - that's what keeps a stray postcode
// ("... GULLY VIC 3156") or house number from parsing as a date.
bool date_parse_flex(const char *s, uint16_t *out_year, uint8_t *out_month, uint8_t *out_day);

// First weekday name found in s ("Weekly collection on Wednesday" -> 3), as
// struct tm.tm_wday (0=Sunday..6=Saturday), or -1 if none. Case-insensitive,
// full names only - abbreviations like "Wed" are ambiguous inside ordinary
// prose and nothing observed emits them.
int date_parse_weekday(const char *s);
