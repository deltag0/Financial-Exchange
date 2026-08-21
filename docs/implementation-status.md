# Implementation Status

This is the only project document that describes current repository behavior and completeness.

See [Exchange Rules](exchange-rules.md) for adopted and unresolved behavior. See
[Architecture](architecture.md) for the intended system design. Neither document proves that a
feature is implemented.

## Status definitions

- **Implemented**: the described behavior exists end to end for its stated scope and has relevant
  tests.
- **Partially implemented**: meaningful behavior exists, but important paths or guarantees are
  incomplete.
- **Parsed only**: an external command is recognized or normalized but has no complete business
  processing path.
- **Stubbed**: a type, function, component, or configuration exists but does not provide the intended
  behavior.
- **Missing**: no meaningful implementation exists.
- **Unverified**: code or configuration exists, but this review did not establish that it builds or
  runs successfully.

## Feature and component status

| Area | Status | Current behavior | Relevant code | Existing coverage | Main limitation |
|---|---|---|---|---|---|
| C++ exchange executable | Partially implemented | Source constructs FIX handling, four sequencers, one matching engine, queues, and worker threads | `core/exchange/src/main.cpp` | Full Docker build compiles the executable; Python sharding fixtures expect it | No coordinated shutdown; container does not launch it by default; live startup was not verified |
| FIX acceptor | Partially implemented | Configures a QuickFIX FIX 4.2 acceptor on port 5001 | `core/exchange/src/main.cpp`, `exchange.cfg` | Python test client and sharding tests | One configured session; live startup was not verified |
| New Order Single parsing | Partially implemented | Accepts configured `SPY` v1 limit orders, requires an exact bounded `ClientCommandId`, parses price and quantity from decimal text into strong values, enforces tick/lot and configured maximums, and records instrument/configuration IDs | `core/domain/include/domain_types.hpp`, `core/instrument/include/instrument_config.hpp`, `core/fix/src/fix_parser.cpp` | Deterministic required-client-command-ID, configured-boundary, malformed-value, unknown-instrument, and existing parser tests; domain tests cover client-command-ID bounds | The current session hash is not the adopted stable `ClientId`; authoritative admission/deduplication and responses are not implemented |
| FIX Cancel Request normalization | Missing | Recognizes `35=F` but rejects it before sequencing because FIX `OrigClOrdID` is not an authoritative exchange `OrderId` and no adopted lookup maps it to `TargetOrderId` | `core/fix/src/fix_parser.cpp`, `core/fix/src/fix_task.cpp` | Parser and FixTask tests verify rejection and no sequence-counter consumption or queue insertion | Authoritative FIX-to-`TargetOrderId` mapping and client response delivery are unresolved |
| Order modification | Missing | FIX modification messages are not processed | — | None | No amend or cancel-replace behavior |
| TimeInForce parsing | Implemented for adopted scope | Requires FIX tag 59 for New Order Single and accepts only GTC and IOC without expiry. Missing, DAY, FOK, GTD, GTX, ATC, unknown values, and expiry-bearing orders are rejected before sequencing | `core/fix/src/fix_parser.cpp`, `core/task/include/time_in_force.hpp` | Deterministic acceptance tests for GTC/IOC and rejection tests for every deferred, unknown, missing, and expiry-bearing value | Parser failures are caught by `FixTask` but are not delivered to clients; deferred TimeInForce behavior is not implemented |
| Internal order message | Partially implemented | Uses distinct strong values for command sequence, order ID, target order ID, client ID, client command ID, instrument ID, price ticks, and quantity units. `TargetOrderId` is an explicit optional cancel-only field in the shared message representation | `core/domain/include/domain_types.hpp`, `sequencer/include/sequence_message.hpp` | Compile-time type separation plus parser, bus, sequencer, and matching tests | Legacy hashed `id`, port/topic fields, optional command-specific fields, and several uniqueness scopes remain; the stub JSON representation does not serialize client identity or `TargetOrderId` |
| In-process bounded queue | Partially implemented | Wraps `boost::lockfree::queue` with `push`, `pop`, and `empty` | `core/shared_queue/include/shared_queue.hpp` | Exercised indirectly | It is not process-shared IPC; full-queue policy is incomplete |
| Multicast bus | Partially implemented | Circular in-process buffer with registered reader cursors and overwrite protection; stack-message reads copy an available slot before advancing the cursor | `core/bus/include/bus.hpp` | `BusUnitTests` covers stack-message reads, basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Intended writer model is implicit; the unused legacy pointer/journal read overload is unsafe; journal output is not a recovery log |
| Symbol sharding | Partially implemented | Resolves the sole configured `SPY` instrument, then hashes its symbol into four shard queues | `core/fix/src/fix_parser.cpp`, `core/exchange/src/main.cpp` | `sequencer/tests/test_sequencer_sharding.py` targets `SPY` | Uses implementation-defined hashing; no stable partition configuration or multi-instrument coverage |
| Sequencing | Partially implemented | Each sequencer increments one local counter and wraps it as a `CommandSequence`; NewOrder derives the same-valued `OrderId`, while Cancel retains its independent `TargetOrderId`. A per-port counter is also incremented | `sequencer/include/sequencer.hpp`, `sequencer/src/sequencer.cpp` | Sharding tests and synthetic throughput tests compile in the full build | “Global” numbers are not unique across sequencers; no authoritative durable order or cancel admission path from FIX |
| BUY matching | Partially implemented | Incoming BUY matches the lowest eligible asks, then stable FIFO nodes within each level. The immutable command drives a local remainder. Each maker-price fill updates node/index/aggregate state and emits one ordered `Trade`; a positive GTC remainder rests before one `OrderRested`, while a positive IOC remainder emits one `OrderCancelled(IocRemainder)` without resting. Full fills have no terminal event. GTC capacity and all event storage are preflighted before mutation | `matching_engine/src/matching_engine.cpp` | Exact multi-maker/multi-level trades, unmatched/partial/full GTC and IOC terminal outcomes, event fields/order, maker price, best price, FIFO, aggregate/index cleanup, instrument isolation, command immutability, unsupported-TimeInForce invariant failure, and capacity preflight | Outcomes are still discarded by queue drain; publication and client delivery are missing |
| SELL matching | Partially implemented | Incoming SELL symmetrically matches the highest eligible bids with the same immutable-command, maker-price, event-planning, GTC-rest, IOC-cancel, full-fill, and no-partial-mutation guarantees as BUY | `matching_engine/src/matching_engine.cpp` | Symmetric exact multi-maker/multi-level trades, unmatched/partial/full GTC and IOC terminal outcomes, event fields/order, best price, FIFO, state invariants, isolation, immutability, unsupported-TimeInForce failure, and capacity preflight | Outcomes are still discarded by queue drain; publication and client delivery are missing |
| Order books | Partially implemented | `InstrumentId` keys one per-instrument bid/ask book; strong-price levels own stable FIFO list nodes and strong per-side aggregates. One active-order index maps each resting `OrderId` to owner, instrument, side, price, remaining quantity, and stable node location. Fills and successful cancellation update node/index/aggregate state consistently and clean empty levels/books. A duplicate active authoritative `OrderId` is an invariant failure. Symmetric capacity preflight preserves no-partial-mutation rejection | `matching_engine/include/matching_engine.hpp`, `matching_engine/src/matching_engine.cpp` | Symmetric fill lifecycle, successful BUY/SELL and partially-filled cancellation, stable FIFO, node/index/aggregate agreement, empty cleanup, instrument isolation, terminal-index absence, duplicate-ID invariant failure, aggregate boundaries, and no-partial-mutation tests | Matching outcomes are not published; reconstruction and retransmission state are absent |
| Cancellation processing | Partially implemented | At the normalized matching-engine seam, Cancel validates required fields, looks up strong `TargetOrderId`, returns exact `OrderNotActive` or `NotOwner` rejections without mutation, or atomically removes the owner’s current remainder and emits one `OrderCancelled(ClientRequested)`. `CANCELREJ`, wrong routing, and inconsistent internal state are invariant failures | `core/domain/include/domain_types.hpp`, `sequencer/include/sequence_message.hpp`, `matching_engine/src/matching_engine.cpp` | Exact BUY/SELL success, partial remainder, terminal targets, rejected/never-seen targets, already-cancelled targets, ownership, immutable commands, FIFO/aggregate/cleanup invariants, required-field failures, and fill-versus-cancel processing order | No matching-engine retransmission/deduplication; FIX target mapping, publication, persistence, and delivery are missing |
| IOC BUY and SELL | Partially implemented | Match immediately on both sides; a positive remainder never rests and emits exactly one `OrderCancelled(IocRemainder)` after all trades. A fully filled IOC emits only trades | `matching_engine/src/matching_engine.cpp` | Exact unmatched, partial, and full outcomes on both sides, including cancellation fields/index, cleanup, aggregate invariants, self-trade, and isolation | Outcomes are not published, persisted, or delivered |
| Resting BUY/SELL | Partially implemented | GTC matches symmetrically, rests a positive local remainder, then emits exactly one `OrderRested`; a fully filled GTC emits only trades. Active remainders can be cancelled at the normalized engine seam | `matching_engine/src/matching_engine.cpp` | Exact unmatched, partial, full, and cancellation outcomes on both sides, including fields/indexes, active state, aggregates, and immutable commands | Publication, persistence, retransmission, and delivery remain incomplete |
| Execution reports | Missing | No complete client-facing acceptance, rejection, fill, or order-state report is produced | — | None | FIX client receives no business result |
| Business event model | Partially implemented | Defines immutable adopted event values and a per-command matching outcome. Matching preplans and reserves NewOrder events; Cancel reserves its single outcome before mutation. Successful client cancellation emits `OrderCancelled(ClientRequested)` at index zero, and state-dependent cancellation failures emit one exact `CommandRejected` at index zero | `core/domain/include/business_events.hpp`, `matching_engine/include/matching_engine.hpp` | Compile-time immutability/category separation, exact symmetric trade/terminal fields and ordering, exact cancellation success/rejection fields, and capacity-rejection assertions | No admission response model, adopted practical event-storage/backpressure policy, event sink, publication, persistence, or delivery |
| Trade events | Partially implemented | BUY and SELL matching construct deterministic adopted `Trade` values in exact price-time order with maker-price executions and post-fill maker/taker quantities | `core/domain/include/business_events.hpp`, `matching_engine/src/matching_engine.cpp` | Exact fields and event IDs for three executions across multiple makers and prices on each taker side; self-trade is covered | Outcomes are discarded by queue drain; no publication, persistence, client delivery, or market-data consumption |
| Market data | Missing | No trade, top-of-book, depth, snapshot, or incremental feed | — | None | No publisher or recovery protocol |
| Journaling | Stubbed | Bus optionally writes message JSON through an `fstream` | `core/bus/include/bus.hpp`, `sequencer/include/sequence_message.hpp` | Bus behavior tests do not verify durability | No authoritative log, record framing, checksum, acknowledgement rule, or loader |
| Snapshots and replay | Missing | No snapshot writer, restore path, or deterministic replay runner | — | None | Crash recovery is not implemented |
| Node backend | Stubbed | Returns a plain-text greeting from every request | `backend/index.js` | None | No health endpoint, order API, validation, or sequencer connection |
| Frontend | Stubbed | Nginx container template only | `frontend/Dockerfile` | None | No frontend application |
| OpenTelemetry and Prometheus | Stubbed | Collector and scrape configuration files exist | `otel/config.yaml`, `prometheus/prometheus.yml` | None | Applications do not emit the documented exchange metrics |
| Docker Compose stack | Unverified | Defines backend, frontend, Prometheus, OTel, and sequencer services | `docker-compose.yml`, component Dockerfiles | No current smoke suite | Port mappings and startup commands do not form a verified working exchange |
| Throughput measurements | Unverified | Timed Google Test cases print message rates | `sequencer/tests/test_e2e_throughput.cpp` | Self-contained test assertions | Important paths bypass matching; no reproducible latency distribution or reference hardware |

