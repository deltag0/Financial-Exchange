# Exchange Rules

## Purpose

This document defines the externally observable behavior of the exchange. It should be precise enough
that two independent implementations following the adopted rules produce the same order states,
trades, rejections, and events from the same command sequence.

This document does not describe source code, threads, queues, libraries, or implementation progress.
See [Implementation Status](implementation-status.md) for current capabilities and
[Architecture](architecture.md) for the intended system design.

## Rule status

Rules have one of two states:

- **Adopted**: approved exchange behavior. Implementations and tests must follow it.
- **Unresolved**: behavior that has not been approved. An unresolved entry may record options and a
  current recommendation, but the recommendation is not an exchange rule.

When a decision is approved, move its result into **Adopted rules** and remove the resolved question
from **Unresolved decisions**. Code must not silently decide unresolved behavior.

## Adopted rules

### Product scope and exchange runs

- The exchange is a bounded, deterministic showcase and learning environment. It does not promise
  permanent operation, regulatory retention, or recovery of deleted history.
- Authoritative activity belongs to exactly one `ExchangeRun`. `ExchangeRunId` is a distinct strong
  type backed by a nonzero `uint64_t`. It monotonically increases across runs created by one exchange
  installation and is never reused, even after a run is deleted. Gaps are permitted.
- Before creating a run journal, the exchange durably reserves its next `ExchangeRunId` in a small
  installation-level run catalog. An uncertain catalog update prevents run creation until the last
  reserved value is recovered; exhaustion prevents creation of another run.
- The canonical external text for `ExchangeRunId` is unsigned decimal ASCII without leading zeroes.
  This representation identifies run context but does not affect matching priority.
- A run fixes its behavioral-rules version, instrument-configuration versions, configured command
  and byte capacities, and any deterministic generated-order scenario before it enters `Ready`.
- A generated-order scenario records its generator version, seed, and parameters. Those values
  reproduce generated input. Exact replay after interactive user input uses the authoritative
  command journal, not the seed alone.
- The run lifecycle states are `Starting`, `Recovering`, `Ready`, `Paused`, `CapacityReached`,
  `Stopped`, `RecoveryFailed`, and `FailStopped`. The exchange-run controller is the sole owner of
  these transitions; other components report outcomes to it rather than changing lifecycle state.
- New unique business commands are accepted only in `Ready`. Entering `Paused` first closes admission
  to new user and generated commands, then drains already committed work. Identical retransmission
  and read-only lookup remain available while the run is retained. Resuming preserves the same run,
  books, sequence domain, and identifiers.
- Closing admission for pause, stop, or shutdown is an ordered barrier at the authoritative admission
  boundary. No first-submission reservation is created after the barrier. Every reservation created
  before it drains through sequencing, durability, matching, and result completion in its established
  FIFO order; if durability or mutation becomes uncertain, the run fail-stops instead of declaring
  the drain complete. The requested lifecycle transition is not complete while a reserved,
  sequenced, committed, or pending-result command remains owned by the pipeline.
- A clean process shutdown pauses and drains the active run; it does not implicitly stop or delete
  the run. Restart validates and recovers that run into `Paused`, and an explicit resume is required
  before new business commands are accepted. A crash also recovers to `Paused` rather than
  automatically reopening admission.
- Reaching either configured run capacity enters `CapacityReached` before another unique command is
  sequenced. Accepted and committed work is never overwritten to extend a run. Identical
  retransmissions remain eligible for lookup while the run is retained. `CapacityReached` cannot
  return to `Ready`; the run must be stopped before another run is created.
- Starting a new run is an explicit boundary. The current run first stops accepting new work and
  drains or fail-stops already committed work. The new run receives a different `ExchangeRunId`,
  empty matching state, and new run-local identifier domains.
- `Stopped` is terminal and read-only. A stopped run cannot be resumed or accept new business work.
- The initial retention policy keeps the active run, including when paused or capacity-reached, and
  at most one most recently stopped run. Retaining more stopped runs requires an explicit rule
  change.
- If stopping the active run would replace an already retained stopped run, the operator or UI must
  identify the older `ExchangeRunId` and obtain explicit confirmation before changing the retained
  selection. The catalog change becomes durable before the older journal is deleted.
- Deleting a stopped run, or explicitly abandoning a `RecoveryFailed` run, is irreversible exchange
  behavior and requires confirmation naming its `ExchangeRunId`. After deletion, the exchange makes
  no deduplication, result retrieval, replay, or recovery promise for that run.
- A run in `FailStopped` can enter `Recovering` only through an explicit recovery attempt. Successful
  recovery enters `Paused`; failure enters `RecoveryFailed`. A `RecoveryFailed` run cannot enter
  `Ready` without a later successful recovery.
- The durable installation catalog records the last reserved `ExchangeRunId`, the active run and its
  persisted disposition (`Open`, `Paused`, `CapacityReached`, `FailStopped`, or `RecoveryFailed`),
  and the retained stopped run. A catalog update is made crash-safe through the adopted checksummed
  generation and atomic-replacement scheme. A lifecycle operation is not reported successful before
  its catalog update and containing directory are durably synchronized. `Open` is a persisted
  disposition meaning the run was in `Ready`; it is not an additional runtime state, and recovery of
  an open catalog entry still enters `Paused`. If no valid latest catalog generation can be
  established, the exchange does not infer a reusable ID or active run from filenames; run creation
  and lifecycle mutation remain unavailable pending explicit catalog recovery.
