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

echo "building test_resolver..."
cc -o "$OUT/test_resolver" test_resolver.c $CFLAGS
echo

# stderr carries the modules' own ESP_LOG output; keep it out of the results.
"$OUT/test_resolver" 2>/dev/null
status=$?

if [ "$1" = "render" ]; then
    echo
    echo "building render_page..."
    cc -o "$OUT/render_page" render_page.c $CFLAGS
    "$OUT/render_page" ""        "$OUT/home-configured.html"
    "$OUT/render_page" --empty   "$OUT/home-unconfigured.html"
    "$OUT/render_page" --max     "$OUT/home-worst-case.html"
    echo "page sizes (HTML_BUF_SIZE must exceed the largest):"
    wc -c "$OUT"/home-*.html
fi

exit $status
