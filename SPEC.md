# Bin Light — Project Specification

Reference document for the full set of requirements gathered so far, so nothing gets
dropped as the build continues. Update this file whenever scope changes.

> **Resuming work / starting a fresh session?** Read **§4 "Current state of the
> code"** first — it records what is written vs. flashed vs. actually verified on
> hardware, the one unflashed change sitting in the working tree, and the
> environment facts (no git, no toolchain here) that aren't visible from the
> source. Then **§1.2** for what actually has to work, and **§4's** open
> questions for what's undecided.
>
> This file is intended to be sufficient on its own: every council API below was
> verified against its live endpoint, with real URLs, parameters, response shapes
> and payload sizes recorded, so no re-research should be needed to implement.

## 1. What this is

A bin-day reminder light: a WS2812 LED (Seeed XIAO ESP32-C6) that lights up in a
colour matching which bin needs to go out, on a schedule, with a web UI to configure
it and (later) Matter smart-home commissioning.

### 1.1 Deployment context (drives prioritisation — read before re-ordering work)

This is **not a single device**. Target build cost is ~$10 each, and the plan is
to hand them out to friends and neighbours. The owner is happy to absorb
occasional council-API breakage and fix it at leisure (which is what makes the
bespoke Merri-bek backend, §3.13.3, an acceptable exception).

Two consequences that reclassify existing "stretch goals" as **prerequisites for
handing a device to anyone else**, rather than nice-to-haves:

1. **Runtime Wi-Fi provisioning (§3.4 AutoAP) is a hard blocker for
   distribution, not a stretch goal.** Wi-Fi credentials are currently
   compile-time Kconfig values. Every device given away would otherwise need
   a per-household rebuild-and-reflash — and would require the recipient to
   hand over their Wi-Fi password to be baked into a binary. AutoAP removes
   both problems and is the difference between "a gadget I can give someone"
   and "a gadget only I can install".
2. **OTA (§3.5) moves from optional to important.** With devices in other
   people's homes, a council-API break (expected, per above) otherwise means
   physically visiting each device with a USB cable. OTA turns a fleet-wide
   fix into a push. Its "where are images hosted / manual vs auto" open
   questions now matter more than when they were first written.

A third, smaller consequence: the **factory-reset button (§3.12)** and the
**boot self-test (§3.10)** are what let a non-technical recipient recover a
misconfigured device and confirm the hardware works, without a serial console.
Both are already specified; this is why they earn their place.

### 1.2 The critical working group (five councils — the definition of "done")

These five LGAs are where devices will **actually be deployed**. They are the
priority for implementation, and the acceptance criteria for the API feature:
**all five must work end to end before any other council coverage matters.**

| # | Council | Backend | Status | Detail |
|---|---|---|---|---|
| 1 | **Maribyrnong** (home) | Impact Apps | ✅ implemented, verified on hardware | §3.3 |
| 2 | **Merri-bek** | bespoke (ArcGIS + council API) | ✅ **implemented** — parser verified against live payload | §3.13.3 |
| 3 | **Knox** | bespoke (2 JSON calls) | ✅ **implemented** — parser verified against live payload | §3.13.4 |
| 4 | **Whitehorse** | bespoke (Weave GIS) | ✅ **implemented** — parser verified against live payload | §3.13.4 |
| 5 | **Monash** | OpenCities / MyArea | ✅ **implemented** — parser verified against live payload | §3.13.4 |

All five backends are written and their parsers pass against real captured
payloads (test/host/fixtures/, fetched live 2026-07-25 from the owner's
machine — see `test_backends.c`). What remains for the working group is
**on-device verification**: each council set up through the real `/api-setup`
flow on real hardware. See "backend abstraction — as built" in §3.13.5.

**Re-verification note (2026-07-25)**: before implementing the backends, all
four bespoke endpoints were re-probed. Knox and Whitehorse returned exactly the
recorded shapes. **Merri-bek did not** — its required `cpage` parameter had
changed value, its parameter names were recorded wrongly, and its date format
was recorded wrongly; see §3.13.3, which now carries the corrected, verified
facts. The lesson holds: recorded API research decays, and each backend should
be re-probed immediately before it is implemented rather than trusted from the
page.

All five are confirmed reachable and parseable on-device; none needs to fall
back to the manual schedule for want of a working backend. Four of the five
are **bespoke, single-council backends** — which is why the pluggable-backend
abstraction (§3.13.4, §3.13.3) is the central architectural task, not the
breadth of the supported-council list.

**Everything else in §3.13 is a bonus.** The two shared platforms (39 Impact
Apps councils, 46 South Australian ones) are cheap to add *because* they're
shared — they're worth including as easy wins for anyone else who ends up with
a device, but they must not displace or delay the five above. If a trade-off
arises, the working group wins.

**Testing implication**: these five are the regression set. A change to the
backend abstraction isn't finished until it's been exercised against all five
real endpoints, since between them they cover every awkward case found —
GUID vs numeric vs multi-field ids, four date formats, HTML-in-JSON vs plain
JSON, and no colour data at all.

## 2. Hardware (done)

- Seeed XIAO ESP32-C6, 4MB flash
- **Two** WS2812 LEDs, daisy-chained (LED1's data-out feeds LED2's data-in — a
  2-pixel chain on the single existing RMT/GPIO0 line, not a second GPIO), through
  a logic-level converter (3.3V → 5V) with a series resistor on the data line and a
  100nF cap across the LEDs' power pins. Physically wired; firmware still only
  drives pixel 0 — see §3.7 for the pending second-LED support work.
- Confirmed empirically: this specific LED batch expects **RGB** byte order on the
  wire, not the WS2812 datasheet-standard GRB — `LED_STRIP_COLOR_COMPONENT_FMT_RGB`
  in [led_state.c](main/led_state.c). Do not "correct" this back to GRB.
- **Enclosure**: transparent/natural PLA, printed. The LEDs are viewed through
  it, so colours are judged **diffused, not bare** — the palette is tuned
  against the real enclosure, and any future colour work should be evaluated
  the same way rather than by the raw RGB values looking correct.
- **Colour balance**: WS2812 green is substantially more luminous than red at
  equal duty, so a naive `(255,255,0)` yellow reads distinctly **green**
  (confirmed on hardware through the PLA cover). `COLOR_PRESETS` yellow is
  therefore `(255,150,0)`, green deliberately pulled down. This is a
  perceptual calibration, not a bug. Same class of empirical finding as the
  RGB byte order above: do not "correct" it back to a mathematically pure
  value.
  - **Red, green and purple are confirmed good** on hardware through the test
    cover — no further tuning expected for those.
  - **Yellow is provisional and deliberately not finalised.** It remains
    hard to get right, and testing showed it's materially affected by
    **viewing angle and diffraction through the cover**, not just by the RGB
    ratio — meaning it can't be settled against a bare test cover, because
    the final enclosure has different geometry and a divider between the two
    LEDs (§3.7). Re-tuning now would be re-work. **Deferred to final
    integration testing in the printed enclosure — see §5.**

## 3. Requirements

### 3.1 Core (implemented)

| Requirement | Where |
|---|---|
| Wi-Fi station connectivity | [wifi_manager.c](main/wifi_manager.c) |
| On-device web server, schedule UI | [web_server.c](main/web_server.c) |
| NVS-backed schedule persistence | [schedule.c](main/schedule.c) |
| NTP sync | [time_sync.c](main/time_sync.c) |
| Runtime timezone selection, web UI | [settings.c](main/settings.c) — see 3.2 |
| Single source-of-truth LED function | [led_state.c](main/led_state.c) |
| Fixed bin night + on/off window | [schedule.h](main/schedule.h) |
| Independent-frequency per-colour rotation | [schedule.h](main/schedule.h) — see 3.6 |
| External bin-collection API (overrides manual) | [waste_api.h](main/waste_api.h) — see 3.3 |
| Second LED — general-waste indicator + concurrent-collection colour | [led_state.c](main/led_state.c) — see 3.7 |
| On-demand "Display Next Collection" button, previews next scheduled colour(s) | [schedule.c](main/schedule.c) — see 3.8 |
| mDNS hostname advertisement (`binlight.local`) | [main.c](main/main.c) — see 3.9 |
| Boot-time LED self-test (both LEDs cycle colours) | [led_state.c](main/led_state.c) — see 3.10 |
| Preferences UI reorganisation + default brightness fix | [web_server.c](main/web_server.c) — see 3.11 |
| Physical buttons (factory reset, display-next / cancel) | **planned, not yet implemented** — see 3.12 |
| Multi-council coverage + per-council backends | **researched, not yet implemented** — see 3.13 |

### 3.2 Timezone selection in the Web UI (implemented)

Timezone was previously a fixed Kconfig default
(`AEST-10AEDT,M10.1.0/2,M4.1.0/3`), baked in at build time. It's now a runtime
setting, changeable from the web UI without reflashing (see [settings.c](main/settings.c)).

**Constraint**: ESP-IDF's newlib doesn't ship an IANA tzdata database — timezone
handling is POSIX `TZ` string based (fixed offset + DST rule), not IANA zone names
like `Australia/Melbourne`. So the UI can't just show a standard IANA picker.

**Approach**:
- A dropdown of common Australian zones with human labels, each mapping to its POSIX
  TZ string, e.g.:
  - "Melbourne/Sydney/Canberra/Hobart (AEST/AEDT)" → `AEST-10AEDT,M10.1.0/2,M4.1.0/3`
  - "Brisbane (AEST, no DST)" → `AEST-10`
  - "Adelaide/Darwin (ACST/ACDT)" → `ACST-9:30ACDT,M10.1.0/2,M4.1.0/3` (Darwin has no DST — separate entry needed, `ACST-9:30`)
  - "Perth (AWST, no DST)" → `AWST-8`
- Plus a "Custom" option accepting a raw POSIX TZ string, for anything outside that
  list.
- Persist the chosen string to NVS (new key, e.g. `tz_string` — reuse the
  `binlight` namespace or add a small `settings` namespace alongside `schedule`).
- On boot, load from NVS before calling `time_sync_start()`; falls back to the
  Kconfig default only if nothing is stored yet (first boot).
- Applying a change from the web UI calls `setenv("TZ", ...); tzset();` immediately
  so the schedule evaluator picks it up on its next tick — no reboot needed.

### 3.3 External API for bin-collection schedule (implemented)

**Decision** (confirmed and built): when the API is enabled and reachable, it
fully replaces the manual schedule as the source of truth for both night and
colour — including turning the light off on weeks it reports nothing due. The
manual schedule (§3.6) is used only when the API is disabled, never configured,
or unreachable.

