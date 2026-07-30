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

No detailed market rules have yet been formally adopted.

The project goals—multiple instruments, deterministic and fair processing, recovery and replay, clear
events, and reliable bounded operation—constrain future decisions, but they do not by themselves
define complete exchange behavior.

## Unresolved decisions

### 1. Supported orders and TimeInForce

**Question:** Which commands and TimeInForce values are part of the first supported rule set and the
eventual project scope?

**Options to decide:**

- Limit orders only initially, or limit and market orders.
- DAY, GTC, IOC, FOK, GTD, ATC, or another supported subset.
- Continuous trading only, or eventual auction phases.
- Whether unsupported order types are rejected at the gateway or as sequenced business rejections.

**Current recommendation:** Begin with limit BUY and SELL plus one explicitly defined resting
TimeInForce. Add cancel next, followed by IOC and FOK. Add expiry-based and auction behavior only
after trading-session rules exist.

### 2. Trading sessions and expiry

**Question:** What do DAY, GTD, and ATC mean?

**Options to decide:**

- Whether the exchange has trading sessions.
- Session timezone and calendar.
- End-of-day treatment of active orders.
- Whether commands outside the trading session are rejected, queued, or accepted into a closed book.
- How clock changes and restart affect expiry.

**Current recommendation:** Do not support session-dependent TimeInForce values until a deterministic
session calendar and replay rule have been adopted.

### 3. Price priority and time priority

**Question:** What matching priority should continuous trading use?

**Options to decide:**

- Price-time priority.
- Pro-rata or another policy.
- Whether one policy applies to every instrument.
- Which authoritative sequence establishes time priority.

**Current recommendation:** Use price-time priority: highest bid and lowest ask first, then the
authoritative command sequence within a price level. Do not use wall-clock timestamps or thread
scheduling to break ties.

### 4. Trade-price determination

**Question:** At what price does a crossing order execute?

**Options to decide:**

- Resting-order price.
- Incoming-order price.
- Another deterministic rule.

**Current recommendation:** Execute at the resting order's price.

### 5. Partial and multiple fills

**Question:** What are the exact state and event rules when one order matches one or more resting
orders?

**Options to decide:**

- Whether partial fills are supported.
- Event ordering for multiple fills.
- Whether one execution event is emitted per resting counter-order or per incoming command.
- Which aggregate and terminal-state events follow the fills.

**Current recommendation:** Support partial and multiple fills. Emit one trade/execution result per
counter-order in matching order. Each emitted quantity must agree exactly with both orders' state
changes.

**Candidate invariants:**

- Filled quantity never exceeds original quantity.
- Remaining quantity never becomes negative.
- Price-level aggregate quantity equals the sum of active order quantities at that level.
- A fully filled order is no longer active.

### 6. Cancellation

**Question:** How is an active order targeted and what result does each cancel situation produce?

**Options to decide:**

- Target by exchange order ID, client order ID plus scope, or both.
- Whether repeated cancellation is idempotent or rejected.
- Results for unknown, already-filled, expired, already-cancelled, or wrong-owner targets.
- Event ordering when fills and a cancellation race.

**Current recommendation:** Sequence cancellation like any other command. It takes effect at its
authoritative sequence position and removes only the remaining active quantity. Earlier fills remain
valid; later commands observe the cancellation.

### 7. Modification

**Question:** Is order modification an atomic amend or cancel-replace, and what happens to priority?

**Options to decide:**

- Support only cancel-replace.
- Permit quantity reduction while preserving priority.
- Treat quantity increase or price change as a new order.
- Reject all direct modifications initially.

**Current recommendation:** Start with cancel-replace. If atomic amendment is added later, consider
preserving priority only for a quantity reduction at the same price.

### 8. Price representation

**Question:** What exact integer unit represents price?

**Options to decide:**

- Currency minor units such as cents.
- One global fixed decimal scale.
- Instrument-specific price ticks.

**Current recommendation:** Represent price internally as integer ticks and configure tick value per
instrument. External decimal prices must convert exactly; otherwise reject them.

**Additional decisions:**

- Supported currencies or non-currency instruments.
- Maximum price and notional.
- Tick-size configuration and versioning.

### 9. Quantity representation

**Question:** What exact unit and range represents quantity?

**Options to decide:**

- Whole shares or contracts only.
- Fractional quantities with a fixed scale.
- Instrument-specific lot units.

**Current recommendation:** Begin with positive integer quantity units and explicit per-instrument lot
rules. Check every conversion and arithmetic operation for range and overflow.

### 10. Instruments and symbols

**Question:** How are instruments identified and configured?

**Options to decide:**

- Text symbol as the internal identity.
- Stable exchange-assigned numeric instrument ID with an external symbol mapping.
- Symbol character set, length, and rename policy.
- Configuration persistence and versioning.

**Current recommendation:** Use a stable numeric `InstrumentId` internally and treat the external
symbol as validated configuration data. Never silently truncate or hash a symbol as its sole identity.

### 11. Order, client, and session identifiers

