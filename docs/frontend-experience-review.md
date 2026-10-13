# Frontend Experience Review — First Design Pass

**Status: Proposed for owner review.** This review examines the
[Frontend Experience Blueprint](frontend-experience-proposal.md) and recommends changes. It adopts
nothing. Exchange behavior comes only from [Exchange Rules](exchange-rules.md), component ownership
from [Architecture](architecture.md), and current completeness from
[Implementation Status](implementation-status.md). Accepted product choices belong in
[Product Direction](product-direction.md) once the owners approve them.

Visual proofs for this review live in [design/exchange-lab-direction-proofs.html](design/exchange-lab-direction-proofs.html).
The proofs use clearly labelled sample data. They are not engine evidence and do not show
implemented features.

Labels used below:

- **Fact** — confirmed from Implementation Status, source, or an adopted rule.
- **Rule** — adopted behavior in Exchange Rules.
- **Recommendation** — a product or design choice that needs owner approval.
- **Open** — an unresolved decision that this review does not settle.
- **Assumption** — something this review relies on but has not verified.

## 1. Critical review of the blueprint

### What is strong and should be kept

1. **One lesson loop.** *Brief → predict → act → watch → inspect → compare → continue* is a
   reusable spine. It lets one workspace serve many lessons and stops each lesson from becoming a
   one-off UI.
2. **Evidence discipline.** The blueprint refuses invented stages, refuses to label playback as live,
   and separates a local prediction from a submitted command. This is the project's most valuable
   design asset and the main thing that separates it from a decorative trading game.
3. **The failure-boundary table** (blueprint §8). Each boundary states what can be established.
   It is precise enough to become acceptance criteria for later incident lessons.
4. **Clickable stations before a walking avatar.** Movement costs travel time and asset work and
   teaches nothing about exchanges.
5. **Vertical slice first.** One complete mission through the whole navigation loop is the right
   way to fix art scale, scenario format, and inspection patterns.
6. **No profit leaderboard.** Rewarding explanation rather than simulated profit matches the
   product principles.
7. **Real-venue comparisons as a separate, labelled category** with a "not publicly known" state.

### Weak assumptions

| # | Assumption in the blueprint | Why it is weak | Recommendation |
|---|---|---|---|
| W1 | The public market is something to watch on a first visit. | **Fact:** Implementation Status records no generated scenario in the local showcase. The selected demo is a generated-only run, but nothing currently supplies its orders, so its book can be empty. A home scene whose secondary action opens an empty book teaches nothing. | Make a recorded lesson the first-visit anchor. Treat "quiet market" as a designed state of the public window, not an error. Do not make watching the demo the primary action until a generated scenario exists. |
| W2 | An anonymous visitor can "follow your first order". | **Rule:** a generated-only run refuses browser commands, and an interactive generated-only lesson protocol is **Open** (blueprint decision 5). The anonymous visitor cannot send anything. "Your order" therefore misdescribes a recorded lesson. | Use third-person framing in recorded lessons ("a buyer wants 4"). Reserve "your order" for My run. |
| W3 | The raw FIFO view in a lesson can carry over into My run. | **Rule:** another participant's raw orders, including generated liquidity in a guest run, are never exposed. A visitor who learns FIFO from a recorded generated-only lesson will see far less detail in their own run. The blueprint does not prepare them for that. | The lesson debrief states the difference before offering "Try it in my run": "In your own run you will see your orders and aggregate levels only." |
| W4 | The machine view begins at a browser gateway. | **Fact:** generated commands enter through a separate trusted generator entrypoint, then use the ordinary admission, journal, matching, and handoff chain. The existing six-stage guide starts at Gateway. A recorded generated-only lesson that shows Gateway would show a stage that command never crossed. | The machine lens draws the entry stage from the command's actual source: *Generator* for generated commands, *Gateway* for the guest's own browser commands. |
| W5 | Six places (Home, Mission board, Market, Engine, Incident bench, My lab) plus a field manual are the right top-level structure. | Engine and Field manual are ways of looking at the same object, not destinations. Making them places forces users to leave the market to understand it. That breaks the blueprint's own goal of moving "from a market result to the relevant engineering explanation". | Separate **places** (where data comes from) from **lenses** (how you look at it). See §2. |
| W6 | "Collected manual pages" are a reward. | Collecting implies locking. Locked explanations conflict with "a field manual that opens in context". | Never lock manual pages. Mark pages as read. Reward completed investigations only. |
| W7 | Learning progress can live in browser storage. | Sound for v1, but the returning visit must handle missing storage (private windows, cleared data) without breaking. | Keep the recommendation; design the "no saved progress" state explicitly. |

