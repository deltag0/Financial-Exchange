# Intended Architecture

## Purpose

This document describes the architecture the project is working toward: component responsibilities,
ownership boundaries, and the flow of commands and events through a deterministic multi-instrument
exchange.

It does not describe implementation completeness. See
[Implementation Status](implementation-status.md) for that information. Observable exchange behavior
is defined only by adopted rules in [Exchange Rules](exchange-rules.md).

## Scope and constraints

The system is intended to be understandable and runnable by engineers on ordinary laptops. It should develop toward:

- bounded, independently identifiable exchange runs that can be paused, replayed, stopped, and
  explicitly reset;
- multiple instruments;
- deterministic and fair command processing;
- explicit sequencing;
- reliable bounded journaling, recovery, and replay;
- clear private execution and public market-data events;
- bounded resource usage and observable backpressure;
- testable component boundaries;
- measurable latency and throughput.

The architecture does not assume custom hardware, FPGAs, kernel-bypass networking, colocation, or a
large distributed deployment. Logical component boundaries do not require separate processes.
Components may initially be linked directly in one executable while retaining the same command,
event, ownership, and recovery contracts.

## Architectural principles

- One logical owner mutates an instrument's order book at a time.
- The sequencer assigns commands one authoritative run-global command sequence.
- Each state-owning partition processes its commands in increasing run-global sequence order and
  completes one command before beginning its next command.
- External protocols are translated into normalized internal commands before business processing.
- Matching behavior does not depend on a gateway protocol, wall-clock tie-breaking, or thread
  scheduling.
- State changes produce immutable typed events.
- An acknowledged command is either completed or recoverable according to the adopted durability
  rule.
- Queues and buffers are bounded and have explicit full, shutdown, and failure behavior.
- Recovery uses the same command and event model as normal processing.
- Components own only the state needed for their responsibility.
- Performance changes follow profiling and reproducible measurement.

## Command and event flow

```text
Client
  |
  v
Gateway and session adapter
  |
  v
Normalization and stateless validation
  |
  v
Authoritative command admission and retransmission
  |
  v
Run-global sequence assignment
  |
  v
Append immutable normalized command
  |
  v
Durable synchronization
  |
  v
Instrument partition
  |
  v
Matching engine and order book
  |
  v
Deterministic event stream
  |                         |
  v                         v
Private client events       Public market data
  |                         |
  v                         v
Gateway adapter             Market-data publisher
  |
  v
Business-result acknowledgement
```

Commands never travel backward through this path. Results are represented as events rather than by
mutating and returning the original command. The journal is authoritative for recovery; replay sends
its commands through the same matching path to regenerate events.

The intended showcase flow is: create or resume a bounded run, optionally start its deterministic
generated participants, observe a clearly labelled educational projection, submit and cancel orders
through the same normalized command path, pause or resume the run, and explicitly stop, replay, or
reset it. UI and operator controls use a control plane around the exchange-run controller; they do
not call the matcher directly. Participant-private output, any future public market data, and the
privileged educational projection remain distinct views even when one local UI displays all three.

## Components

### Exchange-run controller

#### Purpose

Own the lifecycle and fixed configuration of one bounded exchange run without participating in
matching decisions.

#### Owns

- durable monotonic allocation of `ExchangeRunId` and durable active/stopped run selection through a
  small installation-level run catalog;
- immutable run header configuration and capacity limits;
- lifecycle transitions among starting, recovery, ready, paused, capacity reached, stopped, and
  failure states;
- coordinated pause, stop, committed-work drain, and transport-binding invalidation;
- retention selection for the active run and at most one stopped run;
- deterministic generated-order scenario configuration, not generated order priority.

#### Does not own

- command ordering inside a run;
- order-book mutation or matching;
- participant identity;
- silent deletion of an active run;
- permission to weaken journal durability or corruption checks.

