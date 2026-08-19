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
| Cancel Request parsing | Parsed only | Recognizes `35=F` and produces an internal CANCEL message | `core/fix/src/fix_parser.cpp`, `core/fix/src/fix_task.cpp` | Parser and FixTask tests | Does not establish a complete original-order target; engine ignores CANCEL |
| Order modification | Missing | FIX modification messages are not processed | — | None | No amend or cancel-replace behavior |
| TimeInForce parsing | Partially implemented | Defaults to DAY; parses DAY, GTC, IOC, FOK, GTD, and ATC with several expiry checks; rejects GTX | `core/fix/src/fix_parser.cpp`, `core/task/include/time_in_force.hpp` | Parser validation tests | Matching behavior is incomplete or absent for several accepted values |
| Internal order message | Partially implemented | Uses distinct strong values for command sequence, order ID, client ID, client command ID, instrument ID, price ticks, and quantity units while retaining legacy transport fields, fixed symbol storage, configuration version, side, expiry, shard, and TimeInForce | `core/domain/include/domain_types.hpp`, `sequencer/include/sequence_message.hpp` | Parser, bus, sequencer compilation, and matching tests | Legacy hashed `id`, port/topic fields, optional pre-sequencing identity fields, and several uniqueness scopes remain; the stub JSON representation does not serialize the newly added client identity fields |
| In-process bounded queue | Partially implemented | Wraps `boost::lockfree::queue` with `push`, `pop`, and `empty` | `core/shared_queue/include/shared_queue.hpp` | Exercised indirectly | It is not process-shared IPC; full-queue policy is incomplete |
| Multicast bus | Partially implemented | Circular in-process buffer with registered reader cursors and overwrite protection; stack-message reads copy an available slot before advancing the cursor | `core/bus/include/bus.hpp` | `BusUnitTests` covers stack-message reads, basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Intended writer model is implicit; the unused legacy pointer/journal read overload is unsafe; journal output is not a recovery log |
| Symbol sharding | Partially implemented | Resolves the sole configured `SPY` instrument, then hashes its symbol into four shard queues | `core/fix/src/fix_parser.cpp`, `core/exchange/src/main.cpp` | `sequencer/tests/test_sequencer_sharding.py` targets `SPY` | Uses implementation-defined hashing; no stable partition configuration or multi-instrument coverage |
| Sequencing | Partially implemented | Each sequencer increments one local counter, wraps it as a `CommandSequence`, derives the same-valued `OrderId`, and increments a per-port counter | `sequencer/include/sequencer.hpp`, `sequencer/src/sequencer.cpp` | Sharding tests and synthetic throughput tests compile in the full build | “Global” numbers are not unique across sequencers; no authoritative durable order |
| BUY matching | Partially implemented | Incoming BUY can match eligible resting SELL orders by ascending sell price and FIFO queue order; resting GTC capacity is preflighted before mutation and returns one immutable rejection event on failure | `matching_engine/src/matching_engine.cpp` | Partial-fill, unfillable-FOK, aggregate-boundary, aggregate-preflight, and exact rejection-event tests | Normal processing cannot create resting sells; trade, rest, and IOC-remainder events and complete matching invariants are missing |
| SELL matching | Stubbed | `processSellOrder` calls an empty `matchSellOrder` | `matching_engine/src/matching_engine.cpp` | None | SELL orders neither match nor rest through the normal path |
| Order books | Partially implemented | Nested symbol/strong-price maps store FIFO order queues and strong per-side aggregate quantities; a sequenced GTC capacity failure returns exactly one `CommandRejected` with `(commandSequence, eventIndex 0)` and leaves the incoming order and both sides of the book unchanged | `matching_engine/include/matching_engine.hpp`, `matching_engine/src/matching_engine.cpp` | Exact aggregate maximum, next-unit rejection, exact event content, and crossing-GTC no-partial-mutation tests | The engine outcome is not published; duplicate IDs, expiry cleanup, symmetric matching, and safe removal are incomplete |
| Cancellation processing | Stubbed | CANCEL and CANCELREJ switch branches exist but do nothing | `matching_engine/src/matching_engine.cpp` | Parsing only | Cannot cancel an active order or emit a cancel result |
| IOC BUY | Partially implemented | Matches immediately and does not rest the remainder | `matching_engine/src/matching_engine.cpp` | No direct IOC remainder test | BUY side only; no execution result |
| FOK BUY | Partially implemented | Checks aggregate eligible sell quantity before matching | `matching_engine/src/matching_engine.cpp` | One unfillable-FOK test | Successful multi-level FOK and invariant preservation are untested |
| DAY/GTC/GTD BUY | Partially implemented | Matches and rests a remaining BUY quantity | `matching_engine/src/matching_engine.cpp` | No complete lifecycle tests | Resting expiry is not actively enforced |
| ATC | Parsed only | Parser accepts ATC and assigns an expiry | Parser and TimeInForce types | Parser test | Matching branch performs no action |
| Execution reports | Missing | No complete client-facing acceptance, rejection, fill, or order-state report is produced | — | None | FIX client receives no business result |
| Business event model | Partially implemented | Defines immutable `CommandRejected`, `Trade`, `OrderRested`, and `OrderCancelled` value types and a per-command matching outcome; `CommandRejected` accepts only post-sequencing `CommandRejectionReason` values, while `DuplicateCommandConflict` is a distinct admission-only reason; capacity rejection is the only generated event | `core/domain/include/business_events.hpp`, `matching_engine/include/matching_engine.hpp` | Compile-time immutability and rejection-category separation checks, plus deterministic capacity-rejection field assertions | No admission response model, event sink, bounded event queue, publication, persistence, or delivery; normal trades, rests, IOC remainders, and cancellations do not generate events |
| Trade events | Stubbed | The immutable adopted `Trade` schema exists, but matching still mutates quantities without constructing it | `core/domain/include/business_events.hpp` | Schema compilation only | No generated trade event, price/counterparty result stream, or publication |
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