### Confusing transitions

1. **Market → Engine → Market.** The blueprint switches places to change perspective. Users lose
   their place in the book and must rebuild context. A lens switch keeps the selected object fixed
   on screen instead.
2. **Explanation pause versus run pause.** The blueprint names both, correctly, but does not separate
   them visually. They need different icons, locations, and wording (§2.4).
3. **Browser Back inside a lesson.** If each step pushes history, Back walks backwards through steps
   and traps users. If nothing pushes history, a shared link cannot open a step.
4. **Home while something is running.** "Returning home does not imply the server stopped" is
   stated but not shown. The home scene needs a visible sign that My run continues (§2.5).
5. **Reconnect.** **Rule:** the browser private stream has no automatic replay. A returning user who
   sees an empty results list may believe their orders vanished. The reconnect state must say what
   the screen can and cannot know.

### Missing interactions

- **Prediction input.** The blueprint says "predict" but not how. Recommendation: the visitor
  selects the object in the main visual (a resting order, a price level, a stage) rather than a
  separate multiple-choice list. This keeps attention on the book and teaches where to look.
  A text list is kept as the accessible equivalent.
- **Invite entry.** Guest lab entry is invite-only for the first beta, and the admission protocol is
  **Open**. The door to My run needs a designed "invite required" state that does not imply a
  feature that does not exist.
- **Guest identity on load.** **Fact:** the guest credential is an HttpOnly cookie, and
  `POST /v1/guest` creates or renews an identity. The page cannot detect an existing identity
  without calling that route. Calling it automatically on load would consume finite identity
  capacity for every visitor. Recommendation: only the explicit "Open my lab" action calls it; a
  non-secret local hint may relabel the door as "Your lab" for a returning browser.
- **Run capacity reached in My run.** **Rule:** `CapacityReached` cannot return to `Ready`, and
  replacing a retained stopped run requires confirmation naming the older `ExchangeRunId`. The guest
  run-control protocol is **Open**, so the prototype shows only the read-only state and explains
  what it means.
- **Glossary.** Terms such as *spread*, *maker*, *IOC*, and *sequence* need inline definitions on
  hover and focus, linked to the manual page.
- **Manual search** and **"where am I" orientation** inside the manual.
- **Settings** for motion, sound, text size, and single-key shortcuts (which must be possible to turn
  off).

### Unnecessary scope for the first prototype

| Item | Reason to defer |
|---|---|
| Incident bench as an interactive place | Needs a safe fault-injection protocol (**Open**) and real recorded evidence. Keep it as non-interactive scenery until the first incident lesson exists. |
| Real-venue statistics (for example the 2014 TMX Select latency figure) | Historical, venue-specific, and measured at a different boundary. It invites a false comparison with a laptop showcase. Comparison cards should begin with *rules* (price-time priority), not numbers. |
| Branch UI | Branch protocol is **Open**; the rules forbid presenting a simulated rewind as an executable branch. |
| Guide character, dialogue, and portraits | High writing and art cost; untested value. A short written brief does the same job. |
| Sound | Off by default anyway. Add after visuals and motion are stable. |
| Campus or multiple rooms | More environments before one lesson works. |
| Multi-instrument scenes | Phase 5 in Product Direction. |

