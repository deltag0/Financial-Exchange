# Product Direction

## Purpose and authority

This document defines the intended product experience: who it is for, what users should be able to
learn and do, how the simulation should feel, and which features deserve priority.

It is not a source of exchange behavior, architecture, or implementation claims:

- [Exchange Rules](exchange-rules.md) defines adopted exchange behavior.
- [Architecture](architecture.md) defines component responsibilities, ownership, and data flow.
- [Implementation Status](implementation-status.md) defines what currently exists and what has been
  verified.
- [Engineering Decisions and Lessons](engineering-decisions-and-lessons.md) preserves the reasoning
  behind material decisions.

Product ideas in this document are direction, not adopted exchange rules. A feature that changes
matching, identity, validation, event, durability, or recovery behavior must first be specified in
the governing documents.

## North-star vision

Build an interactive, deterministic exchange laboratory where users and simulated participants send
real orders through a transparent exchange, then pause, inspect, replay, and understand exactly why
each order traded, rested, cancelled, or was rejected.

The experience should combine:

- the responsiveness and polish of a modern strategy game;
- the information density of a trading terminal;
- the causal clarity of a debugger; and
- the technical credibility of a real deterministic C++ matching system.

The product is not trying to imitate a brokerage account or claim production exchange status. Its
purpose is to make the design of this particular exchange visible, understandable, and enjoyable to
explore.

## Intended audience

### Curious learners

People who have heard terms such as order book, spread, maker, taker, and price-time priority but
have never seen the mechanics operate.

They need guided scenarios, plain-language explanations, and visuals that connect an action to its
result.

### Software and systems engineers

People interested in deterministic processing, queues, ownership, journaling, replay, backpressure,
and recovery.

They need access to command identities, component boundaries, event order, capacity state, and
failure behavior without reading the source first.

### Experimenters

Users who want to tune simulated participants, submit their own orders, create unusual book states,
or compare alternative actions.

They need a sandbox, repeatable scenarios, speed controls, and useful inspection tools.

## Product principles

### The visualization reflects real exchange state

The UI must be driven by authoritative commands and events. It must not invent fills, reorder events
for dramatic effect, or imply that animation timing determines matching priority.

### Explain cause, not only outcome

Every important result should answer both “what happened?” and “why?” A fill should identify the
maker price and priority relationship. A rejection should identify the governing rule and the stage
that rejected it.

### One system, several views

The product should clearly distinguish:

- the public market view;
- the participant's private orders and results; and
- a privileged educational view of internal exchange processing.

The educational view may expose information that a real participant must not receive, but it must
never be mistaken for the public feed.

### Determinism is a user-facing feature

Users should be able to restart a generated scenario, replay an exchange run, and see the same
authoritative outcomes from the same ordered inputs. Determinism should be demonstrated visually,
not treated only as an internal engineering property.

### Bounded runs are part of the experience

An exchange run is a deliberate, inspectable experiment with a beginning, capacity, and end. Users
can pause it, stop it, replay it, and explicitly start another run. The product does not need an
ever-growing history to feel complete.

### Complexity must earn visibility

Prefer features that teach an exchange concept, reveal an architectural property, or create an
interesting decision. Infrastructure that users cannot observe should be added only when required
for correctness or supported by measurements.

## Core experience loop

1. Choose a guided scenario or configure a sandbox run.
2. Start the exchange and watch simulated participants establish a market.
3. Inspect the spread, price levels, queue priority, trades, and component activity.
4. Submit a limit order or cancellation through the same exchange path as simulated participants.
5. Follow the command through normalization, admission, sequencing, durability, matching, and
   result delivery.
6. Understand why the order rested, traded, cancelled, or failed.
7. Pause, single-step, accelerate, replay, or intentionally stop the run.
8. Start a new run or revisit the retained run to compare the result.

## Experience modes

### Guided lessons

Short deterministic scenarios introduce one concept at a time. Each lesson should state a question,
let the user predict the result, execute a small command sequence, and explain the result.

Initial lessons should cover:

- bid, ask, spread, and crossing;
- price priority;
- time priority at one price;
- maker-price execution;
- partial fills and multi-order sweeps;
- GTC resting behavior;
- IOC remainder cancellation;
- cancellation ownership and inactive orders;
- identical retransmission versus conflicting identifier reuse; and
- pre-sequence rejection versus sequenced business rejection.