**Source, confirmed live** by extracting the referenced Apple Shortcut ("Which
Bin?")'s binary plist and cross-checking against the public
`impact-apps-calendars.web.app` frontend's JS bundles, then **calling the real
endpoints directly with curl** to confirm every field name and shape (no more
guessing from minified JS):

- **"Impact Apps"** platform (impactapps.com.au), white-label, one instance per
  council on a subdomain of `waste-info.com.au`. Public, unauthenticated.
- Base: `https://<council>.waste-info.com.au/api/v1` — confirmed council:
  `maribyrnong`.

**Confirmed endpoints** (all verified with real responses):

| Endpoint | Returns |
|---|---|
| `GET /localities.json` | `{"localities":[{"id","name","postcode","council"}]}` |
| `GET /streets.json?locality=<id>` | `{"streets":[{"id","name","locality"}]}` |
| `GET /properties.json?street=<id>` | `{"properties":[{"id","name","zone","voucher_preferences"}]}` |
| `GET /properties/<id>.json?start=<ISO>&end=<ISO>` | A **bare JSON array** of events. First element is always the recurring general-waste rule (nested `property` object, `dow`/`start_date`, `event_type:"waste"`, no plain `start`). All other elements are one-off instances with `start` (YYYY-MM-DD), `event_type`, and — critically — their **own `color`** as a `"#RRGGBB"` hex string chosen by the council. No event-type→colour mapping table needed, for this council or any other on the platform. |

`event_type` enum (platform-wide, per the frontend's TypeScript source):
`waste`, `recycle`, `organic`, `paper`, `food`, `clean_up`, `hard_waste`,
`greenwaste`. Maribyrnong's real responses only ever showed `waste`/`organic`/
`recycle`. Per the device owner's own long-used, correct Apple Shortcut logic:
`event_type == "waste"` (general bins, weekly) is excluded — it needs no special
reminder — and any other type present is treated as a reminder-worthy event.

**Implementation** ([waste_api.h](main/waste_api.h)/[waste_api.c](main/waste_api.c)):
- Config (persisted, NVS-backed): `enabled`, `council_subdomain` (free text — any
  council on this platform works by changing this one field), `property_id`,
  `property_label` (display only). **Flexibility scope, deliberate**: this covers
  "any council on the identical Impact Apps platform" with zero code changes. A
  genuinely different vendor's API is out of scope — there's no second real
  example to design an abstraction against, and the public API surface
  (`waste_api_get_current()`, config get/set) is already vendor-agnostic, so a
  future second backend would only mean rewriting this file's internals.
- ~~`waste_api_get_current()` returns a **three-way result**
  (`WASTE_API_RESULT_UNAVAILABLE` / `_NO_EVENT` / `_EVENT`) rather than a bool —
  this is what makes "API says nothing due" correctly *not* fall back to the
  manual schedule, while genuine unavailability (disabled/never-fetched/stale)
  does.~~ **Superseded, see "Next-collection model, redesigned" below** — this
  tri-state design is being replaced by a sticky, always-available "next
  collection" concept instead.
- ~~**No cache persistence to NVS** — only config is persisted. The poll task
  fetches immediately on start, so a reboot only means a few seconds of
  correctly-reported `UNAVAILABLE` before the first fetch lands.~~
  **Superseded** — the cache is now persisted (see below); a bare few-second
  gap after reboot is no longer an acceptable "unknown" window under the
  stronger guarantee §3.8 now makes.
- Poll every 12h, fetch a ~~13-day~~ **30-day**-ahead window (widened for
  margin under the new sticky-cache model below — a wider window costs a
  little more bandwidth per poll, not reliability). HTTPS via `esp_http_client` +
  `esp_crt_bundle_attach` (validates against ESP-IDF's bundled CA roots, no
  project-specific PEM needed — `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` was already
  on in this project's sdkconfig). JSON via the `espressif/cjson` managed
  component — **IDF v6.0 removed the old built-in `json` component**, this is
  its replacement.
- Setup UI: a new `GET /api-setup` handler, a stateless no-JS locality → street
  → property wizard driven entirely by query-string links. The device proxies
  every lookup call itself — the browser only ever talks to the device,
  sidestepping any third-party CORS policy entirely. Saving the final selection
  via a GET link (not a POST) is a deliberate simplification given the
  single-user LAN context, not an oversight.
- The manual schedule's `start_minute`/`duration_hours` fields (§3.6) are reused
  as-is for *when* the light turns on and for how long on an API-driven night —
  the API only decides *which* night and *what colour*; there's no separate,
  redundant on/off-window config for API mode.
- **Per-event-type mapping, discovered rather than guessed**: `waste_api_config_t`
  holds up to 8 `{event_type, ignored, color}` rules. A `GET /api-test` page
  fetches the next 4 weeks *raw* (every type the API returns, including
  `waste`, with the API's own colour, unfiltered) so the user can see real data
  for their address, then `POST /api-test` saves a mapping built from exactly
  those types — no hardcoded enum, no guessing what a given council actually
  emits. A type with no explicit rule yet falls back to the original default
  (`waste` ignored, everything else shown with the API's own colour), so a
  fresh setup still behaves sensibly before anyone visits the mapping page.
  This same raw/mapped split let the hardcoded `event_type == "waste"` skip
  move out of the shared fetch path entirely — `fetch_and_parse_events()` is
  now truly raw, and `apply_type_rules()` (used only by the real poll, not the
  diagnostics fetch) is where filtering actually happens.
- **Mapping is auto-populated at setup time, not left for the user to build
  manually**: completing `/api-setup` (choosing a property) now triggers an
  immediate `waste_api_fetch_upcoming()` for that property (4-week lookahead),
  builds one `type_rules` entry per distinct type seen (`waste` defaulted to
  ignored, everything else defaulted to whichever of the 4 presets is closest
  to the API's own colour by squared RGB distance — `nearest_preset_color()`
  in [web_server.c](main/web_server.c)), and saves it as part of the same
  `waste_api_set_config()` call that also wakes the poll task to fetch
  immediately. In practice this means finishing setup is usually enough on its
  own — no separate manual mapping step needed for the common case where a
  council's colours already roughly match red/green/yellow/purple.
- **The mapping lives on the home page, not just `/api-test`**: `/` now shows
  the saved `type_rules` as an editable table (ignore checkbox + colour
  dropdown per type) via a shared `append_type_mapping_form()` helper, reading
  directly from saved config — no live fetch, so the home page stays fast.
  Both the home page's form and `/api-test`'s own (discovery) form post to the
  same `POST /api-test` handler, which now honours a `redirect_to` hidden
  field (constrained to `/` or `/api-test`) so editing from either page
  returns you to that same page rather than always bouncing to `/api-test`.
  `/api-test` remains the place to (re)discover types that didn't appear in
  the original setup-time fetch — e.g. an infrequent `hard_waste`/`clean_up`
  event showing up later.

*(Method note: the shortcut's `shortcut.wflow` is a plain downloadable binary
plist — Apple's iCloud share page doesn't render the action list, but
`plutil -convert json` on the raw asset does. Cross-checked against the
`impact-apps-calendars.web.app/maribyrnong/waste-info/` frontend's JS bundles,
then every field/endpoint was re-verified against live `curl` responses before
locking in the design — the frontend bundle reading alone had gotten the
`start`/`start_date` distinction and the per-event `color` field wrong initially.)*

**Next-collection model, redesigned (implemented)** — found
during real-hardware testing of §3.8's Test button: the old design (a poll
cache that goes to `NO_EVENT`/`UNAVAILABLE` whenever a given poll's fixed
13-day window or 30h freshness timer didn't happen to line up) could report
"nothing known" even though a real future collection genuinely exists — the
timer/window just hadn't caught it yet. The actual, stronger requirement is:
**the device should always know the next collection**, recovered from a
persisted cache or derived from the manual schedule, and the *only*
acceptable "unknown" state is the API's cached date having actually passed
(gone stale) *and* the manual/fallback schedule (§3.6) also being disabled.
Resolved design:
- **Sticky cache, staleness redefined by date, not by a timer.** A poll that
  finds a qualifying event always overwrites the cache (it's authoritative).
  A poll that finds *nothing* leaves the existing cache untouched — it does
  **not** regress to "unknown" — unless the cached date has already passed,
  in which case there's genuinely nothing newer known yet. This directly
  replaces the old `FRESHNESS_SECONDS`/"stale after 30h" mechanism: a
  network hiccup or a briefly-too-narrow window no longer discards a
  perfectly valid future date, only the date itself passing does.
- **Cache persisted to NVS now** (new key, e.g. `waste_api_cache_v1`, separate
  from `waste_api_config_v2` since it's cache, not user config) — the earlier
  "deliberately not persisted, a reboot means a few seconds of correctly
  reported UNAVAILABLE" decision (§3.3, above) is superseded: a few seconds of
  "unknown" is no longer an acceptable state under this stronger guarantee,
  and persisting is cheap.
- **The recurring general-waste weekday (`dow`) is a second, independent
  signal**, not folded into the same staleness model — a weekly recurrence
  doesn't "expire" the way a dated cache entry does. It's still needed for
  dual-colour mode's "light on with just red on a plain waste week" behaviour
  (§3.7) even when no rotating item is imminent.
- **One unified resolver, used by both the live evaluator and "Display Next
  Collection"** (renamed in §3.8) instead of two independently-evolving code
  paths — which is exactly how the Test-button bug happened in the first
  place (the live evaluator's dual-mode logic and the preview logic diverged
  on how they used the waste weekday). New `schedule_get_next_collection()`
  in `schedule.c`:
  1. Next known rotating event (from the sticky cache above), if not stale.
  2. Else/also: next plain general-waste date (from the recurring weekday) —
     whichever of (1)/(2) is **sooner** wins; on a tie, (1) wins since it's
     more specific. If (1) wins, secondary (dual mode) is the second rotating
     colour if one shares that date, else `secondary_default_color`. If (2)
     wins (or (1) is unknown), primary **and** secondary are both
     `secondary_default_color` (general waste).
  3. Only if **neither** (1) nor (2) can be answered (API disabled, or the
     cache is stale and `dow` was never learned): fall back to the manual /
     fallback schedule's (§3.6) next upcoming occurrence, if it's enabled.
  4. Only if that's *also* disabled: genuinely unknown — the one allowed case.
  This replaces `waste_api_get_current()`'s three-way-result design (struck
  through above) with two narrower, clearer `waste_api.h` functions instead:
  `waste_api_get_next_event(...)` (the sticky cache getter) and
  `waste_api_get_next_waste_date(...)` (the weekday-recurrence getter) — the
  three-way enum doesn't map cleanly onto "always-available" semantics, so
  it's being retired rather than patched again.
- **The live evaluator simplifies as a result**: `schedule_task_fn()` calls
  `schedule_get_next_collection()` once per tick and just checks whether
  tonight is that result's date's eve (`is_window_active_for_date()`,
  unchanged) — replacing the previous separate EVENT/NO_EVENT/UNAVAILABLE
  branches with one shared code path that "Display Next Collection" also
  uses, so they can no longer drift apart the way they just did.

**As built** — the design above landed essentially as written, with these
clarifications and corrections found while implementing and testing it:

- **Naming**: the two replacement getters are `waste_api_get_next_event()`
  (returning a `waste_api_next_event_t`) and `waste_api_get_waste_weekday()`
  (kept, not renamed to `..._next_waste_date` — it returns a weekday, and
  turning that into a date is the resolver's job, not the API layer's).
  `waste_api_result_t` / `waste_api_slot_t` / `waste_api_get_current()` are
  gone.
- **`schedule_next_t` carries a `waste_only` flag.** The resolver returns the
  *nearest* occasion, and whether the light should actually turn on for it is
  the caller's decision, not the resolver's: general waste is weekly and needs
  no reminder of its own, so the live evaluator lights a waste-only night in
  dual-colour mode only (where LED2 *is* the general-waste indicator), while
  "Display Next Collection" shows it regardless — it answers "what's next?",
  not "should the light be on?". Keeping this one flag out of the resolver's
  own logic is what lets a single code path serve both callers without
  reintroducing mode-dependence inside it.
- **Everything is now keyed on the collection date**, including the manual
  schedule (whose `bin_night_weekday` is converted to a collection weekday by
  adding a day) and the API's recurring waste weekday. That's what allows
  `is_window_active_for_date()` to be the *only* window check left — the
  weekday-keyed `is_window_active()` / `is_window_active_for_weekday()` pair
  is deleted. It also fixed two off-by-ones, below.
- **Off-by-one #1, fixed: the recurring waste weekday was treated as bin
  night.** The API reports it the same way it reports dated events — as the
  *collection* day — so lighting up on that weekday's evening was a day late.
  Now its eve is used, consistently with dated events.
- **Off-by-one #2, fixed: a manual rule never fired on its own first
  collection.** `rule_due()` was evaluated against bin night while the UI
  field is labelled "First collection", so entering the collection date made
  `days_between()` return −1 on that first occurrence and the rule was skipped
  until the following cycle. Rules are now evaluated against the collection
  date, matching the label.
- **ISO vs `tm_wday`, fixed**: the API's `dow` is ISO-8601 (Mon=1..Sun=7),
  converted on the way in so callers only ever see `tm_wday` (Sun=0..Sat=6).
  These agree for Mon–Sat and differ only on Sunday, which is why
  Maribyrnong's Friday `dow:[5]` always worked and hid it; a Sunday-collection
  council would have produced an out-of-range 7.
- **`days_to_next_collection()`** handles the one genuinely subtle case: when
  *today* is a collection day, its window opened last night, so it is either
  still running (a window that wraps past midnight) or already over. While
  running, today is still the right answer; once it closes, the answer must
  roll to next week rather than stranding on a date that's been and gone.
- **Lookahead widened 13 → 30 days** and the poll event buffer 8 → 16, per the
  design note above.

**DST day-counting bug, found by the host tests and fixed** — this one
predates the rework and would have misfired twice a year. `days_between()`
normalises both dates to local noon so a DST change can't flip the calendar
date, but the two noons are then **23 or 25 hours apart**, not 24 — and C's
integer division truncates *toward zero*, so a −23h gap came out as `0` days
instead of `−1`. Concretely: for a collection falling on the DST-start Sunday
(first Sunday of October in Melbourne), `day_diff` on the Saturday evening
evaluated to 0 instead of −1, so `is_window_active_for_date()` returned false
and **the light simply never came on that night**. Fixed by rounding to the
nearest day (`(secs ± 43200) / 86400`, with the sign handled explicitly since
truncation is asymmetric for negatives) in both `schedule.c` and
`waste_api.c`. Pinned by the `DST boundaries (Melbourne)` cases in
[test/host/test_resolver.c](test/host/test_resolver.c) — it is not reachable
by inspection or by testing on any ordinary date, which is precisely why it
survived this long.

**Host tests** ([test/host/](test/host/)) — added with this rework, since it is
almost entirely date arithmetic and the failure mode is a light that is silently
wrong on one night months from now. `test/host/run.sh` needs no ESP-IDF, no
device and no toolchain setup, just `cc`: it compiles the **real** `schedule.c`
against thin stubs, with `time(NULL)` redirected to a settable fake clock via a
macro installed before the include, and drives 28 assertions over concrete
dates — weekday wraparound, the past-midnight window, staleness, the
event-vs-waste tie-break, the manual fallback's cycles, and both DST
boundaries. `./run.sh render` additionally dumps the home page's real bytes
(see §3.11). Two real bugs were caught by writing these, one of them the DST
bug above.

**TLS trust bug hit during bring-up, fixed** — connecting to
`*.waste-info.com.au` failed with `esp-x509-crt-bundle: No matching trusted
root certificate found`. Root cause, confirmed by comparing certificate
fingerprints and public keys directly (not guessed): the server's chain
terminates in a **cross-signed** "GTS Root R4" (Google Trust Services'
root, itself signed by the old standalone "GlobalSign Root CA" for
backward compatibility) rather than the self-signed version. ESP-IDF's
bundled trust store (`cacrt_all.pem`) has the self-signed GTS Root R4
(confirmed identical public key) but not the old GlobalSign root that
cross-signs it, so mbedtls's default chain-building tries to walk up to
that missing issuer instead of recognising GTS Root R4 as already-trusted
and stopping there. Fix: enabled `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`
(and its dependency `CONFIG_MBEDTLS_X509_TRUSTED_CERT_CALLBACK`) in
`sdkconfig` — an ESP-IDF feature built for exactly this case, ~700 bytes of
extra heap, disabled by default. Not a bug in this project's code; a real
gap in ESP-IDF's default TLS config for a legitimate (if unusually
configured) certificate chain. See §6 for the full list of build/runtime
issues hit and fixed this session.

### 3.4 Wi-Fi setup without Matter ("AutoAP" mode, implemented)

**Reclassified from "stretch goal" to prerequisite for giving a device to
anyone else — see §1.1.** Currently Wi-Fi credentials are Kconfig-only
(compiled in, changing them means reflashing) — a deliberate simplification
made early on, when this was a single device on the author's own network. That
simplification stops working the moment a second household is involved: it
would mean a per-recipient rebuild, and asking them to hand over their Wi-Fi
password to be compiled into a binary. Replace it with runtime provisioning
that doesn't depend on Matter at all:

- ESP-IDF ships a `wifi_provisioning` component supporting either **BLE** or
  **SoftAP** transport, fully independent of Matter/Thread.
- **SoftAP** is the simpler option here: on first boot (or when no credentials are
  stored, or repeated connection attempts fail), the device starts its own AP —
  **"AutoAP" mode** — and serves a small page for picking/entering the home
  Wi-Fi network — this can reuse the `esp_http_server` pattern already built
  for the schedule UI — then switches to station mode once provisioned. No
  companion app needed, just a browser.
- **BLE** transport needs a companion app (Espressif's own provisioning app, or a
  custom one) — more moving parts, no clear benefit over SoftAP for a single
  device.
- Recommendation if/when this gets picked up: SoftAP-based `wifi_provisioning`,
  replacing the Kconfig SSID/password as the only credential path.

**AutoAP naming and LED indication (new decisions):**
- **SSID**: `binlight` + the last 4 hex digits of the device's Wi-Fi station
  MAC address (e.g. `binlight-3A2F`) — unique enough to tell multiple devices
  apart on a network scan without needing any user-entered name, read via
  `esp_wifi_get_mac(WIFI_IF_STA, ...)` at AP-start time.
- **LED indicator**: while in AutoAP mode, both LEDs show a slow, calm
  **breathing white glow** — brightness oscillating smoothly up and down
  (not a hard blink) — signalling "waiting for setup input" without being
  alarming. Both LEDs always move together here (this is a device-state
  indicator, not a bin-colour display, so `light_mode`/§3.7 don't apply).
  Implementation sketch: a dedicated background task calling
  `led_state_set_dual(white, white, b)` on a short tick (e.g. every 30-50ms),
  with `b` following a triangle or sine ramp between a low and high bound
  over a multi-second period — started when AutoAP mode begins, stopped
  (LEDs handed back to `schedule_task_fn`) once provisioning succeeds and the
  device reconnects in station mode.

**As built (2026-07-26)** — in [wifi_manager.c](main/wifi_manager.c), with the
breathing indicator in [led_state.c](main/led_state.c). Deviations from the
design above, all deliberate:

- **Hand-rolled SoftAP provisioning, not the `wifi_provisioning` component.**
  That component's SoftAP transport speaks a protobuf/`protocomm` protocol
  designed for Espressif's phone apps — it does *not* serve an HTML page, so
  "no companion app, just a browser" would have meant shipping a companion app
  after all. The device already has `esp_http_server` and a no-JS UI
  convention, so provisioning is 2 handlers reusing that. Fewer dependencies,
  and it's the browser flow §3.4 actually asked for.
- **Credentials are verified before they are persisted.** The POST handler
  applies them, calls `esp_wifi_connect()` and **blocks on the outcome** (20s
  cap), so the HTTP response *is* the result — "Connected" or "Couldn't join"
  on the same page, no JavaScript and no polling. A typo never reaches NVS,
  so a mistyped password can't strand the device.
- **APSTA during the attempt**, so the phone keeps its connection to the setup
  AP while the station side joins the real network. The AP is torn down ~4s
  *after* success, so the confirmation page reaches the browser first.
- **AutoAP is also the recovery path, not just first boot.** If stored
  credentials exist but can't be used at boot, the device opens AutoAP *and*
  retries the stored network every 60s in the background. One mechanism covers
  both real cases: the router was merely slow or rebooting (it reconnects
  itself and AutoAP closes), or the network is genuinely gone (the user
  provisions the new one).
- **Kconfig credentials are retained as a development fallback**, not removed.
  Priority is NVS → Kconfig → AutoAP. The committed default is blank, so a
  fresh device goes straight to setup mode — the behaviour that matters for
  handing devices out — while the author's bench device can still have its
  network compiled in.
- **`POST /wifi-forget`** on the main UI erases the stored credentials and
  reboots into AutoAP: the deliberate path for moving a device to a different
  network while the old one still works (e.g. handing it to someone else).
  Schedule and council config are untouched. A software-only partial stand-in
  for §3.12's factory-reset button.
- **Scanned SSIDs are HTML-escaped.** They are attacker-controlled bytes —
  anyone in range can broadcast an SSID containing markup at a device sitting
  in setup mode — and they land in both text and attribute contexts.
- **The setup AP is open (no password)**: the standard consumer-IoT trade-off,
  since a WPA password would have to be printed on the device to be usable and
  the AP exists only for the minutes provisioning takes.
- **Breathing is a triangle ramp** (brightness 8→160 over 3s, 40ms ticks) in
  its own small task rather than a sine — smooth enough at that tick rate
  without floating point or a lookup table. It never reaches zero, since fully
  dark would read as an idle device rather than one waiting for input.

**Not yet verified on hardware.** The whole flow — AP visibility, the setup
page, a real join, the reboot path — needs a device. Note that flashing this
onto a device whose Wi-Fi is currently compiled into `sdkconfig` will keep
working via the Kconfig fallback, so *testing* AutoAP means either blanking
the Kconfig SSID or using "Forget this network" from the UI.

### 3.5 Over-the-air (OTA) firmware updates

**Priority raised — see §1.1**: with devices deployed to other households, an
expected council-API break otherwise means physically visiting each one with a
USB cable. The open questions at the end of this section (hosting, manual vs
auto-check, partition sizing) are now worth resolving properly rather than
deferring.

**Confirmed available**: ESP-IDF v6.0.2 ships both `app_update` (OTA partition
management, rollback) and `esp_https_ota` (HTTPS-based image download + flash) —
checked their headers exist in the local toolchain.

**Partition table collision with the current design**: OTA needs at least two app
slots (`ota_0`, `ota_1`) plus an `otadata` partition (tracks which slot is active/
valid). A single `factory` app partition — which is what [partitions.csv](partitions.csv)
currently defines — **cannot receive an OTA update at all**: there's no second slot
to write a new image into while the running one keeps serving. This needs a
partition table rework, not just an addition:
- ESP-IDF's stock two-OTA reference layout (`partitions_two_ota.csv`): `nvs` 16KB,
  `otadata` 8KB, `phy_init` 4KB, `ota_0` 1MB, `ota_1` 1MB — no `factory` slot at all
  in the pure-OTA scheme.
- On our 4MB flash, two OTA slots would each need to be smaller than the current
  single 3MB factory partition to leave room for the same nvs/otadata/phy_init
  allocations — e.g. roughly 1.5MB each, **not yet verified against a real build**.
  Current image size (before any HTTPS/OTA code is added, which will grow it
  further) was estimated at 900KB–1.3MB when the partition table was first sized —
  needs re-measuring once HTTPS is wired in, not assumed.
- This changes the on-flash layout already built this session (§3.1), so adopting
  it means a re-flash (`idf.py erase-flash` or similar) — not a drop-in addition
  alongside the existing table.

**HTTPS requirement**: `esp_https_ota` expects an HTTPS URL by default. It can be
forced to accept plain HTTP by skipping certificate validation, but that means
firmware images arrive unauthenticated — not recommended even for a hobby device,
since it means anything that can spoof the update server can run arbitrary code on
it. This is the same "no TLS/mbedtls wired up yet" gap already flagged for the
external bin-collection API (§3.3) — worth building that plumbing once and sharing
it between both features rather than twice.

**Rollback safety** (confirmed API in `app_update`):
`esp_ota_mark_app_valid_cancel_rollback()` / `esp_ota_mark_app_invalid_rollback()`.
Standard pattern: after flashing a new image and rebooting into it, the new
firmware must explicitly confirm it's healthy (e.g. once Wi-Fi connects and the web
server starts) or ESP-IDF automatically rolls back to the previous slot on the next
boot. Worth having from day one — this is a physical fixture that isn't always
convenient to walk up to with a USB cable, so a botched update shouldn't strand it.

**Trigger mechanism — open question**:
- **Manual, web-UI-triggered** (recommended starting point): a field in the web UI
  to point at a firmware binary URL and kick off `esp_https_ota` on demand. Simplest
  and safest, matches the low-traffic single-user nature of everything else built
  so far — no unattended auto-update risk.
- **Periodic auto-check**: device periodically polls a version endpoint and either
  notifies or auto-installs. Bigger lift (needs a version-comparison scheme and a
  machine-readable "latest version" manifest, not just a raw binary) — not
  recommended as a starting point.

**Open questions to resolve before implementing** (flagging rather than guessing):
1. Where do OTA images get hosted — a private server, GitHub Releases (this project
   isn't currently a git repo, so that's a prerequisite if chosen), or something
   else?
2. Manual-trigger, auto-check, or both (starting with manual)?
3. Real partition sizes once image size is actually measured with HTTPS/OTA code
   included — don't lock in 1.5MB/1.5MB without checking.
4. Keep a `factory` fallback slot (dual-boot factory + single OTA slot, trading
   capacity for a guaranteed-good recovery image) vs. the simpler stock two-OTA-only
   layout?

### 3.6 Alternating multi-week manual / fallback schedule (implemented; rename + reframing planned)

**Requirement**: the bin night (the evening bins go out — not the collection day
itself) is always the same single weekday, and that night's colour cycles through
up to 3 variations on a repeating weekly rotation — e.g. Week 1 = Yellow, Week 2 =
Green, Week 3 = Purple, then back to Week 1. This matches real bin-collection
patterns: bin night is fixed, but which bin (general/recycling/organics) goes out
rotates week-to-week. The schedule is referenced on **bin night**, not the
following collection morning — that's the field the user actually thinks in terms
of and sets directly.

This replaced the earlier per-weekday (7 independent day-entries) schedule model
entirely — it turned out to be the wrong shape for how bin collection actually
works. The new model is simpler, not just different: one bin night, one on/off
window (expressed as a start time + a duration in hours, not two absolute times),
one rotating colour list.

**Resolved design decisions**:
- **One shared bin night**, not per-day config — a single `bin_night_weekday`
  dropdown (Sunday–Saturday), labelled "Bin night" in the UI.
- **On-window as start time + duration**, not two absolute times — "On from" (a
  time picker) plus "Turn off after N hours" (a number input, 1-23). Simpler and
  less error-prone than requiring the user to pick a correct wrap-past-midnight
  end time by hand; the device computes the actual end time and wrap itself.
  Default: on at 3:00pm, off after 20 hours.
- **Each colour has its own independent first-collection date and repeat
  frequency (1-4 weeks)** — superseded a first attempt at this (a single shared
  rotation length + one shared anchor date for all colours) once real councils
  turned out to run genuinely independent, non-aligned cycles per bin type (e.g.
  recycling every 2 weeks from one date, organics every 3 weeks from a different
  date) that a single shared cycle can't express. Up to 3 colour rules, each
  independently enabled/disabled, dated, and timed.
- **Colour set**: reuses the same 4-preset dropdown (Red/Green/Yellow/Purple)
  once per colour rule.
- **Tie-break**: if more than one colour rule is due the same week, the first
  one (in Colour 1/2/3 order) wins — consistent with the "first match wins"
  pattern already used elsewhere (e.g. TZ preset matching).

**Implementation** ([schedule.h](main/schedule.h), [schedule.c](main/schedule.c),
[web_server.c](main/web_server.c)):
- `schedule_t` holds: `enabled`, `bin_night_weekday` (0=Sunday..6=Saturday),
  `start_minute` (on-window start, minutes since midnight on bin night),
  `duration_hours` (1-23; wraps into the following day if it crosses midnight),
  `brightness`, and `rules[3]` of `schedule_color_rule_t { enabled, color,
  first_year/month/day, frequency_weeks }`.
- A rule is "due this week" via `mktime()`-based day-diffing (both dates
  normalized to noon to dodge DST-boundary off-by-one-hour edge cases):
  `days_since_first = (today - first_date) / 86400`; due if
  `days_since_first >= 0 && (days_since_first / 7) % frequency_weeks == 0`. A
  rule whose first date hasn't arrived yet is never due.
- If **no** rule is due, the light stays off that night even though the
  bin-night/time-window would otherwise say "on" — a due colour is a
  precondition for lighting up now, not just the time window.
- **NVS schema version bumped to 4** (`SCHEDULE_STRUCT_VERSION`/new key
  `schedule_v4`) — incompatible with older layouts, resets to disabled defaults
  on first boot after this update, per the version/size-check fallback already
  built into `schedule_init()`. One-time reset, expected and already accounted
  for (this is the second such reset this project has had — each schema change
  so far has been while actively iterating, not against real deployed data).
- **Relationship to the external API (§3.3)**: the API is the source of truth
  for night+colour whenever it's enabled and reachable; this manual rule set is
  the fallback for when it's disabled, unconfigured, or unreachable.

**Rename + reframing (implemented)**: renamed in the web UI
from "Manual schedule" to **"Manual / Fallback Schedule"**, with its
explanatory text updated to state plainly that it's optional and only takes
over when the API can't be reached *or* its cached data is stale (a
collection date in the past) — matching the precise staleness definition
from §3.3's next-collection redesign, rather than the vaguer "disabled,
unconfigured, or unreachable" wording above. This is a documentation/UI
wording change plus the checkbox-gated collapsible layout from §3.11 — the
underlying fallback *behaviour* here is already correct and unchanged.

### 3.7 Second LED — general-waste indicator + concurrent-collection colour (implemented)

**Hardware change** (physically done, see §2): a second WS2812 LED has been added,
daisy-chained off the first LED's data-out pin — a 2-pixel chain on the same
GPIO/RMT line, not a second GPIO. Firmware currently only drives pixel 0
(`LED_COUNT` is still 1 in [led_state.c](main/led_state.c)); this section is the
design for making full use of both pixels.

**Requirement**: the finished enclosure has a physical divider between the two
LEDs and is meant to always light up **fully** — both LEDs illuminate together
whenever the light is on, never one lit while the other stays dark. What varies
is a **"single colour" vs "dual colour" mode**: in single-colour mode both LEDs
show the same colour (today's effective behaviour, just now driven onto two
physical pixels instead of one); in dual-colour mode they show two independent
colours — LED2 defaulting to a "general waste is also going out tonight"
reminder (default **Red**), but deferring to a second real bin-type colour
instead whenever the council's data says two non-waste collections land on the
same night (e.g. recycling **and** glass on the same day) — in that case
general waste shouldn't hog the second slot, since the point of LED2 becomes
"what's the *other* thing going out", which now has a real answer.

**Default colour correlations** (derived from this requirement's own examples,
plus the FOGO/organics case already covered by the existing 4-colour preset set —
no new preset colour needed, `COLOR_PRESETS` in
[web_server.c](main/web_server.c) already has exactly these four):

| Bin / API `event_type` | Default colour |
|---|---|
| General waste (`waste`) | **Red** |
| Recycling (`recycle`) | **Yellow** |
| Organics / garden / food organics / FOGO (`organic`, `greenwaste`) | **Green** |
| Glass (`glass`) | **Purple** |
| Anything else (`paper`, `food`, `clean_up`, `hard_waste`, or an unrecognised string) | existing fallback: nearest preset to the API's own reported colour, via `nearest_preset_color()` |

This name-keyed table is new — today's auto-populate logic (§3.3) only ever
matches by *nearest RGB distance to the API's own colour*, which happens to get
these four right already for Maribyrnong's actual colours but does so by
coincidence, not by design, and would misfire for a council whose `glass` event
happens to be some colour numerically closer to green than purple. The name-keyed
table should be tried **first** when auto-seeding `type_rules` at `/api-setup`
time; nearest-RGB-distance remains the fallback for names not in the table.

**Resolved design decisions:**

- **One global setting, `light_mode`** (`LIGHT_MODE_SINGLE_COLOUR` /
  `LIGHT_MODE_DUAL_COLOUR` — named and exposed in the UI as "Single colour" /
  "Dual colour", not an "enable second LED" toggle, since the second LED is
  always physically illuminated, in either mode), plus one global
  `secondary_default_color` (default Red, only meaningful in dual-colour mode)
  — not per-rule, not per-colour. Lives in `schedule_t` alongside `brightness`,
  since it's a light-wide behaviour setting that applies the same way
  regardless of manual vs. API mode. `SCHEDULE_STRUCT_VERSION` → 5 (another
  one-time NVS reset, consistent with every prior schema change this session).
- **Both LEDs always illuminate together.** Whenever the light is in its
  on-window, both pixels are lit — `light_mode` only decides whether they show
  the identical colour or two independent colours; it never controls whether
  LED2 is lit at all. Nights with nothing due still turn **both** LEDs fully
  off together, exactly as today's single-LED behaviour did.
- **Single-colour mode (default) = zero behaviour change** beyond now driving
  the same computed colour onto both physical pixels instead of just one. All
  of the below (waste-default fallback, concurrent-collection detection, the
  "always something to show on bin night" behaviour) only takes effect in
  dual-colour mode.
- **`led_state.c` gains a dual-pixel path**: `LED_COUNT` → 2. Proposed API:
  `led_state_set_dual(led_color_t primary, led_color_t secondary, uint8_t brightness)`
  sets both pixels independently; `led_state_set(color, brightness)` becomes a
  thin wrapper calling `led_state_set_dual(color, color, brightness)` — no
  caller outside `led_state.c` needs to change for the mirror case.
- **API-mode evaluator** (`waste_api.c`): today `waste_api_get_current()` only
  ever surfaces one colour (the earliest upcoming non-`waste` event), and the
  recurring general-waste rule (the events array's element 0, `dow`/`start_date`,
  no `start`) is used only to *exclude* the literal string `"waste"` from the
  other-events list — its own recurrence has never actually been evaluated.
  Two changes needed:
  1. Actually evaluate that recurring rule's weekly `dow` against a target date,
     so "is tonight a general-waste night" has a real answer even in weeks with
     no other event at all (needed so LED2's waste reminder can light up on a
     plain waste-only week, not just weeks that also have a rotating item).
  2. When more than one qualifying non-waste event shares the same earliest
     date, surface up to two (primary = first in the API's own array order,
     secondary = second), instead of only ever returning one.
  Proposed replacement signature:
  ```c
  typedef struct { bool due; schedule_color_t color; } waste_api_slot_t;
  waste_api_result_t waste_api_get_current(waste_api_slot_t *out_primary, waste_api_slot_t *out_secondary,
                                            uint16_t *out_year, uint8_t *out_month, uint8_t *out_day);
  ```
  `out_secondary->due == false` when only one (or zero) non-waste event applies
  that date — `schedule.c` then falls back to `secondary_default_color` for LED2
  if the recurring waste rule confirms tonight is also a waste night, else LED2
  stays off. This is a C-API signature change only, not a persisted-struct
  change — `waste_api_config_t` itself doesn't change shape, so no
  `WASTE_API_STRUCT_VERSION` bump needed for this part.
- **Manual-mode evaluator** (`schedule.c`): symmetric treatment for consistency.
  `find_due_rule()`'s "first due rule wins, else nothing" becomes "find up to
  two due rules, in array order" when `light_mode == LIGHT_MODE_DUAL_COLOUR`.
  If only one rule is due, LED2 shows `secondary_default_color` instead of
  nothing; if two rules are due the same week (e.g. a 2-week and a 3-week cycle
  landing together), LED1/LED2 show both real due colours and the default is
  not used. Also: **the light now turns on every bin night in dual-colour
  mode**, even weeks where zero rules are due — mirrors the API-mode "waste
  always happens" behaviour, since in dual-colour mode there's always at least
  the `secondary_default_color` reminder to show on LED2. In single-colour
  mode, "nothing due → fully off" is unchanged.
- **Web UI**: two new fields on the home page's existing settings form (folded
  into the current `POST /save` handler, no new endpoint) — a "Light mode"
  dropdown (Single colour / Dual colour) and a "Second LED default colour"
  dropdown (reuses `COLOR_PRESETS`, default Red, only shown/relevant in dual
  mode) — placed near the existing brightness field since it's the same kind
  of light-wide setting.

**Open question, resolved before coding** — re-fetched the real Maribyrnong API
live (`curl` against `properties/2855360.json` for a 30-day window) rather than
assuming the shape still held: element 0 does carry a recurring rule shaped
exactly as expected — `{"dow":[5],"start_date":"2026-07-24","event_type":"waste",
"color":"#EF3340",...}`, no plain `start` field, confirming `dow` is a **JSON
array** (not a bare int) and that this council's `event_type` set is still only
`waste`/`organic`/`recycle` (no `glass`, so that default is speculative for now,
per the platform-wide enum in §3.3, not directly confirmed). Implementation
below parses `dow[0]` and falls back to "unknown" gracefully (LED2's
waste-only-night behaviour just doesn't activate) if a future council's data
lacks this shape.

**Implementation** ([led_state.h](main/led_state.h)/[led_state.c](main/led_state.c),
[schedule.h](main/schedule.h)/[schedule.c](main/schedule.c),
[waste_api.h](main/waste_api.h)/[waste_api.c](main/waste_api.c),
[web_server.c](main/web_server.c)):
- `led_state_set_dual(primary, secondary, brightness)` drives both pixels
  independently (`LED_COUNT` → 2); `led_state_set()` is now a thin wrapper
  calling it with the same colour twice, so no other caller needed to change.
- `schedule_t` gained `light_mode` (`LIGHT_MODE_SINGLE_COLOUR` /
  `LIGHT_MODE_DUAL_COLOUR`, plain `#define`s over a `uint8_t` field, matching
  this file's existing style rather than introducing a new enum type) and
  `secondary_default_color` (default red). `SCHEDULE_STRUCT_VERSION` → 5, NVS
  key `schedule_v5` (another one-time reset, see §6).
- `waste_api_get_current()`'s signature changed to two `waste_api_slot_t{due,
  color}` out-params (primary/secondary) instead of one bare colour —
  `waste_api_config_t` itself didn't need to change shape, so no
  `WASTE_API_STRUCT_VERSION` bump. `do_fetch_events()` now also checks whether
  events sharing the earliest date (index 0 and 1 of the sorted, filtered
  list) are a genuine same-day pair before populating the secondary slot.
- New `waste_api_get_waste_weekday()` reads the recurring rule's `dow[0]`,
  parsed inside `fetch_and_parse_events()` (the only place that ever sees that
  array element, since it's otherwise skipped for lacking a `start` field) and
  cached alongside the rest of the poll result, subject to the same freshness
  window as everything else.
- `schedule_task_fn()`'s evaluator now computes a primary+secondary colour
  pair for both the API and manual paths, gated on `light_mode` exactly as
  designed: single-colour mode mirrors primary onto both LEDs and leaves
  "nothing due → fully off" unchanged; dual-colour mode also lights on a
  plain general-waste night (API: via the new weekday check reusing a
  refactored `is_window_active_for_weekday()`; manual: whenever the bin-night
  window is active even with no rule due) and falls back to
  `secondary_default_color` on LED2 whenever no second real event/rule fills
  that slot.
- Auto-populated `type_rules` (§3.3's `/api-setup` completion step) now try a
  new name-keyed table (`default_color_for_type()`: waste→red, recycle→yellow,
  organic/greenwaste→green, glass→purple) before falling back to
  `nearest_preset_color()` for anything else.
- Web UI: "Light mode" and "Second LED default colour" fields added to the
  home page's existing settings form, next to brightness.

**Planned revision**: the `waste_api_get_current()`-based EVENT/NO_EVENT/
UNAVAILABLE branching described above is being replaced by the unified
`schedule_get_next_collection()` resolver from §3.3's "next-collection model,
redesigned" — `schedule_task_fn()` will call it once per tick instead of
carrying its own copy of this branching logic. The *outcome* (dual-mode
lighting rules, colour precedence) is unchanged; only where that logic lives
is moving, specifically so the live evaluator and "Display Next Collection"
(§3.8) can no longer diverge the way they just did in real testing.

### 3.8 On-demand "Display Next Collection" button on the home page (implemented as "Test"; rename + rework planned)

**Requirement**: once the light is configured (manual/fallback schedule or
API), a button on the home page should light both LEDs immediately — using
whichever colour(s) the **next** upcoming scheduled occurrence would actually
show, rendered according to the current `light_mode` (single vs dual) — so the
colours (and the second LED's wiring) can be checked without waiting for the
real bin night or API poll window.

**Revision** (the first two landed with §3.11; the third is still pending
§3.3's resolver rework):
- ✅ **Renamed to "Display Next Collection"** — describes what it actually
  shows, rather than reading as a generic hardware self-test (that's §3.10's
  boot sequence).
- ✅ **Illumination duration changed from 2 minutes to 30 seconds** — long
  enough to check the colours by eye, short enough not to be an accidental
  always-on override if pressed by mistake.
- ❌ **Always backed by `schedule_get_next_collection()`** (§3.3's redesigned
  resolver) instead of the old per-feature preview logic below — so its
  result is now **guaranteed available** except in the one narrow case that
  resolver defines as genuinely unknown (API stale *and* manual/fallback
  disabled), rather than depending on a poll's lookahead window or freshness
  timer happening to currently agree with reality. This directly targets the
  class of bug already hit once in the original implementation (see below).
- Also used by the new physical button (§3.12) for the same "display next
  collection" action without the web UI.

**Resolved design decisions:**

- **"Next configured schedule", not "today"** — Test previews whatever the
  evaluator would show on the *next* occasion the light would actually turn
  on, not necessarily tonight:
  - **API mode**: `waste_api_get_current()` (§3.7's dual-slot version) is
    already forward-looking within its lookahead window — it returns the
    nearest upcoming qualifying event(s), not just "is something due right
    this second". Test calls it directly and previews whatever it returns,
    with no new lookahead logic needed.
  - **Manual mode**: generalise the existing per-date rule evaluation (today
    it's only ever invoked with "today's" date, since the live evaluator only
    runs on actual bin nights) to accept an explicit target date. Test computes
    the next upcoming `bin_night_weekday` occurrence on or after today, then
    runs the same due-rule logic against that date to get the same
    primary/secondary colours the real evaluator would produce when that night
    arrives.
- **Rendering respects `light_mode`**: single-colour mode → both LEDs show the
  preview's primary colour; dual-colour mode → LED1 = primary,
  LED2 = secondary (a real second due colour if the next occurrence has one,
  else `secondary_default_color`) — exactly the same mapping §3.7 defines for
  live operation, just fed a future date's result instead of today's.
- **Duration and revert**: lights for a fixed ~~2 minutes~~ **30 seconds**
  (revised above), then hands control back to the real evaluator rather than
  needing its own "off" logic — the handler starts a one-shot timer that, on
  expiry, calls the existing `schedule_task_force_check()` (already used for
  "apply immediately after saving") so whatever the actual current state
  should be (on, off, or a different colour) reasserts itself. A second press
  while one is already running simply restarts the timer with a fresh preview.
- **Web UI**: a "Display Next Collection" button (renamed above) on the home
  page, its own small `<form method='POST' action='/test'>` — no query params
  needed, it always previews "next" — wired to a new `POST /test` handler
  that triggers the preview and 303-redirects back to `/`. This is a new URI
  handler (current budget note in §3.3 no longer applies verbatim since
  `/api-test` already pushed the registered-handler count up —
  `max_uri_handlers` will need re-checking against the actual count once this
  and §3.7's changes land).

**Implementation** ([schedule.h](main/schedule.h)/[schedule.c](main/schedule.c),
[web_server.c](main/web_server.c)): `schedule_preview_next()` computes the
preview exactly as designed above (API's own forward-looking result, or the
manual schedule's next upcoming `bin_night_weekday` occurrence via a new
`date_plus_days()` helper feeding the existing `rule_due()`/`find_due_rules()`
logic). `schedule_test_trigger()` renders it through `light_mode` and drives
`led_state_set_dual()` directly, then creates (on first use) or resets a
FreeRTOS one-shot software timer (`xTimerCreate`/`xTimerReset`,
`freertos/timers.h`) whose callback is just `schedule_task_force_check()` —
reusing the existing "wake the evaluator now" mechanism instead of duplicating
any off/revert logic. `POST /test` in `web_server.c` is a two-line handler
that calls `schedule_test_trigger()` and redirects back to `/`.
`max_uri_handlers` bumped 5→8 while adding this and `/favicon.ico` (see §6's
bug log), covering the 7 handlers now registered with a little headroom.

**Pending rework**: `schedule_preview_next()` becomes a thin wrapper (or is
removed outright) once `schedule_get_next_collection()` (§3.3) exists — same
call site in `web_server.c`'s `POST /test` handler, but no longer needing its
own independent API-vs-manual branching. `TEST_PREVIEW_DURATION_MS` changes
from 2 minutes to 30 seconds; the button label and route stay `/test`
(handler name may be renamed to match, e.g. `display_next_collection_post_handler`,
for readability — not load-bearing either way).

**Bug fixed after first real-hardware test**: with the API configured and
enabled, pressing Test could silently do nothing. Root cause:
`schedule_preview_next()`'s `WASTE_API_RESULT_NO_EVENT` branch (nothing
rotating due right now) required `waste_api_get_waste_weekday()` to *also*
succeed before it would preview anything — mirroring the real evaluator's
stricter "only light on a plain waste night if we actually know which
weekday that is" rule from §3.7. But Test's actual promise ("once configured,
pressing this lights both LEDs") is unconditional, not a strict preview of
tonight's real outcome, so tying it to that second, independently-fallible
piece of information was the bug. Fixed by having `NO_EVENT` (and a
momentarily `UNAVAILABLE` API, e.g. before the first poll completes after
boot) always preview `secondary_default_color` on both LEDs whenever the API
is configured, dropping the weekday dependency entirely from this path.

**Still not build-verified beyond this one fix** — the rest of §3.7/§3.10 has
only had this one issue reported back from real hardware so far.

### 3.9 mDNS hostname advertisement (implemented)

**Requirement**: the device registers itself on the LAN via mDNS so it's
reachable as `binlight.local` instead of needing to look up its DHCP-assigned
IP — a real requirement from earlier in the project that had dropped out of
this document at some point and was re-added, not a new ask.

**Implementation** ([main.c](main/main.c)):
- **Dependency**: `espressif/mdns` added to
  [idf_component.yml](main/idf_component.yml) (same managed-component
  mechanism already used for `led_strip`/`cjson` — no `PRIV_REQUIRES` entry
  needed in [CMakeLists.txt](main/CMakeLists.txt), consistent with how those
  two are wired in).
- **Hostname**: fixed `"binlight"` (resolves as `binlight.local`), set via a
  new `mdns_start()` helper — not user-configurable via the web UI, consistent
  with how the LED GPIO pin is a fixed Kconfig value rather than a runtime
  setting.
- **Started from `app_main()`**, right after `wifi_manager_start()`, alongside
  the same non-fatal warn-and-continue pattern already used there and for
  `time_sync_start()` — the mDNS responder attaches to the netif and doesn't
  need to block on an established IP.
- **Also advertises the web UI as an mDNS service** —
  `mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0)` alongside the
  hostname, since the device already runs `esp_http_server` on port 80 — shows
  up correctly in mDNS-browsing tools (e.g. `dns-sd -B _http._tcp`), not just
  bare-hostname resolution.

**Not yet build-verified** — `idf.py build` needs to be run to confirm the
`espressif/mdns` managed component resolves cleanly and the image still fits
the factory partition; not yet flashed/tested on real hardware either.

### 3.10 Boot-time LED self-test (implemented)

**Requirement**: when the device powers on, before normal operation begins,
LED1 should cycle through each of the preset colours, then LED2 should do the
same, then both LEDs should show each colour simultaneously — a visual,
no-web-UI-needed confirmation that both LEDs and their wiring work, right
after flashing or power-cycling the device. Each colour displays for 0.5s.

**Implementation** ([led_state.h](main/led_state.h)/[led_state.c](main/led_state.c),
[main.c](main/main.c)): `led_state_run_self_test()` blocks for the ~6 seconds
this takes (4 colours × 3 phases × 0.5s) — LED1 solo (LED2 off), then LED2
solo (LED1 off), then both together — using a local 4-colour table
(red/green/yellow/purple, the same values as `COLOR_PRESETS` in
`web_server.c`, duplicated rather than shared since `led_state.c` is a
low-level hardware driver and shouldn't depend on the HTTP layer for 4 RGB
literals). Runs at full brightness (255) regardless of the configured
brightness setting, since this is a hardware check, not a preview of normal
operation. Called from `app_main()` right after `led_state_init()`, before
Wi-Fi or anything else starts — the very first thing the device does on
power-up.

### 3.11 Preferences UI reorganisation + default brightness fix (implemented)

**Requirement**: several settings currently grouped under "Manual schedule"
actually apply regardless of whether the manual or the API schedule is
driving the light — Brightness, On From, Turn off After, Light mode, and
Second LED default colour — and shouldn't visually imply they're
manual-schedule-only. They (plus Timezone, currently its own section) move to
a new **"Preferences"** section at the top of the page, above both the API
and Manual/Fallback schedule sections. The API and Manual/Fallback sections
each become a checkbox that reveals their detail fields only once ticked,
rather than always showing a full table.

**Resolved design decisions:**

- **New "Preferences" section**, positioned first (before "Bin collection
  API" and "Manual / Fallback Schedule"), containing: Brightness, On From,
  Turn off After, Light mode, Second LED default colour, and Timezone — all
  moved out of their current locations. Still part of the same single
  `POST /save` form and the same `schedule_t`/`settings` backing values as
  today; this is a rendering/grouping change in `web_server.c`, not a data
  model change.
- **Default brightness bug — FIXED (implemented ahead of the rest of §3.11)**.
  `default_schedule()` left `brightness` at `0` (`schedule_t s = {0}`
  zero-initialises it and nothing set it afterwards, unlike `start_minute`/
  `duration_hours`). Because brightness multiplies every colour channel in
  `led_state_set_dual()`, 0 renders the light black regardless of the resolved
  colour — indistinguishable from broken hardware. See §6 bug 16 for the full
  fix, which is four parts, not one: default 128, a **self-heal on load** for
  devices already carrying a zero-brightness v5 blob, a `schedule_set()` floor,
  and a matching UI slider minimum.
- **Checkbox-gated, collapsible detail sections, CSS-only** — no JavaScript:
  this project has deliberately stayed no-JS everywhere else (the `/api-setup`
  wizard is explicitly a "no-JS wizard" design choice, §3.3), so progressive
  disclosure here uses the standard checkbox + sibling-combinator CSS pattern
  (`input:checked ~ .details { display: block }` with the details block
  `display: none` by default) rather than introducing the project's first bit
  of client-side script for what's purely a cosmetic collapse/expand. The
  checkbox driving this **is** the existing `api_enabled`/`enabled` toggle —
  not a second, separate "show details" checkbox — so there's no new field,
  just a new CSS relationship between an existing field and the block that
  follows it.
- **"Bin collection API" section**: unchanged fields, now hidden until
  `api_enabled` is checked.
- **"Manual / Fallback Schedule" section** (renamed, §3.6): unchanged fields
  (bin night, colour rules) minus the ones promoted to Preferences, now
  hidden until `enabled` is checked.

**Implementation notes** (things the design above didn't anticipate, all in
[web_server.c](main/web_server.c)):

- **`.sect` wrappers are load-bearing, not cosmetic.** The obvious selector
  `input:checked ~ .details` uses the *general* sibling combinator, which
  matches **every** later `.details` sibling — so ticking "Use automatic bin
  collection API" would also expand the Manual/Fallback block below it. Each
  section is therefore wrapped in its own `<div class='sect'>` and the rule is
  scoped `.sect input:checked ~ .details`, which confines the match to one
  section. Verified in a browser across all four checked/unchecked
  combinations.
- **Collapsed ≠ excluded from the form.** `display:none` hides a control but
  does **not** stop it submitting, so collapsing a section still posts every
  field inside it at its current value. That's the behaviour we want —
  collapsing the manual schedule must not silently wipe its stored rules on
  the next Save — but it's worth stating, because the opposite (`disabled`, or
  omitting the markup entirely) would have quietly destroyed config.
- **The colour-mapping form nests inside the /save form's DOM via HTML5's
  `form=` attribute.** Forms can't nest, and the mapping table posts to
  `/api-test` while everything around it posts to `/save`. So an empty
  `<form id='mapform' action='/api-test'>` (carrying the two hidden fields) is
  emitted *before* the `/save` form opens, and each mapping control claims
  membership with `form='mapform'`. Valid HTML5, still no JavaScript, and the
  submitted body is byte-identical to what `/api-test` already parses. This is
  why `append_type_mapping_form()` was split into
  `append_type_mapping_anchor()` + `append_type_mapping_rows()`, and why
  `append_color_select()` gained a `_for()` variant taking a form id.
- **`HTML_BUF_SIZE` raised 7200 → 12288.** Sizes were *measured*, not
  estimated, by compiling `root_get_handler()` on the host against stubs and
  dumping the bytes it emits (see "Rendering the UI off-device" below):
  7145 bytes factory-fresh, 8679 for a realistic setup, **10877** worst case
  (8 colour-mapping rows, i.e. `WASTE_API_MAX_TYPE_RULES`). The fresh-device
  figure is the alarming one — the old 7200 buffer had 55 bytes of headroom on
  an *unconfigured* device and would have truncated on any real
  configuration. `safe_append()` truncates silently, and a truncated page
  **drops form fields**, which then read back as absent/unchecked on the next
  Save — a config-destroying failure that looks like nothing at all. Both
  `root_get_handler()` and `api_test_get_handler()` now log an error if they
  hit the ceiling, so this can never fail silently again.

**Rendering the UI off-device**: `web_server.c`'s handlers can be compiled and
run on the host, which is how the above was measured and how the layout was
checked before flashing. The trick is that the handlers are `static`, so a
harness `#include`s the whole translation unit and supplies (a) thin stub
headers for `esp_err.h` / `esp_log.h` / `esp_http_server.h` — the httpd
functions used are all trivially stubbable, with `httpd_resp_send()` writing
to a file instead of a socket — and (b) definitions for the handful of
`schedule_*` / `settings_*` / `waste_api_*` symbols it calls. Then set up a
`schedule_t` and `waste_api_config_t`, call `root_get_handler()`, and open the
result in a browser. Worth rebuilding if the UI is touched again: it renders
the firmware's real markup rather than a hand-written approximation, and it
catches buffer truncation before the device does.
- **Also landed here** (§3.8's cheap half, independent of its resolver
  rework): the button is renamed to **"Display Next Collection (30 seconds)"**
  and `TEST_PREVIEW_DURATION_MS` dropped from 2 minutes to 30 seconds.

### 3.12 Physical buttons (planned, not yet implemented)

**Requirement**: three physical button actions, without needing the web UI:
1. **Factory reset.**
2. **Display next collection** — the same 30-second preview as §3.8's
   "Display Next Collection" web button.
3. **Cancel tonight's illumination** — turn the light off early (e.g. once
   the bins are actually out), instead of waiting out the full "Turn off
   after" duration (default 20 hours).

Ideally (2) and (3) are the **same physical button**, disambiguated by
current light state: *if the light is currently on, pressing it turns the
light off until the next scheduled occurrence; if the light is currently
off, pressing it displays the next scheduled collection for 30 seconds.*

**Resolved design decisions:**

- **Two physical buttons total**, not three: a dedicated **factory reset**
  button, and one combined **action button** covering both (2) and (3) via
  the state-dependent logic above.
- **Factory reset requires a long press** (proposed: hold 5 seconds) rather
  than a single tap — not explicitly requested, but added deliberately since
  this is a destructive, hard-to-undo action (wipes NVS config back to
  defaults) sitting behind a bare GPIO with no confirmation dialog the way
  the web UI could offer one; a long-press guards against an accidental knock
  or a hand brushing past it. Implementation: `nvs_flash_erase()` (the same
  call already used defensively in `nvs_init_with_erase_retry()`) followed by
  a reboot (`esp_restart()`).
- **Action button, short press, state-dependent**:
  - **Light currently on** → turn it off immediately (`led_state_off()`) and
    **suppress relighting until the next scheduled occurrence** — not just a
    one-shot `led_state_off()`, which the evaluator's next ~30-second tick
    would immediately undo. New `schedule_suppress_current(void)` in
    `schedule.c`: records the specific collection date currently driving the
    light (from `schedule_get_next_collection()`, §3.3); `schedule_task_fn()`
    checks this before lighting up and stays off for that same date, then
    automatically clears the suppression once the resolved "next collection"
    date advances past it (self-clearing, no separate timer or explicit
    "resume" action needed).
  - **Light currently off** → same as "Display Next Collection" (§3.8): call
    `schedule_test_trigger()` (or its renamed equivalent) directly, bypassing
    the web server entirely — the button handler calls straight into
    `schedule.c`.
- **New `buttons.c`/`buttons.h` module** (matching the existing one-module-
  per-concern layout: `wifi_manager`, `time_sync`, `schedule`, `waste_api`,
  `settings`, `led_state`, `web_server`) — a background task polling both
  GPIOs with simple debounce (e.g. a short stable-read delay rather than
  interrupt-driven edge detection, consistent with this project's general
  preference for simplicity over the most sophisticated approach where either
  works fine). New Kconfig options alongside the existing
  `BINLIGHT_LED_GPIO`: `BINLIGHT_BUTTON_RESET_GPIO`,
  `BINLIGHT_BUTTON_ACTION_GPIO`.
- `buttons_init()`/`buttons_task_start()` called from `main.c` alongside the
  other module inits.

### 3.13 Multi-council API coverage — research findings (planned, not yet implemented)

Prompted by [mampfes/hacs_waste_collection_schedule](https://github.com/mampfes/hacs_waste_collection_schedule),
a Home Assistant integration with **224 Australian council entries**. Its value
here isn't its code (Python, `requests`, server-class runtime — none of that
ports to an ESP32) but its **survey of which councils share a backend**, which
directly answers "how much can we cover without writing 200 scrapers".

**Distribution of those 224 AU entries by backend module** (counted from the
repo's own README, not estimated):

| Backend | Councils | Relevance |
|---|---:|---|
| `app_my_local_services_au` (SA, ArcGIS) | 46 | **New backend — highest leverage, see below** |
| `impactapps_com_au` (waste-info.com.au) | 34 | **Already implemented** (§3.3) |
| `frwa_com_au` (SA regional) | 5 | Marginal; partly overlaps My Local Services |
| ~139 bespoke, one council each | 139 | **Deliberately out of scope** — see below |

So **two** backends cover ~85 of 224 entries (~38%), and we already have one of
them. The remaining ~139 are one-off HTML scrapes and bespoke council APIs with
no shared shape — each needing its own parser, and (per that repo's issue
history) breaking whenever a council redesigns its site. That long tail is
exactly what §3.6's manual / fallback schedule already exists to cover, and is
where a "don't chase it" decision is genuinely cheaper than the alternative.

**Decision: stay entirely on-device, no server-side component.** Considered and
rejected, because the research undercuts the main reasons to have one:
- Both high-value backends are plain unauthenticated JSON over HTTPS with small
  responses — verified live, both well inside the buffers the device already
  uses. The ESP32 does this today; a proxy would add nothing but a hop.
- The one thing a server would genuinely help with — scraping the ~139 bespoke
  councils — is precisely the part being declined on maintenance grounds. That
  repo sustains those with a community of contributors; a solo-maintained
  server would inherit that breakage with none of the help.
- A server means hosting + uptime as a new dependency for a device whose whole
  appeal is that it keeps working on the LAN, and it would route the user's
  home address through a third party (this one's own author's) — a real privacy
  cost for zero functional gain.

#### 3.13.1 Impact Apps: ship the council list (fixes a real usability problem)

Today §3.3 asks the user to type a **free-text subdomain**. The research shows
that's frequently unguessable — verified against the live API:

- Bayside Council (NSW) → `rockdale`
- Blue Mountains City Council → `bmcc`
- Horsham Rural City Council → `hrcc`
- Port Macquarie Hastings → `pmhc`
- Queanbeyan-Palerang → `qprc`

Nobody types `rockdale` to mean "Bayside". Replace the free-text field with a
**dropdown of known councils** (keeping free-text as a fallback for any council
not yet listed, so the flexibility §3.3 deliberately built in isn't lost).

**The list was built empirically, not copied.** Three candidate sources were
merged (this repo's hardcoded `SERVICE_MAP`, the live index at
`calendars.impactapps.com.au`, and our own known-good `maribyrnong`) and then
**every candidate was probed against the real `localities.json` endpoint** —
42 candidates in, **39 confirmed working**, 3 rejected (`bayside`, `brcc`,
`burwood-waste` — all 404; note `bayside` and `burwood-waste` are stale/wrong
entries in the HA repo, whose correct subdomains are `rockdale` and `burwood`).
Worth knowing that neither upstream source was correct on its own: the HA list
carries dead entries, and the vendor's own calendars index omits several live
councils including `maribyrnong`. Re-probing is the only trustworthy method if
this list is ever refreshed.

**7 of the 39 aren't covered by that repo's Impact Apps source at all** —
`burwood`, `hobsons-bay`, `kempsey`, `launceston`, `maribyrnong`, `narrabri`,
`pyrenees`. Several (Maribyrnong, Launceston, Hobsons Bay, Kempsey) are handled
there by separate bespoke scrapers instead, so for those councils this device
would use a *more* robust path than the reference project does.

Confirmed working subdomains (probed live, 39):

| Council | Subdomain |
|---|---|
| Baw Baw Shire Council | `baw-baw` |
| Bayside Council | `rockdale` |
| Bega Valley Shire Council | `bega` |
| Benalla Rural City Council | `benalla` |
| Blue Mountains City Council | `bmcc` |
| Brisbane City Council | `brisbane` |
| Burwood Council (NSW) | `burwood` |
| Campbelltown City Council | `campbelltown` |
| City of Ballarat | `ballarat` |
| City of Canada Bay Council | `canada-bay` |
| City of Launceston | `launceston` |
| Clarence Valley Council | `clarence` |
| Coffs Coast Waste Services | `coffs-coast` |
| Cowra Council | `cowra` |
| Cumberland City Council | `cumberland` |
| Forbes Shire Council | `forbes` |
| Gwydir Shire Council | `gwydir` |
| Gympie Regional Council | `gympie` |
| Hobsons Bay City Council | `hobsons-bay` |
| Horsham Rural City Council | `hrcc` |
| Kempsey Shire Council | `kempsey` |
| Ku-ring-gai Council | `ku-ring-gai` |
| Lithgow City Council | `lithgow` |
| Livingstone Shire Council | `livingstone` |
| Maribyrnong City Council | `maribyrnong` |
| Moira Shire Council | `moira` |
| Moree Plains Shire Council | `moree` |
| Murrindindi Shire Council | `murrindindi` |
| Narrabri Shire Council | `narrabri` |
| Penrith City Council | `penrith` |
| Port Macquarie Hastings Council | `pmhc` |
| Port Stephens Council | `port-stephens` |
| Pyrenees Shire Council | `pyrenees` |
| Queanbeyan-Palerang Regional Council | `qprc` |
| Redland City Council | `redland` |
| Snowy Valleys Council | `snowy-valleys` |
| South Burnett Regional Council | `south-burnett` |
| Wellington Shire Council | `wellington` |
| Wollongong City Council | `wollongong` |

Cost: a static `{name, subdomain}` table in flash — ~39 rows, roughly 1.5KB,
no runtime cost and no network dependency to render the dropdown.

**Latent bug found while reading the reference implementation** (not yet hit in
testing): that source generates recurring dates using `daysOfWeek` under
**ISO-8601 numbering (Mon=1 … Sun=7)**, whereas §3.7's implementation parses
`dow[0]` straight into `struct tm.tm_wday` (**Sun=0 … Sat=6**). Those two
conventions agree for Monday–Saturday and differ *only* for Sunday (ISO 7 vs tm
0) — which is why Maribyrnong's `dow:[5]` (Friday) has been correct all along
and the bug never surfaced. A council with Sunday collection would read `7`,
an out-of-range `tm_wday`. Fix is one line where `dow[0]` is parsed:
`if (dow == 7) dow = 0;`. Filed here rather than fixed immediately because it
belongs with the §3.3 next-collection rework that touches the same code.

#### 3.13.2 My Local Services (South Australia): 46 councils, one endpoint

The single highest-leverage addition available. **Verified live** against the
real endpoint (a 2-council-service sample at Lobethal, SA):

- **Public ArcGIS FeatureServer**, no auth, no key:
  `services1.arcgis.com/37apdbovSVEwr4YE/ArcGIS/rest/services/MyLocalServices/FeatureServer/{0,1,2,4}/query`
- **Keyed by lat/lon**, not an address hierarchy — one endpoint serves all 46 SA
  councils with **no per-council configuration at all**. There is no
  subdomain/council list to maintain for this backend, at all.
- Four endpoints (`0,1,2,4`) = one waste stream each; a given address returns a
  feature only from the endpoints that apply to it. Observed response ~2.4KB per
  endpoint, most of it ArcGIS field metadata — comfortably inside the existing
  4KB events buffer, though it does mean 4 sequential requests per poll.
- Returns a **recurrence rule, not a date list**:
  `{Waste_Type, Col_Day, Col_Freq, Colour, Col_Offset, Exclusion, Additional}`.
  Real observed sample: `General Waste, Col_Day 3, Col_Freq 1, Colour "Blue"` and
  `Recycling, Col_Day 3, Col_Freq 2, Colour "Yellow"`.
- Dates are computed on-device: weekly recurrence on `Col_Day`, every
  `Col_Freq` weeks, anchored at 1 January of the current year plus `Col_Offset`
  weeks, then `Additional` dates added and `Exclusion` dates removed (both
  comma-separated `YYYY-MM-DD` lists, empty in the sample). This is integer
  arithmetic on top of the noon-normalised `mktime()` day-diffing
  [schedule.c](main/schedule.c) already does — no new technique needed.
- `Col_Day` uses **1=Sunday … 7=Saturday**, so `tm_wday = Col_Day - 1`. A third
  distinct weekday convention in this project; worth a named helper per backend
  rather than a bare subtraction at the call site.

**Two design consequences, both favourable:**
1. **Being a rule rather than a window, it never goes stale** — it can answer
   "next collection" arbitrarily far ahead with no lookahead window and no
   freshness timer. That's a strictly better fit for §3.3's redesigned
   always-know-the-next-collection guarantee than Impact Apps' dated-event
   list, and it means an SA device that loses Wi-Fi indefinitely still knows
   its schedule.
2. **Colour arrives as a name, not hex** — `"Blue"`, `"Yellow"`, not
   `"#RRGGBB"`. The §3.7 mapping layer needs a name→RGB path alongside the
   existing hex parse. Note also that SA general waste is **Blue**, not the Red
   assumed by §3.7's default table — that table is Victoria-shaped, and lid
   colours genuinely differ by state. The per-type mapping UI (§3.3) already
   lets the user correct this; only the *auto-populated defaults* need to
   become backend-aware.

**UX gap to resolve before building this**: it needs lat/lon, and the device has
no map or geocoder. Options, in rough preference order — (a) a text field to
paste coordinates copied from Google/Apple Maps (simple, no dependency, but
asks something unusual of the user); (b) call a public geocoder to turn a typed
address into coordinates (better UX, but adds a second third-party dependency
and the "keep it local" tension); (c) ship nothing and treat SA as manual-only.
Not resolved here — flagged rather than guessed, consistent with how §3.3's
address wizard was settled only after the real API was understood.

#### 3.13.3 Merri-bek City Council (VIC) — accepted exception to the "no bespoke councils" rule

**Not on Impact Apps** — confirmed by probing `merri-bek`, `merribek` and
`moreland` against `waste-info.com.au` (all HTTP 404). It's one of the ~139
bespoke councils §3.13 otherwise declines. Included anyway as a deliberate,
explicitly-requested exception (the device owner has a specific user there),
accepting the higher maintenance risk called out below.

**Verified live, end to end** (both steps called against the real endpoints,
sample address `1 Vincent Street Oak Park 3046`). Two-step flow:

1. **Address lookup — public ArcGIS FeatureServer**, no auth:
   `services6.arcgis.com/8L5sOwfzTAvcvQur/ArcGIS/rest/services/WasteServices4Bin/FeatureServer/0/query`
   with `where=EZI_Address LIKE '<input>%'`. Returns the address plus the
   opaque codes step 2 needs: `Waste_Rate_Code`, `Recycling_Rate_Code`,
   `FOGO_Rate_Code`, `Glass_Rate_Code`, `Day`, `Zone`, `GlassWeek`, and a
   Web-Mercator (EPSG:3857) `x`/`y` point. ~440 bytes for a single match.
2. **Schedule fetch — the council's own API**:
   `www.merri-bek.vic.gov.au/api/AddressDetails`, passing every value from
   step 1 through verbatim (the point is forwarded as-is — no coordinate
   conversion needed on-device). Returns `allBinDays` / `allRecycleDays` /
   `allFogoDays` / `allGlassDays` as `DD-MM-YYYY` string arrays, plus a
   `publicHolidays` array and server-computed `wasteNext`/`recycleNext`/
   `glassNext`/`fogoNext` summary strings.

**Confirmed current, not a stale published calendar**: called on 2026-07-26,
the API returned "Next collection is on 27 July 2026" and `allBinDays` running
through 2026-12-28 (23 dates still in the future). This is live data.

**Four findings that affect the design:**

- **Response is 4399 bytes — it overflows the existing 4096-byte
  `EVENTS_BUF_SIZE`** and would fail the clean-overflow check in `http_get()`.
  Needs 8192 for this backend. Caught by measuring, not assumed.
- **No colour data whatsoever.** Impact Apps returns hex (`#RRGGBB`), My Local
  Services returns names (`"Blue"`), Merri-bek returns **nothing** — so colours
  must come entirely from §3.7's name-keyed default table
  (Rubbish→Red, Recycling→Yellow, FOGO→Green, Glass→Purple, which matches
  Victorian lid colours). That table stops being a nicety here and becomes the
  only colour source; a third "colour provenance" variant for the mapping layer
  to handle.
- **Three streams can fall on the same day** — Merri-bek runs Rubbish
  (weekly Mon), FOGO (weekly Mon), Recycling (fortnightly Mon) and Glass
  (every 4 weeks Mon), so 27 July 2026 has rubbish + FOGO + recycling
  together. That exceeds the two colour slots §3.7 defines. The existing
  per-type **ignore** rules are the right answer rather than a third LED: the
  two weekly streams carry no reminder value (they go out every week
  regardless), so ignoring `Rubbish` and `FOGO` leaves exactly the
  informative ones — the same reasoning already applied to `waste` in §3.3.
- **Address matching is case- and format-sensitive.** `EZI_Address LIKE
  '1 vincent st oak park%'` returns **0** matches while
  `'1 VINCENT STREET OAK PARK%'` returns 1 — the stored form is uppercase and
  unabbreviated. Fix, verified working: query
  `UPPER(EZI_Address) LIKE '<UPPERCASED INPUT>%'` and uppercase the user's
  input on-device. Also cap with `resultRecordCount=25` — there are **90,298**
  addresses in the layer, and an uncapped broad match would blow any sane
  buffer (a capped 25-row match measures ~2KB).

**Setup UI**: a single search-and-pick step rather than §3.3's three-level
locality→street→property wizard — user types a partial address, device
uppercases it, queries with the cap above, and renders the matches as a
pick-list. Same "device proxies every lookup, browser only ever talks to the
device" property as the existing wizard, one step shorter.

**Config storage consequence**: unlike Impact Apps (subdomain + one property
id), this backend needs ~10 persisted fields (4 rate codes, day, zone, glass
week, x, y, address label). Rather than widening `waste_api_config_t` with
fields only one backend uses, this is the point at which config should become
**per-backend** — a `backend` discriminator plus a union (or simply a separate
NVS key per backend), decided as part of the §3.3 rework that already has to
make `waste_api.h` backend-neutral.

**RE-VERIFIED 2026-07-25 — the notes above were partly wrong, and following
them would have produced a backend that did not work.** Corrections, all
confirmed against live calls:

- **`cpage` is a mandatory query parameter, and its value changed.** It is now
  **`183782`**, not `86612`. Omit it (or send the old value) and the endpoint
  either 500s or returns every field `null` with `"noService":"no service"` —
  which reads exactly like an unserviced address rather than a malformed
  request. This is the same trap `ocsvclang` sets on Monash.
- **The parameter names are the *form field* names, not the ArcGIS field
  names**: `xPoint`, `yPoint`, `wasteDay`, `wasteRateCode`, `recycleRateCode`,
  `fogoRateCode`, `glassRateCode`, `zone`, `glassWeekNumber`, `address`,
  `cpage`. Sending `Waste_Rate_Code` etc. verbatim from step 1 does not work.
- **No coordinate transform is needed — but not for the reason recorded.** The
  council's own JS *does* reproject the point from EPSG:3857 to EPSG:28355
  (UTM zone 55S) via proj4 before sending, so "forwarded as-is" was wrong. But
  the coordinates turn out to be **ignored by the endpoint entirely**: probed
  with the correct UTM values, with `0,0`, and with `999999,999999`, all three
  return byte-identical schedules. So the device can send `xPoint=0&yPoint=0`
  and skip the projection maths completely. Verified, not assumed — this was
  worth checking precisely because implementing a UTM forward projection
  on-device is the kind of thing that quietly doubles a backend's size.
- **The date format is `D-M-YYYY`, NOT zero-padded** — the arrays contain
  `"5-1-2026"` and `"12-1-2026"` side by side. Recorded above (and in §3.13.4's
  four-formats summary) as "DD-MM-YYYY numeric zero-padded", which would have
  broken a fixed-offset parser. The `*Next` summary strings use a *different*
  format again: `"Next collection is on 27 July 2026"` — full month name, day
  not padded.
- **Response is 4397 bytes**, confirming the 8192-byte buffer note below.
- **The page moved.** The calendar now lives at `/waste-calendar26/`
  (`/bin-collection-calendar` 302s to it), and its year is baked into both the
  URL and the `cpage` id — so **this backend should be expected to need a
  parameter update roughly annually**, which sharpens the maintenance-risk note
  below from theoretical to scheduled.
- The four rate codes, day, zone and glass week for the sample address are
  `101 / 142 / 160 / 170`, `Monday`, `B`, `3`; the live response gives
  `wasteNext`/`fogoNext` 27 July 2026, `recycleNext` 27 July 2026 and
  `glassNext` 3 August 2026, plus full `allBinDays`/`allRecycleDays`/
  `allFogoDays`/`allGlassDays` arrays running to the end of the year.

**Practical consequence for the abstraction**: Merri-bek's opaque id must carry
8 fields (4 rate codes, day, zone, glass week, address) — the coordinates drop
out. That is still far more than Knox's `"69454"`, so the "id is an opaque
variable-length string" requirement stands; it just needs ~120 bytes, not a
coordinate pair.

**cpage self-discovery (implemented, 2026-07-25)** — the annual `cpage` rot is
now self-healing rather than a maintenance item:

- Additional live probing refined the failure model: `cpage` must be a **real
  CMS page id** — the *old* id (86612) still works because that page still
  exists, while a made-up id gets **HTTP 500**. So the real risk is old
  calendar pages eventually being deleted after a rotation, not the rotation
  itself instantly breaking things.
- The current id is embedded in the calendar page's own HTML (in its AJAX
  call), but **only in the response to the address form POST** — the plain GET
  page doesn't contain it.
- So when `/api/AddressDetails` actively rejects a request (HTTP 500, or the
  all-null `"no service"` body) while the network is demonstrably up, the
  device POSTs its saved address fields to the **year-derived calendar URL**
  (`.../waste-calendar%02d/` from the device clock — the council bakes the
  year into the URL, `waste-calendar26/` in 2026, `27/` in 2027; tried for
  this year, next year, then last year to cover both edges of the rollover),
  scans the ~135KB response with a **streaming byte-state-machine** (never
  buffered — it's far beyond any buffer here; boundary-safe by construction),
  extracts the fresh id, and retries the fetch once. Transport-level failures
  deliberately do *not* trigger discovery — if the Wi-Fi is down, discovery
  would fail too.
- The whole chain is host-tested end to end in `test_backends.c`: a stale
  cpage (served a 500, as measured live) → discovery against the real
  captured page bytes → retry → all four streams parsed.

**Setup guidance (implemented)**: because the ArcGIS layer stores addresses in
one exact canonical form (uppercase, street type in full, no commas), the
Merri-bek search page shows a tip with a **year-derived link to the council's
own calendar page**: type your address there, let it autocomplete, then copy
the completed string into the device's search. Worked example (verified to
return exactly one match): `3/85 DAVIES STREET BRUNSWICK 3056`.

**Maintenance risk, restated**: this is a single council's own CMS endpoint
with no versioning and no stability guarantee. The cpage rotation is now
handled automatically; what discovery can *not* survive is a redesign that
changes the calendar URL scheme or the form field names themselves. It can break on any site
redesign, and unlike the two shared platforms, a break here helps exactly one
user and has no upstream community watching it. That's the accepted cost of
this exception; §3.6's manual / fallback schedule remains the safety net if it
ever stops responding.

#### 3.13.4 Whitehorse, Monash and Knox (VIC) — family deployment set

Three more councils requested once family is included. **None are on Impact
Apps** (probed `whitehorse`, `monash`, `knox` against `waste-info.com.au` — all
404). All three tested live against their real endpoints and **all three
work** — Monash only after correcting a wrong initial diagnosis (see below):

| Council | Verdict | Shape | Payload |
|---|---|---|---|
| **Knox** | ✅ Easiest of any backend so far | 2 JSON calls | 75B + **363B** |
| **Whitehorse** | ✅ Straightforward | 2 JSON calls | ~1.4KB + 869B |
| **Monash** | ✅ Works (needs `ocsvclang`; HTML payload) | 2 calls, OpenCities | ~2.5KB |

**Knox** — `knox.vic.gov.au/rubbish-collection/autocomplete/find?q=<address>`
returns `[{"value":"69454","label":"1053 Burwood Highway, FERNTREE GULLY VIC
3156"}]`, then `/rubbish-collection/find?address=69454` returns 363 bytes of
exactly what this device needs:
`rubbish_date`/`recycling_date`/`green_date`, each
`"Next collection is <span>05 August 2026</span>"`, plus human-readable
`collection_day` strings. No session cookie was needed despite the reference
implementation establishing one. Verified current (called 2026-07-26, returned
29 July 2026 for the next recycling — a Wednesday, matching its stated
collection day).

**Whitehorse** — a public "Weave" GIS service on `map.whitehorse.vic.gov.au`
(a different host from the council's main site, and unprotected):
`/weave/services/v1/index/search?query=<address>&indexes=index.property&type=EXACT&crs=EPSG:3857`
returns candidate properties with an `id`, then
`/weave/services/v1/feature/getFeaturesByIds?entityId=lyr_vicmap_property&datadefinition=dd_whm_property_waste&ids=<id>`
returns `{"collectionDay":"Wednesday","week":1,"nextRecycle":"29 Jul 2026","nextGOBS":"05 Aug 2026"}`.
Household waste is weekly on `collectionDay` (derived, not given as a date);
recycling and green organics come as explicit next dates. Note the reference
implementation requests `limit=1000` on the search — use a small limit here
(a 5-result search measured 1439 bytes; 1000 would be unusable on-device).

**Monash — Akamai-fronted; blocked from datacenter IPs, fine from
residential.** Initial probes from this project's build sandbox returned
**HTTP 403 "Access Denied"** for `/api/v1/myarea/search`, the plain homepage,
and every combination of browser User-Agent and headers. The first diagnosis
recorded here — that this was TLS-fingerprint (JA3/JA4) blocking, and
therefore fatal for mbedTLS on an ESP32 — **was wrong**, and is kept here
rather than deleted because it's a trap worth not re-entering.

The same `curl` from the owner's home connection returns **HTTP 200** (a 302
to `/Home`, then 200). ~~So Akamai is filtering on **IP reputation**, not on
the TLS handshake.~~ **The second diagnosis was also wrong — corrected
2026-07-25, on the owner's own machine and network path**: plain `curl` (its
honest default User-Agent) got 200, while the *same* `curl` sending a spoofed
Chrome User-Agent got 403. Same IP, same TLS stack, same moment — the only
variable was the UA. So Akamai here is flagging **User-Agent/TLS-fingerprint
inconsistency** (a client claiming to be Chrome while shaking hands like
curl), not datacenter IPs and not the handshake alone. The earlier sandbox
403s were almost certainly this too: those probes cycled *browser* UAs. The
consequence for the device is the happy one: `esp_http_client` sends its own
honest UA over its own mbedTLS handshake — a consistent, unremarkable client —
so **no UA games, ever**, is both sufficient and necessary. The code carries
this warning where the Monash URLs are built.

*Lesson for future backend research*: a 403 needs the *actual variable*
isolated before any conclusion is recorded — this one produced two confident
wrong diagnoses (TLS fingerprint, then IP reputation) before a controlled
same-network A/B (honest vs spoofed UA) found the real trigger. And never
spoof a browser UA from a non-browser TLS stack; it converts an accept into a
block.

**Confirmed working end to end from a residential connection.**

Step 1, `/api/v1/myarea/search?keywords=4 Carson Street, Mulgrave` returns
`{"Items":[{"Id":"e1c469c8-6565-401a-9b9e-f5440daffa82","AddressSingleLine":"4 Carson Street, Mulgrave 3170"}, ...]}`
— a **GUID**, not a numeric id (Knox uses `"69454"`, Whitehorse `"4645521"`),
so the shared backend abstraction's opaque-id field must be a string of at
least 36 characters, not an integer. Search is loose rather than exact (the
query above also returned "4 Stradbroke Crescent"), so the setup UI's
pick-a-match step is required here, not optional.

Step 2, `/ocapi/Public/myarea/wasteservices?geolocationid=<guid>` —
**`ocsvclang=en-AU` is mandatory**. Without it the endpoint returns a bare
`{"success":false}` with no error message and HTTP 200, which reads exactly
like a bad address id. Worth remembering: a `success:false` here means a
malformed request at least as often as an unknown property.

With it, the response is a JSON envelope wrapping an **HTML string**:
`{"success":true,"responseContent":"<div class=\"module-widget waste-services-widget\">…"}`.
cJSON unescapes the payload for free (it arrives `<`-escaped), leaving
plain HTML to scan. Confirmed content for the test address: Landfill Waste
(fortnightly, next Fri 31/7/2026), Recycling (fortnightly, next Fri 7/8/2026),
and Food and Garden Waste.

**Parse the CSS class names, not the `<h3>` text.** Each block is
`<div class="… waste-services-result regular-service general-waste date-precise item-0">`
with an `<h3>Landfill Waste</h3>` inside. The class tokens
(`general-waste`, `recycling`, `green-waste`) are a stable machine vocabulary
shared across OpenCities councils, whereas the `<h3>` is a user-facing label a
council can rename at will ("Landfill Waste" vs "Rubbish" vs "General Waste").
Keying the type mapping (§3.3) off the class token gives a stable identifier
*and* sidesteps per-council label variation. The `date-precise` token also
hints that some deployments return imprecise dates (e.g. a week number) — a
case to detect and skip rather than misparse.

Extract the date from the following `<div class="next-service">` — format
`Fri 31/7/2026`, i.e. weekday + **non-zero-padded** day/month. That is a
fourth distinct date format across the backends, and the non-padding is the
kind of detail that breaks a fixed-offset parser.

This remains the only backend whose payload isn't structured data, and so the
most fragile to a council restyle — but it is a small, regular fragment, and
class-token scanning is considerably more robust than matching display text.

If the API path does turn out to be blocked, the fallback is the manual /
fallback schedule (§3.6), which fits Monash well: its three streams (landfill
waste, recycling, food-and-garden) are exactly the fixed-weekday,
independent-frequency pattern §3.6 was designed for — that is the feature
working as intended, not a gap.

**What this set changes about the overall strategy** — worth stating plainly,
because it inverts §3.13's headline finding for *this* project:

- The real deployment set is now **5 councils, 4 of them bespoke**
  (Maribyrnong on Impact Apps; Merri-bek, Knox, Whitehorse bespoke; Monash
  blocked). The "two shared platforms cover 38% of Australia" result is true
  but largely **irrelevant here** — breadth was never the constraint, and
  §1.1 already flagged that friends cluster into a few LGAs.
- So the central architectural need is a **clean pluggable-backend
  abstraction**, not more council coverage. This upgrades the per-backend
  config note in §3.13.3 from a detail to the main design concern of the
  §3.3 rework.
- **The good news: all four feasible backends share one shape** — (1) free-text
  address search returning an opaque id, (2) fetch-by-id returning next
  collection dates per waste stream. That is a genuinely common interface, not
  four unrelated scrapers. It maps directly onto the existing
  `waste_api_fetch_*` + poll split; what varies per council is only the URLs,
  query parameters, and field names.
- **Two properties hold across every bespoke backend tested**: none return any
  colour data (so §3.7's name-keyed default table is load-bearing, not a
  convenience), and most return *next collection date per stream* directly —
  which is precisely the shape §3.3's redesigned always-know-the-next-collection
  model wants. The bespoke councils are, if anything, a **better** fit for that
  model than Impact Apps' windowed event list.
- **Every one of the five target councils is now confirmed reachable and
  parseable on-device.** No council in the deployment set needs to fall back
  to the manual schedule for lack of a working backend.
- Date formats vary and mostly need English month names — **five distinct
  formats** across four backends: `"05 August 2026"` (Knox, full month,
  zero-padded day), `"29 Jul 2026"` (Whitehorse, abbreviated month),
  `"5-1-2026"` (Merri-bek arrays, numeric **non**-padded D-M-YYYY — corrected
  from "DD-MM-YYYY zero-padded", see §3.13.3), `"27 July 2026"` (Merri-bek
  `*Next` summary strings, full month, non-padded day) and `"Fri 31/7/2026"`
  (Monash, weekday + non-padded D/M/YYYY). One shared date-parsing helper with
  a month-name table, tolerant of missing zero-padding and of leading prose —
  not five ad-hoc parsers.
- Opaque address ids are **not** a consistent type: `"69454"` (Knox),
  `"4645521"` (Whitehorse), a 36-char GUID (Monash), and a bundle of ~10
  fields (Merri-bek). The abstraction must treat the id as an opaque
  variable-length string, and Merri-bek's multi-field case is why per-backend
  config (§3.13.3) is unavoidable rather than a single shared id column.

#### 3.13.5 Unified LGA dropdown — one council list across all backends (implemented for Impact Apps)

**Status**: built and shipping in [councils.h](main/councils.h)/[councils.c](main/councils.c),
covering the **39 Impact Apps councils**. Built ahead of the sequencing below
at the owner's request. Implementation notes and the deviations from the design
that follows are at the end of this subsection.


**Requirement**: automatic/API mode is selected by picking a council from a
**single flat dropdown of supported LGAs**. The user should never see, choose,
or need to understand "which backend" — they pick their council by name and the
device resolves the rest.

**Design:**

- **One `<select>`, every supported council, backend inferred.** A static table
  in flash maps each LGA to `{display_name, state, backend, backend_param}`.
  `backend` is an enum (`IMPACT_APPS`, `MY_LOCAL_SERVICES`, `MERRI_BEK`,
  `KNOX`, `WHITEHORSE`, `MONASH`); `backend_param` carries whatever that
  backend needs to identify the council — the subdomain for Impact Apps, the
  council's own domain for OpenCities, and **nothing at all** for the 46 SA
  councils (that backend is purely lat/lon; those entries exist only so a
  South Australian user can find their council name and be routed to the right
  address-entry flow).
- **Sizing, measured not assumed**: 39 (Impact Apps) + 46 (SA) + 4 (bespoke) =
  **89 entries**. That's ~5.2KB of flash for the table and ~3.9KB of generated
  HTML for the `<select>` — the latter fits the existing 20000-byte
  `SETUP_HTML_BUF_SIZE` on `/api-setup` with plenty of room, so no buffer
  rework is needed. It would *not* fit the 7200-byte home-page buffer, which
  is a reason to keep council selection on `/api-setup` rather than moving it
  to `/`.
- **Grouped by state with `<optgroup>`** (VIC / NSW / QLD / SA / TAS),
  alphabetical within each group. States are how people actually locate their
  council in a long list, and it visually shrinks 89 entries to a handful of
  short sections. No JavaScript needed — `<optgroup>` is plain HTML, keeping
  the project's no-JS property (§3.11).
- **The free-text subdomain field stays**, demoted to an "Other council on the
  waste-info.com.au platform" escape hatch below the dropdown. This preserves
  the deliberate flexibility from §3.3 (any Impact Apps council works without a
  firmware change) while making the common path a simple pick. Losing that
  would be a real regression for anyone whose council isn't listed yet.
- **The dropdown choice drives which address flow runs next**, since the
  backends genuinely differ: Impact Apps → the existing locality → street →
  property wizard; Merri-bek / Knox / Whitehorse / Monash → a single
  search-and-pick step; SA → coordinate entry (the unresolved UX question in
  §3.13.2). The wizard therefore branches on backend after step one rather
  than being one fixed sequence — worth building that branch deliberately,
  because it's the point where a "one size fits all" assumption would force
  the awkward councils into the wrong shape.
- **Ordering is neutral** — the §1.2 working group is *not* pinned to the top
  of the list. That grouping is a development and testing priority, not
  something a user in another council should see reflected in their UI.

**Suggested sequencing** (not started), ordered by the §1.2 priority — the
five working-group councils first, shared-platform breadth last:

1. **§3.3 next-collection rework** — prerequisite for everything else here,
   since it's what gives `waste_api.h` a backend-neutral shape. Also fixes the
   live Test-button bug.
2. **Backend abstraction + per-backend config** (§3.13.3's config note,
   §3.13.4's common shape). Build it against **Knox** first: it's the smallest
   backend of the five (363-byte payload, two clean JSON calls) so it exercises
   the abstraction without the abstraction being obscured by parsing work.
3. **Whitehorse, then Merri-bek, then Monash** — increasing order of
   awkwardness (plain JSON → multi-field config → HTML scraping). By Monash the
   abstraction should be settled; if it isn't, that's the signal it's wrong.
   **After this step, the §1.2 working group is complete** and the feature is
   done by the definition that matters.
4. **§3.13.5 unified LGA dropdown**, once the backends it selects between
   actually exist. Building the dropdown earlier would mean designing a picker
   for backends that aren't written yet.
5. **§3.13.1 Impact Apps council list** (39 councils) — a data-only change once
   the dropdown exists.
6. **§3.13.2 South Australia** (46 councils) — last, and gated on resolving its
   lat/lon UX question. Largest coverage win, but zero value to the working
   group.

**Backend abstraction — as built (2026-07-25).** All four bespoke backends
(Knox, Whitehorse, Merri-bek, Monash) landed in one pass, in
[waste_api.c](main/waste_api.c), against endpoints re-verified the same day.
The shape that made this cheap:

- **One normalised vocabulary.** Every backend maps its streams onto Impact
  Apps' `event_type` names — `waste` / `recycle` / `organic` / `glass`
  (Knox `rubbish_date`→waste, `green_date`→organic; Whitehorse
  `nextGOBS`→organic; Merri-bek `fogoNext`→organic; Monash CSS tokens
  `general-waste`/`recycling`/`green-waste`). Everything downstream — type
  rules, the default-ignore for weekly `waste`, the name-keyed colour table,
  the sticky cache, the resolver, both UIs — is completely backend-agnostic;
  a backend is *only* a fetch-and-normalise function plus a search function.
- **`fetch_events_for_config()` is the single dispatch point** for both the
  poll and the diagnostics/auto-map fetch (`waste_api_fetch_upcoming()` now
  takes the config, not subdomain+id). `waste_api_search_address()` is the
  matching dispatch for setup.
- **Config is v3** (`waste_api_v3`): adds a `backend` discriminator
  (persisted as the `council_backend_t` value — append-only, never renumber)
  and a 127-char opaque `address_id` for the bespoke backends. **A v2 blob is
  migrated, not discarded** — the flashed device's Maribyrnong setup survives.
  Merri-bek's "id" packs its 7 lookup fields plus the address, '|'-separated;
  nothing outside the Merri-bek functions knows or cares.
- **No colour data exists in any bespoke backend**, so events are coloured at
  the source from the Victorian-lid defaults (including the tuned yellow) and
  remain remappable via type rules like any other event.
- **Whitehorse's `collectionDay` feeds the recurring waste-weekday signal**
  (same mechanism as Impact Apps' recurring rule). **Knox's is deliberately
  not used**: its payload has two weekday strings ("Weekly …"/"Fortnightly …")
  and which one is general waste is ambiguous — but every Knox stream arrives
  as a dated event anyway, so nothing is lost.
- **`EVENTS_BUF_SIZE` 4096 → 8192** for Merri-bek's measured 4397-byte
  response.
- **Setup flow branches by backend** (§3.13.5's design realised): dropdown →
  Impact Apps continues into the locality/street/property wizard; a bespoke
  council gets a single search box → pick-a-match → save. Search results are
  capped at 12 (`SETUP_SEARCH_MAX`) because each Merri-bek result link
  carries its ~110-byte packed id URL-encoded.
- **Parsers are tested against real captured payloads**, not synthetic ones:
  `test/host/fixtures/` holds each endpoint's live response from 2026-07-25,
  and `test_backends.c` compiles the real `waste_api.c` (with the real cJSON
  from the managed component) against an HTTP stub that serves those bytes in
  512-byte chunks. 26 assertions cover both steps of all four backends, the
  tie-ordering, the type-rule default, malformed-id handling, and the v2→v3
  config migration. `test_dates.c` covers the shared date scanner separately.

**As built** (deviates from the design above in two deliberate ways):

- **Only councils with a working backend are listed.** The design imagined all
  89 entries including the 4 bespoke Victorian councils and the 46 SA ones,
  with SA entries existing purely to route users to a different address flow.
  Those backends don't exist yet, so listing them would mean a user picks their
  own council by name, gets an address wizard, and only *then* discovers
  nothing works — strictly worse than not listing it. The table ships with the
  39 Impact Apps councils and grows as backends land. `council_backend_t` is
  an enum with the other backends' slots left to be added, so this is an
  append, not a rework.
- **A state `<select>` filtering the council list, not one `<optgroup>` list.**
  Requested by the owner during implementation ("default to VIC and have a
  state and LGA drop down"). Filtering server-side costs one extra page load
  when changing state — there's no JavaScript to filter client-side — so the
  page defaults to **VIC** and a Victorian user never sees that round trip.
  Two separate `<form>`s rather than one with two submit buttons, so which
  button does what is unambiguous. This also keeps the rendered page small
  (2.3KB for VIC, ~3KB for the largest state) rather than the ~3.9KB the
  single-list design estimated, and it scales when SA's 46 arrive.
- **The default state resolves in three steps**: an explicit `?state=` wins,
  else the currently-configured council's own state, else VIC. So returning to
  the page after setup shows the list you actually chose from.
- **The free-text subdomain field survives**, demoted below the dropdown as
  designed, preserving §3.3's deliberate "any council on this platform works
  without a firmware change". Its hardcoded `maribyrnong` default is gone; it
  now prefills from the saved config.
- **Subdomains no longer leak into the UI.** The home page, `/api-setup` and
  `/api-test` all show the display name via `council_display_name()`, which
  falls back to the raw subdomain for unlisted councils. The internal property
  id is no longer shown either — it was never actionable for a user.
- **The two ambiguous councils were resolved empirically, not assumed.**
  `campbelltown` and `wellington` both exist in more than one state. Fetching
  each one's locality list settled it: Airds and Ambarvale place
  `campbelltown` in NSW; Boisdale and Briagolong place `wellington` in
  Gippsland, VIC.
- **All 39 subdomains were re-probed live before shipping the table** (39/39
  returned HTTP 200), rather than trusting the list recorded in §3.13.1.
- **Structural tests**: [test/host/test_councils.c](test/host/test_councils.c)
  guards the things a compiler can't — duplicate subdomains or names, a
  typo'd state that would silently hide a council from every dropdown, empty
  fields, and the sort order the single-pass renderer depends on. It also
  prints per-state counts so a table edit shows up as a visible diff.

## 4. Current state of the code — READ THIS FIRST after a context reset

This section exists so work can resume from this file alone. It records what is
**written**, what is **flashed**, and what is **actually verified on hardware** —
three different things that are easy to conflate.

### Status by feature

| Feature | Code | Flashed | Verified on hardware |
|---|---|---|---|
| §3.1–3.3, 3.6 core + Impact Apps API | ✅ | ✅ | ✅ working |
| §3.9 mDNS (`binlight.local`) | ✅ | ✅ | ✅ user browsed via it |
| `/favicon.ico` (§6 bug 15) | ✅ | ✅ | ✅ console 404 gone |
| §3.7 second LED / dual-colour | ✅ | ✅ | ⚠️ LED2 not *separately* confirmed — see note |
| §3.10 boot self-test | ✅ | ✅ | ⚠️ not explicitly confirmed |
| §3.8 "Display Next Collection" button | ✅ | ✅ | ✅ **works** — confirmed on hardware |
| §3.11 brightness default fix (§6 bug 16) | ✅ | ✅ | ✅ **works** — see below |
| Warmer yellow `(255,150,0)` (§2) | ✅ | ✅ | ⚠️ red/green/purple good; yellow deferred (§5) |
| §3.11 Preferences UI reorganisation | ✅ | ❌ **not yet flashed** | ❌ |
| §3.8 button rename + 30s duration | ✅ | ❌ **not yet flashed** | ❌ |
| §3.3 next-collection rework (sticky cache, unified resolver) | ✅ | ❌ **not yet flashed** | ⚠️ 28 host tests pass; no device time |
| §3.13.5 council dropdown (39 Impact Apps LGAs) | ✅ | ❌ **not yet flashed** | ⚠️ table tests pass; 39/39 probed live |
| §3.13.3/3.13.4 all four bespoke backends (Knox, Whitehorse, Merri-bek, Monash) | ✅ | ❌ **not yet flashed** | ⚠️ parsers pass against real captured payloads; nothing on-device yet |
| waste_api config v2→v3 migration | ✅ | ❌ **not yet flashed** | ⚠️ host-tested; watch the first boot's log |
| Merri-bek cpage self-discovery + year-derived URLs | ✅ | ❌ **not yet flashed** | ⚠️ host-tested end to end against real page bytes |
| DST day-count fix (§6 bug 17) | ✅ | ❌ **not yet flashed** | ⚠️ host tests only (next real chance: Oct 2026) |
| §3.12 physical buttons | ❌ not written | — | — |
| §3.4 AutoAP provisioning + Wi-Fi forget | ✅ | ❌ **not yet flashed** | ❌ needs a device |
| §3.13.2 South Australia (46 councils) | ❌ not written (research complete, gated on the lat/lon UX question) | — | — |
| §3.5 OTA | ❌ not written (needs a partition-table rework) | — | — |

**The §3.11 UI build is committed but NOT yet flashed** — the working tree is
ahead of the device. Everything before it (yellow/brightness) is flashed and in
step. Build is clean (`idf.py build`, 0x130b60 bytes, 60% of the app partition
free) and the collapsible CSS + form-attribute wiring were verified in a real
browser against a static render of the generated markup, but nothing here has
been exercised on the device yet.

**Two things still worth an explicit look**, both cheap and both currently
assumed-but-unverified:
- **LED2** has never been confirmed to light *independently*. Colours have
  been judged through the enclosure, but nobody has stated that the second
  pixel works — a two-pixel chain fails silently to one pixel if the data
  link between them is bad. The boot self-test (§3.10) drives LED1 solo, then
  LED2 solo, then both, so watching one boot settles it conclusively.
- **The boot self-test itself** has not been explicitly reported as seen. The
  same single boot confirms both.

### ⚠️ Likely second cause of the Test-button failure — check this first

`default_schedule()` in [schedule.c](main/schedule.c) does **not** set
`brightness`, so the zero-initialised default is **0**. Both
`schedule_task_fn()` and `schedule_test_trigger()` pass that value straight to
`led_state_set_dual(..., brightness)`, which scales every channel by
`brightness/255` — **so at brightness 0 the LEDs are black no matter what colour
was resolved.**

The §3.7 work bumped the NVS schema to v5, which forced a one-time reset to
those defaults on the user's device. Unless brightness was manually moved off 0
in the web UI afterwards, **the device has been sitting at brightness 0 ever
since** — which would make the Test button appear to do nothing regardless of
whether the preview logic was right.

**RESOLVED — flashed and confirmed working on hardware.** The button now
lights the LEDs. Since the light was previously black and nothing else about
the colour resolution changed, **the brightness-0 bug was the real cause**,
and the earlier `schedule_preview_next()` relaxation (flashed at the same
time) was fixing a secondary issue at most. The self-heal path in
`schedule_init()` is therefore also confirmed working — the device repaired
its stored zero-brightness blob without a factory reset, exactly as designed.

Retained as a worked example: the reported symptom ("Test button does
nothing") pointed at the feature that was just written, while the actual
defect was a zero default in unrelated, older code. The discriminator that
would have found it faster is the boot self-test, which runs at hard-coded
brightness 255 and so isolates the LED path from all stored state.

**Useful discriminator**: the §3.10 boot self-test runs at hard-coded brightness
255, deliberately ignoring the stored setting. So if the LEDs cycle colours at
boot but the Test button does nothing, that is near-conclusive evidence of the
brightness-0 bug rather than a colour-resolution bug — the hardware and the LED
path are demonstrably fine.

### Environment facts that aren't visible from the source

- **Not a git repository.** No version control, no history, no branches — every
  edit is destructive, and there is no "revert" available. Worth initialising
  before the large §3.3/§3.13 refactor; also a prerequisite if GitHub Releases
  is ever chosen for OTA hosting (§3.5).
- **No ESP-IDF toolchain in the assistant's environment.** `idf.py build` has
  never been run by the assistant and cannot be. Every build/flash/verify step
  is the user's, and nothing below the "Code" column above should be assumed to
  compile until they say so.
- **Wi-Fi credentials live in `main/Kconfig.projbuild` as plaintext defaults.**
  This is a real publication hazard given §3.5 contemplates GitHub-hosted OTA
  images and §1.1 contemplates giving devices away: **if this project is ever
  pushed to a public repository, those credentials go with it** (and remain in
  git history even if later removed). Resolve by moving them to a
  git-ignored `sdkconfig.defaults.local`, or by landing §3.4 AutoAP first so
  compiled-in credentials stop existing altogether — the latter is the real
  fix, and is already a §1.1 prerequisite for distribution.
- **Assistant network probes run from a datacenter IP.** Council endpoints may
  return 403 here and work fine from the user's home connection — this already
  produced one wrong conclusion about Monash (§3.13.4). Never conclude "blocked"
  without a residential re-test.

### ⚠️ Palette changes do not reach already-saved rules (known wrinkle)

Colours are stored in NVS as **literal RGB triples**, in
`waste_api_config_t.type_rules[].color` and `schedule_t.rules[].color` — not as
a reference to `COLOR_PRESETS`. So editing the palette (e.g. the yellow
recalibration in §2) changes what the *palette* means but leaves every
previously-saved rule holding the old value, and the LED keeps showing the old
colour.

**After any palette change, press Save once on each affected form** (the colour
mapping form and/or the manual schedule form on `/`). The dropdowns now
preselect the *nearest* preset rather than requiring an exact RGB match, so the
right entry is already chosen — saving just snaps the stored value onto the
current palette. Without the nearest-match behaviour a stale value would match
no `<option>` at all and the browser would display the first entry, making a
yellow rule look as though it were set to Red.

**⚠️ Open question on the current yellow assessment**: it is not established
whether the mapping form was re-saved *after* flashing `(255,150,0)`. If it
wasn't, the stored rule still holds the old `(255,255,0)` and the light will
still be showing the original over-green yellow — meaning the "yellow is still
problematic, defer it" conclusion (§5) may have been formed against the old
value rather than the new one. **Worth re-checking before spending any effort
on enclosure-level fixes**: press Save on the colour mapping form, or watch the
boot self-test, which uses `SELF_TEST_COLORS` directly and so always shows the
current palette regardless of what is stored in NVS. If yellow looks right in
the boot self-test but wrong in normal operation, that is this wrinkle, not
diffraction.

**Proper fix, deferred deliberately**: move `COLOR_PRESETS` +
`nearest_preset_color()` out of [web_server.c](main/web_server.c) into a small
shared module, and snap stored colours to the current palette in
`schedule_init()`/`waste_api_init()` — the same self-heal-on-load pattern used
for brightness (§6 bug 16). Not done yet because the palette is currently
presentation-layer data, and §3.13 needs a shared colour module anyway (for
name→RGB mapping, since none of the bespoke council backends return colours) —
so both should land together rather than moving the same code twice.

### ▶ Agreed next step: flash, then verify the working group on-device

**Everything §1.2 requires is now written**: all five councils' backends, the
council dropdown that selects them, the sticky-cache resolver they feed, and
the setup flows for both backend families. The owner's directive stands:
*the five working-group councils are the definition of done; everything else
is nice-to-have.* What remains is on-device verification, which no host test
can substitute for:

0. **Note the Wi-Fi change first (§3.4).** Credentials now come from NVS, with
   the compiled-in Kconfig pair only as a fallback. Your bench device has its
   network in `sdkconfig`, so it will keep connecting exactly as before — but
   that also means AutoAP won't be exercised until you either blank the
   Kconfig SSID or press "Forget this network and restart setup" on the home
   page. Worth doing once deliberately, since it's the flow every handed-out
   device will start in.
1. **Flash** (this also carries the still-unflashed §3.11 UI and §3.3 work
   below). Watch the first boot for `migrated waste API config v2 -> v3` —
   the existing Maribyrnong setup must survive, not reset.
2. **Maribyrnong** keeps working (regression check: the poll logs
   `next=known`).
3. **Each of the other four councils**, one at a time, via
   `/api-setup` → VIC dropdown → address search → save. For each: the
   auto-built colour mapping looks right, `/api-test` shows the real upcoming
   dates, and "Display Next Collection" lights the right colours. The four
   test addresses used to verify the parsers are in
   [test/host/test_backends.c](test/host/test_backends.c); real deployments
   will use the real households' addresses.
4. **Watch Monash specifically** — its 403 behaviour is UA-fingerprint
   consistency (see §3.13.4); `esp_http_client`'s honest UA is expected to
   pass, but this is the one backend whose device-side network behaviour
   differs most from curl's.
5. Then return devices to the family deployments at leisure. Merri-bek's
   `cpage` (§3.13.3) is the known annual-maintenance item.

The older per-step verification list from the previous next-step note follows,
still applicable to the same flash:

#### (carried forward) flash-and-verify details for §3.11 + §3.3

**Two substantial changes are written and unflashed**, so the device is well
behind the tree:

1. **§3.11 UI** — Preferences section, "Manual / Fallback Schedule" rename,
   CSS-only collapsible sections, plus §3.8's rename and 30-second duration.
2. **§3.3 next-collection rework** — sticky NVS-persisted cache, staleness by
   date rather than by timer, and one unified `schedule_get_next_collection()`
   resolver shared by the live evaluator and the Display button. Three
   off-by-ones and a DST day-counting bug were fixed along the way; 28 host
   tests pass (`./test/host/run.sh`) but none of it has run on hardware.

**Next action is a flash and a verification pass**, which also settles the two
long-standing unverified items above (LED2 independently, and the boot
self-test) in the same boot.

Worth a specific eye on, since none of it has run on the device:
- The two UI sections expand/collapse from their own checkbox only.
- Saving with a section **collapsed** does not wipe that section's stored
  values (it shouldn't — hidden fields still submit — but this is the one
  failure mode that would silently destroy config).
- "Save mapping" still posts to `/api-test` and returns to `/`, now that its
  controls sit inside the `/save` form's DOM via `form='mapform'`.
- No `web_server: home page truncated` error in the serial log.
- **The first poll after flashing**: expect a `waste_api: poll complete: N
  event(s) in window, next=known` line, then a `restored next-collection
  cache` line on the *following* boot — that pair is the whole point of the
  sticky-cache change and is the one thing host tests can't prove.
- **"Display Next Collection" should now always do something** once anything
  is configured. If it no-ops, the log says why
  (`display-next requested but the next collection is unknown`).
- Note the manual schedule's "First collection" dates now behave as labelled
  (see §3.3 "off-by-one #2") — any existing manual rule will fire one cycle
  earlier than it used to. Worth re-checking those dates if the manual
  fallback is in use.

After the working group is verified on-device, what's left is (in the owner's
priority order, all nice-to-have): §3.12 physical buttons, §3.4 AutoAP (the
distribution blocker for handing devices out), §3.5 OTA, and the two shared
platforms already researched in §3.13 (Impact Apps list is done; SA remains,
gated on its lat/lon UX question).

### Open questions not yet resolved

1. **§3.13.2 (SA)**: how the user supplies lat/lon with no map or geocoder on
   the device — paste coordinates, add a geocoding dependency, or skip SA.
2. **§3.12**: which GPIOs the two buttons use on the XIAO ESP32-C6 — pins not
   yet chosen, and the board's usable/strapping pins not yet checked.
3. **§3.5 OTA**: image hosting, manual vs auto-check, and real partition sizes
   once an image with TLS is actually measured.
4. **§3.7**: the `glass` → Purple default is inferred from the platform-wide
   `event_type` enum, not observed live — Maribyrnong never returns it.

## 5. Still correctly deferred

- **Matter over Wi-Fi commissioning** — its own phase, per the "core first, Matter
  last" decision. Nothing in 3.2–3.4 blocks it; the single `led_state_set()` seam
  still applies.

- **Final yellow calibration — do this during final integration testing, in the
  real printed enclosure.** Red, green and purple are confirmed good and need
  no further work. Yellow is the one problem colour: `(255,150,0)` is an
  improvement on `(255,255,0)` but still not right, and testing established
  that it's affected by **viewing angle and diffraction**, not just the
  channel ratio. It therefore cannot be settled against the flat test cover —
  the production enclosure has different geometry and a divider between the
  two LEDs (§3.7), both of which change how the light mixes.

  When doing this:
  1. Judge it **assembled, in the final enclosure**, from the angles the light
     will actually be viewed at (it sits by the bins — likely seen from a
     distance and off-axis, not head-on at arm's length).
  2. Use the **boot self-test** (§3.10) to compare: it runs all four colours
     at full brightness with no configuration involved, so it isolates the
     palette from schedule/brightness state.
  3. Change the `Yellow` entry in `COLOR_PRESETS`
     ([web_server.c](main/web_server.c)) **and** the matching entry in
     `SELF_TEST_COLORS` ([led_state.c](main/led_state.c)) — two places, kept
     in sync deliberately (see §4's note on the shared-colour-module refactor
     that would collapse them into one).
  4. Then **press Save once** on the colour mapping form, or stored rules will
     keep the old value — see §4's "palette changes do not reach already-saved
     rules".
  5. If the ratio alone can't fix it, the remaining levers are physical:
     diffuser thickness/finish, LED standoff distance, or matte vs glossy
     inner surface.

## 6. Bugs fixed (troubleshooting reference)

Chronological, so a stale memory of "I already fixed X" can be checked against
what's actually landed. Build errors are compiler-caught; runtime bugs needed a
flash-and-observe or a live diagnostic to catch.

1. **Missing `esp_heap_caps.h` include** in the `espressif/led_strip` managed
   component's `led_strip_spi_dev.c` — `MALLOC_CAP_DEFAULT`/`heap_caps_calloc`
   undeclared. Patched directly in `managed_components/` (⚠️ lives outside this
   repo's tracked source — a fresh `idf.py` dependency refetch could silently
   revert it; re-check if this exact error reappears after clearing
   `managed_components/`).
2. **sdkconfig flash size mismatch** — was set to 2MB, board (Seeed XIAO
   ESP32-C6) actually has 4MB. Fixed early, before the partition table existed.
3. **Partition table offset alignment bug** (self-inflicted) — an early draft
   of [partitions.csv](partitions.csv) hand-picked a `factory` offset that
   wasn't 64KB-aligned, which ESP-IDF's partition tool requires for app
   partitions. Fixed by leaving offsets blank, matching how every stock
   ESP-IDF partition table does it (the tool computes correct alignment).
4. **`espressif/led_strip` 2.5.0→3.0.3 API rename** — `led_pixel_format`/
   `LED_PIXEL_FORMAT_GRB` became `color_component_format`/
   `LED_STRIP_COLOR_COMPONENT_FMT_GRB`. Build error, fixed in
   [led_state.c](main/led_state.c).
5. **LED colour byte order** — this specific LED batch needed
   `LED_STRIP_COLOR_COMPONENT_FMT_RGB`, not the WS2812 datasheet-standard GRB.
   Found empirically by diagnosing a red/green swap on real hardware, not a
   guess. See §2.
6. **Missing `#include <time.h>` in [settings.c](main/settings.c)** —
   `tzset()` implicit-declaration build error (it's declared in `time.h`, not
   `stdlib.h`).
7. **Form values weren't generically URL-decoded** — `httpd_query_key_value()`
   doesn't decode percent-encoding on its own. Only the colour field had an
   ad-hoc `%23`-prefix workaround; time fields (`13:45` → `13%3A45` as
   submitted) would have silently parsed as garbage the first time a real
   schedule with real times was saved. Fixed with a generic
   `url_decode_inplace()` applied to every relevant field in
   [web_server.c](main/web_server.c).
8. **`-Werror=format-truncation=`** on `minutes_to_hhmm()` — the output buffer
   was sized for the actual 0–1439 minute range used, but GCC's truncation
   check reasons about `uint16_t`'s full range (up to 65535). Fixed by sizing
   the buffer for the type's true worst case instead of the value's expected
   range.
9. **`main/CMakeLists.txt` missing component requirements**, found as two
   separate build errors: `esp_http_client` wasn't in `PRIV_REQUIRES` at all,
   then after adding it, `esp_crt_bundle.h` still wasn't found because that
   header lives in the `mbedtls` component specifically — `esp_http_client`
   depending on `mbedtls` internally doesn't expose `mbedtls`'s include path to
   `main`; each component that's `#include`d directly must be listed
   explicitly.
10. **`main/idf_component.yml` missing `espressif/cjson`** — ESP-IDF v6.0
    removed the old built-in `json` component; `espressif/cjson` (a managed
    component, same mechanism as `espressif/led_strip`) is the replacement.
11. **httpd server task stack too small (6144B) once `/api-setup` started
    making blocking HTTPS/TLS calls on that same task** — TLS handshakes are
    stack-hungry; caught in review (not a reported crash) and bumped to 8192,
    matching the dedicated `waste_api` poll task's stack.
12. **`GET /api-test` and `POST /api-test` handlers were written but never
    registered** with `httpd_register_uri_handler()` — caught in a review pass
    before the user hit it, not from an error report.
13. **Garbled ternary expression** in `web_server.c`'s colour-mapping default
    (`COLOR_PRESETS[0].r == 0 ? ... : ...`, leftover from thinking through the
    logic) — caught in review, replaced with a plain literal default.
14. **TLS trust failure connecting to `*.waste-info.com.au`** —
    `esp-x509-crt-bundle: No matching trusted root certificate found`. Not a
    stale-build issue (checked and ruled out via file timestamps) and not a
    missing/wrong certificate in ESP-IDF's bundle (confirmed the public key of
    the bundled "GTS Root R4" exactly matches what the live server presents).
    Root cause: the server's certificate chain terminates in a **cross-signed**
    GTS Root R4 (signed by the old standalone "GlobalSign Root CA", for
    legacy-client compatibility) rather than the self-signed version, and that
    specific old GlobalSign root isn't in ESP-IDF's default bundle — so
    mbedtls's chain-building tries to walk up to it instead of recognising GTS
    Root R4 as already-trusted. Fixed by enabling
    `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` (+ its dependency
    `CONFIG_MBEDTLS_X509_TRUSTED_CERT_CALLBACK`) in `sdkconfig` — an ESP-IDF
    feature built for exactly this case, ~700 bytes extra heap, off by
    default. See §3.3 for the full diagnosis.
15. **Browser console 404 on `/favicon.ico`** — every browser auto-requests
    `/favicon.ico`; the server had no handler for it at all. Fixed with a
    `GET /favicon.ico` handler serving a small inline wheelie-bin icon (SVG,
    not a binary ICO — browsers key off the response's `Content-Type`, not the
    URL extension) plus an explicit `<link rel='icon'>` in each page's `<head>`.
    Bumped `max_uri_handlers` 5→8 to fit the new handler with headroom for
    §3.8's planned `/test` endpoint.
16. **Default brightness of 0 made the light permanently black** — and was
    very likely the real cause of the §3.8 "Test button does nothing" report,
    which had previously been attributed solely to preview logic.
    `default_schedule()` never assigned `brightness`, so the `{0}`
    zero-initialiser left it at 0; `led_state_set_dual()` scales every channel
    by `brightness/255`, so *every* colour resolved to black. Found by reading
    the code while auditing state before a context compaction, not from a bug
    report — the symptom (a dead Test button) pointed at entirely the wrong
    subsystem. Fixed in four parts, because the obvious one-line fix would not
    have helped the already-flashed device:
    - `default_schedule()` sets `SCHEDULE_DEFAULT_BRIGHTNESS` (128, 50%).
    - **`schedule_init()` self-heals on load**: a stored blob with brightness
      below the floor is repaired in place and re-persisted. Without this the
      new default would only apply to a factory-reset device — the §3.7 work
      had already written a *valid* v5 blob containing brightness 0, and
      `schedule_init()` would happily keep loading it forever. Deliberately
      **not** done via a `SCHEDULE_STRUCT_VERSION` bump, which would have
      discarded an otherwise-working configuration to fix one field.
    - `schedule_set()` clamps to `SCHEDULE_MIN_BRIGHTNESS` (10, ~4%) so the
      value can't be driven back to 0 from the UI.
    - The web UI slider's `min` moved 0→10 to match, so the floor is visible
      rather than silently applied after saving.

    Design note: "off" is expressed by the schedule not being due, never by a
    zero multiplier — which is why a floor costs nothing and removes a whole
    class of "is it broken or just dark?" confusion.

17. **DST day-counting: the light would have failed to come on twice a year.**
    `days_between()` normalises both dates to local noon so a daylight-saving
    change can't flip the calendar date — but the two noons are then 23 or 25
    hours apart, not 24, and C's integer division truncates *toward zero*. A
    −23h gap therefore evaluated to `0` days instead of `−1`. Concretely: for
    a collection on the DST-start Sunday (first Sunday of October in
    Melbourne), `day_diff` on the Saturday evening came out 0 rather than −1,
    `is_window_active_for_date()` returned false, and the light stayed dark on
    a night it should have been lit. The mirror case exists at the April
    transition.

    Predates the §3.3 rework — it was latent in the original date maths.
    **Found by writing the host tests, not by inspection**: it is unreachable
    on any ordinary date, and the noon-normalisation comment actively suggests
    the case is already handled (it handles the date flipping, just not the
    residual hour). Fixed by rounding to the nearest day rather than
    truncating — `(secs + 43200) / 86400`, with the negative branch written
    explicitly as `-((-secs + 43200) / 86400)` since truncation is asymmetric
    across zero — in both `schedule.c` and `waste_api.c`.

    Pinned by the `DST boundaries (Melbourne)` cases in
    [test/host/test_resolver.c](test/host/test_resolver.c). Note the next real
    opportunity to observe this on hardware is October 2026, which is exactly
    why it is tested rather than trusted.

18. **The recurring general-waste weekday was treated as bin night** (§3.3).
    The API reports it the same way it reports dated events — as the
    *collection* day — so the light came on a day late on plain waste weeks in
    dual-colour mode. Fixed by the unified resolver, which converts every
    source to a collection date and takes its eve.

19. **A manual colour rule never fired on its own first collection** (§3.6).
    `rule_due()` compared against bin night while the UI field is labelled
    "First collection", so entering the collection date made `days_between()`
    return −1 on the first occurrence and the rule was skipped until the
    following cycle — it then worked correctly forever after, which is why it
    went unnoticed. Fixed by evaluating rules against the collection date.
    Note this changes when existing configured rules fire.

20. **ISO-8601 vs `struct tm` weekday** (§3.3). The API's `dow` is
    Mon=1..Sun=7; `tm_wday` is Sun=0..Sat=6. The parsed value was used
    directly as a `tm_wday`. These agree for Mon–Sat and differ only on
    Sunday, so Maribyrnong's Friday `dow:[5]` masked it completely — a
    Sunday-collection council would have produced an out-of-range 7. Converted
    at the parse site so no caller ever sees the ISO form.

**Breaking NVS schema changes** (not bugs, but worth tracking since each one
resets user-configured state on first boot after flashing): `schedule_t` went
through v2 → v3 → v4 → v5 as the manual schedule model evolved (per-weekday
grid → single bin-night + shared rotation → single bin-night + independent
per-colour frequency, per §3.6 → added `light_mode`/`secondary_default_color`,
per §3.7); `waste_api_config_t` went v1 → v2 (adding the per-type mapping
rules, per §3.3). Every reset so far has happened mid-iteration, before any
real deployed/long-term data existed — none have hit a "production" device.

`waste_api_config_t` **v2 → v3** (backend discriminator + opaque `address_id`,
per §3.13.5's abstraction) broke the pattern deliberately: it **migrates**
rather than resets, because by then a real configured device existed. The v2
layout is kept in `waste_api.c` solely for that one-time load; the migration
is host-tested in `test_backends.c`. This is the template for future config
changes now that devices are deployed: migrate, or self-heal (bug 16), never
casually reset.