- “Exchange run,” FIX transport session, and any future trading session are distinct concepts. A
  transport session must be bound to one active `ExchangeRunId`; a new run invalidates prior
  bindings so stale identifiers cannot address the new run.

### Initial command and TimeInForce scope

- The initial order-entry scope is limit BUY and limit SELL orders.
- GTC and IOC are supported TimeInForce values.
- Cancellation is a supported command.
- Other order types, TimeInForce values, modification, expiry, and auction behavior are deferred and
  are not part of the adopted initial behavior.
- A GTC order may rest any quantity remaining after matching.
- An IOC order never rests. After all possible executions, any remaining quantity is cancelled.

### Price and quantity representation

- `Price` is a strong unsigned type backed by `uint64_t`, representing a number of ticks.
- The meaning of one tick is defined by versioned instrument configuration. A journaled command must
  be associated with the configuration version needed to interpret its price during replay.
- `Quantity` is a strong unsigned type backed by `uint64_t`, representing a number of quantity
  units.
- Price and quantity are distinct semantic domains and must not be treated as interchangeable values.
- Valid order prices and quantities are greater than zero.
- Every arithmetic operation that could overflow or underflow is checked. Checks occur before the
  affected matching-state mutation becomes externally visible.
- For every execution, `ExecutionQuantity` is no greater than either order's quantity remaining
  immediately before that execution.
- Subtracting an execution or cancellation quantity cannot underflow, and adding an order to a
  price-level aggregate cannot overflow.
- A price-level aggregate is the sum of remaining quantity at one
  `(InstrumentId, Side, Price)` and therefore treats bid and ask levels separately.
- The initial configured test instrument has external symbol `SPY`, `InstrumentId` 1, and
  `ConfigurationVersion` 1. This configuration does not claim connectivity to or trading in the
  real-world SPY instrument.
- For `SPY` configuration version 1:
  - one tick is exactly 0.0001 quote units;
  - the minimum price is 1 tick, or 0.0001 quote units;
  - the maximum price is 10,000,000,000 ticks, or 1,000,000.0000 quote units;
  - one quantity unit and the lot size are both 1;
  - the maximum quantity of one order is 100,000,000 quantity units;
  - the maximum aggregate at one price level is 1,000,000,000 quantity units.
- External price and quantity fields are parsed as decimal text without conversion through binary
  floating point. Equivalent price spellings such as `12.34` and `12.3400` normalize to 123,400
  ticks. Non-zero precision beyond four decimal places is a tick-size violation. Quantity must be
  mathematically integral and a multiple of the configured lot size.
- Price and order-quantity limit violations are rejected before sequencing. A price-level aggregate
  violation is state-dependent and produces `BookCapacityExceeded` after sequencing without trades
  or matching-state mutation.

### Identifiers and retransmission

- `CommandSequence` is the monotonically increasing authoritative command position within one
  `ExchangeRun`. Its underlying representation is `uint64_t`, starts at 1, and is never reused
  within that run. Monotonicity does not by itself prohibit gaps.
- Every normalized NewOrder has an `OrderId` whose numeric value equals its `CommandSequence`.
  `OrderId` is a distinct strong type backed by `uint64_t` and is unique within that run.
- A fully qualified command or order identity includes its `ExchangeRunId`. Numeric
  `CommandSequence` and `OrderId` values may recur in a different run without identifying the same
  business object.
- `ClientId` is a stable exchange-assigned client identity. It remains the same across transport
  sessions and reconnects. It is a distinct strong type backed by `uint64_t`.
- Initially, persisted exchange configuration maps each authorized FIX identity to its numeric
  `ClientId`. Reconnecting, or using another permitted session for the same client, preserves that
  `ClientId`. A transport-session hash is not a client identity.
- `ClientCommandId` is selected by the client and is unique within that `ClientId` and exchange run.
  The logical command identity is `(ExchangeRunId, ClientId, ClientCommandId)`. `ClientCommandId` is
  a bounded-string strong type containing between 1 and 64 ASCII bytes. Comparison is
  case-sensitive and exact byte for byte; no trimming, case folding, or text normalization is
  applied. It must not be represented solely by a hash.
- Repeating the same logical command with identical normalized contents returns its previously
  determined result and does not execute the business action again or emit new business events.
- Reusing the same logical command identity with different normalized contents receives the
  admission rejection reason `DuplicateCommandConflict`, receives no `CommandSequence`, emits no
  business event, and does not mutate matching state.
- The exchange must retain or reconstruct enough deduplication state to preserve these rules after
  reconnect and recovery for every retained run. Deleting a run ends that guarantee.
- `InstrumentId` is a stable exchange-defined numeric identifier. Matching rules use it rather than
  an external symbol string. It is a distinct strong type backed by `uint64_t`.
- `EventId` is `(ExchangeRunId, CommandSequence, EventIndex)`. It is unique in the deterministic
  business event history of all retained runs. `EventIndex` is a distinct strong type backed by
  `uint32_t`.