## 2. Recommended experience structure

### 2.1 Places, lenses, and overlays

The core recommendation is to organize the product around **where the data comes from**, then let
users choose **how to look at it** without leaving.

**Places** — each has one data source and one provenance stamp:

| Place | Data source | Home-scene object | Stamp |
|---|---|---|---|
| Lessons | Recorded engine evidence from generated-only runs (sample data in the prototype) | Cork board of lesson cards above the desk | RECORDED LESSON |
| Public market | Live `/v1/public` stream of the selected demo run | The board across the street, seen through the window | PUBLIC DEMO |
| My run | The guest's own isolated run and private stream | A door with a key tag | MY RUN |

**Lenses** — available inside any place, scoped to what that place may show:

| Lens | Shows | Limits by place |
|---|---|---|
| Market | Book, levels, trades | Public market and My run show aggregate levels; only Lessons may show other participants' raw orders, because they come from generated-only runs. |
| Machine | The command path for the selected object | Lessons: recorded stages. My run: own command stages only where evidence exists. Public market: run-level status only. |
| Structure | Price levels, FIFO at one level, the active-order index | Always a **schematic** stamp unless the view is a real capture. |

**Overlays** — open on top of any place without changing it: Field manual, comparison drawer,
settings. They slide in beside the work and never cover the selected object.

The cabinet in the home scene opens the Machine lens on a schematic tour. It is a shortcut, not a
fourth data source.

### 2.2 Provenance stamps

Every panel that shows data carries exactly one stamp. Stamps differ by shape, icon, and words, not
color alone.

| Stamp | Meaning | Shape and icon |
|---|---|---|
| PUBLIC DEMO · LIVE | Current public view of the generated demo. Same for everyone. | Round window frame, broadcast dot |
| MY RUN · LIVE | Your isolated run. Only you see it. | Key tag |
| RECORDED LESSON | Playback of retained evidence. Not live. Shows the recording's run and build. | Reel with a play triangle |
| SCHEMATIC | A drawing that explains a mechanism. Not runtime state. | Dashed blueprint corner |
| EVIDENCE | A value read from authoritative output: a sequence number, journal position, run status. | Solid ledger tab with a check |

Freshness modifiers attach to the live stamps: *LIVE*, *RECONNECTING · NOT LIVE*, *STALE*. A
prototype adds a sixth banner, **PROTOTYPE · SAMPLE DATA**, that cannot be hidden.

### 2.3 The persistent shell

- **Top bar:** Home · breadcrumb (place › lesson › step) · stamp · connection state · Manual ·
  Settings.
- **Step rail** (lessons and own-run history only): Previous · step markers · Next · explanation
  speed.
- **Context line** under the top bar: one sentence about what this view is allowed to show. For
  example, "Recorded from a generated-only run. Every order here is scripted."

Nothing else is permanent. Detail lives in the inspector that opens on selection.

### 2.4 Two kinds of pause

| Control | Where | Icon | Effect |
|---|---|---|---|
| Explanation speed / pause | Step rail | Hourglass | Slows or pauses animation only. Processing and stream state continue. |
| Run controls | My run status panel only, once a run-control protocol is adopted | Lever | Changes exchange-run lifecycle. Requires confirmation where the rules require it. |

The two never share a location, icon, or verb.

### 2.5 Movement and transitions

