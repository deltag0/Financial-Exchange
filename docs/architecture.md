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

- multiple instruments;
- deterministic and fair command processing;
- explicit sequencing;
- reliable journaling, snapshots, recovery, and replay;
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
- The sequencer assigns commands one authoritative global command sequence.
- Each state-owning partition processes its commands in increasing global sequence order and
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
Global sequence assignment
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

## Components

### Gateway and session adapters

#### Purpose

Translate an external protocol into normalized exchange commands and translate private exchange
events into protocol-specific responses.

#### Owns

- network connection and session lifecycle;
- protocol framing and parsing;
- authentication or session-to-client association;
- association of each transport session with a stable `ClientId` that survives reconnects;
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

### Command admission and retransmission

#### Purpose

Enforce logical command uniqueness across every gateway before a new global sequence is assigned.

#### Owns

- the authoritative `(ClientId, ClientCommandId)` admission index;
- atomic first-submission reservation across concurrent gateways;
- comparison of retransmissions with the original normalized command;
- coalescing or waiting while an identical original command is still in flight;
- returning the original result for an identical completed command;
- rejecting conflicting reuse before sequencing;
- reconstruction of the admission index from the command journal during recovery.

#### Does not own

- protocol parsing or session state;
- global command sequence assignment;
- matching or order-book mutation;
- creation of new business events for a retransmission;
- independent durable state that cannot be reconstructed from the authoritative journal.

This boundary may initially be an in-process object adjacent to the sequencer. It is logically shared
by all gateways and must not be implemented as unrelated gateway-local caches. If a process fails
before an admitted command becomes journal-durable, losing that in-flight reservation is safe because
the business action was not committed.

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

The sequence scope is global. Partitions may process independent instruments concurrently, but each
partition observes the global positions assigned to its commands and cannot introduce a different
priority order.

### Journal

#### Purpose

Maintain the authoritative durable record required to recover acknowledged exchange state.

#### Owns

- append ordering at the adopted durability boundary;
- record framing, checksum, version, and corruption detection;
- durable-position reporting;
- log rotation or retention policy;
- input required by recovery and replay.

#### Does not own

- matching policy;
- order-book mutation;
- gateway protocol state;
- public market-data formatting.

The normalized, sequenced command journal is authoritative. Initially the journal completes one
`fdatasync`, `fsync`, or platform-equivalent durable synchronization per command before that command
is processed. Replication is not required. A later group-commit implementation may batch durable
synchronization without acknowledging any command before its batch is durable.

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
- deterministic event production for each command.

#### Does not own

- network sessions;
- protocol parsing;
- command sequencing;
- durable acknowledgement policy;
- event delivery retries;
- market-data transport.

The matching engine is a deterministic state machine. Given the same instrument configuration,
initial state, and sequenced commands, it produces the same final state and ordered business events.

An order enters the active-order index only when positive quantity rests in the book. Full fill and
successful cancellation remove it from both the book and index as one state transition. A Cancel is
routed by its `InstrumentId`; the owning partition looks up `TargetOrderId` in this index before
checking `ClientId`. A missing entry produces `OrderNotActive`. The retransmission index is separate:
it allows an identical repeated cancel to receive its original result even though the order has since
left the active-order index.

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
command object. Its stable identity is `(commandSequence, eventIndex)`, where `eventIndex` starts at
zero for each command. A global publication merge across independent partitions remains an
exchange-rule decision.

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

### Market-data publisher

#### Purpose

Derive and publish the adopted public view of trades and order-book changes.

#### Owns

- public event filtering and aggregation;
- market-data sequence numbers;
- snapshot generation;
- incremental-update publication;
- consumer gap-recovery protocol.

#### Does not own

- private client execution state;
- matching decisions;
- client order entry;
- authoritative order-book mutation.

### Snapshot and recovery

#### Purpose

Restore exchange state to a known sequence position and replay the authoritative record to the latest
recoverable position.

#### Owns

- snapshot creation and validation;
- association between a snapshot and journal position;
- deterministic state restoration;
- replay coordination;
- recovery verification before processing resumes.

