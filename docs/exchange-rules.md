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
- A self-trade produces two recipient-private trade views in deterministic maker-then-taker order so
  both owned orders are represented.
- The transport-neutral public trade projection emits exactly one public value for each authoritative
  `Trade`, preserving event order and `EventId`. It contains only `EventId`, `InstrumentId`, execution
  price, execution quantity, and aggressor side. It contains no client, client-command, order, or
  remaining-quantity identity and retains no authoritative event or result batch.
- A self-trade produces one public trade. Non-trade events produce no public market-data value.
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

### Participant-private result handoff

- The private handoff unit is one command's complete ordered participant-private fan-out. It contains
  one envelope per affected `ClientId`, in deterministic first-event appearance order, and preserves
  the projected private-event order within each envelope. An empty projection creates no entry.
- One fan-out batch is accepted atomically. Partial recipient insertion or delivery, silent drop,
  overwrite, and overtaking are forbidden.
- Handoff capacity is an explicit bound on the total number of contained private events, not on the
  number of command batches or recipient envelopes. Configuration must use checked arithmetic and
  provide room for at least `2 × MaxEventsPerCommand` private events so one worst-valid projection
  fits when the queue is empty. The factor of two accounts for a self-trade producing maker and taker
  private events from one authoritative `Trade`.
- When the private boundary cannot accept a complete fan-out, the dispatcher retains exactly that
  one projected batch and consumes no later authoritative command result until acceptance succeeds.
- Admission completes exactly once. The dispatcher retains the immutable authoritative command
  result until its private and public projections are each empty or atomically accepted by their
  respective handoffs. A retry never recompletes admission or reprojects either view.
- Private and public acceptance are independent; one channel may accept before the other. Each
  channel independently preserves command and event order, and neither may overtake its own pending
  earlier batch.
- Successful private-queue acceptance transfers handoff ownership to the private-delivery component,
  which routes each envelope by its stable `ClientId`. Protocol formatting and session selection do
  not change the accepted private result.
- An identical completed retransmission uses the retained authoritative-result lookup. It does not
  enqueue another unsolicited live private result.
- Replay and recovery may regenerate private projections for reconstruction and validation but do not
  enqueue unsolicited delivery. The live private queue is not persisted across process failure; the
  journal and reconstructed admission result remain authoritative for explicit retransmission or
  retained-run lookup.
- Pause and stop never discard a pending or queued private fan-out. If the private-delivery consumer
  cannot relieve backpressure, lifecycle advancement reports retryable incomplete progress rather
  than successful completion.
- FIX formatting, FIX session selection, FIX acknowledgements, and external result-history request
  formats remain deferred.

### Public-trade handoff

- The public handoff unit is one command's complete ordered public-trade batch. A command whose
  authoritative result contains no trades creates no public handoff entry.
- One batch is accepted atomically. Partial insertion, partial publication, silent drop, and
  overwrite are forbidden.
- Handoff capacity is an explicit bound on the total number of queued public-trade records, not only
  the number of batches. It must be large enough to accept one command containing the maximum
  supported result size when empty.
- When the boundary cannot accept a complete batch, the dispatcher retains that one batch and stops
  consuming later command results until acceptance succeeds. The dispatcher retains at most one
  pending batch outside the queue.
- Successful queue acceptance ends exchange-side handoff ownership. Subsequent transport delivery
  is the market-data publisher's responsibility and cannot change the accepted exchange result.
- `EventId` remains the stable identity of the authoritative source event. Because commands without
  trades create no entry and no public feed sequence is adopted, consumers must not interpret
  `EventId` as a gap-free market-data sequence.
- Replay and recovery regenerate public projections for internal validation without unsolicited
  external publication.
- Pause and stop never discard a pending or queued public-trade batch. If the consumer cannot relieve
  backpressure, lifecycle advancement reports retryable incomplete progress rather than successful
  completion.
- No snapshot, gap-recovery protocol, top-of-book, or depth is adopted by this handoff contract.

### Browser application event delivery

- Browser HTTP and WebSocket traffic is served only through TLS with a minimum protocol version of
  TLS 1.2. There is no plaintext listener or fallback. Missing, invalid, or mismatched configured
  certificate and private-key material prevents the browser gateway from becoming ready.
- HTTPS serves the built browser assets and the adopted login/session surface. Once authenticated,
  the application WebSocket is bidirectional: complete inbound messages carry participant commands,
  and complete outbound messages carry the public and participant-private envelopes defined below.
  Inbound messages cannot supply or override the server-side participant identity.