- `CommandSequence` and `EventIndex` never wrap. The exchange stops admitting affected work before
  identifier exhaustion rather than reusing an identifier; configured capacity must prevent one
  command from requiring more event positions than `EventIndex` can represent.
- No independent `TradeId` is required initially; the `EventId` of a `Trade` identifies that trade.
- `ExchangeRunId` never wraps. The exchange stops creating runs before exhaustion rather than
  recycling an earlier identity.
- Authoritative exchange-assigned numeric identifiers are never reused within their exchange run.
  The `ExchangeRunId` prevents reuse from creating the same fully qualified identity.

### Normalized command schemas

The normalized, sequenced NewOrder command contains:

- `ExchangeRunId`;
- `CommandSequence`;
- `ClientId` and `ClientCommandId`;
- `InstrumentId` and `ConfigurationVersion`;
- `Side`;
- `Price` and `Quantity`;
- `TimeInForce`.

Its `OrderId` is derived by wrapping the same underlying numeric value as `CommandSequence`; it does
not require a second independently generated identity.

The normalized, sequenced Cancel command contains:

- `ExchangeRunId`;
- `CommandSequence`;
- `ClientId` and `ClientCommandId`;
- `InstrumentId` and `ConfigurationVersion` for routing, validation, and replay;
- `TargetOrderId`.

The cancellation target remains the active run's `TargetOrderId`. `InstrumentId` does not form part
of the order's identity. A command from a transport or API context not bound to the active run is
rejected before sequencing and cannot address a numerically equal order in another run.

### Matching priority and trade formation

- Continuous trading uses price-time priority.
- BUY orders receive best-price priority at the highest eligible bid price. SELL orders receive
  best-price priority at the lowest eligible ask price.
- Within one price level, the lower authoritative command sequence has priority.
- Wall-clock timestamps and thread scheduling do not establish matching priority.
- A crossing incoming order executes at the resting order's price.
- Partial fills and multiple fills are supported.
- One `Trade` event represents exactly one match between an incoming order and one resting order.
- Trades created by one incoming command are emitted in matching order: best price first, then time
  priority within each price level.
- Filled quantity cannot exceed original quantity, remaining quantity cannot be negative, and a
  fully filled order is no longer active.
- The aggregate quantity at a price level equals the sum of the remaining quantities of its active
  orders.

### Command sequencing and processing

- The sequencer assigns every sequenced command one authoritative run-global command sequence.
- Newly admitted commands enter one authoritative FIFO sequencing boundary. Their order is the order
  in which that boundary accepts them. The initial rules make no stronger cross-client or
  cross-session fairness guarantee.
- Every command that passes pre-sequencing admission receives a sequence, including a command that
  later produces a state-dependent `CommandRejected` event.
- A candidate sequence becomes assigned, consumed, and authoritative only when its complete command
  record is confirmed durably committed. No later sequence may be committed first. If append or
  durable synchronization fails or has an uncertain outcome, admission halts and recovery inspects
  the journal before selecting the next sequence.
- Commands assigned to a state-owning partition are processed in increasing authoritative command
  sequence order.
- A state-owning partition completes all matching-state transitions and event generation for one
  command before it begins processing its next command.
- Independent partitions may operate concurrently. Each partition makes a completed command's event
  batch eligible for publication as soon as it can hand off the complete batch. Within one
  partition, commands are published in increasing `CommandSequence`; events from one command are
  published in increasing `EventIndex`.
- Consumers are not guaranteed to receive events in run-globally increasing `CommandSequence` order
  across independent partitions. The authoritative run-global command order remains reconstructible
  from the command journal and event identifiers.

### Business events

- Events caused by one command are emitted in the same order as their corresponding state
  transitions.
- Every event is identified by `(ExchangeRunId, CommandSequence, EventIndex)`.
- `eventIndex` starts at zero for each command and increases by one for every event produced by that
  command.
- The initial business-event types are:
  - `CommandRejected`;
  - `Trade`;
  - `OrderRested`;
  - `OrderCancelled`.
- A `Trade` contains:
  - `EventId`;
  - `InstrumentId`;
  - `MakerOrderId` and `MakerClientId`;
  - `TakerOrderId` and `TakerClientId`;
  - `TakerSide`;
  - `ExecutionPrice` and `ExecutionQuantity`;
  - `MakerRemainingQuantity` and `TakerRemainingQuantity`.
- An `OrderRested` contains:
  - `EventId`;
  - `OrderId` and `ClientId`;
  - `InstrumentId`;
  - `Side` and `Price`;
  - `RemainingQuantity`.
- An `OrderCancelled` contains:
  - `EventId`;
  - `OrderId` and `ClientId`;
  - `InstrumentId`;
  - `CancelledQuantity`;
  - `CancelReason`.
- A `CommandRejected` contains:
  - `EventId`;
  - `CommandType`;
  - `ClientId` and `ClientCommandId`;
  - an optional `RelevantOrderId`;
  - a stable machine-readable `CommandRejectionReason`.