| From → to | Trigger | Transition | Preserved |
|---|---|---|---|
| Home → place | Click or Enter on a scene object; or the labelled menu | Push-in toward the object (240 ms, 6 stepped frames), then 120 ms crossfade. The object becomes the place's header emblem. | Focus target recorded for the return trip |
| Place → Home | Home button, Esc at top level, or Back | Reverse push-out | Focus returns to the object the user came from |
| Lens → lens | Lens tabs or `1`/`2`/`3` | Selected token stays fixed; surrounding panels slide 200 ms | Selection, step, stamp |
| Open overlay | Manual button, `?`, or "Why?" on a result | Slides in from the right; the main view narrows rather than being covered | Selection and scroll; Esc closes and returns focus |
| Step → step | Previous / Next, `[` / `]` | Animation explains the change (300–600 ms). Pressing Next during it completes it at once; inputs never queue. | Prediction and selection |
| Live update | Stream message | Changed level marked for two frames | Nothing moves under reduced motion |
| Disconnect | Stream loss | Freeze, desaturate, show RECONNECTING · NOT LIVE; never animate stale data | Last shown values remain visibly stale until a new snapshot replaces them |

Under reduced motion every spatial transition becomes a 120 ms crossfade and ambient loops stop.

**Browser history:** a place and a lesson are history entries. Steps inside a lesson update the URL
with `replaceState`, so a step is linkable but Back leaves the lesson instead of walking its steps.
Navigation never sends, resubmits, pauses, or resumes exchange work.

**Home while My run exists:** the door shows a small lit window and "Your run · open" when the page
holds a live private connection. Leaving My run does not close the connection in the same tab.

### 2.6 First visit

1. **Arrive.** The home scene loads with ambient motion and three labelled objects. One primary
   card: *"Start here: Who trades first? · about 4 minutes · recorded lesson."* Secondary links:
   *Watch the public market*, *Read the field manual*.
2. **The public window** shows its real state. If the demo is quiet, the board reads *"Quiet market —
   no resting orders right now"* rather than looking broken.
3. **Start the lesson.** Push-in to the cork board. The brief is three sentences.
4. **Predict** by selecting a resting order in the book. No penalty for a wrong choice.
5. **Watch** the recorded match. The outcome appears first, then the explanation.
6. **Inspect.** "Why?" opens the manual beside the book at *Price-time priority*, with the adopted
   rule quoted and the decision record linked.
7. **Look inside.** Switch to the Machine lens. The same order card is now a token on the command
   path, entering at *Generator*, receiving a sequence number, and reaching *Matching*.
8. **Debrief.** One next lesson, a link to try the same idea in My run (with the detail difference
   stated), and Home.

The first decision happens within three actions of arriving.

### 2.7 Returning visit

- **Progress present:** the primary card becomes *"Continue: Who trades first? · step 5 of 7"*. A
  "Lessons" count shows completed investigations. Progress is labelled *saved in this browser*.
- **Progress missing:** the first-visit card appears with a quiet note: *"No saved progress in this
  browser."* Nothing implies an account.
- **Opened a lab before:** a local hint relabels the door *"Your lab"*. Entering still requires a
  click, which calls the guest route.
- **Back in My run:** a fresh private connection. A banner states: *"Results sent while you were away
  are not shown here. This view starts from now."* The banner stays until dismissed and links to the
  manual page on result delivery. Until lookup or active-order reconstruction exists, the view never
  implies a complete history.
- **Demo changed:** if the public run ID differs from the one last seen, the window says *"New
  public run"* so returning users do not compare two different books.

## 3. Art directions

Both directions keep the same information architecture, lesson workspace layout, stamps, and
controls. Only the art treatment differs. The proofs file renders each direction's home scene and
the same lesson step.

### Direction A — Lamplight Workshop (recommended)

**Concept.** A small exchange workshop at dusk, drawn as a front-elevation cutaway. Warm walnut and
plaster, an amber desk lamp, a cork board of lesson cards, a brass-and-glass machine cabinet with a
pneumatic tube from the desk, and a door with a key tag. The window looks across the street at a
public market board that everyone can see.

**Why it fits.** The subject's own materials carry the metaphors. The journal is a literal ledger.
FIFO queues are index cards pinned in order. Schematics are blueprint sheets. Public data is
outside, on a board anyone in the street can read. Private data is behind your own door. Each
provenance stamp has a physical counterpart in the room.

