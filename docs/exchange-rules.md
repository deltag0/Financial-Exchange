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

- `CommandSequence` is the globally unique, monotonically increasing authoritative command identity.
  Its underlying representation is `uint64_t`. It is never reused. Monotonicity does not by itself
  prohibit gaps.
- Every normalized NewOrder has an `OrderId` whose numeric value equals its `CommandSequence`.
  `OrderId` is a distinct strong type backed by `uint64_t` and is never reused.
- `ClientId` is a stable exchange-assigned client identity. It remains the same across transport
  sessions and reconnects. It is a distinct strong type backed by `uint64_t`.
- Initially, persisted exchange configuration maps each authorized FIX identity to its numeric
  `ClientId`. Reconnecting, or using another permitted session for the same client, preserves that
  `ClientId`. A transport-session hash is not a client identity.
- `ClientCommandId` is selected by the client and is unique within that `ClientId`. The logical
  command identity is `(ClientId, ClientCommandId)`. `ClientCommandId` is a bounded-string strong
  type containing between 1 and 64 ASCII bytes. Comparison is case-sensitive and exact byte for
  byte; no trimming, case folding, or text normalization is applied. It must not be represented
  solely by a hash.
- Repeating the same logical command with identical normalized contents returns its previously
  determined result and does not execute the business action again or emit new business events.
- Reusing the same logical command identity with different normalized contents receives the
  admission rejection reason `DuplicateCommandConflict`, receives no `CommandSequence`, emits no
  business event, and does not mutate matching state.
- The exchange must retain or reconstruct enough deduplication state to preserve these rules after
  reconnect and recovery.
- `InstrumentId` is a stable exchange-defined numeric identifier. Matching rules use it rather than
  an external symbol string. It is a distinct strong type backed by `uint64_t`.
- `EventId` is `(commandSequence, eventIndex)`. It is globally unique in the deterministic business
  event stream. `EventIndex` is a distinct strong type backed by `uint32_t`.
- `CommandSequence` and `EventIndex` never wrap. The exchange stops admitting affected work before
  identifier exhaustion rather than reusing an identifier; configured capacity must prevent one
  command from requiring more event positions than `EventIndex` can represent.
- No independent `TradeId` is required initially; the `EventId` of a `Trade` identifies that trade.
- Authoritative exchange-assigned identifiers are never reused.

### Normalized command schemas

The normalized, sequenced NewOrder command contains:

- `CommandSequence`;
- `ClientId` and `ClientCommandId`;
- `InstrumentId` and `ConfigurationVersion`;
- `Side`;
- `Price` and `Quantity`;
- `TimeInForce`.

Its `OrderId` is derived by wrapping the same underlying numeric value as `CommandSequence`; it does
not require a second independently generated identity.

The normalized, sequenced Cancel command contains:

- `CommandSequence`;
- `ClientId` and `ClientCommandId`;
- `InstrumentId` for routing and validation;
- `TargetOrderId`.

The cancellation target remains `TargetOrderId`. `InstrumentId` does not form part of the order's
identity.

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

- The sequencer assigns every sequenced command one authoritative global command sequence.
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
- Consumers are not guaranteed to receive events in globally increasing `CommandSequence` order
  across independent partitions. The authoritative global command order remains reconstructible
  from the command journal and event identifiers.

### Business events

- Events caused by one command are emitted in the same order as their corresponding state
  transitions.
- Every event is identified by the pair `(commandSequence, eventIndex)`.
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

- The full authoritative `Trade` is available internally.
- Each participant receives a private execution view containing the event identity, instrument, its
  own order identity, role or side, execution price and quantity, and its own remaining quantity.
  It does not disclose the counterparty's `ClientId`, `OrderId`, or remaining quantity.
- Sanitized trade information may later be public. The initial rules do not yet adopt a public trade
  feed or its format.
- `OrderRested`, `OrderCancelled`, and `CommandRejected` are private to the affected client. Their
  resulting book or quote changes may later produce separate public market-data events.
- Committed private results survive client disconnection. Initially, retransmitting the identical
  `(ClientId, ClientCommandId)` is the required way to wait for or retrieve the original result.
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
- Retransmitting the same `(ClientId, ClientCommandId)` returns the previously determined result and
  does not perform cancellation again.
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
- After normalization and before global sequence assignment, one authoritative command-admission
  boundary enforces `(ClientId, ClientCommandId)` uniqueness for all gateways. This is not an
  independent cache owned by each gateway.
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
- The initial stable `AdmissionRejectionReason` is `DuplicateCommandConflict` for conflicting
  logical command reuse. An admission rejection is a gateway response, not a `CommandRejected`
  business event, because it has no `CommandSequence` or `EventId`.
- Initial stable `CommandRejectionReason` values for sequenced commands are:
  - `OrderNotActive` for a new cancellation targeting an order that is not active;
  - `NotOwner` for a cancellation targeting another client's active order;
  - `BookCapacityExceeded` when a sequenced command cannot be applied within configured matching
    capacity without violating a numeric or resource bound.

### Authoritative journal, durability, and replay

- The normalized, sequenced command journal is the authoritative recovery record.
- Commands recorded in the journal are immutable.
- Replay processes the journal through the same deterministic matching behavior and regenerates
  business events with the same identifiers, order, and content.
