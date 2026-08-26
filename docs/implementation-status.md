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
| C++ exchange executable | Partially implemented | Source constructs FIX handling, one shared 100,000-record command-admission index, four sequencers, one matching engine, command queues, a 1,000-batch command-result boundary, and worker threads | `core/exchange/src/main.cpp` | Targeted Docker build compiles the executable; Python sharding fixtures expect it | No admission-completion or result-boundary consumer wiring and no coordinated shutdown; container does not launch it by default; live startup was not verified |
| FIX acceptor | Partially implemented | Configures a QuickFIX FIX 4.2 acceptor on port 5001. Each session requires a nonzero exact-decimal `ExchangeClientId`; the repository's `EXCHANGE`/`CLIENT` session maps to stable `ClientId{1}` | `core/exchange/src/main.cpp`, `exchange.cfg` | Configuration-loading identity test plus Python client and sharding tests | One production session is configured; live startup was not verified |
| FIX client identity | Implemented for adopted initial scope | At startup, an immutable resolver copies exact QuickFIX `SessionID` mappings from exchange configuration. Multiple explicitly configured sessions may map to one strong nonzero `ClientId`; unknown identities reject before normalization is queued. `FixTask` borrows the composition root's longer-lived resolver. The legacy session hash remains only in non-authoritative `port` plumbing | `core/fix/include/client_identity.hpp`, `core/fix/src/client_identity.cpp`, `core/exchange/src/main.cpp` | Stable repeated/reconnect resolution, alternate same-client session, distinct clients, exact qualifier comparison, unknown rejection without parser/command-sequence consumption, configured repository session, invalid configuration, and forced changed/colliding legacy-port ownership integration | Configuration reload, authentication beyond exact configured QuickFIX identity, and private rejection delivery are not implemented |
| Command admission | Partially implemented | One composition-root-owned, mutex-protected, bounded in-process index atomically classifies exact strong `(ClientId, ClientCommandId)` records as first, identical in-flight, identical completed, conflicting reuse, or unavailable. Records retain the complete normalized NewOrder or Cancel business command by value and never use a hash as sole identity. Completed records retain their original shared immutable result batch without eviction; explicit complete, retrieve, and pre-commit abandon APIs exist. `FixTask` reserves after normalization and forwards only first submissions | `core/admission/include/command_admission.hpp`, `core/admission/src/command_admission.cpp`, `core/fix/src/fix_task.cpp`, `core/exchange/src/main.cpp` | Deterministic NewOrder/Cancel identity and conflict matrices, generated/transport-field exclusion, cross-client reuse, exact completed-batch retrieval, no-sleep concurrent reservation, abandon/retry, bounded fail-closed behavior, and FIX-to-sequencer duplicate/conflict/unavailable gating | Matching results are not yet wired back to complete reservations, and no client result delivery/redelivery exists. Consequently executable reservations remain in flight until later completion wiring and the configured bound eventually fails closed. Journal reconstruction, recovery replay, and private FIX responses are absent |
| New Order Single parsing | Partially implemented | Accepts configured `SPY` v1 limit orders, resolves the session's configured stable `ClientId`, requires an exact bounded `ClientCommandId`, parses price and quantity from decimal text into strong values, enforces tick/lot and configured maximums, records instrument/configuration IDs, and passes normalized commands through first-submission admission gating | `core/domain/include/domain_types.hpp`, `core/instrument/include/instrument_config.hpp`, `core/fix/src/fix_parser.cpp`, `core/fix/src/fix_task.cpp` | Deterministic stable-client, required-client-command-ID, configured-boundary, malformed-value, unknown-instrument, admission-gating, and existing parser tests; domain tests cover client-command-ID bounds | Private responses and admission result-delivery/completion wiring are not implemented |
| FIX Cancel Request normalization | Implemented for adopted scope | Accepts `35=F` only from a configured stable `ClientId` with an exact bounded `ClOrdID(11)`, configured symbol, and positive exact-decimal uint64 `OrderID(37)`. It stores `OrderID` as strong `TargetOrderId`, leaves NewOrder-only `OrderId` zero, resolves stable `InstrumentId`, does not use `OrigClOrdID(41)` as order identity, and passes normalized commands through first-submission admission gating. Invalid inputs reject before queueing or sequencing | `core/fix/src/fix_parser.cpp`, `core/fix/src/fix_task.cpp` | Stable same-client alternate-session ownership, distinct-client `NotOwner`, exact target/identifier separation and uint64 boundaries, malformed/unknown rejection, admission gating, sequencing, and focused FIX-to-matcher cancellation | Admission result-delivery/completion wiring, private FIX responses, persistence, and recovery are not implemented |
| Order modification | Missing | FIX modification messages are not processed | — | None | No amend or cancel-replace behavior |
| TimeInForce parsing | Implemented for adopted scope | Requires FIX tag 59 for New Order Single and accepts only GTC and IOC without expiry. Missing, DAY, FOK, GTD, GTX, ATC, unknown values, and expiry-bearing orders are rejected before sequencing | `core/fix/src/fix_parser.cpp`, `core/task/include/time_in_force.hpp` | Deterministic acceptance tests for GTC/IOC and rejection tests for every deferred, unknown, missing, and expiry-bearing value | Parser failures are caught by `FixTask` but are not delivered to clients; deferred TimeInForce behavior is not implemented |
| Internal order message | Partially implemented | Uses distinct strong values for command sequence, order ID, target order ID, client ID, client command ID, instrument ID, price ticks, and quantity units. `TargetOrderId` is an explicit optional cancel-only field in the shared message representation | `core/domain/include/domain_types.hpp`, `sequencer/include/sequence_message.hpp` | Compile-time type separation plus parser, bus, sequencer, identity, and matching tests | Legacy hashed `id` and non-authoritative `port`/topic fields, optional command-specific fields, and several uniqueness scopes remain; the stub JSON representation does not serialize client identity or `TargetOrderId` |
| In-process bounded queue | Partially implemented | Wraps `boost::lockfree::queue` with `push`, `pop`, and `empty` | `core/shared_queue/include/shared_queue.hpp` | Exercised indirectly | It is not process-shared IPC; full-queue policy is incomplete |
| Command-result boundary | Partially implemented | A mutex-protected bounded in-process FIFO atomically accepts one shared immutable `CommandResultBatch` containing command sequence, processing result, and the complete ordered event vector. Matching retains one failed handoff and retries it before reading another command | `matching_engine/include/command_result_queue.hpp`, `matching_engine/src/matching_engine.cpp` | Deterministic complete multi-event delivery, event order, capacity-one saturation, repeated full-boundary retries, recovery, and exactly-once dequeue assertions | No consumer, persistence, recovery reconstruction, client routing, or market-data fan-out; the executable eventually remains backpressured when its 1,000 accepted batches are not drained |
| Multicast bus | Partially implemented | Circular in-process buffer with registered reader cursors and overwrite protection; stack-message reads copy an available slot before advancing the cursor | `core/bus/include/bus.hpp` | `BusUnitTests` covers stack-message reads, basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Intended writer model is implicit; the unused legacy pointer/journal read overload is unsafe; journal output is not a recovery log |
| Symbol sharding | Partially implemented | Resolves the sole configured `SPY` instrument, then hashes its symbol into four shard queues | `core/fix/src/fix_parser.cpp`, `core/exchange/src/main.cpp` | `sequencer/tests/test_sequencer_sharding.py` targets `SPY` | Uses implementation-defined hashing; no stable partition configuration or multi-instrument coverage |
| Sequencing | Partially implemented | Each sequencer increments one local counter and wraps it as a `CommandSequence`; NewOrder derives the same-valued `OrderId`, while Cancel receives its own sequence, keeps NewOrder-only `OrderId` zero, and preserves its independent `TargetOrderId`. The normalized FIX path admits only a first submission before sequencing. A legacy per-port topic counter is also incremented but does not determine `ClientId` | `sequencer/include/sequencer.hpp`, `sequencer/src/sequencer.cpp` | Deterministic stable-identity, normalized FIX Cancel, and admission-gating integration verifies unknown/duplicate/conflict/unavailable outcomes do not consume a sequence, distinct sequence assignment, target/client preservation, and forwarding to matching; sharding and synthetic throughput tests also exist | “Global” numbers are not unique across sequencers; the adopted single FIFO boundary and durable sequence-assignment rule are not implemented |
| BUY matching | Partially implemented | Incoming BUY matches the lowest eligible asks, then stable FIFO nodes within each level. The immutable command drives a local remainder. Each maker-price fill updates node/index/aggregate state and emits one ordered `Trade`; a positive GTC remainder rests before one `OrderRested`, while a positive IOC remainder emits one `OrderCancelled(IocRemainder)` without resting. Full fills have no terminal event. The exact result size, including a terminal event, and GTC capacity are preflighted before mutation; results above 4,096 events produce one `CommandRejected(BookCapacityExceeded)` at index zero. Queue drain hands each complete outcome to the bounded command-result boundary | `matching_engine/src/matching_engine.cpp` | Exact multi-maker/multi-level trades, unmatched/partial/full GTC and IOC terminal outcomes, event fields/order, maker price, best price, FIFO, aggregate/index cleanup, instrument isolation, command immutability, unsupported-TimeInForce invariant failure, capacity preflight, 4,095/4,096/4,097-event boundaries, and atomic result handoff | Accepted batches have no consumer, persistence, publication, or client delivery |
| SELL matching | Partially implemented | Incoming SELL symmetrically matches the highest eligible bids with the same immutable-command, maker-price, exact result-size planning, GTC-rest, IOC-cancel, full-fill, atomic result handoff, and no-partial-mutation guarantees as BUY | `matching_engine/src/matching_engine.cpp` | Symmetric exact multi-maker/multi-level trades, unmatched/partial/full GTC and IOC terminal outcomes, event fields/order, best price, FIFO, state invariants, isolation, immutability, unsupported-TimeInForce failure, capacity preflight, 4,095/4,096/4,097-event boundaries, and atomic result handoff | Accepted batches have no consumer, persistence, publication, or client delivery |
| Order books | Partially implemented | `InstrumentId` keys one per-instrument bid/ask book; strong-price levels own stable FIFO list nodes and strong per-side aggregates. One active-order index maps each resting `OrderId` to owner, instrument, side, price, remaining quantity, and stable node location. Fills and successful cancellation update node/index/aggregate state consistently and clean empty levels/books. A duplicate active authoritative `OrderId` is an invariant failure. Symmetric aggregate-capacity and 4,096-event preflights preserve no-partial-mutation rejection | `matching_engine/include/matching_engine.hpp`, `matching_engine/src/matching_engine.cpp` | Symmetric fill lifecycle, successful BUY/SELL and partially-filled cancellation, stable FIFO, node/index/aggregate agreement, empty cleanup, instrument isolation, terminal-index absence, duplicate-ID invariant failure, aggregate and event-count boundaries, and no-partial-mutation tests | Matching outcomes are not published; reconstruction and retransmission state are absent |
| Cancellation processing | Partially implemented | Authoritative FIX identity and `OrderID(37)` normalization pass first-submission admission before sequencing and the existing matching seam. Matching uses stable `ClientId` ownership, returning exact `OrderNotActive` or `NotOwner` rejections without mutation, or atomically removing the owner's current remainder and emitting one `OrderCancelled(ClientRequested)`. `CANCELREJ`, wrong routing, and inconsistent internal state are invariant failures | `core/fix/src/fix_parser.cpp`, `core/admission/src/command_admission.cpp`, `sequencer/src/sequencer.cpp`, `matching_engine/src/matching_engine.cpp` | Focused identical/conflicting Cancel admission, alternate-session same-client success and different-client `NotOwner` with forced legacy-port changes/collisions, plus exact matcher BUY/SELL cancellation, inactive targets, ownership, FIFO/aggregate/cleanup invariants, required fields, and fill-versus-cancel order | Admission completion/redelivery, private FIX response, publication, persistence, recovery, and delivery are absent; direct internal matcher submissions bypass FIX admission by design |
| IOC BUY and SELL | Partially implemented | Match immediately on both sides; a positive remainder never rests and emits exactly one `OrderCancelled(IocRemainder)` after all trades. A fully filled IOC emits only trades | `matching_engine/src/matching_engine.cpp` | Exact unmatched, partial, and full outcomes on both sides, including cancellation fields/index, cleanup, aggregate invariants, self-trade, and isolation | Outcomes are not published, persisted, or delivered |
| Resting BUY/SELL | Partially implemented | GTC matches symmetrically, rests a positive local remainder, then emits exactly one `OrderRested`; a fully filled GTC emits only trades. Active remainders can be cancelled at the normalized engine seam | `matching_engine/src/matching_engine.cpp` | Exact unmatched, partial, full, and cancellation outcomes on both sides, including fields/indexes, active state, aggregates, and immutable commands | Publication, persistence, retransmission, and delivery remain incomplete |
| Execution reports | Missing | No complete client-facing acceptance, rejection, fill, or order-state report is produced | — | None | FIX client receives no business result |
| Business event model | Partially implemented | Defines immutable adopted event values and a per-command matching outcome. NewOrder uses checked arithmetic to preplan its exact event count, including any terminal event, and reserves normal event storage only after enforcing the 4,096-event limit. An over-limit result produces one independently constructed `CommandRejected(BookCapacityExceeded)` at index zero before mutation. Cancel reserves its single outcome before mutation. Queue drain moves the complete outcome into one immutable batch and atomically hands it to a bounded FIFO; a failed handoff remains pending and blocks the next command | `core/domain/include/business_events.hpp`, `matching_engine/include/command_result_queue.hpp`, `matching_engine/include/matching_engine.hpp` | Compile-time immutability/category separation, exact symmetric event fields and ordering, exact cancellation outcomes, event-limit boundaries, and deterministic atomic-handoff/backpressure coverage | Accepted-batch persistence, recovery, publication, client routing, market-data fan-out, and external delivery are not implemented |
| Trade events | Partially implemented | BUY and SELL matching construct deterministic adopted `Trade` values in exact price-time order with maker-price executions and post-fill maker/taker quantities | `core/domain/include/business_events.hpp`, `matching_engine/src/matching_engine.cpp` | Exact fields and event IDs for three executions across multiple makers and prices on each taker side; self-trade is covered | Complete outcomes enter the in-process result boundary, but no consumer, publication, persistence, client delivery, or market-data use exists |
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