- There are no separate `PartialFill`, `FullFill`, or `OrderFilled` events.
- A zero remaining quantity in a `Trade` records that the corresponding order became fully filled.
- A fully filled incoming order produces its `Trade` events and no additional terminal event.
- `OrderRested` is emitted only when positive remaining quantity actually enters the order book.
- `OrderCancelled` reports exactly the quantity removed and distinguishes at least
  `ClientRequested` from `IocRemainder` through `CancelReason`.
- After all `Trade` events from an incoming order:
  - a GTC remainder produces `OrderRested`;
  - an IOC remainder produces `OrderCancelled`.
- A rejected command changes no matching-engine state and produces exactly one `CommandRejected`
  event.

### Event visibility and result redelivery

- The full authoritative `Trade` is available only to trusted internal processing and an explicitly
  privileged educational view. The privileged view is not a public market-data or participant
  interface.
- Each participant receives a private execution view containing the event identity, instrument, its
  own order identity, role or side, execution price and quantity, and its own remaining quantity.
  It does not disclose the counterparty's `ClientId`, `OrderId`, or remaining quantity.
- Sanitized trade information may later be public. The initial rules do not yet adopt a public trade
  feed or its format.
- `OrderRested`, `OrderCancelled`, and `CommandRejected` are private to the affected client. Their
  resulting book or quote changes may later produce separate public market-data events.
- Committed private results survive client disconnection while their exchange run is retained.
  While a run still accepts retransmissions, retransmitting the identical
  `(ExchangeRunId, ClientId, ClientCommandId)` is the required way to wait for or retrieve the
  original result. A retained stopped run remains available for read-only result lookup and replay;
  the external result-history protocol remains unresolved.
- Retrieval or redelivery preserves the original `CommandSequence` and `EventId` values. It creates
  no new command, sequence, event, trade, or matching-state mutation.
- Automatic unsolicited replay to a reconnected session is not required initially.

### Result capacity and event handoff

- The initial `MaxEventsPerCommand` is 4,096 business events, including any terminal event. This
  limit applies to every command. A future change to the limit must preserve the historical value
  used to replay earlier commands.
- Before any matching-state mutation, the matching engine calculates the exact number of events the
  command would produce using checked arithmetic.
- If that count exceeds `MaxEventsPerCommand`, the command produces exactly one
  `CommandRejected(BookCapacityExceeded)` and makes no matching-state mutation.
- Capacity for that single rejection is reserved independently of the normal result batch. A failure
  outside the configured operating bounds causes fail-stop and recovery rather than an invented
  business rejection.
- Handoff accepts the complete event batch for one command atomically. If the downstream boundary is
  full, the affected partition applies backpressure until the batch is accepted or the partition
  enters fail-stop and recovery.
- Once a command is durable, downstream saturation cannot change its business result. Committed
  events are never silently dropped.

### Cancellation

- A cancellation targets an exchange `OrderId`.
- An initial FIX Cancel Request supplies that target as the exact decimal exchange `OrderId` in
  `OrderID(37)`. `ClOrdID(11)` identifies the new Cancel command. `OrigClOrdID(41)` may be retained
  for protocol correlation but is not authoritative and is never hashed or reinterpreted as an
  `OrderId`.
- A FIX session is bound to the active `ExchangeRunId`. Stopping or replacing that run invalidates
  the binding and requires a new binding before business commands are accepted.
- An order is active exactly when it is resting in its instrument's order book with positive
  remaining quantity.
- Fully filled, previously cancelled, IOC-terminal, rejected, and never-accepted orders are not
  active.
- Only the owning `ClientId` may cancel an active order.
- A successful cancellation removes exactly the order's current remaining quantity.
- A partially filled order may be cancelled for its remaining quantity; earlier fills remain valid.
- A successful cancellation emits one `OrderCancelled` containing the quantity actually removed.
- A target that is not active produces exactly one `CommandRejected` with reason `OrderNotActive`.
- An active target owned by another client produces exactly one `CommandRejected` with reason
  `NotOwner`.
- Retransmitting the same `(ExchangeRunId, ClientId, ClientCommandId)` returns the previously
  determined result and does not perform cancellation again.
- A new cancel command with a new `ClientCommandId` against an already-terminal order is rejected as
  `OrderNotActive`.
- Fill-versus-cancel outcomes are determined only by authoritative command sequence order on the
  target order's state-owning partition.
- When processing a Cancel, the owning partition first looks up `TargetOrderId` in its active-order
  state. Absence produces `OrderNotActive`; presence followed by an owner mismatch produces
  `NotOwner`; otherwise the remaining quantity is removed atomically from both the order book and
  active-order state.
- A Cancel is routed using its normalized `InstrumentId`, and `TargetOrderId` is looked up only in
  that instrument's owning partition. If it is not active there, the result is `OrderNotActive`; no
  cross-partition search is performed for diagnosis. A mismatch caused by an internal routing error
  is an invariant failure rather than a business rejection.

### Self-trading

- Self-trading is allowed in the initial rule set.
- No self-trade prevention behavior is claimed until a different rule is explicitly adopted.

### Validation boundary

- Before sequencing, the gateway and normalization boundary rejects:
  - malformed protocol messages;
  - missing required fields;
  - numeric parsing and overflow failures;
  - unknown or invalid enum values;
  - unknown or inactive instruments;
  - tick-size and lot-size violations under the authoritative versioned instrument configuration.