### Live sandbox

Users configure generated participants and submit their own orders into a continuously changing
book. Useful controls include participant count, order-arrival rate, aggressiveness, preferred
spread, cancellation rate, and simulation speed.

Generated participants must submit through the normal exchange pipeline. They must not mutate order
books directly.

### Exchange debugger

Users pause the run and step through authoritative commands. Selecting a command reveals:

- normalized fields and identifiers;
- admission classification;
- sequence and journal position;
- book state immediately before matching;
- every resulting state transition and event;
- private and public projections; and
- book state immediately afterward.

### Reliability laboratory

Controlled demonstrations make failure semantics visible:

- temporary gateway saturation;
- exchange-run capacity reached;
- failure before durable commitment;
- failure after commitment but before delivery;
- incomplete journal tail;
- recovery and deterministic replay; and
- an invariant failure that causes fail-stop rather than an invented client rejection.

Fault injection must be explicit and deterministic. The interface should explain which state is
authoritative at the selected failure point.

### Replay and comparison

A replay timeline lets users scrub through a retained run and reconstruct the book at each command.
A later “what-if fork” can replay to a chosen position, replace or add one user action, and compare
the resulting books and events side by side.

## Visual direction

The visual identity should feel like a living systems diagram layered over a trading interface. It
should be precise and inviting, not a wall of terminal text and not a decorative casino aesthetic.