- Legacy message `id`, session-derived `port`, and symbol sharding still use `std::hash`; collisions
  and cross-platform values remain unsuitable for authoritative message, topic, or partition
  identity. Stable `ClientId` resolution and matching ownership do not use those hashes.
- The immutable FIX client resolver loads exact identities only at startup. Adding or changing an
  authorized identity requires a configuration change and process restart; authentication beyond
  QuickFIX's configured session identity is not implemented.
- Deferred TimeInForce and expiry behavior is deliberately rejected at FIX normalization. If an
  unsupported TimeInForce bypasses normalization, matching treats it as an invariant failure before
  book mutation.
- FIX cancellation normalizes positive exact-decimal `OrderID(37)` into `TargetOrderId`, resolves
  stable configured `ClientId`, and ignores `OrigClOrdID(41)` for order identity. No private FIX
  business response exists.
- The authoritative in-process admission index gates normalized FIX commands before sequencing.
  Its complete/retrieve APIs preserve original immutable result batches, but matching-result
  completion and client delivery/redelivery are not connected yet. Until that wiring exists,
  executable reservations remain in flight and the configured 100,000-record bound eventually
  fails closed without eviction, sequencing, or a business event.
- Direct internal matching-engine submissions do not pass through admission. A new direct Cancel
  command against an already-terminal target deterministically returns `OrderNotActive`.
