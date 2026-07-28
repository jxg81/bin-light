# Bin Light — UI Specification

Reference document for **how the web UI presents itself**: information
architecture, voice, visual system, page layouts. Written 2026-07-28 against
the working tree at commit `ccc02bd`.

> ### Relationship to `SPEC.md` — read this before anything else here
>
> **`SPEC.md` is authoritative for behaviour.** Functional decisions, hardware
> calibration, protocol and API decisions, NVS schemas, button and OTA
> semantics all live there and **must not be contradicted by this file**.
> Where the two disagree, `SPEC.md` wins and this file is the thing that is
> wrong.
>
> This file is authoritative only for **presentation**: wording, grouping,
> layout, markup and CSS. It exists so that a UI change can never quietly
> become a behaviour change. Anything in here that would alter what the device
> *does* is collected in **§9 — Proposed behaviour changes** and is not
> approved by the existence of this document; it needs the owner's explicit
> sign-off, and once agreed it belongs in `SPEC.md`, not here.
>
> **Status: phase 1 (design) complete. Nothing in this document is
> implemented.** No `.c` or `.h` file has been touched.

---

## Contents

1. [Who this is for, and what they are trying to do](#1-who-this-is-for-and-what-they-are-trying-to-do)
2. [What is wrong today — the evidence](#2-what-is-wrong-today--the-evidence)
3. [Design rules derived from the hard constraints](#3-design-rules-derived-from-the-hard-constraints)
4. [Information architecture](#4-information-architecture)
5. [Voice and terminology](#5-voice-and-terminology)
6. [The visual system](#6-the-visual-system)
7. [Page-by-page layout decisions](#7-page-by-page-layout-decisions)
8. [Budget: buffers and flash](#8-budget-buffers-and-flash)
9. [Proposed behaviour changes (need sign-off)](#9-proposed-behaviour-changes-need-sign-off)
10. [Rejected ideas, and why](#10-rejected-ideas-and-why)
11. [Unresolved — needs the owner or hardware](#11-unresolved--needs-the-owner-or-hardware)
12. [Implementation order for phase 2](#12-implementation-order-for-phase-2)

---

## 1. Who this is for, and what they are trying to do

Per `SPEC.md` §1.1, this is **not one device**. Units are built at ~$10 and
handed to friends and neighbours. The recipient did not build it, did not
choose it, and will not read a manual. They have no serial console and no
interest in one.

There are exactly three jobs, and they have wildly different frequencies:

| # | Job | How often | Where they are |
|---|---|---|---|
| **A** | *"Which bin goes out?"* | Weekly, in a hurry | On a phone, standing next to the bin, possibly in the dark |
| **B** | *"Make it work."* | Once, ever | On a phone, on the sofa, first evening |
| **C** | *"Change something."* | Rarely — a house move, a brightness tweak | On a phone or a laptop |

The UI today is built almost entirely for **C**, offers no answer at all to
**A**, and actively obstructs **B** (§2.1).

**The design principle that follows:** the home page answers A and nothing
else. B is a linear, numbered path that ends and does not come back. C lives
somewhere you have to go looking for, and that is correct.

### The two people who are *not* the audience

Worth stating, because the current UI serves both of them well and they are
why it reads the way it does:

- **The engineer who built it**, who wants to see the raw API payload, the
  event-type strings and the resolved colours. That need is real and is
  served by `/api-test` — which should keep its diagnostic content, just stop
  being the only place the mapping lives (§7.4).
- **The engineer debugging it in six months.** Their needs are served by
  `SPEC.md`, the host tests and the serial log, not by page copy.

---

## 2. What is wrong today — the evidence

Every figure below is measured, not estimated. Renders come from
`./test/host/run.sh render` against the current working tree; DOM measurements
come from loading `test/host/out/*.html` in a browser.

### 2.1 A factory-fresh device offers no way to set itself up

This is the headline finding and it should be fixed first.

`test/host/out/home-unconfigured.html` — the exact page a new owner sees —
contains **two links, and both are invisible**:

```
links: [ { href: "/api-setup",  visible: false },
         { href: "/api-test",   visible: false } ]
```

Both sit inside `<div class='details'>`, which `.sect input:checked ~ .details`
reveals only when the section's own checkbox is ticked. On an unconfigured
device `api_cfg.enabled` is false, so the checkbox renders unchecked, so the
block is `display:none`.

The checkbox is labelled **"Use automatic bin collection API"**.

So the complete instruction for setting up a bin light is: *tick the box about
the automatic bin collection API, and then a link will appear.* A recipient who
does not know what an API is has no route forward from the home page at all.

The disclosure pattern is not the bug — it is a good pattern. The bug is that
the **only** entry point to setup is hidden behind it, and that the reveal is
tied to a form field whose label is implementation vocabulary.

### 2.2 The device knows the answer to job A and never says it

`schedule_get_next_collection()` ([schedule.h:105](main/schedule.h:105))
returns a fully-resolved `schedule_next_t` — date, primary colour, secondary
colour, and a `waste_only` flag — and is documented as read-only, touching no
persisted or in-RAM state. `schedule_light_is_on()` and `time_sync_is_valid()`
are equally available.

The home page calls none of them. Its nine `<h2>` headings, in order, are:

```
Preferences · Bin collection API · Colour mapping · Manual / Fallback Schedule
· How the colour rules work · Wi-Fi · Firmware · Restart · Factory reset
```

Not one answers *"which bin goes out?"*. The first thing on the page below the
title is a button labelled **"Display Next Collection (30 seconds)"** — which
answers the question on the *hardware*, in the other room, for thirty seconds,
instead of on the screen already in the user's hand.

### 2.3 The page scrolls sideways on a phone

Measured on `home-unconfigured.html` with the body constrained to a 375px
viewport (iPhone SE / 13 mini width):

```
body content width available: 343px  (375 − 2 × 1em padding)
Preferences table intrinsic width: 447px
→ 104px of horizontal overflow
```

The cause is structural, not a missing media query: `Preferences` is a
two-column `<table>` whose right cells hold `<select>`s with long option text
(`"Dual colour (second LED shows its own colour)"`) and whose left cells hold
labels that will not wrap below their longest word. `table{width:100%}` cannot
shrink a table below its minimum content width.

Consequences visible in the render: `Second LED default colour` wraps to four
lines in a squeezed label column while the note text runs off the right edge
mid-sentence.

This is the one constraint the brief calls **mobile first**, and it is the one
the current layout fails outright.

### 2.4 Grouping follows modules, not goals

The page is laid out in the order the C modules were written:

- **"Bin collection API"** and **"Manual / Fallback Schedule"** are two ways of
  answering one question — *when are your bins collected?* — presented as two
  unrelated top-level sections named after their implementations. The manual
  one is additionally described by its role in the resolver ("fallback")
  rather than by what the user would use it for ("my council isn't listed").
- **"Colour mapping"** — which colour means which bin — is nested *inside* the
  API section, because that is the struct it lives in
  (`waste_api_config_t.type_rules`). To a user it is a property of their bins,
  not of a data source.
- **Wi-Fi**, **Firmware**, **Restart** and **Factory reset** are four separate
  top-level sections for what is, to a user, one idea: *things I do to the
  device itself, rarely, and one of them is scary.*

### 2.5 The colour-mapping form exists twice

`append_type_mapping_rows()` renders it on `/`
([web_server.c:366](main/web_server.c:366)); `api_test_get_handler()` renders
an almost identical form on `/api-test`
([web_server.c:1741](main/web_server.c:1741)). Both POST to `/api-test`.

The home-page copy is the reason for the `form='mapform'` escape hatch
([web_server.c:352](main/web_server.c:352)) — forms cannot nest, and the
mapping table sits inside the `/save` form. The hack is well-built and well
commented, but it exists to support a duplicate.

It is also expensive. Measured per-section, worst case with 8 event types and
every escapable field full of `'`:

```
Colour mapping section on / : 5137 B   ← of a 14592 B page
```

That single duplicated section is **35% of the worst-case home page** and is
the dominant term in why `HTML_BUF_SIZE` had to be raised to 16384.

### 2.6 Measured section costs (the basis for every estimate in §8)

| Section | fresh | configured | worst-case escaped |
|---|---:|---:|---:|
| `<head>` + inline `<style>` | 403 | 403 | 403 |
| Title + test button | 187 | 345 | 345 |
| Preferences | 1984 | 1984 | 2269 |
| Bin collection API | 547 | 441 | 1023 |
| Colour mapping | — | 1472 | **5137** |
| Manual / Fallback Schedule | 2811 | 2818 | 2818 |
| How the colour rules work | 997 | 997 | 997 |
| Wi-Fi | 356 | 356 | 507 |
| Firmware | 265 | 265 | 265 |
| Restart | 254 | 254 | 254 |
| Factory reset | 354 | 354 | 354 |
| **Total** | **8378** | **9909** | **14592** |

Other pages, for reference: `api-setup` 2457 (worst case escaped 12147),
`api-setup` Merri-bek 1570, `factory-reset-confirm` 1246, `update-*` 1149–1525.

### 2.7 The stylesheet is stored eight times

`body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}`
appears verbatim **7 times in `web_server.c` and once in `wifi_manager.c`**.
`.note{color:#888;}` appears 5 times. Every page carries its own `<style>`
block inside its own page buffer.

Total inline CSS + `<head>` boilerplate across the eight pages: **≈ 2.4 KB**,
paid once in flash *per page* and again in RAM every time a page renders.

### 2.8 Empty and error states are written for the author

The device is currently unconfigured, so these are the real strings a new owner
meets, verbatim from the renders:

| Situation | What it says today |
|---|---|
| Nothing set up | `No council/address configured yet.` |
| Council API unreachable (`/api-test`) | `Couldn't reach the API just now — check the device's Wi-Fi connection and the council subdomain, then try again.` |
| Council API unreachable (`/api-setup`) | `Couldn't reach "maribyrnong.waste-info.com.au" — check the council subdomain and try again.` |
| Address search failed | `Couldn't search Knox's address lookup just now` |
| Nothing due | `Nothing scheduled in that window.` |
| Clock not synced | *nothing at all — the page does not mention it* |
| No council configured (`/api-test`) | `No council/address configured yet — set one up first.` |

Four of these name a subdomain, an API or a "window". The user cannot act on
any of that. The clock-not-synced case — which `SPEC.md` §4 records as one of
the two things that stop the light working after a factory reset — is not
surfaced anywhere in the UI at all.

### 2.9 Two of the seven surfaces cannot be rendered or reviewed

`test/host/render_page.c` covers `/`, `/api-setup`, `/update` and
`/factory-reset`. It does **not** cover:

- **`/api-test`** — which uses `HTML_BUF_SIZE` and, once it gains the mapping
  table (§7.4), becomes one of the two largest pages. Its size has never been
  measured or asserted.
- **The AutoAP provisioning page** in `wifi_manager.c` — *the first thing a new
  owner ever sees*, on its own server with its own `PROV_HTML_BUF`, never
  rendered, never size-checked.

Both gaps must close **before** phase 2 edits those pages, not after. See §12.

### 2.10 `PROV_HTML_BUF` can be overflowed by the RF environment alone

Found while sizing the AutoAP page. Not a UI defect, but it constrains the UI
design, so it is recorded here and reported to the owner (§9.4).

`prov_root_get_handler()` renders up to `SCAN_MAX_SHOWN` = 15 scanned SSIDs,
each as `<option value='ESC'>ESC</option>`. `prov_escape()` expands `"` to
`&quot;` — 6 bytes out per byte in — so a 32-byte SSID of all quotes escapes to
192 bytes, and each `<option>` costs `2 × 192 + 26 = 410` bytes.

```
15 options × 410 B                     = 6150 B
PROV_HTML_BUF                          = 6144 B
```

The options alone exceed the buffer, before the ~1080 bytes of head, intro,
password field and submit button. `prov_append()` clamps silently, so the page
would truncate mid-`<option>` — losing the closing `</select>`, the password
field and the **Connect** button. A device in that RF environment cannot be
provisioned at all, with no error message and no serial console to consult.

This is an adversarial worst case, not something seen in the wild; ordinary
SSIDs contain nothing escapable and cost ~40 bytes each. But the margin is
thinner than it looks, and it means **the AutoAP page has no spare bytes for
styling**. See design rule **D6**.

---

## 3. Design rules derived from the hard constraints

These outrank every goal in §1. A design idea that conflicts with one of these
loses; it goes in §10 with its reason, it does not get worked around.

### D1 — No JavaScript. At all.

Plain HTML over HTTP on a LAN: forms, links, CSS. No `<script>`, no inline
handlers, no `javascript:` URLs, no JSON endpoints.

Two existing patterns carry the load and are preserved:

- **CSS-only progressive disclosure.** `.sect` + `input:checked ~ .details`
  ([web_server.c:488](main/web_server.c:488)). The `.sect` wrapper is
  load-bearing: a bare `input:checked ~ .details` matches *every* later
  `.details` on the page, so ticking one section would expand all the
  following ones. Do not "simplify" it away.
- **HTML5 `form=` to escape form nesting.** An empty `<form id='x'>` emitted
  before the outer form opens, with controls claiming membership by id
  ([web_server.c:341](main/web_server.c:341)). §7 removes its only current
  use, but the pattern is recorded here because it is the correct answer any
  time a control must sit visually inside one form and submit to another.

### D2 — No external assets.

No CDN, no web fonts, no icon fonts, no remote images. The device is the only
server and offers the browser no internet path — a `<link>` to a font would
hang, not degrade. Inline or `data:` URI only.

One same-origin exception is introduced deliberately: **`/s.css`**, served by
the device itself (§6.1). It is not an external asset; it is the device's own
stylesheet on its own server, exactly as `/favicon.ico` already is.

### D3 — Fixed page buffers. Overflow truncates silently.

| Buffer | Where | Current | Largest page today |
|---|---|---|---|
| `HTML_BUF_SIZE` | [web_server.c:48](main/web_server.c:48) | **16384** | 14592 (`home-worst-case-escaped`) |
| `SETUP_HTML_BUF_SIZE` | [web_server.c:1199](main/web_server.c:1199) | **20000** | 12147 (`api-setup-worst-case-escaped`) |
| `PROV_HTML_BUF` | [wifi_manager.c:40](main/wifi_manager.c:40) | **6144** | not measured — see §2.10 |

> The brief quoted `HTML_BUF_SIZE` as 14336. The in-flight security work has
> already raised it to **16384**, because routing more strings through
> `html_escape_attr()` pushed the escaped worst case 256 bytes over the old
> ceiling. Every figure in this document uses 16384. Re-check before phase 2 in
> case it moves again.

Rules:

- **Every proposed page carries a byte estimate** (§7) and every implemented
  page must be re-measured by the harness, which asserts the limits rather than
  printing them.
- **Truncation is not a cosmetic failure.** A truncated page drops form fields;
  a dropped field reads back as "absent"/unchecked on the next save; `/save`
  builds `schedule_t new_schedule = {0}` and writes it wholesale. A page 200
  bytes too long silently erases the user's schedule.
- **Raising a buffer is permitted, but it is heap on a microcontroller and must
  be justified in bytes.** Lowering one is equally a design act and needs the
  same evidence. §8 proposes lowering `HTML_BUF_SIZE`.
- **Collapsing content does not save buffer.** `display:none` and `<details>`
  hide bytes from the eye, not from the buffer. The only way to reduce a page
  is to not emit it — move it to another page, or cut it.

### D4 — Storage footprint matters, but is not the binding constraint.

Page HTML is string literals in flash inside a `0x1F0000` OTA slot. Current
build: **1,323,344 B image, 708,272 B (35%) free**. Prefer one shared
stylesheet and terse markup — but when flash and *buffer* pull in opposite
directions, **buffer wins**, because 4 KB of heap on an ESP32-C6 is worth more
than 4 KB of a 708 KB surplus.

### D5 — Mobile first, and structurally so.

The target is a 375 px-wide phone held in one hand. Therefore:

- **No `<table>` for layout.** Tables are for tabular data only (the upcoming
  collections list, the colour mapping grid). Settings use a stacked
  label-above-control block (§6.3), which cannot overflow because it has no
  minimum content width to overflow with.
- **Every table gets an `overflow-x:auto` wrapper**, so genuinely wide data
  scrolls inside its own box and the page body never does.
- **Touch targets ≥ 44 px tall.** The current `<a class='item'>` address rows
  are `.35em 0` — roughly 28 px. They are the tap targets in the setup wizard,
  used once, under time pressure, on a phone.
- **No hover-only affordances.**

### D6 — The AutoAP page is the one place to spend nothing.

It is the first thing a new owner sees, it runs on a separate server with a
separate 6144-byte buffer, that buffer is already thin (§2.10), and
`SECURITY-REMEDIATION.md` §B3 flags the provisioning server as the most
fragile thing in the project — "a mistake there strands a device with no way
in".

So the AutoAP page:

- **keeps its own inline `<style>`** and does *not* link `/s.css`. A stylesheet
  fetch that fails inside an OS captive-portal mini-browser would render the
  one page that must never look broken. The duplication is deliberate.
- receives **copy and structure changes only, ≤ 200 bytes net**.
- must have its buffer headroom measured (§12 step 1) before it is edited.

### D7 — The `reject_cross_origin()` guard is load-bearing and fails silently.

Seven handlers in `web_server.c` currently begin with:

```c
    if (reject_cross_origin(req)) {
        return ESP_FAIL;
    }
```

`save_post_handler`, `api_test_post_handler`, `update_post_handler`,
`wifi_forget_post_handler`, `reboot_post_handler`,
`factory_reset_post_handler`, `test_post_handler` — i.e. **every POST handler
in the file**.

**The rule, stated so a UI refactor cannot get it wrong in either direction:**

- **Every state-changing (POST) handler in `web_server.c` begins with the
  guard, as its first statement, before any body is read.** If a restructure
  moves, renames or splits a handler, the guard moves with it. If a
  restructure adds a POST route, it gets one.
- **GET handlers must NOT have it.** `SECURITY-REMEDIATION.md` §B2 is explicit:
  *"Do not apply it to any GET handler."* Ordinary browser navigations carry no
  `Origin`, so a guard on a GET is a no-op at best; on `GET /update` — which
  shares `update_post_handler` — the existing single guard is deliberate and
  already verified by check V5. The new GET routes this document proposes
  (`/settings`, `/s.css`) therefore get **no** guard.
- **The provisioning server in `wifi_manager.c` gets no guard**, per
  `SECURITY-REMEDIATION.md` §B3. It is a different server, reachable only on
  the temporary setup AP.

Because this is exactly the kind of small prologue a refactor deletes by
accident, and because deleting it produces no test failure and no log line,
**phase 2 must add a count assertion to `test/host/run.sh`**: the number of
`reject_cross_origin(` call sites in `web_server.c` must equal the number of
`HTTP_POST` handler registrations. `render_page.c --origin-check` already
exercises the behaviour; the count assertion catches the case where a handler
loses its guard while the tested ones keep theirs.

### D8 — Presentation only.

This document may not change what the device does. In particular:

- **`COLOR_PRESETS` values are hardware calibration, not taste.**
  `Yellow = (255,150,0)` reads correctly through the printed PLA enclosure;
  a mathematically purer value does not. See `SPEC.md` §2. **The UI may
  display these values and must never adjust them for on-screen appearance.**
  Screen chrome uses a separate palette (§6.2) that shares no values with
  `COLOR_PRESETS` — see the rejected idea in §10.7.
- **The two-step factory-reset confirmation stays two steps.**
- **Button semantics, OTA restart semantics and NVS schemas are settled.**
- **The `/api-setup` `save`/`bsave` steps stay GET links**, per
  `SECURITY-REMEDIATION.md` §8. The wizard's query-string threading works; do
  not convert it to POST as part of a visual redesign.
- Anything that *would* touch behaviour is quarantined in §9.

### D9 — One form per `schedule_t`.

`save_post_handler()` reconstructs the entire `schedule_t` from one POST body
and writes it wholesale. **Every field it reads must be present in the same
`<form>` on the same page**: `brightness`, `start`, `duration_hours`,
`light_mode`, `secondary_default_color`, `tz`, `tz_custom`, `api_enabled`,
`enabled`, and all of `rule1_*`/`rule2_*`/`rule3_*`.

This forecloses an otherwise attractive design — putting brightness on the
home page and the rest under settings. Splitting the form across pages would
zero every field left behind. The information architecture in §4 is shaped by
this constraint.

---

## 4. Information architecture

### 4.1 The shape of the change, in one line

> **Split the one dense page into a *status page* that answers "which bin?",
> a *setup path* that is numbered and ends, and a *settings page* you have to
> go looking for — and move the colour mapping to the page about bins.**

### 4.2 Page inventory

| URL | Working title | Job | New? |
|---|---|---|---|
| `/` | Bin Light | **A** — which bin, when, what the light will do | rewritten |
| `/api-setup` | Where do you live? | **B** step 1 — council → address | reworded |
| `/api-test` | Your bins | **B** step 2 — confirm colours; later, check upcoming | restructured |
| `/settings` | Settings | **C** — everything you rarely touch | **new route** |
| `/update` | Software update | **C** — firmware | reworded |
| `/factory-reset`, `/reboot`, `/wifi-forget` | — | **C** — destructive, reached from Settings | reworded |
| `/s.css` | — | shared stylesheet | **new route** |
| AutoAP `/` | Set up your bin light | **B** step 0 — join Wi-Fi | reworded |

Two new GET routes. `max_uri_handlers` is currently **14 with 12 in use**
([web_server.c:1881](main/web_server.c:1881)) — adding two lands exactly on
the cap. `httpd_register_uri_handler()`'s return value is not checked anywhere,
so exceeding the cap fails silently. **Raise `max_uri_handlers` to 16** as part
of phase 2 and keep the "N in use" comment accurate.

### 4.3 Why this split, and not the alternatives

**Why a separate `/settings` rather than collapsing settings on `/`?**

1. The brief asks for a first-run path *distinct* from settings you rarely
   revisit. A collapsed section on the same page is not distinct; it is the
   same page with a lid.
2. It is the only change that meaningfully buys back buffer. §8 shows the
   worst-case page dropping from 14592 to ~7.6 KB, letting `HTML_BUF_SIZE` go
   from 16384 back to 12288 — 4 KB of heap returned at every render.
3. D3 says collapsing saves no bytes. Splitting is the only lever that does.

The alternative — keep everything on `/`, put the status card on top, wrap the
rest in one big `.sect` — is recorded in §10.1 with its trade-offs. It is the
fallback if the owner rejects a new route.

**Why does the colour mapping move to `/api-test` rather than `/settings`?**

Because "which colour is which bin" is a fact about *bins*, and `/api-test` is
already the page about bins — it shows what the council says is coming, and it
already renders an editable mapping form. Putting the mapping on `/settings`
would recreate today's duplication with a new pair of URLs. Moving it to the
bins page deletes the duplicate, deletes the `mapform` hack, and takes 5137
worst-case bytes off the biggest page.

Cost: editing colours is one extra tap from `/`. Colours are set once at setup
and essentially never revisited, so this is the right thing to make slightly
harder.

**Why keep `/api-setup` and `/api-test` as URLs at all?**

Their *flows* are correct and verified against five live council endpoints
(`SPEC.md` §1.2). Renaming the routes would churn the wizard's query-string
threading, which `SECURITY-REMEDIATION.md` §8 explicitly says not to disturb.
The URLs are not user-facing — nobody types them — so they cost nothing in
plain-English terms. **Page titles and headings change; routes do not.**

### 4.4 The first-run path

```
   ┌─ Step 1 of 3 ────────┐   join the light's own Wi-Fi network
   │  AutoAP setup page   │   pick your home network, enter password
   └──────────┬───────────┘
   ┌─ Step 2 of 3 ────────┐   ⚠ THE HANDOVER — see §4.4.1
   │  AutoAP success page │   the setup network disappears; the user must
   │  "switch back"       │   put their PHONE back on their own Wi-Fi
   └──────────┬───────────┘   before binlight.local can resolve
              │ user re-joins home Wi-Fi, opens http://binlight.local
   ┌──────────▼───────────┐
   │  /  (unconfigured)   │   one card, one button: "Set up my bins"
   └──────────┬───────────┘
   ┌─ Step 3 of 3 ────────┐   Where do you live?
   │  /api-setup          │   state → council → address
   │                      │   ← timezone is set from the council here (§9.5)
   └──────────┬───────────┘
              │ (redirect target changes — see §9.1)
   ┌──────────▼───────────┐
   │  /api-test           │   Which colour is which bin?
   │  mapping + upcoming  │   "Finish setup" →
   └──────────┬───────────┘
              │            ┌───────────────────────────────────┐
              ├───────────►│ optional: set bin days by hand    │
              │            │ /settings#byhand — never required │
              │            └───────────────────────────────────┘
   ┌──────────▼───────────┐
   │  /  (configured)     │   the status card, populated. Done.
   └──────────────────────┘
```

#### 4.4.1 The handover between step 2 and step 3 is the riskiest moment in the product

When provisioning succeeds, `run_autoap()` lingers `AUTOAP_LINGER_MS` (4
seconds), then tears the AP down and drops to STA mode
([wifi_manager.c:549](main/wifi_manager.c:549)). From the user's side: **the
network they are joined to vanishes while they are reading the page.**

Everything that follows depends on the user understanding, unprompted, that
they must now put *their phone* back on *their own* Wi-Fi. Nothing on screen
can help them once the AP is gone — the device is not reachable from wherever
the phone landed.

Failure modes, all of them silent:

- The phone auto-rejoins a *different* remembered network, or falls back to
  mobile data, and `binlight.local` does not resolve.
- The phone keeps showing the dead `binlight-XXXX` network for a while.
- The user is inside an OS captive-portal mini-browser, which may close by
  itself when the AP dies, taking the instructions with it.
- `binlight.local` needs mDNS, which some Android builds and some VPN/private-
  DNS configurations do not do.

**Therefore the success page must be a real, numbered step, not a paragraph.**
It is the last thing the device can say before it is unreachable, so it must
carry the whole handover: what is about to happen, what to do, and what to do
when the name does not work. There is no IP fallback to offer here — the
device's address on the home network is not known to it at render time and
would be wrong anyway — so the recovery advice is the router's client list or
simply power-cycling and watching for the light to stop breathing white.

This is presentation, and it is the highest-value presentation change in the
onboarding path.

**The step chrome is conditional.** "Step 1 of 2" and the "Finish setup" button
render only when `!waste_api_config_complete(&cfg)`. Someone returning later to
change their address sees the same pages without wizard framing. Cost ≈ 80
bytes, only on the fresh-device path, where there is the most headroom.

**The path ends.** After the mapping is saved, there is no "next" — the user
lands on a home page that now answers their question, which is the only
completion signal that means anything.

### 4.5 Grouping on `/settings`

Grouped by what the user is trying to achieve, not by which module implements
it. Order is by descending likelihood of being wanted:

| Heading | Contains | Was |
|---|---|---|
| **When the light comes on** | brightness, on-from, turn-off-after | *Preferences* (rows 1–3) |
| **How the light shows colours** | light mode, second-light colour | *Preferences* (rows 4–5) |
| **Your bin days** | who is providing them (council name + address, or "set by hand"), link to change | *Bin collection API* header |
| **Set bin days by hand** | the manual schedule, collapsed | *Manual / Fallback Schedule* |
| **Date and time** | timezone | *Preferences* (rows 6–7) |
| **The light itself** | Wi-Fi, software update, restart, factory reset | four separate `<h2>`s |

Note **Your bin days** and **Set bin days by hand** are adjacent and framed as
two answers to one question, with the relationship stated in one plain sentence
instead of the current paragraph about resolver precedence.

`api_enabled` and `enabled` remain the disclosure checkboxes for their own
sections (D1) and remain in the single `/save` form (D9).

#### 4.5.1 "Set bin days by hand" becomes a real destination

Owner's direction (2026-07-28). The manual schedule stops being a *fallback
buried in settings* and becomes the answer to two questions, reachable by link
from both:

- **"My council isn't listed"** — from `/api-setup`, replacing the removed
  subdomain field (§7.3).
- **"I'd like a backup for when the council's site is down"** — from the finish
  step on `/api-test`, offered and **explicitly optional** (§7.4).

Both link to `/settings#byhand`. To make an anchor link actually reveal the
section without JavaScript, add one rule to `/s.css`:

```css
.sect:target .details{display:block}
```

~34 bytes, additive, and it composes with the existing
`.sect input:checked ~ .details` rather than replacing it (D1). Arriving via
the link expands the section so the user can see what they are being offered;
the checkbox remains the actual enable control and still has to be ticked,
which is correct — the manual schedule genuinely is off until switched on.
The section's copy therefore opens with an instruction to tick it.

**Nothing about this makes the manual schedule mandatory**, and the automatic
path never routes through it.

---

## 5. Voice and terminology

### 5.1 Rules

1. **A user must never meet an implementation word.** Not in a heading, not in
   a label, not in an error, not in a placeholder.
2. **Say what it does, not what it is.** "Get bin days from my council", not
   "Bin collection API".
3. **Second person, present tense, active.** "The light comes on", not "The
   light will be activated".
4. **Errors name the thing the user recognises and the thing they can do.**
   "Couldn't reach Knox just now — the light will try again in a few minutes"
   beats "Couldn't search Knox's address lookup".
5. **Numbers a user can act on, none they can't.** "Next collection: Tuesday
   29 July" yes; "Networks found: 15" no; "#ff9600" no.
6. **Australian spelling** — colour, not color. (Field *names* in the HTML stay
   as they are: `secondary_default_color` etc. are wire format, not copy.)
7. **Sentence case for headings.** "Factory reset", not "Factory Reset".
8. **Never apologise, never blame the user.** "Double-check the password" is
   good and already in the code — keep it.

### 5.2 Jargon → plain English

Implementation words the UI currently shows, and what to say instead:

| Says today | Say instead |
|---|---|
| Bin collection API | Your council's collection days |
| Use automatic bin collection API | Get bin days from my council |
| Manual / Fallback Schedule | Set bin days by hand |
| the API is authoritative / overrides the manual schedule | When your council's days are working, they're used. The days you set by hand are the backup. |
| API (as a noun) | your council |
| backend | *(never shown)* |
| manifest | *(never shown)* — "an update" |
| NVS | your saved settings |
| AutoAP / AutoAP mode | setup mode |
| subdomain | *(control removed entirely — §9.7)* |
| locality | suburb |
| property / property label | your address |
| event type / type | bin |
| type rules / rotation rules | which colour is which bin |
| Colour mapping | Which colour is which bin |
| Ignore *(checkbox)* | Don't remind me |
| POSIX TZ string / Custom TZ | Timezone code *(under "Advanced")* |
| LED / LED1 / LED2 | the light / the first light / the second light — **see §11.2** |
| Light mode: Single colour (both LEDs match) | Show one colour |
| Light mode: Dual colour (second LED shows its own colour) | Show two colours |
| Second LED default colour | What the second light shows on an ordinary week |
| Display Next Collection (30 seconds) | Show me the next colour |
| Test API (show upcoming weeks) | Check my bin days |
| Bin Collection API Test *(page title)* | Your bins |
| Bin Collection API Setup *(page title)* | Where do you live? |
| raw data for the next 28 days (nothing filtered out yet) | What your council says is coming up |
| firmware *(in headings)* | software — "Software update" |
| Check for updates | Check for an update |
| Running version | Version |
| cross-site request | *(403 body — current wording is already fine)* |

**Words that stay**, deliberately:

| Word | Why |
|---|---|
| `binlight.local` / `192.168.4.1` | Addresses the user must type. |
| `binlight-XXXX` | The network name they must find in a phone's Wi-Fi list. |
| 2.4GHz | Genuine, actionable troubleshooting. Already well-phrased. |
| Factory reset | Universally understood on consumer devices. |
| Wi-Fi, password, network | Not jargon. |
| Timezone | Not jargon. *POSIX TZ string* is. |
| Council names, suburb names, addresses | The user's own words. |

### 5.3 State copy

The complete set of states each surface can be in, and what it says. This
table is the specification for §7's markup.

**Home page status card.** Conditions use only existing, read-only calls.

| # | Condition | Heading | Body |
|---|---|---|---|
| S1 | `!waste_api_config_complete(&cfg) && !s.enabled` | **Let's set your bins up** | It takes about a minute. You'll need to know which council you're in. → **Set up my bins** |
| S2 | clock not synced (`!time_sync_is_valid()`) | **Just getting started** | The light is checking today's date. Give it a minute, then reload this page. |
| S3 | configured, but `!next.known` | **Nothing from *Knox* yet** | The light has asked *Knox* for your collection days and hasn't heard back. It keeps trying. → **Check my bin days** |
| S4 | `next.known`, collection is tonight | **Bins go out tonight** | *Tuesday 29 July* + swatches |
| S5 | `next.known`, collection is tomorrow night | **Bins go out tomorrow night** | *Wednesday 30 July* + swatches |
| S6 | `next.known`, further away | **Next collection** | *Tuesday 4 August* + swatches |
| S7 | `next.known && schedule_light_is_on()` | **The light is on now** | (as S4/S5, plus this line) |
| S8 | `next.known && next.waste_only` | as S4–S6 | *General rubbish only — the light stays off for these unless you've asked for two colours.* |

S2 is new information: the clock-not-synced state is currently invisible in
the UI, and `SPEC.md` §4 names it as one of two things that leave a
freshly-reset light showing nothing.

**Error states elsewhere.**

| Where | Today | Instead |
|---|---|---|
| `/api-setup` council lookup failed | `Couldn't reach "x.waste-info.com.au" — check the council subdomain and try again.` | **Couldn't reach *Maribyrnong* just now.** Check the light is on your Wi-Fi, then try again. |
| `/api-setup` address search failed | `Couldn't search Knox's address lookup just now — check the device's connection and try again.` | **Couldn't reach *Knox* just now.** Check the light is on your Wi-Fi, then try again. |
| `/api-setup` no matches | `No matches for "x". Try just the house number and street name.` | *(keep — it is already plain and actionable)* |
| `/api-test` fetch failed | `Couldn't reach the API just now — check the device's Wi-Fi connection and the council subdomain, then try again.` | **Couldn't reach *Knox* just now.** Your saved bin days are still being used. |
| `/api-test` nothing due | `Nothing scheduled in that window.` | ***Knox* isn't showing any collections in the next 4 weeks.** That can happen around public holidays. |
| `/api-test` not configured | `No council/address configured yet — set one up first.` | **You haven't told the light where you live yet.** → **Set up my bins** |
| `/update` check failed | `Couldn't check for updates. The light couldn't reach the update manifest…` | **Couldn't check for an update.** The light needs an internet connection for this. Everything else keeps working. |
| `/settings` no council | `No council/address configured yet.` | **Not set up yet.** → **Set up my bins** |

Note the `/api-test` failure copy gains a reassurance — *"Your saved bin days
are still being used"* — which is true (the sticky cache, `SPEC.md` §3.3) and
is exactly the thing that stops a non-technical user concluding the device is
broken.

---

## 6. The visual system

CSS only. No JavaScript, no external assets, no images beyond the existing
inline SVG favicon.

### 6.1 One shared stylesheet at `/s.css`

**Decision:** serve the stylesheet from its own URI with a long
`Cache-Control`, exactly as `/favicon.ico` already is
([web_server.c:161](main/web_server.c:161)), and replace every inline
`<style>` block in `web_server.c` with:

```html
<link rel=stylesheet href=/s.css>
```

**Why:**

- **It takes CSS out of the page buffers.** ~403 bytes off `/` and `/api-test`,
  ~330 off `/api-setup`, ~150–250 off each of the four small pages — bytes that
  currently count against `HTML_BUF_SIZE` at every render.
- **It stores the CSS once in flash** instead of eight times (§2.7).
- **It lets the design be richer than 400 bytes.** A stylesheet paid once and
  cached for a week can afford dark mode, proper spacing and real touch
  targets; eight inline copies cannot.
- **It degrades safely.** If the fetch fails the pages are still plain,
  readable, fully functional HTML forms. There is no layout that depends on it.

**Handler:** a GET, therefore **no `reject_cross_origin` guard** (D7). Set
`Content-Type: text/css` and `Cache-Control: public, max-age=604800`.

**Exception:** the AutoAP page does **not** link it (D6).

### 6.2 Two palettes that must never merge

This is the single most important thing in this section.

**Palette 1 — the LED palette.** `COLOR_PRESETS` in `web_server.c`. Red
`(255,0,0)`, Green `(0,255,0)`, Yellow `(255,150,0)`, Purple `(128,0,128)`.
These are **empirical hardware calibration**, tuned by eye through the printed
PLA enclosure (`SPEC.md` §2). They describe what the *light* does.

**Palette 1a — the display palette.** A second, UI-only set of hex values, one
per preset, used for **every swatch on every page**:

| Preset | Emit — what the LED does | Display — what the screen shows |
|---|---|---|
| Red | `(255,0,0)` `#ff0000` | `#d93025` |
| Green | `(0,255,0)` `#00ff00` | `#1e8e3e` |
| Yellow | `(255,150,0)` `#ff9600` | `#f9c22e` |
| Purple | `(128,0,128)` `#800080` | `#8e44ad` |

> **This reverses an earlier decision in this document, on the owner's
> direction (2026-07-28), and the reversal is right.**
>
> The first draft printed the stored RGB verbatim, arguing that screen and
> light must agree. That was wrong. `(255,150,0)` is **green-channel
> compensation for a WS2812 seen through printed PLA** — it exists because
> green is more luminous than red at equal duty (`SPEC.md` §2). It is a
> hardware correction, and a hardware correction is an implementation detail.
> Printing it as an orange square asks the user to look at the workaround
> instead of the idea. The rule the brief sets — *a user should never meet an
> implementation word* — applies just as much to implementation **values**.
>
> **The yellow bin is yellow. Show yellow.**

Rules that keep this from drifting back into the LED path:

- **The display value is never stored, never submitted and never emitted.**
  `<option value=...>` still carries the **emit** hex, which is what
  `parse_hex_color()` reads and what reaches NVS. Only `.sw`'s inline
  `background` uses the display value. A save round-trip is byte-identical to
  today's.
- **Swatches resolve through `nearest_preset_index()`**, which already exists
  ([web_server.c:228](main/web_server.c:228)) and already handles the case of a
  stored triple that matches no preset exactly. Nearest preset → that preset's
  display value.
- **Adding the display value is a small struct addition, not a behaviour
  change** — see §9.6. It must not become an excuse to touch the emit values.
- **The council's own reported colours are not shown as swatches.** See §7.4:
  a coloured square on these pages always means one of the four bins, with no
  exceptions.

**Palette 2 — the interface palette.** Backgrounds, text, borders, links,
buttons. Shares **no values** with palette 1 and is defined only in `/s.css` as
custom properties.

**The interface palette is deliberately not a bin colour.** The accent is a
slate blue (`#2c5d8a`) and the destructive colour a dark, desaturated red
(`#9b2c2c`) — chosen so that no button, link or border on any page can be
mistaken for one of the four bin colours. An earlier draft used a green accent;
that was wrong, because a green button beside a green bin swatch teaches the
user that green is decoration. **Colour on these pages means a bin, and nothing
else means a bin.** (Owner's direction, 2026-07-28: reliable colour
reproduction in the enclosure matters more than flexibility. That priority
governs §10.7 and this rule alike.)

> **Rule: `COLOR_PRESETS` is device output, not a UI theme.** The interface may
> not draw its chrome from it, and must never be "unified" with it. A future
> change to the interface palette must not touch `COLOR_PRESETS`; a future
> recalibration of `COLOR_PRESETS` (the yellow work deferred in `SPEC.md` §5)
> must not touch the interface palette.

### 6.3 The stylesheet

Proposed in full. Written to be readable in source; the `%%` escaping needed
for `safe_append`'s format string does not apply here because `/s.css` is
served with `httpd_resp_send(..., HTTPD_RESP_USE_STRLEN)` from a plain literal,
like `FAVICON_SVG` — **no `printf` formatting, so no `%%` doubling**.

```css
:root{--bg:#fff;--fg:#1b1b1b;--mut:#666;--line:#dcdcdc;--card:#f5f5f4;--acc:#2c5d8a;--warn:#9b2c2c}
@media(prefers-color-scheme:dark){
:root{--bg:#141414;--fg:#ededed;--mut:#9b9b9b;--line:#333;--card:#1e1e1e;--acc:#7fb3e0;--warn:#d08a8a}}
*{box-sizing:border-box}
body{font:16px/1.45 system-ui,sans-serif;color:var(--fg);background:var(--bg);
max-width:30rem;margin:0 auto;padding:1rem 1rem 3rem}
h1{font-size:1.3rem;margin:.2rem 0 1rem}
h2{font-size:1.05rem;margin:1.7rem 0 .5rem}
h3{font-size:.95rem;margin:1.2rem 0 .4rem}
p{margin:.55rem 0}
a{color:var(--acc)}
.note{color:var(--mut);font-size:.87rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:.6rem;
padding:.9rem 1rem;margin:1rem 0}
.big{font-size:1.5rem;font-weight:600;margin:.15rem 0}
.f{margin:.9rem 0}
.f label{display:block;font-size:.87rem;color:var(--mut);margin-bottom:.25rem}
.f input,.f select{width:100%;font:inherit;padding:.5rem;border:1px solid var(--line);
border-radius:.35rem;background:var(--bg);color:var(--fg)}
.f input[type=checkbox]{width:auto;margin-right:.4rem}
.f input[type=range]{padding:0}
button{font:inherit;padding:.6rem 1rem;border:1px solid var(--line);border-radius:.4rem;
background:var(--card);color:var(--fg)}
.pri{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600;
width:100%;padding:.8rem}
.dgr{border-color:var(--warn);color:var(--warn);background:transparent}
.warn{border:2px solid var(--warn);border-radius:.5rem;padding:.8rem 1rem}
.sw{display:inline-block;width:1.1em;height:1.1em;border-radius:.25em;
border:1px solid var(--line);vertical-align:-.18em;margin-right:.4em}
.tw{overflow-x:auto}
table{width:100%;border-collapse:collapse;font-size:.92rem}
td,th{padding:.5rem .3rem;text-align:left;border-bottom:1px solid var(--line)}
.sect{margin:1.6rem 0}
.details{display:none}
.sect input:checked~.details{display:block}
.sect:target .details{display:block}
.nav a{display:block;padding:.8rem .2rem;border-bottom:1px solid var(--line);
text-decoration:none;color:var(--fg)}
.nav a:after{content:'\203A';float:right;color:var(--mut)}
.item{display:block;padding:.8rem .2rem;border-bottom:1px solid var(--line);
text-decoration:none;color:var(--fg)}
.bk{display:inline-block;margin:1.5rem 0 0;font-size:.9rem}
```

**Estimated size: ~1,750 bytes** as written (whitespace included; the line
breaks above are for readability and can be stripped for ~150 bytes). Stored
once in flash, served once per week per browser, and **counted against no page
buffer**.

Notes on specific choices:

- `system-ui,sans-serif` rather than bare `sans-serif`: gives SF on iOS and
  Roboto on Android instead of Helvetica/Arial. 11 bytes, paid once.
- `max-width:30rem` preserves the existing 480 px measure.
- `padding-bottom:3rem` so the last control clears a phone's home indicator.
- Dark mode via `prefers-color-scheme` costs ~135 bytes and matters: job A
  happens at night. There is no toggle — the page has no state and no
  JavaScript, so it follows the phone.
- `.nav a:after{content:'\203A'}` is a text chevron, not an image (D2).
- `.item` replaces the current `a.item{padding:.35em 0}` — the address-picker
  rows go from ~28 px to ~44 px, meeting D5.

### 6.4 Type, spacing, states

| | |
|---|---|
| Base | 16 px / 1.45, system stack |
| Scale | h1 1.3rem · h2 1.05rem · h3 0.95rem · body 1rem · note 0.87rem · `.big` 1.5rem |
| Rhythm | Section gap 1.7rem · field gap 0.9rem · paragraph 0.55rem |
| Measure | 30rem (480 px) max |
| Focus | Browser default. Do **not** set `outline:none` — keyboard users on the laptop path have no other affordance. |
| Disabled | Not used. Nothing in this UI is disabled-but-visible; it is either present or absent. |
| Primary action | `.pri` — one per page, full width, accent fill. |
| Destructive | `.dgr` — outlined in `--warn`, never filled. A filled red button invites the tap it should discourage. |
| Warning block | `.warn` — the existing factory-reset box, kept, restyled to variables. |

---

## 7. Page-by-page layout decisions

Markup below is the proposed output, not the C. Attribute quotes are omitted
where HTML permits (`class=card`), which is valid and saves 2 bytes per
attribute across every page — but note that **any attribute holding escaped
user data keeps its single quotes**, because `html_escape_attr()`'s output is
only safe inside a quoted attribute.

### 7.1 `/` — the status page

**Job:** A. Answers "which bin?" above the fold, on a phone, at night.

**Header, all pages served by `web_server.c`** (~150 bytes, was ~403):

```html
<!DOCTYPE html><html><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<link rel=icon href=/favicon.ico type=image/svg+xml>
<link rel=stylesheet href=/s.css><title>Bin Light</title></head><body>
```

**Configured, collection known (S6):**

```html
<h1>Bin Light</h1>
<div class=card>
  <p class=note>Next collection</p>
  <p class=big>Tuesday 4 August</p>
  <p><span class=sw style=background:#ff9600></span>Yellow
     <span class=sw style=background:#00ff00></span>Green</p>
  <p class=note>The light comes on the night before, from 3:00 pm.</p>
</div>
<form method=POST action=/test><button class=pri>Show me the next colour</button></form>
<nav class=nav>
  <a href=/api-test>My bins</a>
  <a href=/settings>Settings</a>
</nav>
<p class=note>Maribyrnong &middot; 12 Example St, Footscray</p>
```

**Unconfigured (S1):**

```html
<h1>Bin Light</h1>
<div class=card>
  <p class=big>Let's set your bins up</p>
  <p>It takes about a minute. You'll need to know which council you're in.</p>
</div>
<p><a class=pri href=/api-setup>Set up my bins</a></p>
<nav class=nav><a href=/settings>Settings</a></nav>
```

*(`.pri` on an `<a>` needs `display:block;text-align:center;text-decoration:none`
added to the `.pri` rule — +55 bytes in the stylesheet, paid once.)*

**Decisions and rationale:**

| Decision | Why |
|---|---|
| Status card is first and is the only thing above the fold | The whole point. Job A is weekly and urgent; everything else is not. |
| Colour **names** beside swatches, not just swatches | Colour alone fails for colour-blind users and in a dark hallway. The name comes free from `nearest_preset_index()`, which already exists ([web_server.c:228](main/web_server.c:228)). |
| Swatches print the stored RGB verbatim | §6.2. Screen and light must agree. |
| "Show me the next colour" hidden when `!next.known` | `schedule_test_trigger()` is documented as a no-op when the next collection is unknown ([schedule.h:137](main/schedule.h:137)). A button that does nothing teaches the user the device is broken. Presentation-only: the handler is unchanged. |
| Council + address as a quiet footer line | Confirms the light is set up for *your* house without competing with the answer. |
| Only two nav rows | Every additional row is a decision the user has to make weekly. Firmware, Wi-Fi and factory reset live one level deeper, under Settings, where they belong. |
| No auto-refresh | A `<meta refresh>` on a page with no forms would be harmless, but the page is the one place a user lands repeatedly; a reload that fires while they are reading is worse than stale minutes. `/update`'s progress poll keeps its refresh — that page has a genuinely changing number. |
| No live API fetch | `SPEC.md` §3.3 records the deliberate decision that `/` renders from saved config with no live fetch "so the home page stays fast". Preserved. |

**Estimated size:** header 150 + card 340 (S6, worst case with two long colour
names) + button 110 + nav 130 + footer line 160 escaped ≈ **~900 B**;
worst case with a maximally-escaped council name and 63-char address ≈
**~1.4 KB**. Compare 8378–14592 today.

### 7.2 `/settings`

**Job:** C. Everything rarely touched, in one `/save` form (D9) plus the
separate POST forms for the device actions.

```html
<h1>Settings</h1>
<a class=bk href=/>&larr; Back</a>
<form method=POST action=/save>

<h2>When the light comes on</h2>
<p class=f><label for=br>Brightness</label>
  <input type=range id=br name=brightness min=10 max=255 value=200></p>
<p class=f><label for=st>Comes on at</label>
  <input type=time id=st name=start value=15:00></p>
<p class=f><label for=du>Stays on for</label>
  <input type=number id=du name=duration_hours min=1 max=23 value=20> hours</p>
<p class=note>On the night before a collection. It can run past midnight.</p>

<h2>How the light shows colours</h2>
<p class=f><label for=lm>Colours</label><select id=lm name=light_mode>
  <option value=0 selected>Show one colour</option>
  <option value=1>Show two colours</option></select></p>
<p class=f><label for=sd>Second light, ordinary week</label>
  <select id=sd name=secondary_default_color>…</select></p>
<p class=note>With two colours, the second light shows general rubbish (red)
unless two different bins are due the same night.</p>

<h2>Your bin days</h2>
<div class=sect>
  <p><b>Maribyrnong</b><br>12 Example St, Footscray</p>
  <p><label><input type=checkbox id=ae name=api_enabled checked>
     Get bin days from my council</label></p>
  <p><a href=/api-setup>Change council or address</a> &middot;
     <a href=/api-test>My bins</a></p>
</div>

<h2>Set bin days by hand</h2>
<div class=sect id=byhand>
  <p class=note>A backup for when your council's days aren't available.
     While your council's days are working, nothing here is used.</p>
  <p><label><input type=checkbox id=me name=enabled> Set bin days by hand</label></p>
  <div class=details>
    …bin night, then three colour blocks…
    <details><summary>How the colour rules work</summary>…</details>
  </div>
</div>

<h2>Date and time</h2>
<p class=f><label for=tz>Timezone</label><select id=tz name=tz>…</select></p>
<details><summary class=note>Somewhere else</summary>
  <p class=f><label for=tzc>Timezone code</label>
    <input type=text id=tzc name=tz_custom value='AEST-10AEDT,M10.1.0/2,M4.1.0/3'></p>
  <p class=note>Only used when the list above is set to "Somewhere else".</p>
</details>

<p><button type=submit class=pri>Save</button></p>
</form>

<h2>The light itself</h2>
<p>Connected to <b>MyHomeWiFi</b>. Version <b>1.0.6</b>.</p>
<nav class=nav>
  <a href=/update>Check for an update</a>
</nav>
<form method=POST action=/reboot><p><button>Restart the light</button></p></form>
<p class=note>Nothing is lost. Same as holding the button for 3 seconds.</p>
<form method=POST action=/wifi-forget><p><button>Move to a different Wi-Fi</button></p></form>
<p class=note>Restarts into setup mode. Everything else is kept.</p>
<form method=POST action=/factory-reset><p><button class=dgr>Factory reset&hellip;</button></p></form>
<p class=note>Erases everything, including Wi-Fi. You'll be asked to confirm.</p>
```

**Decisions and rationale:**

| Decision | Why |
|---|---|
| `<p class=f>` stacked fields replace the two-column `<table>` | Fixes §2.3 structurally, not with a media query. A stacked block has no minimum content width, so it cannot overflow at any viewport. Byte cost ≈ neutral: `<tr><td>x</td><td>` (22 B) → `<p class=f><label for=x>` (24 B). |
| "Comes on at" / "Stays on for" replace "On from" / "Turn off after" | Reads as one sentence about the light rather than two switch operations. |
| Timezone code inside `<details>` | It is the one genuinely technical control that must stay (`SPEC.md` §3.2 — newlib has no IANA tzdata, so a POSIX string is the only escape hatch). Hiding it behind a summary keeps it available without putting `AEST-10AEDT,M10.1.0/2,M4.1.0/3` in front of a new owner. **`<details>` hides it visually only — the bytes are still in the buffer (D3).** |
| `<details>` here, `.sect`+checkbox there | Deliberate split. `.sect` is used where the disclosure control **is a real form field** (`api_enabled`, `enabled`) — conflating "on" and "expanded" is elegant and is why the pattern exists. `<details>` is used for **pure disclosure** with no field behind it (the colour-rules essay, the timezone code). Do not convert one to the other. |
| Colour-rules explainer moves into `<details>` and is trimmed | The five-bullet worked example is 997 B of the page and is read once, if ever. Trim to ~500 B and collapse. |
| Device actions are *outside* the `/save` form | They already are — they POST elsewhere and forms cannot nest. Unchanged. |
| Four device sections become one **The light itself** | §2.4. Restart / Wi-Fi / update / reset are one idea to a user. |
| Factory reset stays last, stays two-step, stays outlined-not-filled | Two-step is settled (D8). Last and unfilled is presentation. |

**Estimated size:** ~7.6 KB worst case escaped — see §8.2.

### 7.3 `/api-setup` — "Where do you live?"

**Structure unchanged.** The state → council → address flow is verified against
five live endpoints and its GET-link threading is explicitly off-limits
(`SECURITY-REMEDIATION.md` §8). This is a copy, spacing and touch-target pass
only.

| Change | From | To |
|---|---|---|
| `<title>` / `<h1>` | Bin Collection API Setup | Where do you live? |
| Wizard framing (fresh device only) | *(none)* | `<p class=note>Step 1 of 2</p>` |
| Currently-configured line | `Currently configured: <b>X</b>` | `Set up for <b>X</b>` |
| Empty state | `No council/address configured yet.` | `The light doesn't know where you live yet.` |
| Section heading | `Set up a new council / address` | `Find your address` |
| State picker button | `Show councils` | `Show councils` *(keep)* |
| Council picker button | `Find my address` | `Find my address` *(keep — already good)* |
| Escape hatch heading | `Council not listed?` | `Council not listed?` *(keep)* |
| **Escape-hatch form** | subdomain text field + `Find my suburb` button | **Removed from the UI.** Replaced by: `Your council isn't one the light knows about yet. You can still use it — set your bin days by hand instead.` → link to `/settings#byhand`. See §4.5.1. |
| Lookup failures | see §5.3 | see §5.3 |
| Address rows | `a.item{padding:.35em 0}` | `.item` — ~44 px tap target (D5) |
| Merri-bek tip | *(keep verbatim)* | It is specific, correct, hard-won and cannot be shortened without losing the instruction. The format template (`3/85 EXAMPLE STREET BRUNSWICK 3056`) stays — `SPEC.md` §4 records why it is a template and not a real address. |
| Back link | `&larr; Back to schedule` | `&larr; Back` |

**Byte impact:** header −180 (external CSS), copy roughly neutral (the
escape-hatch rewrite is +40, the shorter headings −60), step chrome +45 on the
fresh path only. **Net ≈ −200 B.** `SETUP_HTML_BUF_SIZE` stays at 20000 with
its 7853 B of worst-case headroom intact.

### 7.4 `/api-test` — "Your bins"

**Job:** B step 2, then C. This page gains the colour mapping as its *only*
home and keeps its diagnostic table.

```html
<h1>Your bins</h1>
<a class=bk href=/>&larr; Back</a>
<p class=note>Step 2 of 2</p>            <!-- fresh device only -->

<h2>Which colour is which bin</h2>
<p>Pick a colour for each bin, and tick anything you don't want reminding
about. General rubbish goes out every week, so it starts off unticked.</p>
<form method=POST action=/api-test>
<input type=hidden name=type_count value=3>
<div class=tw><table>
<tr><th>Bin</th><th>Don't remind me</th><th>Colour</th></tr>
<tr><td>recycle<input type=hidden name=type0_name value='recycle'></td>
    <td><input type=checkbox name=type0_ignored></td>
    <td><select name=type0_color>…</select></td></tr>
…
</table></div>
<p><button type=submit class=pri>Save</button></p>   <!-- "Finish setup" when fresh -->
</form>

<h2>What Maribyrnong says is coming up</h2>
<div class=tw><table>
<tr><th>Date</th><th>Bin</th><th>Council's colour</th></tr>
<tr><td>Tue 4 Aug</td><td>recycle</td>
    <td><span class=sw style=background:#ffd700></span>#ffd700</td></tr>
</table></div>
<p class=note>Straight from the council, before your choices above are applied.</p>
```

**Decisions and rationale:**

| Decision | Why |
|---|---|
| Mapping first, raw data second | Order by job. During setup the mapping is the task; the raw table is evidence. Today the raw table comes first and the mapping is below it. |
| The mapping form lives **only** here | §2.5, §4.3. Deletes the duplicate, deletes `append_type_mapping_anchor()` / `append_type_mapping_rows()` / the `form_id` parameter of `append_color_select_for()`, and takes 5137 worst-case bytes off `/`. |
| The `form=` pattern is documented in D1 but its instance is removed | Honest note: this design deletes the only current use of a good, well-commented pattern. It is recorded in D1 so it is available the next time nesting bites, rather than lost. |
| `redirect_to` becomes constant | With no home-page mapping form, the only caller posts from here. The allowlist check ([web_server.c:1858](main/web_server.c:1858)) can collapse to `/api-test` — or, per §9.1, `/` when finishing setup. **This is the one place the mapping-move touches a handler's logic; see §9.2.** |
| Dates as `Tue 4 Aug`, not `2026-08-04` | ISO dates are for logs. |
| `#ffd700` kept beside the council's swatch | Diagnostic value for the engineer (§1), harmless to the user, and already correct. Under a heading that says *what the council says*, not *what your light does*. |
| Both tables wrapped in `.tw` | D5 — genuinely tabular, so it may scroll inside its own box, but the page body must not. |
| `waste` row not special-cased visually | The copy explains it once. A special row style would need its own CSS for one case. |

**Estimated size:** header 150 + mapping copy 320 + mapping table 8 types
escaped ~5200 + upcoming table 12 rows ~1400 + surrounding copy 400 ≈
**~7.5 KB worst case**. Under a proposed 12288 `HTML_BUF_SIZE` with ~4.8 KB
spare — **but this page has never been rendered by the harness (§2.9), so the
estimate is arithmetic, not measurement.** Rendering it is step 1 of phase 2.

### 7.5 `/update` — "Software update"

Small page, small changes.

| Change | From | To |
|---|---|---|
| `<title>` / `<h1>` | Firmware Update | Software update |
| Version line | `Running version: <b>1.0.6</b>` | `Version <b>1.0.6</b>` |
| Check failed | `Couldn't check for updates. The light couldn't reach the update manifest. Check its internet connection and try again.` | `Couldn't check for an update.` / `The light needs an internet connection for this. Everything else keeps working.` |
| Up to date | `Up to date. The published version is 1.0.6, which is what this light is running.` | `Up to date.` / `Version 1.0.6 is the latest.` |
| Available | `Version 1.0.7 is available.` | *(keep)* |
| Install button | `Download and install` | `Install it` |
| Auto checkbox | `Install updates automatically` | *(keep)* |
| Progress / success / rollback copy | | *(keep verbatim — it is already plain, reassuring and accurate, and it is the copy someone reads while anxious)* |
| Back link | `&larr; Back to schedule` | `&larr; Back to settings` (→ `/settings`) |
| CSS | inline `<style>` incl. `code{background:#eee}` | `/s.css` |

**The `<meta http-equiv=refresh>` polls stay exactly as they are** (3 s while
running, 15 s after success). No JavaScript, and this is the one page with a
number that genuinely changes.

**Byte impact:** −250 (CSS out) + ~−80 (shorter copy) ≈ **−330 B**. Page goes
from 1149–1525 to ~850–1200.

### 7.6 `/factory-reset`, `/reboot`, `/wifi-forget`

**Semantics unchanged. Two steps stay two steps (D8).**

| Page | Change |
|---|---|
| `/factory-reset` confirm | `.warn` box restyled to variables; list items reworded (`the council and address setup, and the bin colour mapping` → `your council, your address and your bin colours`; `the manual / fallback schedule and its colour rules` → `any bin days you set by hand`; `brightness, on-time, light mode and timezone` → `brightness, when the light comes on, and your timezone`). Confirm button keeps its full sentence: `Yes, erase everything and restart`. |
| `/factory-reset` done, `/wifi-forget` | Both explain rejoining `binlight-XXXX`. **Keep this copy nearly verbatim** — it is the instruction that gets a stranded device back, and it correctly gives both `binlight.local` and `192.168.4.1`. Restyle only. |
| `/reboot` | `Restarting` — keep. `You'll see the LEDs run their startup colour test` → `You'll see the lights run through their colours`. |
| All three | inline `<style>` → `/s.css`; `<link rel=icon>` kept. |

**Byte impact:** −150 to −230 each. All three are ≤ 1.3 KB today and stay far
under any buffer.

### 7.7 The AutoAP setup page (`wifi_manager.c`)

**The first thing a new owner ever sees, and the one place with no byte
budget (D6, §2.10).**

Keeps its own inline `<style>`. Changes are copy and structure only, target
**≤ 200 B net**:

| Change | From | To | Δ |
|---|---|---|---|
| `<h1>` | `Bin Light Setup` | `Set up your bin light` | +6 |
| Intro | `Connect this bin light to your home Wi-Fi. Pick your network, enter its password, and press Connect.` | `Step 1 of 3 — get the light onto your Wi-Fi.` / `Pick your network below and enter its password.` | +5 |
| Add a "what happens next" line | *(none)* | `<p class=note>Then you'll tell it which council you're in, and which colour means which bin.</p>` | +105 |
| Diagnostic footer | `Networks found: %d. Reload this page to scan again.` | `Can't see your network? Reload this page to look again.` | −20 |
| Password hint | `Leave the password empty only if your network has none.` | *(keep)* | 0 |
| Success page | `This setup network will now disappear and the light will carry on with its normal job. Reconnect your phone to your own Wi-Fi, then find the light at http://binlight.local to set up its schedule.` | …`then open http://binlight.local to finish setting up.` | −25 |
| Failure page | `Double-check the password (this is almost always the password) and that the network is a 2.4GHz one — this device can't see 5GHz-only networks.` | *(keep verbatim — best troubleshooting copy in the project)* | 0 |
| Inline CSS | as-is | add `button{font:inherit;padding:.6rem 1rem}` and bump `p.field label` spacing | +45 |

**Net ≈ +116 B.** Acceptable *only if* the buffer arithmetic in §2.10 is
resolved first — see §9.4 and §12.

Removing `Networks found: %d` is deliberate: it is a diagnostic number the user
cannot act on (rule 5), and the actionable half of the sentence ("reload to
scan again") is kept.

---

## 8. Budget: buffers and flash

### 8.1 Where things stand today

```
Build:            1,323,344 B image · 2,031,616 B slot · 708,272 B (35%) free
HTML_BUF_SIZE          16384    largest page 14592  (home-worst-case-escaped)
SETUP_HTML_BUF_SIZE    20000    largest page 12147  (api-setup-worst-case-escaped)
PROV_HTML_BUF           6144    never measured — see §2.10
max_uri_handlers          14    12 in use
```

### 8.2 Projected page sizes

| Page | Today (worst case) | Proposed | Δ |
|---|---:|---:|---:|
| `/` | 14592 | ~1400 | **−13.2 KB** |
| `/settings` | — | ~7600 | new |
| `/api-test` | not measured (~2.4 KB + mapping) | ~7500 | +mapping |
| `/api-setup` | 12147 | ~11900 | −250 |
| `/update` | 1525 | ~1200 | −325 |
| `/factory-reset` | 1246 | ~1050 | −196 |
| AutoAP `/` | not measured | +116 | +116 |

`/settings` derived from the §2.6 table: Preferences 2269 + Manual 2818 +
explainer 997 trimmed to ~500 + Wi-Fi 507 + Firmware 265 + Restart 254 +
Factory reset 354 + a bin-days summary ~400 + header 150 + new headings ~250
≈ **7567 B**.

### 8.3 Proposed buffer changes

| Buffer | Now | Propose | Rationale |
|---|---:|---:|---|
| `HTML_BUF_SIZE` | 16384 | **12288** | Largest consumer becomes `/api-test` at ~7.5 KB → 4.8 KB headroom, better than today's 1.8 KB. **Returns 4 KB of heap at every render** of `/`, `/settings`, `/api-test` and `/update`. |
| `SETUP_HTML_BUF_SIZE` | 20000 | **20000** | Unchanged. `/api-setup` barely moves and its 12147 B worst case is dominated by 12 Merri-bek address links at ~800 B each — a functional cost, not a presentational one. |
| `PROV_HTML_BUF` | 6144 | **see §9.4** | Not a UI decision. The arithmetic in §2.10 says it is already thin; resolving it belongs with whoever owns the provisioning server's robustness. |
| `max_uri_handlers` | 14 | **16** | Two new GET routes land exactly on the cap, and `httpd_register_uri_handler()`'s return is unchecked, so overflowing it fails silently. |

**Net RAM: −4096 B peak heap per page render**, from lowering
`HTML_BUF_SIZE`. This is the single strongest technical argument for the IA
split, and it is worth more than the flash it costs (D4).

> Every number in §8.2 is an estimate derived from §2.6's measurements.
> `HTML_BUF_SIZE` must not actually be lowered until the harness renders
> `/settings` and `/api-test` and asserts the figures. Lowering it on the
> strength of arithmetic would be exactly the silent-truncation failure D3
> exists to prevent.

### 8.4 Flash

| | Δ |
|---|---:|
| `/s.css` stylesheet literal | +1750 |
| Eight inline `<style>` + `<head>` blocks removed | −2400 |
| Eight `<link rel=stylesheet>` tags added | +360 |
| `append_type_mapping_anchor()` + `append_type_mapping_rows()` removed | ~−700 |
| `/settings` page literals (mostly moved, some new headings) | +300 |
| Status card + eight state strings on `/` | +900 |
| Reworded copy across all pages | ~±0 (plain copy tends shorter) |
| **Net** | **≈ +200 B** |

Against 708,272 B free. **Flash is not the binding constraint here; buffers
are** (D4). The §2.7 duplication is worth removing for maintenance and for
buffer relief, not for flash.

---

## 9. Proposed behaviour changes (need sign-off)

**Nothing in this section is approved by this document.** Each item alters what
the device does, not how it looks. They are listed separately so they cannot be
absorbed into a visual refactor. If accepted, each belongs in `SPEC.md`.

### 9.1 Redirect targets change

| Handler | Today | Proposed | Why |
|---|---|---|---|
| `/api-setup` `step=save` / `step=bsave` | `303 → /` | `303 → /api-test` **when the device was not previously configured**, else `/` | Makes the first-run path continue into step 2 instead of dumping the user on a home page whose bins aren't mapped yet. |
| `/save` | `303 → /` | `303 → /settings` | The form now lives on `/settings`; returning to `/` would hide whether the save took. |
| `/api-test` POST `redirect_to` | allowlist `/` or `/api-test` | `/api-test`, or `/` on "Finish setup" | The home-page mapping form that needed `/` is removed. |

**Risk:** low, all three are `Location` headers. **But** the `/api-setup`
variant needs `waste_api_config_complete()` evaluated *before* the config is
overwritten, which is a real ordering requirement in code that is otherwise
off-limits (`SECURITY-REMEDIATION.md` §8 says don't disturb the wizard).
Recommend implementing it as a single `bool was_configured` captured at the top
of the handler, and nothing else.

### 9.2 `/api-test` POST loses its `redirect_to` allowlist branch

Consequence of removing the home-page mapping form. The allowlist
([web_server.c:1858](main/web_server.c:1858)) exists to stop an
attacker-chosen redirect; simplifying it removes code that is currently
defensive. **Recommendation: do not remove it.** Keep the allowlist and add
`/settings` to it if needed. Cheaper to keep three `strcmp`s than to reason
about it again later.

### 9.3 `/` calls `schedule_get_next_collection()` and `time_sync_is_valid()`

Strictly this is *presentation* — both are documented read-only
([schedule.h:103](main/schedule.h:103)) and touch no persisted or in-RAM state,
and neither performs a network fetch. It is flagged anyway because it is a new
call on the httpd task and it *looks* like new functionality.

**Not proposed:** plumbing the bin *type name* (`"recycle"`, `"organic"`)
through to `/`. `schedule_next_t` carries colours only; adding a name field is
a header change and therefore behaviour. The status card names colours instead,
which is free and matches the user's post-setup mental model ("yellow light =
yellow bin"). Revisit only if the owner wants type names on the home page.

### 9.4 ⚠️ `PROV_HTML_BUF` may be overflowable by SSIDs alone — found, not fixed

**This is the most serious thing found in phase 1 and it is not a UI issue.**

Arithmetic in §2.10: 15 scanned SSIDs of 32 all-quote bytes escape to 6150 B of
`<option>` markup against a 6144 B buffer, before any of the page's other
~1080 B. `prov_append()` clamps silently, so the page truncates and loses the
password field and the **Connect** button. A device in such an RF environment
cannot be provisioned, silently, with no console.

Adversarial worst case, never observed; ordinary SSIDs cost ~40 B each. But it
is a first-run, no-way-back failure, and this document's §7.7 adds ~116 B to
that page — so it must be settled first.

**Options, in order of preference:**

1. Cap the total escaped option bytes and stop adding options at a budget
   (~4 KB), rather than at a fixed count of 15. Bounded by construction.
2. Reduce `SCAN_MAX_SHOWN` from 15 to 10 — still 4100 B worst case, still
   thin.
3. Raise `PROV_HTML_BUF` to 8192 (+2 KB heap, only during provisioning).

**Recommendation:** option 1, plus a rendered size assertion in the harness.
This belongs to whoever owns provisioning robustness, not to the UI pass, and
`SECURITY-REMEDIATION.md` §B3 explicitly fences the provisioning server off
from the security work — so it currently has no owner. **Flagging it to the
owner rather than acting on it.**

### 9.5 ⚠ Set the timezone automatically from the chosen council

**Owner's direction (2026-07-28), and explicitly acknowledged as not strictly
UI. This is a behaviour change and needs to land in `SPEC.md` §3.2.**

`SPEC.md` §4 records an unset timezone as one of two things that leave a
freshly-reset light showing nothing at all. Asking a recipient to find and set
it is exactly the kind of step that strands a device. The council they just
picked already implies it.

**The mapping is small**, because `STATE_ORDER` is currently only
`{VIC, NSW, QLD, TAS}` ([councils.c:81](main/councils.c:81)) — SA, WA and NT
are not supported yet (`SPEC.md` §3.13.2). That is **two distinct TZ strings**:

| State | POSIX TZ | Matches preset |
|---|---|---|
| VIC, NSW, TAS | `AEST-10AEDT,M10.1.0/2,M4.1.0/3` | Melbourne / Sydney / Canberra / Hobart |
| QLD | `AEST-10` | Brisbane |
| *(SA, when added)* | `ACST-9:30ACDT,M10.1.0/2,M4.1.0/3` | Adelaide |
| *(NT, when added)* | `ACST-9:30` | Darwin |
| *(WA, when added)* | `AWST-8` | Perth |

**Where the table lives:** a `settings_tz_for_state(const char *state)` helper
in `settings.c`, which already owns the TZ. **Not in `councils.c`** —
`SECURITY-REMEDIATION.md` §8 fences that file off, and keying on the existing
`council_t.state` string needs no change there at all.

**Call site:** the `step=save` and `step=bsave` branches of
`api_setup_get_handler()`, guarded on `council != NULL`.

**Overwrite policy — recommend: always overwrite, and say so on screen.**
There is no "was this ever set by the user" flag and adding one is an NVS
schema change (§9.8 below). Always overwriting is right for this audience: a
correct timezone derived from the council beats preserving a rare deliberate
override, and the override is one tap away in Settings. To stop it being a
*silent* overwrite, the finish step reports it:

> Timezone set to **Melbourne / Sydney / Canberra / Hobart**. Change it in
> Settings if that's wrong.

**Two things this fixes for free:**

1. **It resolves §11.1.** The timezone no longer needs to be a wizard step or
   a nag on the finish screen — it disappears from onboarding entirely.
2. **Removing the subdomain escape hatch (§7.3) makes it total.** That was the
   one path that reached `step=save` with no `council_t`, and therefore no
   state to derive a zone from. With it gone, every automatic setup has a
   council, so every automatic setup gets a timezone.

**Residual risk:** a user on a state border, or one who wants a genuinely
custom POSIX string, gets clobbered if they later change address. Rare,
visible (per the message above), and one tap to correct.

### 9.6 A display value per colour preset (small struct addition)

Required by §6.2, on the owner's direction. `color_preset_t` gains a
`const char *display` field holding a UI-only hex string; `.sw` swatches print
that instead of the stored RGB.

**Classified as presentation, flagged anyway because it edits the same struct
as the calibrated values.** It adds ~60 bytes of flash (4 pointers + 4 short
strings) and changes no emitted colour, no `<option value>`, no parsed value
and no NVS byte. A save round-trip is byte-identical to today's.

**Interaction worth knowing:** `SPEC.md` §4 already plans to move
`COLOR_PRESETS` + `nearest_preset_color()` out of `web_server.c` into a shared
colour module, because §3.13 needs name→RGB mapping anyway. The display field
should move with them rather than being added twice.

### 9.7 Removing the `waste-info.com.au` subdomain escape hatch

Owner's direction (2026-07-28). Recorded here rather than in §7 alone because
it **withdraws a capability**, not just a control.

`SPEC.md` §3.3 describes the free-text subdomain field as deliberate
flexibility: any council on the Impact Apps platform works without a firmware
change. Removing the form means a recipient can no longer configure an unlisted
council on that platform.

**Why it is nevertheless right:** the field asks a non-technical user to find
their council's bin-day lookup page and extract a subdomain from its URL.
That is the single most jargon-dense control in the product, and the audience
(§1) cannot use it. It serves the owner, not the recipient.

**Where the capability goes instead:** adding a council to `COUNCILS[]` and
pushing an update. `SPEC.md` §1.1 already treats council breakage as
"fix at leisure and push"; adding a council is the same shape of fix, and OTA
is proven end to end. This trades a control nobody can use for a release
nobody has to think about.

**Recommendation on the handler:** *keep the `step=locality` branch*, just stop
linking to it. It costs nothing to leave in place, remains reachable by typing
a URL for the owner's own debugging, and deleting it is a larger change to
wizard code that `SECURITY-REMEDIATION.md` §8 asks not to disturb.

**Consequence to accept:** `SPEC.md` §3.3's flexibility claim becomes
owner-only and should be reworded when this lands.

### 9.8 Not proposed, recorded so it is not re-proposed

- **A "has the timezone ever been set" flag.** `settings_get_tz()` returns the
  stored value or the default, indistinguishably. Detecting "never configured"
  would need a new NVS key — an NVS schema change, and those are settled (D8).
  Instead, `/api-test`'s finish step carries one line pointing at the timezone
  in Settings (~90 B, fresh-device path only). See §11.1.
- **Any change to `COLOR_PRESETS`.** §6.2, §10.7.
- **Any change to the two-step factory reset, button semantics, OTA restart
  semantics, or the `/api-setup` GET-link flow.**

---

## 10. Rejected ideas, and why

Recorded so they are not re-proposed. Grouped by what killed them.

### Rejected on the no-JavaScript rule (D1)

| Idea | Why it dies |
|---|---|
| Live countdown to the next collection ("in 2 days, 4 hours") | Needs a clock in the browser. A server-rendered relative phrase ("tomorrow night") gets 90% of the value for zero JS — adopted instead (§5.3, S4–S6). |
| Address autocomplete on `/api-setup` | Needs XHR. The existing search-then-pick flow is the no-JS answer and works against five live councils. |
| A colour picker | Needs JS or `<input type=color>`, which would let a user pick a value outside `COLOR_PRESETS` — breaking the hardware calibration (D8) as well as the enum. `<select>` of four presets is correct. |
| A real OTA progress bar | `ota_get_message()` returns a string, not a percentage. Parsing a percentage out of it in the page is JS; producing one is a behaviour change. The 3-second meta-refresh stays. |
| `:target`-based tab UI to fake multiple pages on one route | Works without JS, but breaks the back button, does not survive a form POST, and — crucially — **saves no buffer** (D3), since every tab's bytes are still emitted. Splitting into real routes does both jobs. |
| Client-side form validation | Server-side clamping already exists (`parse_hhmm`, `min`/`max` attributes). Nothing to add without JS. |

### Rejected on the no-external-assets rule (D2)

| Idea | Why it dies |
|---|---|
| A web font (Inter, system-ui fallback) | No internet path from the browser to a CDN. A `<link>` to fonts.googleapis.com hangs. |
| An icon font, or Feather/Lucide SVG sprites | Same, plus flash cost. A `\203A` chevron in CSS `content` costs 6 bytes. |
| Photographic or raster bin illustrations as `data:` URIs | Even a 2 KB PNG per bin colour is 8 KB of flash for decoration. Rejected. |
| Inline SVG wheelie-bin icons per colour | More defensible — the favicon proves the pattern — but ~250 B per icon in flash and, if inlined per row, in the *page buffer* too. A CSS `.sw` rounded square costs 0 additional bytes per use. Rejected for the swatch; reconsider only if the owner wants a stronger identity and the buffer arithmetic in §8 lands with room to spare. |

### Rejected on the buffer rule (D3)

| Idea | Why it dies |
|---|---|
| "Just collapse the settings into `<details>` and keep one page" | `<details>` hides bytes from the eye, not from the buffer. The page would still be 14.5 KB worst case and `HTML_BUF_SIZE` would stay at 16384. Splitting routes is the only lever that returns heap. |
| A 4-week calendar grid on `/` | Needs a live API fetch on the home page, which `SPEC.md` §3.3 deliberately rules out to keep `/` fast, and would add ~1.5 KB to the page that must stay smallest. The data already has a home: `/api-test`. |
| Showing all 8 possible `type_rules` rows always, for a stable layout | Worst case is already the sizing case at 5137 B. Padding the common case toward it for symmetry is backwards. |
| Repeating the status card on `/settings` | ~340 B on a page that is the new sizing constraint, to restate something one tap away. |

### Rejected on other technical grounds

| # | Idea | Why it dies |
|---|---|---|
| 10.7 | **Adjust `COLOR_PRESETS`' emit values so swatches look better on screen** — a purer yellow, a softer green | **Hardware calibration, not taste** (`SPEC.md` §2, D8). `(255,150,0)` reads correctly through the printed PLA enclosure because WS2812 green is far more luminous than red at equal duty; a purer value reads green. Screen appearance is *not* a reason to touch these values. **This is still rejected** — but note the actual problem it was reaching for is solved properly by the separate **display palette** in §6.2: the screen shows `#f9c22e`, the LED emits `(255,150,0)`, and neither constrains the other. If someone proposes editing the emit values for screen reasons, they want §6.2 and have not read it. |
| 10.8 | Convert the `/api-setup` `save`/`bsave` GET links to POSTs while restyling | `SECURITY-REMEDIATION.md` §8 explicitly rejects this: the query-string threading is fiddly and works, and these steps only change settings, which is cheap to correct. Not the UI pass's call to reopen. |
| 10.9 | Put `reject_cross_origin()` on the new `/settings` and `/s.css` GET handlers "for consistency" | `SECURITY-REMEDIATION.md` §B2: *do not apply it to any GET handler*. See D7 for the reconciled rule. Getting this backwards in either direction is silent. |
| 10.10 | Add `reject_cross_origin()` to the AutoAP provisioning handlers | `SECURITY-REMEDIATION.md` §B3 — leave the provisioning server alone. Different server, temporary AP, most fragile surface in the project. |
| 10.11 | Link `/s.css` from the AutoAP page too | D6. A stylesheet fetch inside an OS captive-portal mini-browser is exactly where a second request is least dependable, and the one page that must never render broken. Deliberate duplication. |
| 10.12 | Replace the `.sect` + `input:checked ~ .details` pattern with `<details>` everywhere | Where the disclosure control **is** a form field (`api_enabled`, `enabled`), conflating "on" with "expanded" is correct and free. `<details>` would need a *separate* checkbox plus a `<summary>`, i.e. more markup and a second control for one idea. Adopted only for pure disclosure (§7.2). |
| 10.13 | Drop the `.sect` wrapper as redundant | It is load-bearing. A bare `input:checked ~ .details` matches every later `.details` on the page. Already commented in the source; repeated here because it looks redundant. |
| 10.14 | Add a password or PIN to the web UI now that there is a "settings" page | Rejected by the owner in `SECURITY-REMEDIATION.md` §7B: *"These are gifts to non-technical people; setup friction is a real cost."* A settings page does not change that calculus. |
| 10.15 | Auto-refresh `/` every 60 s so the status card stays current | A refresh that fires mid-read is worse than a stale date, and the card's content changes at most daily. `/update`'s progress poll keeps its refresh because its number genuinely moves. |
| 10.16 | Rename the `/api-setup` and `/api-test` routes to plain-English URLs | Nobody types them, so they cost nothing in plain-English terms, and churning them would disturb the wizard's query-string threading (10.8). Titles and headings change; routes do not. |
| 10.17 | Show the bin *type name* ("Recycling") on `/` instead of the colour name | Requires a new field on `schedule_next_t` — a header change, therefore behaviour (§9.3). Colour names come free from the existing `nearest_preset_index()`. |
| 10.18 | Merge the two colour-mapping forms by keeping the home-page one and deleting `/api-test`'s | Backwards: the home-page copy is the expensive one (5137 B on the page that must be smallest) and is the reason the `mapform` hack exists. |

---

## 11. Unresolved — needs the owner or hardware

### 11.1 ~~Where does the timezone belong in the first-run path?~~ RESOLVED

**Resolved 2026-07-28 by the owner: set it automatically from the chosen
council.** See §9.5. The timezone leaves the onboarding path entirely rather
than becoming a step in it. What remains is not a UI question but the
overwrite policy, for which §9.5 recommends "always overwrite, and say so on
screen".

### 11.2 What are the two LEDs called, physically?

`SPEC.md` §2 records two daisy-chained WS2812s with a divider between them in
the printed enclosure, but not their orientation. The copy needs a name:
"first light / second light" is safe but vague; "top / bottom" or "left /
right" is far clearer if the enclosure fixes the orientation.

The spec uses **"the first light" / "the second light"** as a placeholder.
**Needs the owner or a look at the printed enclosure.**

### 11.3 Do the swatch colours read correctly on a phone?

`(255,150,0)` is orange on a screen and `(0,255,0)` is a harsh lime. §6.2
argues honesty beats prettiness — the swatch's job is to match the light. But
whether a user standing at the bin recognises the orange swatch as "the yellow
bin" is an empirical question about **the printed enclosure and a real phone**,
and cannot be settled from renders.

Related and unresolved in `SPEC.md` §4: it is not established whether the
colour-mapping form was re-saved after `(255,150,0)` was flashed, so the stored
rule may still hold `(255,255,0)`. **The swatch will faithfully show whichever
is stored** — which makes the home page a rather good diagnostic for that
wrinkle, and means any judgement about swatch colour must be made after a
re-save.

### 11.4 Is a new `/settings` route acceptable?

The IA in §4 rests on it. It costs one handler slot, ~300 B of flash and one
extra tap for job C, and it buys 4 KB of heap plus a genuinely distinct
first-run path. §10 records the single-page fallback if the owner would rather
not add a route.

### 11.5 `PROV_HTML_BUF` ownership

§9.4. Real, first-run, silent, and currently owned by nobody — the security
work fences the provisioning server off and the UI work has no mandate to
change buffer sizes there. **Needs an owner assigned.**

### 11.6 Not verifiable without hardware

- Whether the status card is legible at arm's length in the dark, in dark mode.
- Whether ~44 px address rows are comfortable one-handed in the setup wizard.
- Whether the AutoAP page renders correctly inside the iOS and Android captive
  portal mini-browsers, which have their own quirks and are not ordinary
  Safari/Chrome. This is the highest-risk untested surface in the design.

---

## 12. Implementation order for phase 2

Not a plan to execute now. Recorded so phase 2 starts from evidence.

1. **Close the rendering gap first (§2.9).** Extend `test/host/render_page.c`
   to render `/api-test` and the AutoAP provisioning page, and extend
   `run.sh`'s `check_page` assertions to cover them. **Do this before editing
   either page** — both are currently unmeasured, and the AutoAP one has a
   buffer question hanging over it (§9.4). Note `render_page.c` and `run.sh`
   are already modified in the working tree by the security pass; coordinate.
2. **Add the guard-count assertion (D7).** `reject_cross_origin(` call sites
   must equal `HTTP_POST` handler registrations in `web_server.c`. Cheap, and
   it is the thing that stops this refactor silently dropping a guard.
3. **Settle §9.4** with the owner before touching `wifi_manager.c` at all.
4. `/s.css` + the shared header. Mechanical, touches every page, no layout
   change yet. Verify sizes drop as predicted.
5. `/` — the status card and the eight states. The highest-value change and
   the one that fixes §2.1.
6. `/settings` — the new route, stacked fields (fixes §2.3), regrouping.
   Keep the whole `schedule_t` in one form (D9).
7. `/api-test` — take ownership of the mapping; delete the home-page copy,
   `append_type_mapping_anchor()` and the `form_id` parameter.
8. **Re-measure everything, then and only then lower `HTML_BUF_SIZE`** to
   whatever the harness proves is safe (§8.3).
9. `/api-setup`, `/update`, the destructive pages — copy passes.
10. AutoAP page, last, smallest, after step 3 has an answer.

Steps 5–7 are where §9.1's redirect changes land, so they need sign-off before
step 5.

---

*Written 2026-07-28 against working tree `ccc02bd`. Figures from
`./test/host/run.sh render` and `idf.py build` on that tree. `SPEC.md` remains
authoritative for behaviour.*