- Configured finite limits bound HTTP headers and bodies, WebSocket inbound messages, connection
  count, handshakes, idle connections, static assets, outbound bytes, and shutdown flushing. A
  request or message that exceeds its limit is rejected before command submission or lifecycle
  mutation. Handshake or idle timeout closes the affected connection. Exact deployment values are
  configuration, not exchange constants.
- The first external delivery surface is a versioned JSON application event stream carried by one
  WebSocket connection between an authenticated browser and the application event gateway. It is
  the showcase's participant and public-event surface; FIX result delivery remains separate and
  deferred.

#### Browser participant authentication and sessions

- The showcase has no public registration, password reset, password credential, or account database.
  Before startup, an operator provisions a bounded one-to-one list of fixed `ClientId` values and
  SHA-256 access-token digests. The typed application configuration supplies each digest as exactly
  32 bytes; any external configuration adapter must decode it from exactly 64 lowercase hexadecimal
  characters. One positive configured maximum credential count bounds the nonempty list. Because
  each configured client can have at most one session, the credential count also bounds active
  sessions. No raw access token enters startup configuration.
- Each raw access token is exactly 32 bytes generated by a cryptographically secure random source.
  Operators distribute its canonical unpadded base64url representation: exactly 43 ASCII characters
  using only `A-Z`, `a-z`, `0-9`, hyphen, and underscore. Authentication hashes the decoded 32 bytes.
  A decoder also requires that re-encoding those bytes reproduces the input exactly, rejecting unused
  trailing-bit variants rather than accepting another spelling of the same token.
  Token generation and secure delivery are operator responsibilities; the process cannot infer the
  original token's entropy from a digest.
- An empty credential list, duplicate `ClientId` values, duplicate digests, malformed digests, a zero
  maximum credential count, a credential list above that count, and non-positive or unbounded
  session lifetimes fail startup before the HTTPS listener opens.
- Login uses `POST /v1/session` over HTTPS with exactly one `Content-Type` field whose media type is
  `application/json` and has no parameters. Its bounded V1 body
  is one JSON object containing exactly `schemaVersion` with integer value `1` and `accessToken` with
  the canonical 43-character token representation. Each property occurs once, unknown properties are
  rejected, and property order is not significant. The request contains no participant identity.
- Authentication derives `ClientId` exclusively from the configured digest mapping. Browser input
  cannot select or override it. The implementation scans the bounded credential set without an
  early match exit and compares each digest in constant time. A malformed or unknown token receives
  the same `401` response with `Content-Type: application/json`, `Cache-Control: no-store`, and exact
  body `{"schemaVersion":1,"error":"AUTHENTICATION_FAILED"}`. Malformed JSON, missing, duplicate or
  unknown properties, the wrong schema version or JSON types, and a malformed or unknown token all
  use that response. HTTP framing or configured header/body-limit failures remain transport errors;
  an unsupported method or media type is rejected before authentication.
- For `/v1/session`, a method other than `POST` or `DELETE` returns `405` with `Allow: POST, DELETE`;
  a `POST` with a missing, duplicate, parameterized, or different media type returns `415`; configured
  header or body overflow returns the existing bounded HTTP error; malformed HTTP framing returns
  `400`; and Origin or authority failure returns `403`. These failures perform no credential or
  session lookup.
- A successful login returns `200`, `Content-Type: application/json`, `Cache-Control: no-store`, the
  session cookie below, and exact body
  `{"schemaVersion":1,"clientId":"<canonical unsigned decimal ClientId>"}`. Raw access tokens,
  configured credential digests, session identifiers, and cookies are never logged or returned
  through diagnostics; the successful response exposes only the authenticated client's ordinary
  public identifier.
- A successful login creates an opaque in-memory session identifier as exactly 32 bytes from a
  cryptographically secure random source and represents it as 43 canonical unpadded base64url
  characters under the same decode-and-re-encode rule as an access token. The cookie name is
  `__Host-exchange-session`; it has `Secure`, `HttpOnly`,
  `SameSite=Strict`, `Path=/`, and no `Domain`, `Expires`, or `Max-Age` attribute. Server-side
  deadlines remain authoritative even if a browser retains a stale cookie.