### Primary workspace

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│ Run status · scenario · seed · speed · sequence · capacity · pause / step  │
├───────────────────────┬───────────────────────────┬─────────────────────────┤
│                       │                           │                         │
│ Aggregated asks       │ Price ladder / book       │ Trade tape              │
│                       │ Best ask                   │                         │
│ Spread and midpoint   │ ───────────────────────   │ Selected trade details  │
│                       │ Best bid                   │                         │
│ Aggregated bids       │ Queue position            │ Participant activity    │
│                       │                           │                         │
├───────────────────────┴───────────────────────────┴─────────────────────────┤
│ Command journey: Gateway → Admission → Sequence → Journal → Match → Events │
├───────────────────────────────────┬─────────────────────────────────────────┤
│ My orders and private results     │ Explain this result / rule inspector    │
└───────────────────────────────────┴─────────────────────────────────────────┘
```

The layout may adapt by screen size, but the relationship between market state, the user's orders,
and internal processing should remain visible.

### Motion and causality

- Animate authoritative transitions after they occur; animation never determines state.
- Use a consistent visual token for one command as it crosses component boundaries.
- Highlight the resting maker and incoming taker when explaining a trade.
- Show quantity moving between orders and the trade tape rather than making levels disappear without
  explanation.
- Let users slow or pause explanatory animation without slowing authoritative processing unless they
  explicitly pause the exchange run.
- Collapse animation at high simulation speeds and preserve accurate totals, ordering, and alerts.

### Color and accessibility

- Do not rely on red and green alone to distinguish BUY and SELL.
- Pair color with direction, labels, icons, and position.
- Reserve strong warning colors for rejections, saturation, and failure states.
- Maintain readable contrast, keyboard navigation, reduced-motion behavior, and a color-blind-safe
  palette from the first visual system.
- Display exact values in tooltips and detail panels even when charts aggregate or scale them.

### Information hierarchy

The default view should answer, in order:

1. Is the exchange running and healthy?
2. What is the current market?
3. What happened to my order?
4. Why did it happen?
5. What did the exchange do internally?

Advanced identifiers, queue metrics, and journal details should be easy to reveal without
overwhelming a first-time user.

## High-value product features

### Market understanding

- aggregated order-book depth;
- best bid, best ask, spread, and midpoint;
- trade tape with maker/taker explanation;
- selected price-level queue visualization;
- participant activity indicators; and
- before-and-after book comparison for one command.

### Personal interaction

- exact tick- and quantity-aware order ticket;
- GTC and IOC selection;
- cancellation from the user's active-order list;
- immediate private order state and rejection explanation;
- prediction prompts in guided lessons; and
- a history of actions within the current retained run.

### Exchange transparency

- command-journey visualization;
- authoritative sequence and event identifiers;
- rule inspector linked to each result;
- public/private/educational view toggle;
- queue occupancy and backpressure state;
- run-capacity meter; and
- recovery progress and invariant status.

### Simulation controls

- deterministic seed and named scenario;
- pause, single-step, normal speed, accelerated speed, and maximum speed;
- generated participant templates such as noise trader, passive market maker, aggressive taker, and
  cancellation-heavy participant;
- scenario reset and explicit new-run creation; and
- export of a compact replayable run when an export format is adopted.

## Game and progression ideas

Game mechanics should reward understanding rather than encourage gambling behavior.

Possible challenges include:

- predict the next execution price;
- obtain a fill without crossing the spread;
- demonstrate that an earlier equal-price order fills first;
- create and then close a spread using a market-maker bot;
- identify why an IOC order did not rest;
- distinguish an admission conflict from a business rejection;
- recover a deliberately interrupted run without changing its result; and
- find the command that caused a visible book transition.

Progression can unlock more complex scenarios, additional instruments, fault injection, and deeper
debug panels. Scores should measure explanation accuracy, scenario goals, or market-making quality;
they should not imply real investment skill.

Positions and simulated profit and loss may be added later as a separate educational projection.
They must not silently introduce accounts, balances, margin, risk, settlement, or matching rules
into the exchange core.

## Feature priorities

### Phase 1 — The exchange is visible

- complete private result delivery;
- expose a stable application-facing event stream;
- show the live book, spread, trade tape, and user's orders;
- provide an order ticket for adopted NewOrder and Cancel behavior; and
- clearly show current limitations and run health.

### Phase 2 — The exchange explains itself

- add the command journey and result explanation;
- add pause, step, and deterministic named scenarios;
- distinguish public, private, and educational views;
- add queue position and before/after state inspection; and
- build the initial guided lessons.

### Phase 3 — Runs become replayable experiments

- implement exchange-run identity and lifecycle;
- add the bounded authoritative journal;
- reconstruct state and results through full replay;
- add replay controls and the run-capacity experience; and
- retain and inspect the most recently stopped run.

Implementation dependencies may require some Phase 3 foundations before all Phase 1 UI work is
finished. The phases describe user-visible product maturity, not a license to violate architectural
ordering.

### Phase 4 — Reliability becomes interactive

- controlled fault injection;
- recovery visualization;
- saturation and backpressure scenarios;
- invariant dashboard; and
- deterministic original-versus-recovered comparison.

### Phase 5 — A richer simulation

- several fictional instruments with different tick and quantity characteristics;
- configurable generated participants;
- multi-instrument partition visualization;
- what-if forks and side-by-side comparison; and
- optional positions, P&L, or learning challenges outside the matching authority.

## Explicit non-goals

Unless the product direction changes, do not prioritize:

- real-money trading, custody, deposits, or withdrawals;
- brokerage behavior or investment recommendations;
- live external market connectivity;
- regulatory or permanent archive retention;
- unlimited exchange history;
- accounts, margin, settlement, or portfolio risk inside the matcher;
- advanced order types added only to increase a feature count;
- distributed deployment without a measured need;
- artificial nanosecond or high-frequency claims; or
- visuals that conceal or contradict authoritative exchange behavior.

## Choosing future features

Evaluate each proposal with these questions:

1. Does it teach an exchange concept or expose an important design property?
2. Does it create a meaningful user decision or a clearer explanation?
3. Can it be driven by authoritative commands and events?
4. Is it exchange behavior, simulation orchestration, or presentation?
5. Can it be tested deterministically?
6. Does it fit within bounded exchange runs and ordinary laptop resources?
7. Would implementing it introduce a much larger hidden domain such as accounts or settlement?
8. Is its complexity visible and valuable to the user?

Prioritize features that score strongly on learning, interaction, and architectural transparency.
Defer infrastructure whose primary justification is an imagined permanent exchange rather than the
intended showcase.

## Product quality bar

The showcase is successful when a new user can:

- explain bid, ask, spread, price priority, time priority, and maker pricing after using it;
- submit and cancel an order without understanding FIX;
- trace one order through the exchange and explain its result;
- replay a deterministic scenario and recognize the identical outcome;
- distinguish public data from private participant results and privileged diagnostics; and
- observe a controlled failure and understand why recovery preserves correctness.

For an engineer reviewing the project, the product should make ownership, sequencing, durability,
backpressure, and deterministic state transitions easier to understand than reading an architecture
diagram alone.