- Replication is not required by the initial durability model.
- A command is externally committed only after its normalized, sequenced journal record completes
  the configured durable-write operation. The initial operation is `fdatasync`, `fsync`, or the
  platform-equivalent durable synchronization selected by the implementation.
- Initially, every command receives one durable synchronization. A later group-commit policy may
  make multiple commands durable with one synchronization operation, but no command may be reported
  as committed before the group containing it is durable.
- Performance results must state the durability policy. A benchmark must not claim improved durable
  latency by acknowledging before durability without explicitly describing the weaker guarantee.
- The initial commit path is:
  1. select the next candidate global command sequence;
  2. append the immutable normalized command containing that candidate;
  3. complete the configured durable synchronization, at which point the sequence becomes
     authoritative;
  4. process the command;
  5. publish the resulting events;
  6. acknowledge the business result.

## Unresolved decisions

### 1. Command representation and numeric limits

Command fields, identifier semantics, underlying integer widths, unsigned Price and Quantity, and
checked arithmetic are adopted. The following remain unresolved:

- Serialized command representation and schema-version compatibility.
- Maximum notional and any notional conversion or rounding rules.
- Numeric limits, tick value, quantity unit, and lot size for instruments other than the initial
  `SPY` test configuration.

### 2. Sequencer admission, fairness, and gaps

The global sequence scope, initial FIFO boundary, and no-later-commit failure behavior are adopted.
The following remain unresolved:

- Handling of missing positions, duplicate internal delivery, and restart.

### 3. Event visibility and delivery

Initial private visibility, identical-command result retrieval, and concurrent cross-partition
publication are adopted. The following remain unresolved:

- Protocol-specific result-history requests beyond identical command retransmission.
- Whether a protocol automatically sends unacknowledged results after reconnect.

### 4. Instrument and configuration lifecycle

Stable numeric instruments and versioned configuration are adopted. The following remain unresolved:

- Supported currencies or other instrument classes.
- External symbol format, rename policy, and mapping to `InstrumentId`.
- Configuration activation, persistence, compatibility, and version migration.

### 5. Trading sessions and expiry

**Question:** What do DAY, GTD, and ATC mean?

**Options to decide:**

- Whether the exchange has trading sessions.
- Session timezone and calendar.
- End-of-day treatment of active orders.
- Whether commands outside the trading session are rejected, queued, or accepted into a closed book.
- How clock changes and restart affect expiry.

**Current recommendation:** Do not support session-dependent TimeInForce values until a deterministic
session calendar and replay rule have been adopted.

### 6. Deferred order and TimeInForce behavior

GTC, IOC, and cancellation are in the initial scope. The following remain unresolved and deferred:

- Market orders.
- DAY, FOK, GTD, ATC, GTX, and any other TimeInForce values.
- Trading-session and auction behavior.
- Expiry behavior.
- The eventual order-type and TimeInForce scope.

### 7. Modification

**Question:** Is order modification an atomic amend or cancel-replace, and what happens to priority?

**Options to decide:**

- Support only cancel-replace.
- Permit quantity reduction while preserving priority.
- Treat quantity increase or price change as a new order.
- Reject all direct modifications initially.

**Current recommendation:** Start with cancel-replace. If atomic amendment is added later, consider
preserving priority only for a quantity reduction at the same price.

### 8. Market-data events

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

**Current recommendation:** Start with deterministic trade and top-of-book events. Do not describe the
feed as reliable until consumers can recover from a sequence gap using a snapshot.

### 9. Recovery and replay edge cases

The authoritative command journal and initial durability point are adopted. The following remain
unresolved:

- Record framing, checksums, format compatibility, and log rotation.
- Handling a truncated final record versus corruption in the middle of a journal.
- Snapshot frequency, contents, and validation.
- Recovery-time target and clean-shutdown guarantees.
- Whether recovery republishes events and how consumers deduplicate them.
- How configuration versions are retained and loaded for replay.

### 10. Backpressure and unavailable components

**Question:** What happens when an ingress or internal boundary reaches capacity or a component is
unavailable?

The per-command result bound and post-commit event-handoff behavior are adopted. Other ingress and
internal boundaries still require decisions.

**Options to decide:**

- Block the producer.
- Reject before sequencing.
- Persist and defer processing.
- Apply bounded retry.

**Additional decisions:**

- Client-visible errors.
- Behavior while an instrument partition recovers.
- Fatal invariant failure versus command rejection.

**Current recommendation:** Use bounded queues with observable saturation. Once a command has been
acknowledged as accepted, it must complete or remain recoverable; it must not be silently discarded.

### Initial matching-engine decision status

No additional state-dependent `CommandRejectionReason` is required for the adopted initial command
scope. `OrderNotActive`, `NotOwner`, and `BookCapacityExceeded` remain the complete initial set.
Wrong client input is handled at the documented validation or state boundary; an internal routing
contradiction is an invariant failure.

Publication across independent partitions is not globally merged. Per-partition and per-command
ordering remain required as specified in the adopted rules.

Trading sessions, expiry, modification, additional order types, market data, snapshots, and group
commit can remain deferred. New-order state-machine tests can begin before event delivery and
cross-partition publication are implemented, provided the tests use the adopted event schemas and
ordering.