- Several queue transitions remove a message from the previous queue before confirming the next
  queue accepted it. Failure can log or be ignored without a client-visible result.
- Four sequencer threads merge into one matching-engine queue, so cross-shard processing order is
  determined by concurrent insertion rather than the adopted single FIFO sequencing boundary.
- `FixTask` and `MatchingEngine` spin continuously when idle, while sequencers sleep for 10 ms between
  polls.
- Trade, terminal, cancellation, and command-rejection outcomes enter the bounded in-process result
  boundary as complete immutable batches. No consumer exists yet, so the executable's 1,000-batch
  queue eventually fills and matching retains one pending result while applying backpressure.
- Per-command events use an exactly planned, dynamically allocated vector, and NewOrder enforces the
  adopted 4,096-event limit before mutation. The first atomic in-process handoff and pre-accept
  backpressure stage is implemented, but accepted batches are not durable or recoverable.
- The unused legacy `Bus::read` pointer overload does not return its pointer assignment to the
  caller, can underflow its overwrite test, and allocates one message before indexing it as an
  array. Current callers use the tested stack-message overload instead.

## Existing test coverage

| Test area | Files | What is covered | Important gaps |
|---|---|---|---|
| Multicast bus | `core/bus/tests/bus_test.cpp` | Basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Multiple writers, cursor lifetime, journal behavior |
| FIX client identity | `core/fix/tests/test_client_identity.cpp` | Repository configuration, deterministic reconnect/alternate-session mapping, distinct clients, exact identity comparison, configured NewOrder/Cancel identity, unknown pre-sequencing rejection, and invalid configuration | Runtime configuration reload, richer authentication, client-visible rejection |
| Command admission | `core/admission/tests/test_command_admission.cpp`, `sequencer/tests/test_e2e_throughput.cpp` | Exact strong keying; identical in-flight NewOrder/Cancel; exact completed-batch and event-ID retrieval; conflict detection for all adopted NewOrder and Cancel business fields; exclusion of generated, parser, timestamp, transport, and legacy fields; cross-client reuse; no-sleep concurrency; abandon/retry; bounded no-eviction failure; and FIX queue/sequence gating | Matching-result completion wiring, waiting/coalesced delivery, private responses, persistence, recovery reconstruction, and reconnect replay |
| FIX parsing | `core/fix/tests/test_fix_parser.cpp`, `test_fix_parser_edgecases.cpp`, `test_fix_malformed.cpp` | Basic order parsing, stable configured client identity, required and bounded client command ID, exact `SPY` v1 price/quantity normalization, configured maxima, off-tick/lot/range rejection, unknown instrument, supported TimeInForce, and authoritative Cancel normalization with exact target preservation and malformed/zero/overflow rejection | Other instruments, remaining independent required-field tests, client-visible responses |
| FIX task | `core/fix/tests/test_fix_task.cpp`, `test_client_identity.cpp`, edge-case tests, and `sequencer/tests/test_e2e_throughput.cpp` | `fromApp` queues only first-submission normalized NewOrder and Cancel messages, catches parser failures, and leaves invalid, unauthorized, duplicate, conflicting, and admission-unavailable submissions out of downstream queues without sequence consumption | Continuous run loop, deterministic normalization-queue saturation, admission completion, and client rejection/result delivery |
| Matching engine | `matching_engine/tests/test_matching_engine.cpp` | Strong-domain/event separation, symmetric matching and terminal outcomes, immutable commands, client-request cancellation of BUY/SELL and partial remainders, exact success/rejection events, all inactive target classes, ownership, FIFO/aggregate/active-index/empty cleanup invariants, fill-versus-cancel order, required cancel fields, invalid internal kinds, duplicate active IDs, aggregate-capacity and 4,095/4,096/4,097-event boundaries, complete immutable result batches, event order, saturation backpressure, recovery, and no duplicate handoff | Retransmission/deduplication, accepted-batch persistence/recovery, routing, publication, and external delivery |
| C++ pipeline | `sequencer/tests/test_e2e_throughput.cpp` | Queue and parser plumbing under timed load plus deterministic admission, normalized FIX Cancel, and stable-client paths through sequencing to matching, including duplicate/conflict/unavailable no-sequence behavior, alternate-session cancellation, cross-client `NotOwner`, unknown no-sequence behavior, and changed/colliding legacy ports | Result completion/redelivery integration, reproducible performance, percentiles, drops, controlled hardware/build settings |
| Live FIX sharding | `sequencer/tests/test_sequencer_sharding.py` | Same symbol observed on a consistent shard | Business result, matching, recovery, globally defined fairness |

