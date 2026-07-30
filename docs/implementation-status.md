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
| C++ exchange executable | Partially implemented | Constructs FIX handling, four sequencers, one matching engine, queues, and worker threads | `core/exchange/src/main.cpp` | Python sharding fixtures expect the executable | No coordinated shutdown; container does not launch it by default |
| FIX acceptor | Partially implemented | Configures a QuickFIX FIX 4.2 acceptor on port 5001 | `core/exchange/src/main.cpp`, `exchange.cfg` | Python test client and sharding tests | One configured session; live startup was not verified |
| New Order Single parsing | Partially implemented | Recognizes `35=D`, limit BUY/SELL, quantity, price, symbol, client order ID, TimeInForce, and expiry | `core/fix/src/fix_parser.cpp` | `core/fix/tests/test_fix_parser*.cpp`, `test_fix_malformed.cpp` | Symbol and ID requirements are incomplete; numeric conversion has range and precision risks |
| Cancel Request parsing | Parsed only | Recognizes `35=F` and produces an internal CANCEL message | `core/fix/src/fix_parser.cpp`, `core/fix/src/fix_task.cpp` | Parser and FixTask tests | Does not establish a complete original-order target; engine ignores CANCEL |
| Order modification | Missing | FIX modification messages are not processed | — | None | No amend or cancel-replace behavior |
| TimeInForce parsing | Partially implemented | Defaults to DAY; parses DAY, GTC, IOC, FOK, GTD, and ATC with several expiry checks; rejects GTX | `core/fix/src/fix_parser.cpp`, `core/task/include/time_in_force.hpp` | Parser validation tests | Matching behavior is incomplete or absent for several accepted values |
| Internal order message | Partially implemented | Stores IDs, sequence fields, price, quantity, fixed symbol buffer, side, expiry, shard, and TimeInForce | `sequencer/include/sequence_message.hpp` | Used throughout C++ tests | Several field meanings and uniqueness scopes are undefined |
| In-process bounded queue | Partially implemented | Wraps `boost::lockfree::queue` with `push`, `pop`, and `empty` | `core/shared_queue/include/shared_queue.hpp` | Exercised indirectly | It is not process-shared IPC; full-queue policy is incomplete |
| Multicast bus | Partially implemented | Circular in-process buffer with registered reader cursors and overwrite protection | `core/bus/include/bus.hpp` | `core/bus/tests/bus_test.cpp` | Intended writer model is implicit; journal output is not a recovery log |
| Symbol sharding | Partially implemented | Hashes the truncated symbol into four shard queues | `core/fix/src/fix_parser.cpp`, `core/exchange/src/main.cpp` | `sequencer/tests/test_sequencer_sharding.py` | Uses implementation-defined hashing; no stable partition configuration |
| Sequencing | Partially implemented | Each sequencer increments one local counter and a per-port counter | `sequencer/include/sequencer.hpp`, `sequencer/src/sequencer.cpp` | Sharding tests and synthetic throughput tests | “Global” numbers are not unique across sequencers; no authoritative durable order |
| BUY matching | Partially implemented | Incoming BUY can match eligible resting SELL orders by ascending sell price and FIFO queue order | `matching_engine/src/matching_engine.cpp` | Two focused matching tests | Normal processing cannot create resting sells; complete matching invariants are untested |
| SELL matching | Stubbed | `processSellOrder` calls an empty `matchSellOrder` | `matching_engine/src/matching_engine.cpp` | None | SELL orders neither match nor rest through the normal path |
| Order books | Partially implemented | Nested symbol/price maps store FIFO order queues and aggregate quantity | `matching_engine/include/matching_engine.hpp`, `matching_engine/src/matching_engine.cpp` | Partial BUY-side tests | Duplicate IDs, expiry cleanup, symmetric matching, and safe removal are incomplete |
| Cancellation processing | Stubbed | CANCEL and CANCELREJ switch branches exist but do nothing | `matching_engine/src/matching_engine.cpp` | Parsing only | Cannot cancel an active order or emit a cancel result |
| IOC BUY | Partially implemented | Matches immediately and does not rest the remainder | `matching_engine/src/matching_engine.cpp` | No direct IOC remainder test | BUY side only; no execution result |
| FOK BUY | Partially implemented | Checks aggregate eligible sell quantity before matching | `matching_engine/src/matching_engine.cpp` | One unfillable-FOK test | Successful multi-level FOK and invariant preservation are untested |
| DAY/GTC/GTD BUY | Partially implemented | Matches and rests a remaining BUY quantity | `matching_engine/src/matching_engine.cpp` | No complete lifecycle tests | Resting expiry is not actively enforced |
| ATC | Parsed only | Parser accepts ATC and assigns an expiry | Parser and TimeInForce types | Parser test | Matching branch performs no action |
| Execution reports | Missing | No complete client-facing acceptance, rejection, fill, or order-state report is produced | — | None | FIX client receives no business result |
| Trade events | Missing | Matching mutates quantities without creating a trade record | — | None | No trade ID, price, counterparties, or audit event |
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
- Floating-point FIX price conversion multiplies by 10,000 and casts to `uint64_t` without a complete
  exactness or upper-bound check.
- Missing or overlong symbols and missing client order IDs are not consistently rejected. Long
  symbols are truncated.
- Resting order expiry is not regularly checked, and `checkOrderExpiry` is not part of the normal
  matching loop.
- An accepted parser TimeInForce can reach a matching branch that performs no business action.
- Several queue transitions remove a message from the previous queue before confirming the next
  queue accepted it. Failure can log or be ignored without a client-visible result.
- Four sequencer threads merge into one matching-engine queue, so cross-shard processing order is
  determined by concurrent insertion rather than one documented sequence.
- `FixTask` and `MatchingEngine` spin continuously when idle, while sequencers sleep for 10 ms between
  polls.
- Matching state changes are not paired with durable or client-visible events.

## Existing test coverage

| Test area | Files | What is covered | Important gaps |
|---|---|---|---|
| Multicast bus | `core/bus/tests/bus_test.cpp` | Basic ordering, wrap-around, multiple cursors, capacity, and one-writer/one-reader concurrency | Multiple writers, cursor lifetime, journal behavior |
| FIX parsing | `core/fix/tests/test_fix_parser.cpp`, `test_fix_parser_edgecases.cpp`, `test_fix_malformed.cpp` | Basic order parsing, selected invalid fields, TimeInForce/expiry combinations, malformed values | Identifier scope, exact price conversion, upper bounds, independent required-field tests |
| FIX task | `core/fix/tests/test_fix_task.cpp` and edge-case tests | `fromApp` queues supported parsed messages and catches parser failures | Continuous run loop, queue saturation, client rejection delivery |
| Matching engine | `matching_engine/tests/test_matching_engine.cpp` | Queue drain, one partial BUY fill, one unfillable FOK case | SELL path, full fill, multiple fills, price priority, FIFO, cancel, duplicate ID, expiry, events |
| C++ timed pipeline | `sequencer/tests/test_e2e_throughput.cpp` | Queue and parser plumbing under timed load | Real matching behavior, reproducibility, percentiles, drops, controlled hardware/build settings |
| Live FIX sharding | `sequencer/tests/test_sequencer_sharding.py` | Same symbol observed on a consistent shard | Business result, matching, recovery, globally defined fairness |

## Build and runtime verification

- **Last source inspection:** 2026-07-26
- **Inspected commit:** `1e80a02`
- **Build performed during this documentation change:** No
- **Tests performed during this documentation change:** No
- **Runtime stack verification performed:** No

The status table is based on source and test inspection. “Unverified” must remain until the relevant
commands are run and their results are recorded.