- Session-identifier generation is attempted once. Generator failure or collision fails closed,
  leaves any existing session unchanged, latches new login unavailable until restart, and returns a
  `503` response with `Content-Type: application/json`, `Cache-Control: no-store`, and exact body
  `{"schemaVersion":1,"error":"SERVICE_UNAVAILABLE"}`. Once latched, later login requests receive the
  same response without credential comparison. No partially created session or cookie is exposed.
- Each `ClientId` has at most one active browser session. A successful replacement first generates
  and validates a unique new session identifier. It then atomically revokes the old session, removes
  its gateway binding, and activates the new session. The network owner asynchronously closes the
  old socket after authorization has been removed; socket shutdown is not part of the atomic
  replacement. If the successful login request presented another valid session cookie, that session
  and binding are revoked in the same transition; a failed login never revokes the presented session.
- Every session has configured positive finite idle and absolute lifetimes. Authenticated activity
  may extend the idle deadline but never the absolute deadline. Every authenticated lookup and
  WebSocket action checks both deadlines against a monotonic clock before granting access; timers
  perform cleanup only and cannot extend authorization when their callbacks run late. Logout, idle
  expiry, and absolute expiry revoke the session, remove its gateway binding, and asynchronously
  close its authenticated WebSocket.
- Successful WebSocket upgrade and each syntactically valid complete authenticated HTTP request or
  WebSocket application message refresh the idle deadline. Business rejection after that point does
  not undo the refresh. Outbound delivery, TCP/TLS traffic, WebSocket ping/pong or close frames,
  malformed input, failed authentication, and unauthorized requests do not refresh it.
- Sessions and WebSocket bindings are process-local. Process restart invalidates every session
  identifier, so any retained browser cookie becomes stale and is rejected. Sessions are not
  journaled or recovered, and there is no refresh token or automatic reissuance.
- A WebSocket upgrade requires both a valid session cookie and an exact match with the one configured
  HTTPS `Origin`, plus the same effective-authority validation required for state-changing HTTP
  requests. These checks occur before session lookup or upgrade. The authenticated session supplies
  `ClientId`; the browser sends no identity claim. The browser explicitly requests one
  `ExchangeRunId`, which the gateway validates and binds to the current controller-owned run. At most one live connection is bound to a
  `(ClientId, ExchangeRunId)` pair; a replacement closes the previous binding first.
- State-changing HTTP requests, including login and logout, enforce the same exact configured Origin.
  Configuration accepts one canonical ASCII HTTPS origin: `https://` followed by a lowercase DNS
  name, canonical IPv4 literal, or bracketed canonical IPv6 literal and an optional canonical decimal
  port. Port `443` is omitted; another port is in `1..65535` with no leading zero. User information,
  an empty host, a trailing dot, path, query, fragment, and trailing slash are invalid.
  Before authentication, the request must yield exactly one effective authority. A `Host` value must
  be present exactly once; missing, duplicate, or conflicting authority values are rejected, and the
  effective authority must equal the authority of that Origin. The gateway reuses this one Origin
  setting and provides no permissive CORS behavior.
- Every request parser inspects all `Cookie` fields and pairs while ignoring unrelated cookie names.
  More than one `__Host-exchange-session` pair anywhere in the request is a malformed request and is
  rejected before session lookup. Login may omit the cookie; one canonical valid value participates
  in successful cross-client replacement, while one malformed, stale, or expired value grants no
  authority and causes no revocation. Logout may omit it and treats one malformed, stale, or expired
  value as absent. An authenticated HTTP request or WebSocket upgrade requires exactly one canonical
  session-cookie value and a successful session lookup.
- Logout uses `DELETE /v1/session` with no request body or `Content-Type`. Either field is rejected
  with `400` before session
  lookup. After Origin and authority validation logout is idempotent: exactly one valid session cookie
  is revoked, while a missing, stale, malformed, or expired cookie changes no server state.
  Every such request returns `204`, `Cache-Control: no-store`, and clears
  `__Host-exchange-session` with the same cookie attributes plus `Max-Age=0`; it returns no body and
  discloses no prior session state.
- Participant authentication authorizes only order submission and cancellation, access to that
  participant's own private results, and receipt of public events. Exchange-run lifecycle controls
  and privileged educational views remain denied until a separate authorization policy is adopted.
- This configuration establishes a stable local-showcase participant identity. It is not
  cryptographic proof of a real person, organization, or regulated institution.

#### Application-event JSON V1 schema