#### Does not own

- changing historical matching results;
- inventing missing configuration;
- silently skipping corrupt records;
- protocol-specific client recovery.

Recovery must use versioned instrument and behavioral configuration. A mismatch or corrupt record is
reported explicitly rather than guessed.

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
Commands for independent instruments may run in parallel after global sequencing and durable journal
append. Each partition processes its assigned commands in increasing global sequence order and
finishes one command's state transitions and event generation before starting its next command.

Partition selection must be stable for replay and must use a persisted instrument identity rather
than an implementation-defined hash. Rebalancing or migrating an instrument requires a handoff at a
known sequence position so that exactly one partition remains authoritative.

The exchange rules define fairness and whether cross-instrument order is meaningful. Architecture
must not create an additional scheduler-dependent priority rule.

## Commands and events

Normalized commands and business events form stable boundaries between components. They should use
explicit fixed-width or otherwise well-defined representations for identifiers, integer price ticks,
integer quantity units, sequence positions, and configuration versions.

In C++, `CommandSequence`, `OrderId`, `ClientId`, and `InstrumentId` are distinct strong types backed
by `uint64_t`; `EventIndex` is backed by `uint32_t`; and `Price` and `Quantity` are distinct unsigned
strong types backed by `uint64_t`. Sharing an underlying representation must not make these semantic
types implicitly interchangeable. `ClientCommandId` is instead a bounded-string strong type and must
preserve the validated client value without reducing it to a hash. It stores 1 to 64 ASCII bytes and
uses exact, case-sensitive byte comparison without normalization.

Commands represent requests. Events represent results. A command may produce no state change but must
still produce the adopted business result. A command that produces several trades emits those events
in the deterministic order defined by the exchange rules. Commands are immutable after journaling;
matching produces separate immutable events rather than mutating the command.

## Journaling, snapshots, and replay

The normalized, sequenced command journal is the local authoritative source from which committed
state is reconstructed. An external database, replica, or cache is not required for the initial
recovery model.

The initial processing path is normalization, authoritative command admission, global sequence
assignment, immutable command append, durable synchronization, matching, event publication, and
business-result acknowledgement. If the process fails after durable synchronization but before
acknowledgement, recovery reprocesses the committed command and regenerates its deterministic events.
Recovery also reconstructs the mapping from
`(ClientId, ClientCommandId)` to the normalized command and its original result so retransmission
cannot execute the business action twice. The command-admission boundary enforces this mapping before
assigning a new sequence; the exact gateway redelivery mechanism remains an architectural decision.

Snapshots reduce replay time but do not replace the authoritative journal. Each snapshot identifies:

- format and configuration version;
- last included command or event sequence;
- instrument-partition ownership;
- complete state required to resume deterministic replay.

Recovery loads a valid snapshot, replays later normalized commands through the same matching state
machine, regenerates their events, verifies invariants, and only then allows new commands for the
recovered state.

## Backpressure and failure boundaries

Every asynchronous boundary defines:

- capacity;
- producer and consumer ownership;
- behavior when full;
- whether a command has already been acknowledged;
- shutdown and drain behavior;
- metrics and client-visible failure behavior.

Before acknowledgement, capacity pressure may result in a defined rejection. After acknowledgement,
the command must remain recoverable and may not be silently dropped. Bounded retry must preserve
identity and ordering.

An invariant failure isolates or stops the affected state owner and reports the failure. Continuing
with silently corrupted order-book state is not an acceptable degraded mode.

## Laptop-scale deployment and growth

The first complete architecture may run in one process with direct calls or bounded in-process
queues. This is not throwaway work when the normalized commands, sequencing boundary, matching state
machine, events, and recovery contracts remain the same.

Growth should proceed through measured changes:

1. complete deterministic behavior and state-machine tests;
2. add durable local journaling and replay;
3. measure one-process throughput and latency;
4. partition independent instruments across a small number of workers;
5. separate processes only when ownership, isolation, or measured capacity justifies it;
6. optimize data layout, allocation, copying, and serialization after profiling.

No architectural stage depends on specialized exchange infrastructure.