**Lesson workspace.** The camera pushes into the cork board. The book is a board of price rows;
resting orders are index cards; the incoming order slides in on a brass rail. Text panels are
paper with ink, which supports long reading and high contrast.

**Strengths.** Front elevation is the cheapest perspective to keep consistent: rectangles,
horizontal shelves, no angle drift. Tokens move along straight rails, so animation is simple and
legible. Paper surfaces hold body text well. Warmth matches "cozy" without the trading-terminal
look the product direction rejects.

**Risks.** Light paper over warm wood can become busy; it needs a disciplined value structure (dark
room, light work surfaces). Market data can feel less "live" on paper; live status must come from
the stamp and the window board, not from glow. A dark-mode variant needs its own lighting pass
(lamp-only lighting), not an inversion.

### Direction B — Night Signal Room

**Concept.** A compact isometric control room at night, presented as a floating diorama. Indigo
walls, rain on the window, rows of phosphor-green monitors, one warm lamp. The lesson workspace is
an illuminated schematic table; orders are glowing chips on a grid.

**Strengths.** Isometric reads as a "system" and suits the Machine lens. Night lighting gives
strong focus: only the active object glows. Dark UI suits long sessions.

**Risks.** Isometric pixel art costs about twice as much per object and is easy to get wrong
(2:1 line drift, inconsistent heights). Text cannot sit on isometric planes, so UI and world
separate more sharply. Glow is a tempting default that conflicts with "do not fill every surface
with glow". Phosphor-on-navy sits close to generic crypto and terminal aesthetics, which weakens
recognition. Paper metaphors for the journal and manual do not fit.

### Recommendation

Choose **Direction A**. Borrow Direction B's schematic language for the SCHEMATIC stamp
(blueprint sheets pinned in the workshop) and its night lighting for an optional dark theme.

## 4. Asset plan

### 4.1 Tools and assets available in this pass

- **Available:** Python 3 with Pillow 12.1, HTML canvas in the browser pane, and the existing
  CSS-only hero art in `examples/showcase-static`.
- **Not available:** no image-generation tool, no Aseprite, no ImageMagick, and no existing pixel-art
  sprites, tilesets, fonts, or sounds in the repository.
- The proofs are original compositions drawn procedurally on canvas. They establish layout, scale,
  value structure, and palette. They are not production art.

### 4.2 Palette (Direction A)

32 colors, organized as ramps. Market sides use blue and orange (distinguishable with common color
vision deficiencies) and are always paired with ▲/▼ and BUY/SELL text. Alert red is reserved for
failure and rejection and always appears with a hazard-stripe pattern or icon.

| Role | Ramp (dark → light) |
|---|---|
| Ink and night | `#17141f` `#25222f` `#3a3346` |
| Walnut | `#3e2620` `#5c3a2c` `#7f513a` `#a3714d` |
| Plaster and paper | `#c9a983` `#e2c9a2` `#f2e4c8` `#fbf5e8` |
| Lamp | `#b86a2a` `#f0a640` `#ffd27a` `#fff0c2` |
| Sage | `#3f5240` `#617a57` `#8fa77a` |
| Dusk sky | `#2c3557` `#4a4d7c` `#8a6d93` `#d98b74` |
| BUY (blue) | `#1f4f7a` `#2f78b4` `#7fb4e0` |
| SELL (orange) | `#8a4519` `#d4782c` `#f2b072` |
| Blueprint | `#284a73` `#9cc0e6` |
| Alert / uncertain | `#b8322c` (alert), `#7d5bb0` (uncertain, always dotted) |

Text on paper uses ink `#25222f` on `#fbf5e8` (well above 4.5:1). Text never sits directly on
scene art; it sits on a paper or ink surface.

### 4.3 Pixel scale and grid

- **World layer:** native canvas 320 × 180. Displayed at 4× (1280 × 720) on the 1280 × 720 and
  1440 × 900 review sizes, centered with a framed margin at 1440. Integer scaling only;
  `image-rendering: pixelated`.