## Known correctness limitations

These are implementation findings, not exchange rules:

- Legacy client order IDs, sessions, and symbol sharding rely on `std::hash`; collisions and
  cross-platform results are not handled as identity concerns.
- Client IDs are still represented by session hashes rather than the adopted stable identity model.
- Deferred TimeInForce and expiry behavior is deliberately rejected at FIX normalization. If an
  unsupported TimeInForce bypasses normalization, matching treats it as an invariant failure before
  book mutation.
- FIX cancellation is rejected before sequencing because no authoritative mapping exists from
  `OrigClOrdID` to exchange `TargetOrderId`. No hash or textual reinterpretation is used.
- Cancel retransmission/deduplication is not implemented at the matching-engine seam. A new cancel
  command against an already-terminal target deterministically returns `OrderNotActive`.
- Several queue transitions remove a message from the previous queue before confirming the next
  queue accepted it. Failure can log or be ignored without a client-visible result.
- Four sequencer threads merge into one matching-engine queue, so cross-shard processing order is
  determined by concurrent insertion rather than one documented sequence.
- `FixTask` and `MatchingEngine` spin continuously when idle, while sequencers sleep for 10 ms between
  polls.
- Trade, terminal, and book-capacity rejection results exist in the returned matching outcome, but the
  queue-drain path discards it; no events are durable, published, or client-visible.