- Each payload is compact UTF-8 JSON with no insignificant whitespace. Properties appear in the
  exact order specified below so identical values produce identical bytes. The outer
  `schemaVersion` is the JSON integer `1`.
- Every 64-bit identifier, price, and quantity is a JSON string containing its canonical unsigned
  decimal representation: ASCII digits only, with `"0"` as the only zero form and no sign or leading
  zero. `EventIndex` is a JSON integer. Enum values use only the stable names specified below, never
  their underlying integer values.
- Every `eventId` object has properties in this order:
  `exchangeRunId`, `commandSequence`, `eventIndex`. Its first two values are decimal strings and its
  event index is an integer.
- JSON strings escape quotation mark and reverse solidus as `\"` and `\\`; use `\b`, `\f`, `\n`,
  `\r`, and `\t` for those ASCII controls; and encode other bytes below `0x20` plus `0x7f` as a
  lowercase `\u00xx` escape. Other permitted ASCII bytes are emitted unchanged.
- A public envelope has properties in this order: `schemaVersion`, `type`, `exchangeRunId`,
  `trades`. `type` is `"publicTrades"`; `trades` is one nonempty complete ordered public-trade batch.
  Each trade has properties in this order: `eventId`, `instrumentId`, `executionPrice`,
  `executionQuantity`, `aggressorSide`. These are exactly the fields of `PublicTrade`.
- A private envelope has properties in this order: `schemaVersion`, `type`, `exchangeRunId`,
  `recipientClientId`, optional `correlation`, `events`. `type` is `"privateResult"`; `events` is one
  nonempty complete ordered recipient result. `correlation` is omitted when absent. When present, it
  has properties in this order: `exchangeRunId`, `clientId`, `clientCommandId`, `commandSequence`.
- A private event begins with `type`, followed by these ordered properties:
  - `"privateTrade"`: `eventId`, `instrumentId`, `orderId`, `side`, `role`, `executionPrice`,
    `executionQuantity`, `remainingQuantity`;
  - `"orderRested"`: `eventId`, `orderId`, `clientId`, `instrumentId`, `side`, `price`,
    `remainingQuantity`;
  - `"orderCancelled"`: `eventId`, `orderId`, `clientId`, `instrumentId`, `cancelledQuantity`,
    `reason`; and
  - `"commandRejected"`: `eventId`, `commandType`, `clientId`, `clientCommandId`, optional
    `relevantOrderId`, `reason`. `relevantOrderId` is omitted when absent.
- Stable JSON enum names are: side `"BUY"` or `"SELL"`; private trade role `"MAKER"` or `"TAKER"`;
  command type `"NEW_ORDER"` or `"CANCEL"`; cancellation reason `"CLIENT_REQUESTED"` or
  `"IOC_REMAINDER"`; and command-rejection reason `"ORDER_NOT_ACTIVE"`, `"NOT_OWNER"`, or
  `"BOOK_CAPACITY_EXCEEDED"`.
- Public JSON contains no client, client-command, order, correlation, remaining-quantity, or
  authoritative-event fields. `PrivateTrade` contains no counterparty client, order, or remaining
  quantity. Other private variants retain only the affected participant's projected fields.
- Serialization rejects an empty batch, an event whose run differs from the outer run, events from
  different command sequences, a correlation whose run or sequence differs from its events, a
  correlation whose client differs from the recipient, a private terminal event for another client,
  or an unknown enum. Validation failure leaves the caller's output unchanged. Serialization builds
  no externally visible partial payload.
- `EventId` remains the stable source-event identity. No additional feed sequence is implied.
- One public batch or one recipient-private envelope occupies one WebSocket message. A message is
  queued and written as a whole. Public batches preserve command-batch and intra-batch order;
  private messages preserve command order for each recipient and event order within the recipient
  envelope. Different recipients, and the public and private channels, have no cross-connection or
  cross-channel delivery-order guarantee.
- Every authenticated connection bound to the run is eligible for the same sanitized public
  envelopes. A private envelope is eligible only for the connection whose server-side `ClientId`
  equals its recipient. Taking ownership of a complete private fan-out is atomic, but observation on
  different recipient connections is not simultaneous or atomic.
- The application event gateway owns a batch after it pops the corresponding exchange handoff
  queue. It retains a complete popped item until it has routed every eligible envelope; it never
  returns it to matching or changes the authoritative result. Each per-connection outbound queue has
  one configured capacity measured in total serialized bytes and must hold one maximum valid
  controller-produced V1 handoff. A configured maximum connection count bounds broadcast expansion.
  The gateway does not pop a later handoff item while its retained item cannot be routed.