Generated participants submit through the same gateway-neutral normalization, admission,
sequencing, journaling, and matching path as interactive participants. Their seed controls generated
input; it does not replace the journal as exact replay input after user interaction.

The controller is the only lifecycle state writer. A clean process shutdown closes admission,
drains committed work, and persists the active run as paused. Restart and crash recovery also leave
the recovered run paused until an explicit resume. `CapacityReached` and `Stopped` are terminal for
new business submissions. Recovery components validate and report outcomes; they do not
independently publish lifecycle transitions.

The catalog durably stores the last reserved run ID, the active run and its persisted disposition,
and the retained stopped run. Catalog version 1 is one fixed 56-byte little-endian snapshot:

| Offset | Size | Field | Version 1 value or meaning |
|---:|---:|---|---|
| 0 | 4 | Magic | ASCII bytes `FXRC` |
| 4 | 2 | CatalogVersion | `1` |
| 6 | 2 | TotalLength | `56` |
| 8 | 8 | Generation | Monotonic nonzero snapshot generation |
| 16 | 8 | LastReservedRunId | `0` only before the first allocation |
| 24 | 8 | ActiveRunId | `0` if no active run |
| 32 | 1 | ActiveDisposition | `0` none, `1` Open, `2` Paused, `3` CapacityReached, `4` FailStopped, `5` RecoveryFailed |
| 33 | 7 | Reserved | All zero |
| 40 | 8 | RetainedStoppedRunId | `0` if no stopped run is retained |
| 48 | 4 | CRC32C | Same CRC32C parameters as journal V1 |
| 52 | 4 | Reserved | All zero |

The catalog checksum covers bytes 0 through 47 followed by bytes 52 through 55. Active and stopped
IDs must be distinct, nonzero IDs must not exceed `LastReservedRunId`, disposition zero is valid only
with active ID zero, and nonzero active ID requires a nonzero disposition. Generation and run ID
exhaustion stop further catalog mutation rather than wrapping.

An update writes the complete next generation to a new temporary file in the configured data
directory, durably synchronizes that file, atomically replaces the canonical catalog, and durably
synchronizes the directory. The implementation must reload and validate the canonical catalog after
an uncertain outcome. It never chooses an ID from a filename or an invalid temporary file. A missing
catalog is a new installation only when no run journals exist. Run journal files use the canonical
name `run-<ExchangeRunId>.fxjr` in that directory.

The implementation must finish the catalog update before reporting an allocation, retention
replacement, stop, or destructive reset as complete. An ID reserved before journal creation is never
reused even if creation fails.

The lifecycle transition table is:

| From | Trigger | To | Required boundary |
|---|---|---|---|
| `Starting` | Run selected or new header made durable | `Recovering` | No business admission |
| `Recovering` | New empty run validates and was explicitly started | `Ready` | Catalog exposes the active run before admission opens |
| `Recovering` | Existing run validates after restart or failure | `Paused` | Rebuild books, admission, and results; external replay suppressed |
| `Recovering` | Validation or replay fails | `RecoveryFailed` | Preserve evidence and keep admission closed |
| `Ready` | Pause or clean shutdown requested | `Paused` | Close new admission first, then drain committed work |
| `Paused` | Explicit resume | `Ready` | Revalidate active binding and capacity before admission opens |
| `Ready` | Next unique command cannot fit a run limit | `CapacityReached` | Persist disposition before returning the capacity response |
| `Ready`, `Paused`, or `CapacityReached` | Explicit end or replacement by a new run | `Stopped` | Drain committed work, invalidate bindings, then durably change retention |
| Running state | Durability or mutation becomes uncertain | `FailStopped` | Preserve owned work and close all new admission |
| `FailStopped` | Explicit recovery attempt | `Recovering` | Obtain exclusive journal ownership |
| `RecoveryFailed` | Corrective recovery attempt | `Recovering` | Never skip or reinterpret invalid records |