- Per-command events use an exactly planned, dynamically allocated vector. The configured SPY v1
  quantity and lot limits bound execution count below `EventIndex` exhaustion, but the repository
  has no adopted practical event-storage limit or backpressure/stop policy. This change does not
  invent an event-capacity rejection or other unresolved behavior.
- The unused legacy `Bus::read` pointer overload does not return its pointer assignment to the
  caller, can underflow its overwrite test, and allocates one message before indexing it as an
  array. Current callers use the tested stack-message overload instead.

## Existing test coverage

| Test area | Files | What is covered | Important gaps |
|---|---|---|---|
| Multicast bus | `core/bus/tests/bus_test.cpp` | Basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Multiple writers, cursor lifetime, journal behavior |
| FIX parsing | `core/fix/tests/test_fix_parser.cpp`, `test_fix_parser_edgecases.cpp`, `test_fix_malformed.cpp` | Basic order parsing, required and bounded client command ID, exact `SPY` v1 price/quantity normalization, configured maxima, off-tick/lot/range rejection, unknown instrument, accepted GTC/IOC, rejected unsupported TimeInForce inputs, and explicit pre-sequencing rejection of unmapped FIX cancels | Stable client identity, authoritative FIX cancel target mapping, other instruments, remaining independent required-field tests, admission/deduplication, client-visible responses |
| FIX task | `core/fix/tests/test_fix_task.cpp` and edge-case tests | `fromApp` queues supported parsed messages and catches parser failures | Continuous run loop, queue saturation, client rejection delivery |
| Matching engine | `matching_engine/tests/test_matching_engine.cpp` | Strong-domain/event separation, symmetric matching and terminal outcomes, immutable commands, client-request cancellation of BUY/SELL and partial remainders, exact success/rejection events, all inactive target classes, ownership, FIFO/aggregate/active-index/empty cleanup invariants, fill-versus-cancel order, required cancel fields, invalid internal kinds, duplicate active IDs, capacity boundaries, and no-partial-mutation rejection | Retransmission/deduplication, bounded event-storage policy, publication, persistence, and external delivery |
| C++ timed pipeline | `sequencer/tests/test_e2e_throughput.cpp` | Queue and parser plumbing under timed load | Real matching behavior, reproducibility, percentiles, drops, controlled hardware/build settings |
| Live FIX sharding | `sequencer/tests/test_sequencer_sharding.py` | Same symbol observed on a consistent shard | Business result, matching, recovery, globally defined fairness |