- Instrument resolution and the applicable configuration version are therefore established before
  sequencing and recorded in the normalized command.
- After normalization and before run-global sequence assignment, one authoritative command-admission
  boundary enforces `(ExchangeRunId, ClientId, ClientCommandId)` uniqueness for all gateways. This
  is not an independent cache owned by each gateway.
- An identical retransmission returns or waits for the original result and receives no new
  `CommandSequence`. Conflicting reuse receives `DuplicateCommandConflict` as an admission rejection
  and also receives no new `CommandSequence`.
- An identical retransmission received while the original command is still in flight attaches to
  that operation and waits for the same result rather than creating parallel business work.
- The admission index is reconstructed from the authoritative command journal during recovery. An
  in-flight reservation lost before journal durability may be admitted again after restart because
  no business action was durably committed.
- A failure before sequencing receives no `CommandSequence`, is not written to the authoritative
  command journal, and cannot emit an EventId-bearing business event. It receives the appropriate
  gateway admission or protocol rejection response instead.
- After sequencing and durable journal append, state-dependent validation includes:
  - order ownership;
  - whether a cancellation target exists and is active;
  - state-dependent order conflicts;
  - any adopted book- or session-dependent rules.
- A state-dependent failure changes no matching state and emits exactly one `CommandRejected`.
- Initial stable `AdmissionRejectionReason` values are:
  - `DuplicateCommandConflict` for conflicting logical command reuse;
  - `ExchangeRunUnavailable` when a new business submission targets an unknown, deleted, paused,
    stopped, or incorrectly bound run, or when any lookup targets an unknown or deleted run;
  - `RunCapacityReached` when the active run cannot accept another unique command within its fixed
    command or journal-byte capacity;
  - `GatewayBusy` when a temporary bounded pre-sequence ingress or admission resource is saturated.
- Admission applies these outcomes in this order: reject an unknown, deleted, or incorrectly bound
  run; look up an existing logical command key and return its identical result or
  `DuplicateCommandConflict`; then evaluate whether the run is `Ready`, capacity remains, and
  temporary ingress is available for a previously unseen key. Thus retained history remains
  authoritative in `Paused`, `CapacityReached`, and `Stopped`, while only a new key can receive
  `ExchangeRunUnavailable`, `RunCapacityReached`, or `GatewayBusy` from run state or capacity.
- An admission rejection is a gateway response, not a `CommandRejected` business event, because it
  has no `CommandSequence` or `EventId`.
- Initial stable `CommandRejectionReason` values for sequenced commands are:
  - `OrderNotActive` for a new cancellation targeting an order that is not active;
  - `NotOwner` for a cancellation targeting another client's active order;
  - `BookCapacityExceeded` when a sequenced command cannot be applied within configured matching
    capacity without violating a numeric or resource bound.

### Authoritative journal, durability, and replay

- Each retained exchange run owns one bounded normalized-command journal. It is the authoritative
  recovery record for that run.
- Commands recorded in the journal are immutable.
- Journal capacity is fixed in the run header. Committed records are never overwritten to admit new
  work.
- Replay processes the journal through the same deterministic matching behavior and regenerates
  business events with the same fully qualified identifiers, order, and content.
- Replication, permanent archive, and cross-machine disaster recovery are not required.
- A command is externally committed only after its normalized, sequenced journal record completes
  the configured durable-write operation. The initial operation is `fdatasync`, `fsync`, or the
  platform-equivalent durable synchronization selected by the implementation.
- The adopted durability policy synchronizes one command at a time. A group-commit API is not
  required in advance. Adopting group commit later requires an explicit durability rule and
  reproducible latency and throughput evidence.
- Performance results must state the durability policy. A benchmark must not claim improved durable
  latency by acknowledging operating-system page-cache acceptance as durability.
- The initial commit path is:
  1. select the next candidate run-global command sequence;
  2. append the immutable normalized command containing that candidate;
  3. complete the configured durable synchronization, at which point the sequence becomes
     authoritative;
  4. process the command;
  5. publish the resulting events;
  6. acknowledge the business result.

#### Journal record and file rules

- The journal begins with the exact versioned `RunHeader` frame defined below. It contains
  `ExchangeRunId`, behavioral-rules version, instrument-configuration versions, configured command
  and byte capacities, and any generator version, seed, and parameters required to reproduce
  generated inputs.
- Journal records use an explicitly encoded, versioned binary format, not a dump of an in-memory
  C++ structure. Journal format version 1 uses little-endian unsigned integers, has no implicit
  padding, and contains exactly one `RunHeader` frame first, followed by zero or more command frames.
- Every version 1 frame starts with this 36-byte envelope:

  | Offset | Size | Field | Version 1 value or meaning |
  |---:|---:|---|---|
  | 0 | 4 | Magic | ASCII bytes `FXJR` |
  | 4 | 2 | FormatVersion | `1` |
  | 6 | 2 | RecordType | `1` RunHeader, `2` NewOrder, `3` Cancel |
  | 8 | 4 | TotalLength | Envelope plus payload; must equal `36 + PayloadLength` |
  | 12 | 8 | ExchangeRunId | Nonzero and equal in every frame in the file |
  | 20 | 8 | CommandSequence | `0` for RunHeader; exact next sequence for a command |
  | 28 | 4 | PayloadLength | Exact number of bytes following the envelope |
  | 32 | 4 | CRC32C | Checksum defined below |