## Build and runtime verification

- **Last source inspection:** 2026-08-25
- **Inspected base commit:** `ef80f9c`. The worktree was clean at the start of this admission slice;
  the committed stable FIX identity, authoritative Cancel normalization, event-limit, and atomic
  result-handoff work were preserved without reset or stash.
- **Build performed during this change:** Yes. `docker compose build sequencer` completed the full
  configured CMake build, including the new `command_admission_tests`, existing FIX targets,
  `sequencer_app`, `e2e_throughput_tests`, and `matching_engine_tests`. Native CMake was not run
  because the supported Docker toolchain covered every affected C++ target.
- **Tests performed during this change:** Yes. Focused stable identity passed 6/6, parser and Cancel
  normalization passed 28/28 in the parser binary, FixTask behavior passed 23/23, admission passed
  11/11, sequencer admission/identity/Cancel integration passed 3/3, and matching cancellation plus
  result-handoff regression passed 10/10. Full deterministic `CommandAdmissionUnitTests`,
  `MatchingEngineUnitTests`, `FixClientIdentityUnitTests`, `FixParserUnitTests`, and
  `FixTaskUnitTests` passed through CTest (5/5). Docker-hosted
  `clang-format --dry-run --Werror` passed over new/fully formatted files and changed legacy ranges
  after one changed-range wrap was corrected; `git diff --check` passed.
- **Runtime stack verification performed:** No; live FIX reconnect/logon, startup, and private result
  delivery remain outside this scoped admission change.

The `InstrumentId`-keyed books, active-index lifecycle, symmetric price-time matching, immutable
incoming command handling, deterministic maker-price trades and GTC/IOC terminal outcomes,
aggregate-capacity behavior, the exact 4,096-event NewOrder bound, supported-TimeInForce boundary,
normalized client cancellation, first-submission admission gating, atomic complete-batch handoff,
bounded-boundary backpressure, and stack-message bus read used by `FixTask` are covered by passing
deterministic tests in the documented Docker build environment. No admission result-completion
wiring, accepted-batch consumer, persistence, recovery, private client response routing,
market-data use, external end-to-end result, sanitizer run, timed benchmark execution, or live
runtime behavior was verified during this change. The Docker
environment is command-reproducible but not bit-for-bit pinned: the Ubuntu base tag and apt package
versions remain mutable.