`Stopped` has no transition back to an active state. An explicitly confirmed destructive reset may
remove a stopped or `RecoveryFailed` run and its guarantees, but does not convert it into another run.

### Gateway and session adapters

#### Purpose

Translate an external protocol into normalized exchange commands and translate private exchange
events into protocol-specific responses.

#### Owns

- network connection and session lifecycle;
- protocol framing and parsing;
- authentication or session-to-client association;
- association of each transport session with a stable `ClientId` that survives reconnects;
- binding each transport session to exactly one active `ExchangeRunId` and invalidating that binding
  when the run ends;
- protocol-level validation;
- lossless conversion to and from internal command/event representations;
- delivery state needed by the protocol.

#### Does not own

- authoritative command ordering;
- authoritative logical-command deduplication;
- order books;
- matching decisions;
- trade formation;
- durable exchange state;
- public market-data policy.

Multiple gateway protocols may use the same normalized command model.

Initially, persisted exchange configuration maps each authorized FIX identity to its numeric
`ClientId`. A reconnect or another permitted session for the same client uses the same mapping;
transport-session hashes are not authoritative identities.

### Normalization and validation

#### Purpose

Create a protocol-independent command with explicit identifiers, instrument, side, price, quantity,
order type, TimeInForce, and any required ownership information.

#### Owns

- stateless required-field checks;
- exact external-to-internal numeric conversion;
- supported protocol-value mapping;
- external-symbol resolution to a stable `InstrumentId`;
- versioned instrument-configuration lookup;
- tick-size and lot-size validation;
- rejection of malformed or lossy representations.

#### Does not own

- state-dependent order existence checks;
- active-ID uniqueness decisions that require exchange state;
- matching-state ownership and cancellation checks;
- matching;
- sequencing policy.

The adopted rules determine which validation results must be sequenced for deterministic replay.
For the initial FIX adapter, `OrderID(37)` is parsed as the authoritative exchange-assigned
`TargetOrderId` for a Cancel. `OrigClOrdID(41)` may be kept for protocol correlation, but it is not
used to derive exchange identity.

### Command admission and retransmission

#### Purpose

Enforce logical command uniqueness across every gateway before a new run-global sequence is
assigned.

#### Owns

- the authoritative `(ExchangeRunId, ClientId, ClientCommandId)` admission index;
- atomic first-submission reservation across concurrent gateways;
- comparison of retransmissions with the original normalized command;
- coalescing or waiting while an identical original command is still in flight;
- returning the original result for an identical completed command;
- rejecting conflicting reuse before sequencing;
- reconstruction of the admission index from the command journal during recovery.

#### Does not own

- protocol parsing or session state;
- run-global command sequence assignment;
- matching or order-book mutation;
- creation of new business events for a retransmission;
- independent durable state that cannot be reconstructed from the authoritative journal.

This boundary is logically shared by all gateways and must not be implemented as unrelated
gateway-local caches. One bounded in-memory table owns reserved, sequenced, and completed command
records for the active run. Its required maximum size is bounded by `MaxRunCommands`, and recovery
reconstructs it and its exact original results from the run journal. A fingerprint may accelerate a
lookup, but exact canonical comparison determines retransmission behavior.

The retained stopped run never enters live admission. On its first replay or result-lookup request,
the recovery/result service validates its journal and builds one separate immutable read-only view
using the same replay path. That view may remain cached until retention changes, but it cannot reserve
commands or mutate matching state. Supported capacity configuration must fit the worst-case active
table and one maximum-capacity stopped-run view concurrently on the reference laptop; the
implementation may not make a promised retained-run lookup fail merely because this documented
memory was not budgeted.

If a process fails before a reservation becomes journal-durable, losing that reservation is safe
because the business action was not committed. Completed records remain available while the run is
retained. A separate disk-backed index is an optional measured optimization, not part of the initial
architecture.

### Sequencer

#### Purpose