- **UI layer:** pixel-art borders, icons, and tokens on a 2× grid (one art pixel = 2 CSS pixels).
  Icons are 12 × 12 or 16 × 16 art pixels.
- The two layers never mix inside one object. Text is never scaled bitmap art.
- Base tile 16 × 16 art pixels; furniture aligns to 4-pixel increments.

### 4.4 Perspective and lighting

- **Perspective:** front elevation, no vanishing point. Floor plane shown as a 24-pixel strip.
  Objects may show a 2–4 pixel top face for depth. Workspaces are flat orthographic surfaces.
- **Light:** one warm key light (the desk lamp, upper center), one cool fill (window, left), one
  accent per interactive object (a small status light). Shadows fall down and right. Dithering is
  limited to large gradients (sky, lamp pool) with a 2 × 2 ordered pattern.
- Interactive objects are always one value step lighter than their surroundings at rest, so they
  read as interactive before hover.

### 4.5 Typography

| Role | Face | Use |
|---|---|---|
| Display | Silkscreen (OFL), at 16 or 32 CSS px only | Wordmark, place names, section headings |
| Body | IBM Plex Sans (OFL) | Briefs, explanations, manual |
| Numbers and stamps | IBM Plex Mono (OFL) with tabular figures | Prices, quantities, sequence numbers; uppercase stamps and labels at 11–12 px with letter-spacing |

Minimum body size 15 px; minimum label size 12 px. Fonts should be self-hosted so the public site
does not send visitors to a third-party font host. **Fact:** the C++ static server's MIME map has no
`.woff2` entry, so fonts would be served as `application/octet-stream`. Adding that type is a small
gateway change outside this pass.

### 4.6 Reusable objects

| Group | Objects |
|---|---|
| Room | Wall, floor, window with street board, cork board, desk, lamp, chair, shelves, cabinet, pneumatic tube, door with key tag, plant, mug, sleeping cat, covered bench (scenery) |
| Market | Price row, aggregate bar (BUY/SELL), resting-order card, incoming-order card, trade slip, spread gap marker |
| Machine | Stage housing (Generator, Gateway, Admission, Sequence, Journal, Matching, Results), tube segment, token capsule, ledger page |
| Structure | Price-level shelf, FIFO card chain, index drawer (active-order lookup) |
| UI | Stamps (6), buttons (primary, secondary, quiet), tabs, step rail, inspector frame, manual book, comparison drawer, toast, banner |
| Icons | Home, manual, settings, hourglass, lever, broadcast, key, reel, blueprint, ledger, ▲, ▼, check, warning, dotted question |

### 4.7 Interactive states

Every interactive object ships with: **rest**, **hover** (lifts 1 art pixel, outline brightens),
**focus** (2 px ink ring plus 2 px lamp-yellow ring, visible on light and dark), **pressed** (drops
1 pixel), **selected** (pinned tab or highlighted card), **disabled** (desaturated, with a reason
in text). Data views ship with **loading** (a calm pen-on-paper loop, never a fake value),
**empty** (says what will appear and why it is empty), **error** (what failed, what still works, what
to do), **disconnected/stale** (frozen, desaturated, stamp changes), and **capacity/terminal**
(read-only, explained).

### 4.8 Animation

| Animation | Frames | Timing | Reduced motion |
|---|---|---|---|
| Lamp warmth, mug steam, cat breathing | 4–6 | 150–250 ms per frame, loops | Static frame |
| Street board live blink | 2 | 1 s | Static "LIVE" text |
| Hover lift | 1 | Instant | Same |
| Push-in to place | 6 stepped | 240 ms + 120 ms fade | 120 ms fade |
| Order card slide | Continuous on pixel grid | 300–500 ms, ease-out | Card appears in place, outline flash |
| Trade formation | 4 | 400 ms | Static result with arrow |
| Disconnect | 1 | Instant desaturate | Same |

