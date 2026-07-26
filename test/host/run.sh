#!/bin/sh
# Host-side tests. No ESP-IDF, no device, no toolchain setup - just cc.
#
# These compile the *real* main/*.c against thin stubs (see stub/), so they
# exercise the shipping logic rather than a reimplementation of it. Two things
# live here:
#
#   test_resolver  - drives schedule_get_next_collection() and the on-window
#                    check through concrete dates via a fake clock. Covers
#                    weekday wraparound, the past-midnight window, staleness,
#                    the event-vs-waste tie-break, and DST boundaries.
#   render_page    - dumps the exact bytes root_get_handler() would send, for
#                    eyeballing the UI in a browser and for checking the page
#                    still fits HTML_BUF_SIZE.
#
# Usage:
#   ./run.sh            run the tests
#   ./run.sh render     also write the three home-page renders to out/
set -e

cd "$(dirname "$0")"
MAIN=../../main
OUT=out
mkdir -p "$OUT"

CFLAGS="-I stub -I $MAIN -Wall -Wno-unused-function"

status=0

echo "building test_resolver..."
cc -o "$OUT/test_resolver" test_resolver.c $CFLAGS
echo
# stderr carries the modules' own ESP_LOG output; keep it out of the results.
"$OUT/test_resolver" 2>/dev/null || status=1

echo
echo "building test_councils..."
cc -o "$OUT/test_councils" test_councils.c $MAIN/councils.c $CFLAGS
echo
"$OUT/test_councils" 2>/dev/null || status=1

echo
echo "building test_dates..."
cc -o "$OUT/test_dates" test_dates.c $MAIN/date_parse.c $CFLAGS
echo
"$OUT/test_dates" 2>/dev/null || status=1

echo
echo "building test_buttons..."
cc -o "$OUT/test_buttons" test_buttons.c $CFLAGS
echo
"$OUT/test_buttons" 2>/dev/null || status=1

echo
echo "building test_captive_dns..."
# Parses packets from anything that can associate to an open AP, so this one is
# built with UBSan on - the bugs that matter here are memory-safety bugs, which
# a plain assertion pass would sail straight past.
#
# AddressSanitizer would be the better tool and is deliberately NOT used: on
# this Mac an -fsanitize=address binary hangs before main() (confirmed both
# inside and outside the tool sandbox, so it is the local toolchain, not the
# harness). If you are on a machine where it works, add `address,` below - the
# test is written to be worth it, and the loops at the end exist precisely to
# give a sanitizer something to catch.
cc -o "$OUT/test_captive_dns" test_captive_dns.c $CFLAGS -fsanitize=undefined -g
echo
"$OUT/test_captive_dns" 2>/dev/null || status=1

echo
echo "building test_backends..."
# Compiles the real waste_api.c against captured council payloads (fixtures/).
# cJSON comes straight from the managed component - same code as the device.
CJSON=../../managed_components/espressif__cjson/cJSON
cc -o "$OUT/test_backends" test_backends.c $MAIN/date_parse.c $MAIN/councils.c "$CJSON/cJSON.c" \
   -I "$CJSON" $CFLAGS
echo
"$OUT/test_backends" 2>/dev/null || status=1

if [ "$1" = "render" ]; then
    echo
    echo "building render_page..."
    cc -o "$OUT/render_page" render_page.c $MAIN/councils.c $CFLAGS
    "$OUT/render_page" ""        "$OUT/home-configured.html"
    "$OUT/render_page" --empty   "$OUT/home-unconfigured.html"
    "$OUT/render_page" --max     "$OUT/home-worst-case.html"
    "$OUT/render_page" --setup   "$OUT/api-setup.html"
    "$OUT/render_page" --merribek "$OUT/api-setup-merribek.html"
    "$OUT/render_page" --reset-confirm "$OUT/factory-reset-confirm.html"
    "$OUT/render_page" --update-uptodate  "$OUT/update-uptodate.html"
    "$OUT/render_page" --update-available "$OUT/update-available.html"
    "$OUT/render_page" --update-progress  "$OUT/update-progress.html"
    echo "page sizes (HTML_BUF_SIZE must exceed the largest home-*;"
    echo "SETUP_HTML_BUF_SIZE must exceed api-setup):"
    wc -c "$OUT"/*.html

    echo
    echo "== factory reset confirmation gate =="
    # An unconfirmed POST must render the warning and must NOT wipe. The
    # harness's factory_reset_perform() never returns, so if the gate ever
    # broke, --reset-confirm would hang rather than produce this file -
    # but assert on the content too, so a silently-empty page is caught.
    if grep -q "Are you sure" "$OUT/factory-reset-confirm.html" &&
       ! grep -q "has been erased" "$OUT/factory-reset-confirm.html"; then
        echo "PASS unconfirmed POST warns and does not erase"
    else
        echo "FAIL unconfirmed POST did not render the confirmation"
        status=1
    fi
fi

exit $status