Establish the authoritative processing position for commands that can affect the same state.

#### Owns

- sequence assignment;
- fair ordering at the adopted sequencing boundary;
- duplicate/gap handling required by the adopted rules;
- routing the sequenced command to its instrument partition;
- coordination with the journal boundary.

#### Does not own

- protocol parsing;
- order-book mutation;
- matching priority;
- client response formatting;
- market-data formatting.

The sequence scope is run-global. Partitions may process independent instruments concurrently, but
each partition observes the run-global positions assigned to its commands and cannot introduce a
different priority order.

The initial sequencing boundary is FIFO in the order it accepts newly admitted commands. It selects
the next candidate sequence for the journal, but that sequence becomes authoritative only after the
complete command record is confirmed durable. A failed or uncertain append halts admission and
requires journal recovery before another sequence is selected.

Before the journal stage exists, one bounded composition-root-owned in-process queue may model the
sequencing ingress. Gateway session threads first publish admitted normalized commands to a bounded
gateway-owned MPSC staging queue. One gateway worker drains that staging FIFO and owns at most one
pending handoff while the sequencing ingress is full; it retries the pending command before taking
later staged work. Successful sequencing-ingress enqueue order defines only the current process-local
FIFO order, and exactly one sequencer consumes that queue and owns its checked sequence counter. This
stage is not durable and does not make its sequence values authoritative across restart.
`InstrumentId` and any non-authoritative partition metadata pass through unchanged; instrument
routing belongs after the future journal boundary.

### Journal

#### Purpose

Maintain the authoritative durable record required to recover acknowledged exchange state.

#### Owns

- append ordering at the adopted durability boundary;
- record framing, checksum, version, and corruption detection;
- durable-position reporting;
- execution of controller-selected journal creation and deletion;
- input required by recovery and replay.

#### Does not own

- matching policy;
- order-book mutation;
- gateway protocol state;
- public market-data formatting.

Each retained exchange run owns one authoritative bounded append-only journal file. Its versioned
header records run identity, capacities, rules and configuration versions, and deterministic
generator inputs. Command records use the adopted explicit binary envelope and canonical encoding
rather than serialized C++ object memory.

The writer completes one `fdatasync`, `fsync`, or platform-equivalent durable synchronization per
command before that command is processed. Group commit is deferred until an explicit rule and
measurements justify it; the initial API need not speculate about its eventual shape. Replication is
not required.

Retained journals reside on local durable storage. A run enters `CapacityReached` and stops accepting
new unique commands before its configured command or byte limit would be exceeded. Retention cleanup
deletes only explicitly stopped runs selected by policy; there is no archive worker or indefinite
history tier in the initial architecture.

### Instrument router and partitions

#### Purpose

Assign each instrument to exactly one active state owner while allowing independent instruments to
process concurrently.

#### Owns

- stable instrument-to-partition assignment;
- delivery of sequenced commands to the owning partition;
- lifecycle and migration coordination;
- preventing simultaneous writers for one instrument.

#### Does not own

- client fairness before sequencing;
- matching rules;
- external symbol parsing;
- public event formatting.

A partition may own many instruments. Adding partitions is a scaling mechanism, not a change to
exchange behavior.

### Matching engine and order books

#### Purpose

Apply adopted exchange rules to one sequenced command at a time and produce deterministic state
changes and business events.

#### Owns

- active order state;
- an active-order index from `OrderId` to owner, instrument, remaining quantity, and order-book
  location;
- per-instrument bid and ask books;
- price and time priority;
- fill and trade formation;
- stateful order, cancellation, modification, and expiry decisions;
- book invariants;
- deterministic event production for each command;
- retention of a completed command result until the event-stream boundary accepts its complete
  batch.

#### Does not own

- network sessions;
- protocol parsing;
- command sequencing;
- durable acknowledgement policy;
- event delivery retries after the event-stream boundary accepts a batch;
- market-data transport.