Motion snaps to whole art pixels. Animation never runs ahead of evidence and never reorders events.

### 4.9 File formats and budget

- **Source:** `.aseprite` (or `.pxo` for Pixelorama) under a separate art-source directory, not
  served.
- **Runtime:** 1× PNG sprite sheets with Aseprite-style JSON frame data; one sheet per scene, one
  for UI, one for icons. PNG and JSON already have MIME types in the C++ static server.
- **Budget:** the example showcase configuration limits each static asset to 1 MiB
  (`max_static_asset_bytes`). Target under 200 KB per sheet and under 1.5 MB total first load.
- **Fonts:** WOFF2 once its MIME type is supported; until then, system fallbacks.

### 4.10 Sourcing

| Option | Use |
|---|---|
| Commissioned pixel artist | Recommended for the home scene and lesson board — they define identity. Provide this palette, scale, and object list as the brief. |
| Owners in Aseprite or Pixelorama | UI icons, tokens, stamps — small, repetitive, rule-driven. |
| CC0 packs (for example Kenney) | Placeholder only, never shipped beside final art at a different scale. |
| AI image generation | Moodboards only. Not for shipped sprites: palette and grid drift, and unclear provenance. |

**Gaps:** every final sprite, the font files, sound, and a recorded-lesson data format produced from
real engine runs.

## 5. Prototype journey: "Who trades first?"

### 5.1 Scope

A static, offline prototype that walks one complete loop: Home → Lesson → Predict → Watch → Why
(manual) → Machine lens → Debrief → Home. It uses one sample fixture labelled as sample data. It
makes no network calls and does not touch the C++ server or exchange behavior.

**In:** home scene with three hotspots and the public-window states (live sample, quiet,
reconnecting); the lesson with Market and Machine lenses; field manual with one complete entry
(price-time priority) and search over a few titles; comparison drawer in its "no verified source
yet" state; settings for motion and shortcuts; local progress.

**Out:** live public stream, My run, invite entry, Structure lens beyond one schematic, incident
bench, sound, branch UI, real-venue facts.

### 5.2 Scenario (sample data)

Book before the lesson: asks 100.04 × 2; 100.02 × 3 (order A, sequence 7) and 100.02 × 5 (order
B, sequence 9); bids 100.00 × 6 and 99.98 × 4. A scripted buyer submits BUY 4 @ 100.02 GTC.

**Rule** outcome: A fills 3 at 100.02, then B fills 1 at 100.02. B rests with 4. The buyer is fully
filled; nothing rests on the bid side from this order. The adopted rules that govern it: price-time
priority, lower sequence first within a level, execution at the resting order's price, one trade per
resting order matched.

### 5.3 Steps

1. **Brief.** "Two sellers offer the same price. A buyer wants 4. Who sells first?"
2. **Predict.** Select order A, order B, or "they split it evenly".
3. **Send.** The buyer's card slides in on the rail. Its stamp reads RECORDED LESSON.
4. **Match.** Two trade slips form in order: A × 3, then B × 1.
5. **Explain.** Outcome sentence first, then "Why?" opens the manual at price-time priority.
6. **Look inside.** Machine lens: Generator → Admission → Sequence (A=7, B=9, buyer=12) → Journal →
   Matching → Results.
7. **Debrief.** Prediction result without scoring; one next lesson; "Try it in your own run" with the
   detail difference stated; Home.

### 5.4 Acceptance criteria

**Navigation and history**

- From Home, the prediction is reachable in three or fewer actions.
- Back from any step returns to Home in one press; Previous/Next move between steps.
- Reloading on a step URL restores that step with the prediction shown if one was saved.
- Returning to Home puts focus on the cork board.

**Honesty and provenance**

- Every data panel shows exactly one stamp; the PROTOTYPE · SAMPLE DATA banner is visible on every
  screen and is not dismissible.