**Question:** What identifiers exist and what is each uniqueness scope?

**Identifiers to define:**

- Client or account ID.
- Transport session ID.
- Client-supplied order ID.
- Exchange-assigned order ID.
- Command sequence.
- Event sequence.
- Trade ID.

**Options to decide:**

- Client order-ID uniqueness per session, client, trading day, or retained history.
- Whether a terminal order ID may be reused.
- How retries and duplicate commands are detected.
- How reconnecting sessions retain client identity.

**Current recommendation:** Give exchange orders and trades stable exchange-assigned IDs. Scope client
order IDs to a documented client identity and retain enough information to make retries idempotent.
Do not use implementation-defined hashes as stable identities.

### 12. Validation and rejection

**Question:** Which checks happen before sequencing and which are deterministic business checks at the
matching partition?

**Checks to classify:**

- Required fields and protocol syntax.
- Instrument existence.
- Price and quantity representation and range.
- Supported side, order type, and TimeInForce combinations.
- Duplicate IDs.
- Ownership and authorization.
- Stateful order and cancellation checks.

**Options to decide:**

- Whether pre-sequencing rejections appear in the authoritative event stream.
- Rejection-code taxonomy.
- Whether every syntactically valid command receives a sequence position.

**Current recommendation:** Keep lossless parsing and stateless format/range checks at the gateway.
Sequence state-dependent decisions so replay produces the same result. Every rejection must have a
stable machine-readable reason and must not mutate the book.

### 13. Self-trading

**Question:** Is self-trading allowed, and what ownership scope defines "self"?

**Options to decide:**

- Allow self-trading.
- Cancel the incoming order.
- Cancel the resting order.
- Cancel both.
- Apply another deterministic prevention rule.

**Additional decisions:**

- Account, client, firm, or another ownership scope.
- Events emitted by prevention.
- Priority effects when a resting order is removed.

**Current recommendation:** Do not claim self-trade prevention until stable account ownership exists.
If prevention is adopted, base it on account ownership rather than a transport session.

### 14. Sequencing and fairness

**Question:** What is the authoritative processing order?

**Options to decide:**

- One global command sequence.
- One sequence per instrument partition.
- One sequence per instrument.
- Whether cross-instrument event order is meaningful.

**Additional decisions:**

- The sequencing boundary: gateway normalization, journal append, or partition acceptance.
- Fair merging across client sessions.
- Treatment of gaps, duplicates, retries, and partition migration.
- Whether rejected commands consume sequence positions.

**Current recommendation:** Require one deterministic order for every command that can affect the same
instrument. Independent instruments may process concurrently. Define fairness at a sequencing
boundary before commands enter concurrent processing.

### 15. Order and execution events

**Question:** Which immutable business events are emitted for each command?

**Candidate events:**

- Command accepted.
- Command rejected.
- Order resting.
- Partial fill.
- Full fill.
- Trade.
- Cancel accepted.
- Cancel rejected.
- Order expired.

**Additional decisions:**

- Exact order-state model.
- Event sequence scope.
- Event order when one command creates multiple trades.
- Which events are private to clients.
- Which events are durable or may be regenerated.

**Current recommendation:** Emit immutable typed events rather than mutated command objects. Events
must contain sufficient identifiers and sequence information for routing, audit, and replay.

### 16. Market-data events

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

### 17. Determinism and replay

**Question:** What inputs and configuration must reproduce identical state and events?

**Decisions required:**

- Authoritative command log, event log, or both.
- Versioning of behavioral configuration.
- Treatment of timestamps and expiry during replay.
- Whether recovery republishes events.
- Consumer deduplication.
- Handling corrupted or truncated records.

**Current recommendation:** Given the same initial configuration and authoritative command sequence,
replay should produce identical order state and business events. Wall clocks, random values, memory
addresses, thread scheduling, and implementation-defined hashes must not decide matching results.

### 18. Acknowledgement and durability

**Question:** When may the exchange tell a client that a command was accepted?

**Options to decide:**

- Before journal append.
- After append to process memory.
- After operating-system write.
- After durable flush.
- After matching and event persistence.

**Additional decisions:**

- Snapshot frequency.
- Recovery-time target.
- Tolerated acknowledged-data loss.
- Clean shutdown guarantees.

**Current recommendation:** Do not acknowledge acceptance until the command is recoverable according
to an explicitly adopted durability level. Begin with a local append-only journal and periodic
snapshots rather than requiring external databases.

### 19. Backpressure and unavailable components

**Question:** What happens when an ingress or internal boundary reaches capacity or a component is
unavailable?

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

### Recommended decision order

1. Price, quantity, instrument, client, order, and trade identifiers.
2. Initial supported order and TimeInForce set.
3. Price-time priority, trade price, partial fills, and event ordering.
4. Cancellation, duplicate handling, and modification.
5. Sequencing boundary and fairness.
6. Command and event schemas.
7. Acknowledgement, journaling, snapshots, and replay.
8. Additional TimeInForce and order types.
9. Market-data recovery and later performance work.