- A connection whose outbound queue cannot accept its next complete message is closed as a slow
  consumer before that message is partially queued. An offline or disconnected participant receives
  no unsolicited delivery. Its private command result remains available through the authoritative
  identical-command or retained-result lookup. A disconnected browser can miss public trades because
  no public replay or snapshot is adopted.
- The browser sends no application-level delivery acknowledgement. Successful socket write is not a
  claim that the browser processed or durably stored the message. Reconnect creates a new live
  binding at the current stream position and never triggers automatic replay. The UI must surface a
  connection interruption rather than imply continuity.
- Pause and stop continue to wait until the exchange-side private and public handoff queues have
  transferred their accepted items to the gateway. Once transferred, gateway buffering no longer
  blocks the controller's lifecycle transition. A pause keeps the same run binding and the gateway
  may finish its bounded writes while the run is paused. After a committed stop, the gateway finishes
  or explicitly abandons bounded socket writes under the disconnect rule, then closes that run's
  bindings; a later run requires an explicit new binding and never receives buffered data from the
  stopped run.
- Coordinated shutdown stops new WebSocket bindings and command ingress, pauses the active run while
  continuing to consume its exchange handoffs, then attempts to flush gateway-owned buffers within a
  configured finite deadline before closing connections. Deadline expiry follows the same explicit
  disconnect rule: private results remain recoverable by lookup while missed public data is not.
- Before serializing or enqueueing a private message, the gateway compares the envelope's
  `ClientId` with the server-side connection binding. A mismatch is an invariant and authorization
  failure and closes the connection without sending the envelope. Only the sanitized `PublicTrade`
  model enters a public envelope; authoritative `Trade` and `CommandResultBatch` values never enter
  this participant-facing stream.
- The unsolicited stream is live-only. Journal replay and recovery do not publish it. Explicit
  participant-private lookup remains authoritative for a known command while its active or retained
  run is available; public history, snapshots, and replay remain deferred.

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
  recovery. The browser application stream never replays automatically after reconnect; FIX-specific
  reconnect delivery remains unresolved.
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

Initial private visibility, the bounded participant-private handoff, identical-command result
retrieval, the browser application stream, and concurrent cross-partition publication are adopted.
The following remain unresolved:

- Protocol-specific result-history requests beyond identical command retransmission.
- FIX acknowledgement and reconnect-redelivery behavior.
- The configured private-handoff capacity above the required worst-projection minimum.
- Exact operator token-generation and secure-distribution procedure and the external source from
  which the composition root constructs the adopted typed credential records.
- Deployment values for the allowed HTTPS Origin, maximum credential count, and idle and absolute
  session lifetimes.
- Application-level login-attempt throttling if the showcase is exposed beyond its controlled local
  environment; the initial contract adds no per-source identity or rate-limit state.
- Authorization policy for exchange-run lifecycle controls and privileged educational views.
- Exact gateway outbound-byte capacity, maximum connection count, and shutdown flush deadline.

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

The sanitized public-trade value, one-command atomic handoff unit, record-count capacity rule,
pending-batch backpressure, ownership transfer, suppressed replay publication, and live browser
transport are adopted above. The following remain unresolved and deferred:

- Top-of-book, aggregated-depth, and order-by-order public views.
- A distinct market-data sequence and its relationship to source `EventId` values.
- Snapshot, reconnect replay, and gap-recovery protocols.
- The configured handoff capacity above the required one-maximum-command minimum.

A privileged educational view may inspect internal state but remains visibly distinct from
participant and public views. No live feed is described as recoverable until its snapshot and
sequence-gap behavior are adopted and implemented.

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
`RunCapacityReached` response for run exhaustion, fail-closed durable-capacity behavior,
post-commit event-handoff behavior, and bounded public-trade handoff contract are adopted. Other
internal boundaries still require decisions.

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

Trading sessions, expiry, modification, additional order types, additional market-data views and
transports, snapshots, and group commit can remain deferred. Exchange-run identity, lifecycle,
capacity, retention, and journal metadata must be implemented before durable replay is described as
conforming. New-order state-machine tests can continue before event delivery and cross-partition
publication are implemented, provided the tests use the adopted event schemas and ordering.