- No text in the lesson says "your order".
- The Machine lens starts at Generator, not Gateway.
- The comparison drawer shows "No verified comparison yet" and no venue claim.
- No screen implies a network action.

**Accessibility**

- Every hotspot and control is reachable with Tab in visual order and operable with Enter/Space.
- Focus is visible on every surface at both review sizes.
- The prediction can be made with the keyboard and with a screen reader using the text list.
- `prefers-reduced-motion` and the settings toggle both replace spatial motion with fades and stop
  ambient loops.
- Single-key shortcuts can be turned off.
- Text contrast is at least 4.5:1; color is never the only signal for side, state, or stamp.
- Usable at 200% browser zoom without horizontal scrolling of the main content.

**Visual quality**

- At 1280 × 720 and 1440 × 900, world art renders at an exact integer scale with no blurred pixels.
- One art-pixel size per layer; no mixed scales within a scene.
- The home scene has one primary call to action; the lesson step shows one objective, one main
  visual, and its immediate controls.
- Hover, focus, pressed, selected, disabled, loading, empty, error, and stale states are present for
  every component used.

**State coverage in the prototype**

- The public window cycles through live sample, quiet, and reconnecting using a developer toggle.
- Clearing local storage shows the first-visit state; a blocked storage accessor does not break the
  page.

**Comprehension test (five or more participants)**

- At least four of five explain why order A traded first, using "arrived earlier" or "lower
  sequence".
- At least four of five say the lesson was recorded rather than live.
- At least four of five find the manual entry from the result without help.
- Record time to first prediction and any point where a participant hunted for Back or Home.

The prototype is a design test. Passing it says nothing about engine evidence, recorded-lesson
production, or private-run features, which remain gated by Implementation Status.

## 6. Decisions requested

**Product choices for owner approval** (update Product Direction on approval):

1. Places / lenses / overlays structure, replacing Engine and Field manual as places.
2. Recorded lesson as the first-visit primary action instead of following "your" order.
3. Direction A, Lamplight Workshop, with Direction B's schematic language for SCHEMATIC.
4. The provenance stamp set, including the non-dismissible prototype banner.
5. Third-person framing in recorded lessons.
6. Manual pages never locked.
7. Step URLs that use `replaceState`.

**Decisions that need their own rule or architecture proposals** (not settled here):

- How recorded lessons are produced from real generated-only runs and what evidence they retain.
- A generated scenario for the public demo.
- Invite admission and issuance.
- Guest run-control protocol, including capacity and stopped-run replacement confirmation.
- Private result lookup or active-order reconstruction for reconnect.
- Serving `.woff2` from the C++ static server.

## 7. References and the principle each supports

These are design interpretations. No assets, systems, or text are borrowed.

| Reference | Principle used here |
|---|---|
| *Papers, Please* (Lucas Pope) | Inspection as play: the visitor examines documents to reach a verdict. Used for the inspector and evidence stamps. |
| *Return of the Obra Dinn* (Lucas Pope) | Deduction from fixed evidence, with confirmation only once enough is established. Used for incident lessons, where reconstruction comes from surviving evidence. |
| *Into the Breach* (Subset Games) | Telegraphed outcomes make consequences inspectable before they happen. Used for predict-then-watch. |
| *EXAPUNKS* (Zachtronics) | Technical documentation as an in-world object. Used for the field manual as a book on the desk. |
| *Unpacking* (Witch Beam) | A small room tells a story through objects. Used for the home scene: one room, few objects, each meaningful. |
| *Mini Metro* (Dinosaur Polo Club) | Schematic restraint: few colors, consistent geometry, motion only where meaning changes. Used for the Machine lens. |
| *Stardew Valley* (ConcernedApe) | Welcoming place and a coherent palette. Feeling only. |
| *Celeste* assist mode (Maddy Makes Games) | Accessibility options framed as normal settings. Used for motion and shortcut settings. |

Real-venue comparison sources remain those listed in the blueprint §9, which still require exact
section verification before publication.