- CRC32C uses the reflected Castagnoli polynomial `0x82F63B78`, initial value `0xFFFFFFFF`, and
  final XOR `0xFFFFFFFF`. It covers envelope bytes 0 through 31 followed by the complete payload;
  the checksum field itself is excluded.
- The version 1 `RunHeader` payload is encoded in this order:
  `BehavioralRulesVersion:uint32`, `MaxEventsPerCommand:uint32`,
  `MaxRunCommands:uint64`, `MaxRunJournalBytes:uint64`, `InstrumentCount:uint32`,
  `GeneratorConfigLength:uint32`, then `InstrumentCount` entries sorted by increasing
  `InstrumentId`, each containing `InstrumentId:uint64` and `ConfigurationVersion:uint32`, followed
  by exactly `GeneratorConfigLength` generator-configuration bytes. A zero generator length means no
  generated-order scenario. A nonzero generator block begins with `GeneratorVersion:uint32`,
  `Seed:uint64`, and `ParameterBytesLength:uint32`, followed by exactly that many canonical parameter
  bytes defined by the named generator version. `GeneratorConfigLength` must therefore equal
  `16 + ParameterBytesLength` when nonzero. A generator version must define its parameter codec before
  it can be written.
- The version 1 `NewOrder` payload is encoded in this order:
  `BehavioralRulesVersion:uint32`, `ConfigurationVersion:uint32`, `ClientId:uint64`,
  `InstrumentId:uint64`, `ClientCommandIdLength:uint8`, the exact 1-to-64 ASCII client-command bytes,
  `Side:uint8`, `TimeInForce:uint8`, `Price:uint64`, and `Quantity:uint64`. `Side` uses `1` for BUY and
  `2` for SELL. `TimeInForce` uses `1` for GTC and `2` for IOC.
- The version 1 `Cancel` payload is encoded in this order:
  `BehavioralRulesVersion:uint32`, `ConfigurationVersion:uint32`, `ClientId:uint64`,
  `InstrumentId:uint64`, `ClientCommandIdLength:uint8`, the exact 1-to-64 ASCII client-command bytes,
  and `TargetOrderId:uint64`.
- Version 1 permits no trailing payload fields. A command frame may not exceed 256 bytes; the
  `RunHeader` frame may not exceed 65,536 bytes. Declared and derived lengths must agree exactly.
- Behavioral rules version 1 is the initial rule set in this document and has
  `MaxEventsPerCommand = 4,096`. Each command's behavioral-rules version must equal its run header.
  A command's `(InstrumentId, ConfigurationVersion)` must appear exactly once in the header's sorted
  instrument entries. Duplicate instrument entries, zero required identifiers or versions, unknown
  enum codes, and inconsistent header/command versions are invalid journal data.
- `MaxRunJournalBytes` counts every byte in the journal file, including the `RunHeader` envelope and
  payload. Filesystem allocation overhead and the installation catalog are outside this value. The
  writer calculates the complete next command-frame size before selecting its candidate sequence; a
  frame that would exceed the remaining byte capacity produces `RunCapacityReached` and is not
  appended.
- `MaxRunCommands` counts command frames and excludes the `RunHeader`. Run creation rejects capacity
  configuration that cannot contain its own valid header or that cannot fit the documented
  worst-case in-memory active and retained-run views.
- A new journal is created at a new path without overwriting an existing file. The complete
  `RunHeader` is written and durably synchronized, and creation of its directory entry is durably
  synchronized, before the catalog may expose that run as active. A failure or uncertain outcome
  during this operation leaves the reserved ID unused; gaps are allowed.
- Recovery validates the format magic, supported versions, declared length against a configured
  maximum, checksum, and exact sequence continuity before interpreting a record.
- The canonical command payload contains the business fields needed for deterministic replay:
  `ClientId`, `ClientCommandId`, `InstrumentId`, command kind, side, TimeInForce, price, quantity,
  optional `TargetOrderId`, and the applicable configuration versions. `OrderId` for a NewOrder is
  derived from its authoritative `CommandSequence` under the adopted identity rule.
- Session identity, FIX transport fields, receipt time, wall-clock time, process identity, port,
  topic, and non-authoritative shard metadata are not journal command identity and do not affect
  retransmission comparison.
- The initial durable layout is one bounded append-only journal file per run. Segmentation and a
  manifest are not required. A later layout may segment one run only if it preserves identical
  command, identity, validation, and replay behavior.

#### Command identity and result retention

- `(ExchangeRunId, ClientId, ClientCommandId)` is protected against reuse for the complete retained
  lifetime of that run. A `ClientCommandId` may be reused in a different run because the logical
  identity contains a different `ExchangeRunId`.
- Admission compares the exact canonical normalized command with the original. A fingerprint may
  narrow a lookup, but it never determines the admission outcome without exact authoritative
  comparison. Hash collisions cannot cause two different commands to be treated as identical.