## Build and runtime verification

- **Last source inspection:** 2026-08-21
- **Inspected base commit:** `6de1670`; Stage 4 began with the uncommitted, passing Stage 3 changes
  listed by `git status`, and those changes were preserved and extended without reset or stash.
- **Build performed during this change:** Yes. The documented `docker-compose up --build -d`
  completed every image build, including the complete final CMake target graph in
  `sequencer/Dockerfile`. Stack startup then failed because host port 5000 was already occupied;
  `docker-compose down` removed the partially started stack. `docker compose build sequencer` also
  completed independently. Native CMake was not rerun because the existing host cache records Boost
  as unavailable.
- **Tests performed during this change:** Yes. The dedicated cancellation filter passed eight tests;
  focused matching/FIX CTest targets passed (3/3); and the configured `BusUnitTests`,
  `MatchingEngineUnitTests`, `E2EThroughputTests`, `FixParserUnitTests`, and `FixTaskUnitTests` passed
  (5/5). A separate AddressSanitizer debug build with leak detection passed
  `MatchingEngineUnitTests` (1/1). Docker-hosted `clang-format --dry-run --Werror` and
  `git diff --check` also passed.
- **Runtime stack verification performed:** No; startup stopped at the pre-existing host-port 5000
  conflict, and business-event delivery remains outside this stage.

The `InstrumentId`-keyed books, active-index lifecycle, symmetric price-time matching, immutable
incoming command handling, deterministic maker-price trades and GTC/IOC terminal outcomes,
aggregate/capacity behavior, supported-TimeInForce boundary, normalized client cancellation, and
stack-message bus read used by `FixTask` are covered by passing deterministic tests in the documented
Docker build environment. Matching and cancellation cleanup were also exercised under
AddressSanitizer. No FIX target mapping, event publication, external end-to-end result, or live
runtime behavior was verified. The Docker environment is command-reproducible but not bit-for-bit
pinned: the Ubuntu base tag and apt package versions remain mutable.