The matching engine is a deterministic state machine. Given the same instrument configuration,
initial state, and sequenced commands, it produces the same final state and ordered business events.

Before mutation, it calculates the complete result size. The initial limit is 4,096 business events
per command. A larger result becomes one `BookCapacityExceeded` rejection without mutation, using
capacity reserved for that rejection. Replay must use the historical limit that governed the
original command.

An order enters the active-order index only when positive quantity rests in the book. Full fill and
successful cancellation remove it from both the book and index as one state transition. A Cancel is
routed by its `InstrumentId`; the owning partition looks up `TargetOrderId` in this index before
checking `ClientId`. A missing entry produces `OrderNotActive`. The retransmission index is separate:
it allows an identical repeated cancel to receive its original result even though the order has since
left the active-order index. No cross-partition search is made to diagnose a missing target. An
internal routing contradiction is an invariant failure.

### Event stream

#### Purpose

Provide ordered immutable results of command processing to durability, gateways, market data,
observability, and recovery consumers.

#### Owns

- event identity and sequence;
- ordered publication of one command's events;
- separation of private and public event classes;
- consumer gap and retry semantics;
- backpressure behavior.

#### Does not own

- matching decisions;
- client protocol formatting;
- order-book state;
- market-data aggregation policy.

An event contains enough information to understand the state transition without relying on a mutable
command object. Its stable identity is `(ExchangeRunId, CommandSequence, EventIndex)`, where
`EventIndex` starts at zero for each command. One partition publishes its commands in increasing
`CommandSequence`, and one command's events in increasing `EventIndex`. Independent partitions
publish concurrently, so live
consumers are not promised run-globally increasing `CommandSequence` order. The command journal
remains the source for reconstructing authoritative run-global command order.

The boundary accepts one command's complete event batch atomically. A full boundary applies
backpressure to the affected partition. After a command is durable, saturation cannot change its
business result or silently discard its events.

Until that atomic acceptance succeeds, the state-owning partition retains one immutable complete
batch and does not begin another command. Once accepted, the event-stream side owns the batch and the
partition clears its retry reference. Retrying a full boundary therefore cannot partially publish or
duplicate the batch.

### Private execution delivery

#### Purpose

Route acceptance, rejection, order-state, fill, and cancellation events to the correct client-facing
gateway.

#### Owns

- client/event routing;
- delivery tracking and protocol translation;
- retry or disconnect handling required by the gateway protocol.

#### Does not own

- matching state;
- event creation;
- public market data;
- sequencing policy.

Initially, identical command retransmission within a retained run is the required result-retrieval
path. It waits for or returns the original committed result with the original identifiers. Automatic
unsolicited result replay after reconnect is not required. A deleted or unknown run cannot be used
to retrieve a result or create work in the active run.

The private view of a trade contains the recipient's order identity, role or side, execution price
and quantity, and own remaining quantity. It excludes the counterparty's client identity, order
identity, and remaining quantity. Rest, cancellation, and command-rejection events are private to
the affected client. A future public trade or quote feed is a separate market-data concern.

### Market-data publisher

#### Purpose

Derive and publish the public view adopted for a particular rules version. The initial feed contents
and recovery protocol remain unresolved.

#### Owns

- public event filtering and aggregation;
- market-data sequence numbers;
- market-data snapshot generation when a recoverable feed is adopted;
- incremental-update publication;
- consumer gap-recovery protocol.

#### Does not own

- private client execution state;
- matching decisions;
- client order entry;
- authoritative order-book mutation.

### Run recovery

#### Purpose

Restore exchange state to a known sequence position and replay the authoritative record to the latest
recoverable position.

#### Owns

- run-header and journal validation;
- reconstruction from empty matching state;
- replay coordination;
- recovery verification before processing resumes.

#### Does not own

- changing historical matching results;
- inventing missing configuration;
- silently skipping corrupt records;
- protocol-specific client recovery.

