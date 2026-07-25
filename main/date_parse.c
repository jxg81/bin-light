#include "date_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const MONTHS[12] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december",
};

static const char *const WEEKDAYS[7] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
};

static bool is_year(long y)
{
    return y >= 2000 && y <= 2099;
}

// 1-12 for an alphabetic month token of len chars, or 0. Full names and any
// prefix of 3+ letters ("Jul", "Aug") - every month is unique at 3 letters.
static int month_lookup(const char *s, size_t len)
{
    if (len < 3) {
        return 0;
    }
    for (int m = 0; m < 12; m++) {
        if (len > strlen(MONTHS[m])) {
            continue;
        }
        size_t i;
        for (i = 0; i < len; i++) {
            if (tolower((unsigned char)s[i]) != MONTHS[m][i]) {
                break;
            }
        }
        if (i == len) {
            return m + 1;
        }
    }
    return 0;
}

bool date_parse_flex(const char *s, uint16_t *out_year, uint8_t *out_month, uint8_t *out_day)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            continue;
        }

        char *end;
        long day = strtol(p, &end, 10);
        if (day < 1 || day > 31) {
            p = end - 1; // skip the rest of this number (e.g. a postcode)
            continue;
        }

        const char *q = end;
        long month = 0, year = 0;

        if (*q == '-' || *q == '/') {
            // Numeric D<sep>M<sep>YYYY, same separator both times, no padding
            // required ("5-1-2026", "31/7/2026").
            char sep = *q;
            char *e2;
            long m = strtol(q + 1, &e2, 10);
            if (e2 != q + 1 && m >= 1 && m <= 12 && *e2 == sep) {
                char *e3;
                long y = strtol(e2 + 1, &e3, 10);
                if (e3 - (e2 + 1) == 4 && is_year(y)) {
                    month = m;
                    year = y;
                }
            }
        } else {
            // "D MonthName YYYY" ("05 August 2026", "29 Jul 2026").
            while (*q == ' ') {
                q++;
            }
            if (isalpha((unsigned char)*q)) {
                const char *w = q;
                while (isalpha((unsigned char)*q)) {
                    q++;
                }
                int m = month_lookup(w, (size_t)(q - w));
                if (m > 0) {
                    while (*q == ' ') {
                        q++;
                    }
                    char *e3;
                    long y = strtol(q, &e3, 10);
                    if (e3 - q == 4 && is_year(y)) {
                        month = m;
                        year = y;
                    }
                }
            }
        }

        if (month != 0) {
            if (out_year) *out_year = (uint16_t)year;
            if (out_month) *out_month = (uint8_t)month;
            if (out_day) *out_day = (uint8_t)day;
            return true;
        }
        p = end - 1;
    }
    return false;
}

int date_parse_weekday(const char *s)
{
    for (int w = 0; w < 7; w++) {
        size_t wlen = strlen(WEEKDAYS[w]);
        for (const char *p = s; *p != '\0'; p++) {
            size_t i;
            for (i = 0; i < wlen && p[i] != '\0'; i++) {
                if (tolower((unsigned char)p[i]) != WEEKDAYS[w][i]) {
                    break;
                }
            }
            if (i == wlen) {
                return w;
            }
        }
    }
    return -1;
}