- A full resting SELL fill passes a reference to the queue's front element into removal logic. The
  element is popped before all later reads of that reference complete, creating a potential
  use-after-destruction.
- Active order IDs are stored in a set, but duplicate insertion is not rejected. Two orders can share
  an ID while the index represents only one value.
- Client order IDs, sessions, and symbols rely on `std::hash`; collisions and cross-platform results
  are not handled as identity concerns.
- Client IDs are still represented by session hashes rather than the adopted stable identity model.
- Resting order expiry is not regularly checked, and `checkOrderExpiry` is not part of the normal
  matching loop.
- An accepted parser TimeInForce can reach a matching branch that performs no business action.
- Several queue transitions remove a message from the previous queue before confirming the next
  queue accepted it. Failure can log or be ignored without a client-visible result.
- Four sequencer threads merge into one matching-engine queue, so cross-shard processing order is
  determined by concurrent insertion rather than one documented sequence.
- `FixTask` and `MatchingEngine` spin continuously when idle, while sequencers sleep for 10 ms between
  polls.
- Except for the returned book-capacity rejection, matching results are not represented as business
  events. The queue-drain path currently discards the returned engine outcome, and no events are
  durable, published, or client-visible.
- The unused legacy `Bus::read` pointer overload does not return its pointer assignment to the
  caller, can underflow its overwrite test, and allocates one message before indexing it as an
  array. Current callers use the tested stack-message overload instead.

## Existing test coverage

| Test area | Files | What is covered | Important gaps |
|---|---|---|---|
| Multicast bus | `core/bus/tests/bus_test.cpp` | Basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Multiple writers, cursor lifetime, journal behavior |
| FIX parsing | `core/fix/tests/test_fix_parser.cpp`, `test_fix_parser_edgecases.cpp`, `test_fix_malformed.cpp` | Basic order parsing, required and bounded client command ID, exact `SPY` v1 price/quantity normalization, configured maxima, off-tick/lot/range rejection, unknown instrument, selected TimeInForce/expiry combinations, malformed values | Stable client identity, other instruments, remaining independent required-field tests, authoritative admission/deduplication, client-visible responses |
| FIX task | `core/fix/tests/test_fix_task.cpp` and edge-case tests | `fromApp` queues supported parsed messages and catches parser failures | Continuous run loop, queue saturation, client rejection delivery |
| Matching engine | `matching_engine/tests/test_matching_engine.cpp` | Strong-domain separation and client-command-ID bounds, immutable event definitions, queue drain, one partial BUY fill, one unfillable FOK case, aggregate maximum and next-unit boundary, and an exact single capacity-rejection event without partial mutation | SELL path, generated trade/rest/cancel events, full fill, multiple fills, price priority, FIFO, cancel processing, duplicate ID, expiry, and event publication |
| C++ timed pipeline | `sequencer/tests/test_e2e_throughput.cpp` | Queue and parser plumbing under timed load | Real matching behavior, reproducibility, percentiles, drops, controlled hardware/build settings |
| Live FIX sharding | `sequencer/tests/test_sequencer_sharding.py` | Same symbol observed on a consistent shard | Business result, matching, recovery, globally defined fairness |

## Build and runtime verification

- **Last source inspection:** 2026-08-18
- **Inspected base commit:** `1186420` with the documented working-tree changes
- **Build performed during this change:** Yes. Building `sequencer/Dockerfile` completed the full
  CMake target graph, including all libraries, the exchange executable, and all configured test
  executables. The Dockerfile installs GoogleTest from Ubuntu's package repository. Native CMake
  configuration on the host was not rerun; the earlier attempt failed because Boost was unavailable.
- **Tests performed during this change:** Yes, from that built Docker image: `BusUnitTests`,
  `FixParserUnitTests`, `FixTaskUnitTests`, and `MatchingEngineUnitTests` all passed (4/4).
- **Runtime stack verification performed:** No

The strong-domain migration, focused numeric behavior, capacity-rejection event, and stack-message
bus read used by `FixTask` are covered by passing deterministic tests in the documented Docker build
environment. No event publication, end-to-end, or live runtime behavior was verified. The Docker
environment is command-reproducible but not bit-for-bit pinned: the Ubuntu base tag and apt package
versions remain mutable.
