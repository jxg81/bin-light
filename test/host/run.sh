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

# Fails a rendered page that reached its buffer, and warns below 1KB of
# headroom. Exact equality means safe_append() clamped, i.e. the page was
# truncated - which is the failure this guards, not a near miss.
check_page() {
    file=$1; limit=$2; name=$3
    size=$(wc -c < "$file" | tr -d ' ')
    head=$((limit - size))
    if [ "$size" -ge "$limit" ]; then
        echo "FAIL $(basename "$file") is ${size}B, at or over $name ($limit) - truncated"
        status=1
    elif [ "$head" -lt 1024 ]; then
        echo "WARN $(basename "$file") is ${size}B, only ${head}B under $name ($limit)"
    else
        echo "PASS $(basename "$file") ${size}B, ${head}B under $name"
    fi
}

# The OTA version floor is a release-process setting, not code, and getting it
# wrong is silent: too high and the build refuses to reinstall itself, too low
# and a device can be walked back onto firmware with no URL allowlist. Neither
# shows up on the bench. So it is checked here, on every run.
#
# FIRST_SECURED_RELEASE is the first version carrying the BINLIGHT_OTA_URL_PREFIX
# check, and the floor is pinned there permanently - it is not a version number
# and does not move with releases. Below it, the downgrade bypass is open and
# the build must not ship. Above it, rollback room is being given away for
# nothing. Only a change to the OTA checks themselves should move it.
FIRST_SECURED_RELEASE=1.0.7

ver_ord() {
    a=$(echo "$1" | cut -d. -f1); b=$(echo "$1" | cut -d. -f2); c=$(echo "$1" | cut -d. -f3)
    echo $(( ${a:-0} * 1000000 + ${b:-0} * 1000 + ${c:-0} ))
}

echo "== OTA version floor =="
proj_ver=$(tr -d ' \n' < ../../version.txt)
floor=$(sed -n '/config BINLIGHT_OTA_MIN_VERSION/,/^$/s/^ *default "\(.*\)"/\1/p' $MAIN/Kconfig.projbuild)

if [ -z "$floor" ]; then
    echo "SKIP floor is empty - the version check is disabled by configuration"
elif [ "$(ver_ord "$floor")" -gt "$(ver_ord "$proj_ver")" ]; then
    echo "FAIL floor $floor is above version.txt $proj_ver - this build would refuse to reinstall itself"
    status=1
else
    echo "PASS floor $floor is at or below version.txt $proj_ver"
    if [ "$(ver_ord "$floor")" -lt "$(ver_ord "$FIRST_SECURED_RELEASE")" ]; then
        echo
        echo "  ****************************************************************"
        echo "  DO NOT GIVE A DEVICE AWAY ON THIS BUILD."
        echo "  The floor ($floor) is below $FIRST_SECURED_RELEASE, the first release with the"
        echo "  OTA URL allowlist. Anything that can reach POST /update can still"
        echo "  install pre-$FIRST_SECURED_RELEASE firmware, which accepts any URL at all - so the"
        echo "  allowlist is fully bypassable. This is fine for bench testing."
        echo "  Raise BINLIGHT_OTA_MIN_VERSION to $FIRST_SECURED_RELEASE before shipping."
        echo "  ****************************************************************"
        echo
    elif [ "$(ver_ord "$floor")" -gt "$(ver_ord "$FIRST_SECURED_RELEASE")" ]; then
        echo "NOTE floor is above $FIRST_SECURED_RELEASE, so releases between the two can no longer be"
        echo "     installed and rollback below $floor now needs the two-step. Pin it at"
        echo "     $FIRST_SECURED_RELEASE unless the OTA security checks themselves changed."
    fi
fi
echo

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
    "$OUT/render_page" --max-escaped "$OUT/home-worst-case-escaped.html"
    "$OUT/render_page" --setup   "$OUT/api-setup.html"
    "$OUT/render_page" --setup-escaped "$OUT/api-setup-worst-case-escaped.html"
    "$OUT/render_page" --merribek "$OUT/api-setup-merribek.html"
    "$OUT/render_page" --reset-confirm "$OUT/factory-reset-confirm.html"
    "$OUT/render_page" --update-uptodate  "$OUT/update-uptodate.html"
    "$OUT/render_page" --update-available "$OUT/update-available.html"
    "$OUT/render_page" --update-progress  "$OUT/update-progress.html"
    echo "page sizes (HTML_BUF_SIZE must exceed the largest home-* and"
    echo "update-*; SETUP_HTML_BUF_SIZE must exceed api-setup*):"
    wc -c "$OUT"/*.html

    echo
    echo "== page buffer headroom =="
    # Asserted, not eyeballed. safe_append() truncates silently and a truncated
    # page drops form fields, which read back as "absent" on the next save and
    # quietly destroy config - so a page that outgrows its buffer has to fail
    # the suite rather than print a number someone has to notice. The limits
    # are read from the source so they cannot drift from what ships.
    html_buf=$(sed -n 's/^#define HTML_BUF_SIZE  *\([0-9]*\).*/\1/p' $MAIN/web_server.c)
    setup_buf=$(sed -n 's/^#define SETUP_HTML_BUF_SIZE  *\([0-9]*\).*/\1/p' $MAIN/web_server.c)
    for f in "$OUT"/home-*.html "$OUT"/update-*.html "$OUT"/factory-reset-*.html; do
        check_page "$f" "$html_buf" HTML_BUF_SIZE
    done
    for f in "$OUT"/api-setup*.html; do
        check_page "$f" "$setup_buf" SETUP_HTML_BUF_SIZE
    done

    echo
    echo "== cross-origin gate =="
    "$OUT/render_page" --origin-check || status=1

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