- Admission identities and completed results for a retained run are reconstructed by validating and
  replaying its bounded journal. A permanent result store or disk-backed identity index is not
  required.
- Implementations may cache or materialize results, but those copies are derived and must not become
  a second source of matching truth.
- An identical retransmission or read-only lookup for a retained run retrieves the original result
  with its original identifiers. A business submission for a stopped run receives
  `ExchangeRunUnavailable`. A lookup for a retained stopped run remains valid; a lookup for a deleted
  or unknown run receives `ExchangeRunUnavailable`. None of these paths can create work in the active
  run.

#### Corruption and recovery

- After one complete valid `RunHeader`, recovery may automatically truncate only an incomplete final
  command frame at the physical end of the run journal. An incomplete tail is fewer than 36 bytes or
  a valid envelope whose declared `TotalLength` extends past end of file. A complete envelope with
  invalid magic, type, version, length, or checksum is corruption rather than an incomplete tail.
  Recovery preserves truncated tail bytes for diagnosis where practical.
- A checksum failure in a complete record, invalid length, unsupported format or rules version,
  corruption before the physical tail, sequence gap, duplicate, or decrease is a recovery failure.
  The exchange preserves the evidence and does not skip, synthesize, reorder, or reinterpret
  records to start trading.
- Startup progresses through `Starting` and `Recovering`. A newly created, explicitly started empty
  run may then enter `Ready`; a pre-existing run recovered after restart or failure enters `Paused`
  and requires explicit resume. An unrecoverable startup problem enters `RecoveryFailed`; an
  invariant or durability failure after startup enters `FailStopped` until recovery is completed.
  New FIX business commands are accepted only in `Ready`.
- Recovery loads and validates the run header and required format, behavioral-rules, and
  instrument-configuration versions; obtains exclusive ownership of the journal; starts from empty
  matching state; and replays every complete command through the matching state machine. It rebuilds
  admission and result lookup state and validates invariants before reporting successful recovery to
  the run controller. The controller applies the lifecycle transition defined above.
- Replay uses the stored normalized values and their recorded versions. It does not reparse FIX or
  renormalize historical commands using current configuration.
- The initial behavioral-rules version identifies the adopted GTC, IOC, cancellation,
  price-time-priority, validation, rejection, identity, and `MaxEventsPerCommand` behavior in this
  document. A later semantic change uses an explicit new rules version and retains the old replay
  implementation while its records remain recoverable.
- Replay regenerates private results without publishing external FIX responses, multicast output,
  or market data. Live unsolicited delivery resumes only after the controller reaches `Ready`;
  explicit identical-command or read-only retrieval is available in `Paused` after successful
  recovery. Protocol-specific reconnect delivery remains unresolved.
- Snapshots are not required for the initial bounded run. If measured full-replay time at maximum
  supported run capacity is unacceptable, a later rule may add versioned, checksummed, atomically
  published snapshots as replay accelerators. The journal remains authoritative.
- No numerical recovery-time objective is adopted before a reproducible full-capacity replay
  benchmark exists. Correct validation always takes precedence over recovery speed.

#### Run capacity and retained storage

- Each run has both `MaxRunCommands` and `MaxRunJournalBytes`. Their exact initial values remain a
  configuration decision, but they are immutable after the run enters `Ready` and are included in
  exported or replayed run metadata.
- Configured run capacity must fit documented memory and durable-storage budgets on the reference
  laptop, including reconstructed admission identity and completed-result state. Capacity is a
  supported product limit, not an indefinitely growing historical namespace.
- In-flight queues remain independently bounded. Temporary ingress saturation receives an explicit
  pre-sequence unavailable response and does not consume run history. Exhausting run command or byte
  capacity receives `RunCapacityReached`, enters `CapacityReached`, and does not assign another
  sequence.
- Retained run journals reside on local durable storage. No archive tier, indefinite index, or
  online-history window is required. Retention cleanup removes only stopped runs selected by the
  adopted retention policy and never overwrites the active run.
- If durable storage is unavailable or a write has an uncertain outcome, the exchange stops new
  admission and follows recovery rules. It does not reinterpret the failure as ordinary run
  capacity.
- An expected validation or admission failure follows its documented pre-sequence response and does
  not enter recovery. An unexpected failure may continue only when the component can prove that no
  sequence became authoritative, no matching state changed, and its invariants still hold. If
  durability or mutation is uncertain, the exchange enters `FailStopped`; it does not translate the
  exception into a business rejection or continue on possibly corrupted state.

## Unresolved decisions

### 1. Command representation and numeric limits

Command fields, identifier semantics, underlying integer widths, unsigned Price and Quantity,
checked arithmetic, and the versioned journal envelope are adopted. The following remain unresolved:

- Evolution rules for adding fields to future journal format and behavioral-rules versions.
- Maximum notional and any notional conversion or rounding rules.
- Numeric limits, tick value, quantity unit, and lot size for instruments other than the initial
  `SPY` test configuration.

### 2. Exchange-run representation and capacity

Exchange-run scope, lifecycle, retention count, capacity behavior, and fully qualified identity are
adopted. The following remain unresolved:

- Initial values for `MaxRunCommands` and `MaxRunJournalBytes`, to be selected from measured encoded
  record size, replay cost, result-memory use, and the reference laptop budget.