The run controller owns lifecycle transitions. Recovery reports validated success or failure to it.
Gateways do not accept new business commands before `Ready`. A newly created and explicitly started
empty run may enter `Ready` after validation. A pre-existing run recovered after restart or failure
enters `Paused` on success and requires explicit resume. A startup validation failure enters
`RecoveryFailed`; an invariant or durability failure while running enters `FailStopped` and requires
recovery before admission resumes.

Recovery obtains exclusive journal ownership, validates the run header and exact record sequence,
starts from empty matching state, reconstructs admission and completed-result lookup state, and
replays all complete records using their stored rules and instrument-configuration versions. It
suppresses external FIX, multicast, and market-data output during replay, verifies invariants, and
only then reports successful recovery to the controller, which enters `Ready` for a newly started
empty run or `Paused` for a pre-existing run.

Only an incomplete final physical record may be truncated automatically. Mid-log corruption, a
complete record with a bad checksum, invalid length, unsupported version, or a sequence gap,
duplicate, or decrease enters `RecoveryFailed` without guessing or skipping data.

Full replay is the initial recovery design. Snapshots, segmented files, and derived disk indexes may
be added only if maximum-capacity measurements justify them; they remain accelerators rather than
authority. No numerical recovery objective is claimed before that benchmark exists.

### Observability

#### Purpose

Expose enough evidence to verify health, correctness, capacity, recovery progress, and performance.

#### Owns

- structured operational logs;
- command and event counts;
- rejection counts by reason;
- queue occupancy and saturation;
- processing and end-to-end latency distributions;
- recovery and invariant-failure metrics.

#### Does not own

- exchange behavior;
- business-event durability;
- matching decisions;
- hiding or changing failures.

Instrumentation must not become an undocumented source of state changes or matching nondeterminism.

## Ownership and threading

Each mutable state domain has one logical writer:

- a gateway owns its connection and protocol session state;
- the sequencer owns command ordering state;
- the journal owns its append position;
- an instrument partition owns its books and active-order index;
- an event publisher owns delivery state for its stream.

One process or thread may host several logical owners. The important property is that ownership is
explicit and commands cannot concurrently mutate the same order book.

Cross-component messages use value semantics or explicitly owned immutable data. Raw lifetime
dependencies across asynchronous boundaries should be avoided.

## Sequencing and instrument partitioning

Instrument partitioning enables concurrency without making one order book concurrently writable.
Commands for independent instruments may run in parallel after run-global sequencing and durable
journal append. Each partition processes its assigned commands in increasing run-global sequence
order and finishes one command's state transitions and event generation before starting its next
command.

Partition selection must be stable for replay and must use a persisted instrument identity rather
than an implementation-defined hash. Rebalancing or migrating an instrument requires a handoff at a
known sequence position so that exactly one partition remains authoritative.

The exchange rules define fairness and whether cross-instrument order is meaningful. Architecture
must not create an additional scheduler-dependent priority rule.

## Commands and events

Normalized commands and business events form stable boundaries between components. They should use
explicit fixed-width or otherwise well-defined representations for identifiers, integer price ticks,
integer quantity units, sequence positions, and configuration versions.

In C++, `ExchangeRunId`, `CommandSequence`, `OrderId`, `ClientId`, and `InstrumentId` are distinct
strong types backed by `uint64_t`. `EventIndex` is backed by `uint32_t`, and `Price` and `Quantity`
are distinct unsigned strong types backed by `uint64_t`. Sharing an underlying representation must
not make semantic types implicitly interchangeable.
`ClientCommandId` is a bounded-string strong type and must preserve the validated client value
without reducing it to a hash. It stores 1 to 64 ASCII bytes and uses exact, case-sensitive byte
comparison without normalization.

Commands represent requests. Events represent results. A command may produce no state change but must
still produce the adopted business result. A command that produces several trades emits those events
in the deterministic order defined by the exchange rules. Commands are immutable after journaling;
matching produces separate immutable events rather than mutating the command.

## Exchange-run journaling and replay

The bounded normalized-command journal for one retained exchange run is the local authoritative
source from which its committed state, admission identity, and results are reconstructed. Databases,
result stores, and caches are optional derived lookup or materialization layers, not alternative
commitment sources.

The initial processing path is normalization, authoritative command admission, candidate run-global
sequence selection, immutable command append, durable synchronization and authoritative sequence
assignment, matching, event publication, and business-result acknowledgement. If the process fails
after durable synchronization but before acknowledgement, recovery reprocesses the committed command
and regenerates its deterministic events.
Recovery also reconstructs the mapping from
`(ExchangeRunId, ClientId, ClientCommandId)` to the exact normalized command, journal position, and
original result so retransmission cannot execute the business action twice. The mapping exists for
the retained lifetime of the run and may reset only by changing `ExchangeRunId`.

Recovery begins from empty matching state and replays the entire bounded journal through the same
matching state machine. It regenerates events, reconstructs admission and result state, verifies
invariants, and only then allows new commands for that run. Snapshots or a derived disk index require
measured justification at maximum supported run capacity.

The active and most recently retained stopped-run journals use local durable storage. When retention
cleanup deletes a stopped run, the system deletes its replay and result-retrieval guarantee as one
explicit lifecycle operation; it does not silently evict identities from an active run.

## Backpressure and failure boundaries

Every asynchronous boundary defines:

- capacity;
- producer and consumer ownership;
- behavior when full;
- whether a command has already been acknowledged;
- shutdown and drain behavior;
- metrics and client-visible failure behavior.

The admission table, gateway staging queue, sequencing ingress, result handoff, and run journal have
explicit independent bounds. Temporary ingress saturation returns `GatewayBusy` before sequencing.
Reaching `MaxRunCommands` or `MaxRunJournalBytes` returns `RunCapacityReached` and closes that run to
new unique commands. When durable storage is unavailable or uncertain, admission stops for recovery;
committed data is never overwritten to make room. Identical retransmission lookup may continue while
the run and its required data remain available.

Before matching-state mutation, a command whose planned result exceeds the adopted per-command bound
receives the defined capacity rejection. Once a command is durable, event-handoff pressure is not a
business rejection: the affected partition waits or enters fail-stop and recovery. Committed events
may not be silently dropped. Retry preserves identity and ordering.

An invariant failure isolates or stops the affected state owner and reports the failure. Continuing
with silently corrupted order-book state is not an acceptable degraded mode.

Every worker thread has a top-level exception boundary. Expected parsing and admission errors use
their normal pre-sequence response paths. An unexpected exception is allowed to release a reservation
and continue only if the component proves that no authoritative sequence, durable uncertainty, or
matching mutation exists and all owned invariants still hold. Otherwise the worker retains any owned
pending command or result needed for diagnosis, records the failure, transitions the service to
`FailStopped`, and prevents further admission. An exception must not escape a worker and terminate
the process without establishing the failure state.

## Laptop-scale deployment and growth

The first complete architecture may run in one process with direct calls or bounded in-process
queues. This is not throwaway work when the normalized commands, sequencing boundary, matching state
machine, events, and recovery contracts remain the same.

Growth should proceed through measured changes:

1. complete deterministic behavior and state-machine tests;
2. add bounded per-run identity, lifecycle, durable local journaling, and full replay;
3. measure one-process throughput and latency;
4. partition independent instruments across a small number of workers;
5. separate processes only when ownership, isolation, or measured capacity justifies it;
6. add snapshots, indexes, segmentation, or batching only when profiling at supported run capacity
   justifies them;
7. optimize data layout, allocation, copying, and serialization after profiling.

No architectural stage depends on specialized exchange infrastructure.