- Export/import packaging and whether imported stopped runs are view-only or recoverable.

### 3. Sequencer admission and fairness

The run-global sequence scope, initial FIFO boundary, journal sequence validation, restart recovery, and
no-later-commit failure behavior are adopted. Cross-client fairness policies beyond FIFO acceptance
remain deliberately unspecified.

### 4. Event visibility and delivery

Initial private visibility, identical-command result retrieval, and concurrent cross-partition
publication are adopted. The following remain unresolved:

- Protocol-specific result-history requests beyond identical command retransmission.
- Whether a protocol automatically sends unacknowledged results after reconnect.

### 5. Instrument and configuration lifecycle

Stable numeric instruments and versioned configuration are adopted. The following remain unresolved:

- Supported currencies or other instrument classes.
- External symbol format, rename policy, and mapping to `InstrumentId`.
- Configuration activation, persistence, compatibility, and version migration.

### 6. Trading sessions and expiry

**Question:** What do DAY, GTD, and ATC mean?

**Options to decide:**

- Whether the exchange has trading sessions.
- Session timezone and calendar.
- End-of-day treatment of active orders.
- Whether commands outside the trading session are rejected, queued, or accepted into a closed book.
- How clock changes and restart affect expiry.

**Current recommendation:** Do not support session-dependent TimeInForce values until a deterministic
session calendar and replay rule have been adopted.

### 7. Deferred order and TimeInForce behavior

GTC, IOC, and cancellation are in the initial scope. The following remain unresolved and deferred:

- Market orders.
- DAY, FOK, GTD, ATC, GTX, and any other TimeInForce values.
- Trading-session and auction behavior.
- Expiry behavior.
- The eventual order-type and TimeInForce scope.

### 8. Modification

**Question:** Is order modification an atomic amend or cancel-replace, and what happens to priority?

**Options to decide:**

- Support only cancel-replace.
- Permit quantity reduction while preserving priority.
- Treat quantity increase or price change as a new order.
- Reject all direct modifications initially.

**Current recommendation:** Start with cancel-replace. If atomic amendment is added later, consider
preserving priority only for a quantity reduction at the same price.

### 9. Market-data events

**Question:** What public market data does the exchange publish?

**Options to decide:**

- Trades only.
- Top of book.
- Aggregated depth.
- Order-by-order depth.

**Additional decisions:**

- Snapshot and incremental-update formats.
- Sequence and gap recovery.
- Relationship between private execution events and public market data.

**Current recommendation:** Start with sanitized trades, top of book, and aggregated depth. Do not
publish order-by-order private state. A privileged educational view may inspect internal state but
must remain visibly distinct from participant and public views. Do not describe a live feed as
recoverable until its snapshot and sequence-gap behavior are adopted and implemented.

### 10. Recovery operations and migration

Run headers, journal framing, checksums, strict corruption handling, full versioned replay, and
suppressed external replay publication are adopted. The following remain unresolved:

- Administrative repair and evidence-preservation tooling after recovery failure.
- Migration procedure between future supported journal, rules, and configuration versions.
- Clean-shutdown and operator progress-reporting interfaces.
- Whether measured maximum-capacity replay justifies snapshots.

### 11. Backpressure and unavailable components

**Question:** What happens when an ingress or internal boundary reaches capacity or a component is
unavailable?

The per-command result bound, `GatewayBusy` response for temporary pre-sequence saturation,
`RunCapacityReached` response for run exhaustion, fail-closed durable-capacity behavior, and
post-commit event-handoff behavior are adopted. Other internal boundaries still require decisions.

**Options to decide:**

- Block the producer.
- Reject before sequencing.
- Persist and defer processing.
- Apply bounded retry.

**Additional decisions:**

- Protocol-specific mappings for the adopted admission reasons.
- Behavior while an instrument partition recovers.

**Current recommendation:** Use bounded queues with observable saturation. Once a command has been
acknowledged as accepted, it must complete or remain recoverable; it must not be silently discarded.

### 12. Deferred physical storage choices

The bounded per-run journal, exact comparison, retained-run replay, and fail-closed capacity rules
are adopted. The following implementation choices remain unresolved:

- Local storage budget, reserved free-space allowance, and admission stop watermark below the hard
  byte limit.
- Physical encoding for optional derived materialized results.
- Compression or encryption for exported run bundles.
- Whether measurements justify a snapshot, index, or segmented-file optimization within one run.

### Initial matching-engine decision status

No additional state-dependent `CommandRejectionReason` is required for the adopted initial command
scope. `OrderNotActive`, `NotOwner`, and `BookCapacityExceeded` remain the complete initial set.
Wrong client input is handled at the documented validation or state boundary; an internal routing
contradiction is an invariant failure.

Publication across independent partitions is not globally merged. Per-partition and per-command
ordering remain required as specified in the adopted rules.

Trading sessions, expiry, modification, additional order types, market data, snapshots, and group
commit can remain deferred. Exchange-run identity, lifecycle, capacity, retention, and journal
metadata must be implemented before durable replay is described as conforming. New-order
state-machine tests can continue before event delivery and cross-partition publication are
implemented, provided the tests use the adopted event schemas and ordering.
