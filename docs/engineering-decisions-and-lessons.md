# Engineering Decisions and Lessons

## Why this document exists

This is the project's engineering memory and learning guide. It records not just what was built, but
why important choices were made, what alternatives looked attractive, which costs were knowingly
accepted, and what evidence should cause a decision to be revisited.

It is written primarily for the engineers building this exchange to reread later and recover the
systems-thinking behind the work. It should also help a future contributor or coding agent understand
which properties are deliberate rather than accidental.

This document is not a source of exchange behavior or implementation completeness:

- [Exchange Rules](exchange-rules.md) defines adopted and unresolved behavior.
- [Architecture](architecture.md) defines intended component ownership and data flow.
- [Implementation Status](implementation-status.md) is the only document describing what currently
  exists and which checks have actually run.

If this document conflicts with one of those specialized sources, the specialized source wins and
this document must be corrected.

The initial version was reconstructed on 2026-08-31 from those governing documents, archived design
notes, deterministic test descriptions, and repository history through commit `c4b184b`. Some early
rationale is therefore reconstructed rather than a verbatim contemporaneous record.

## How to use and maintain it

Read the relevant decision and lesson before changing a major boundary. Ask whether the new design
preserves the original property, whether the old tradeoff still applies, and whether new measurements
or requirements justify a different answer.

Add or revise an entry when a choice:

- materially constrains future implementation;
- rejects a plausible alternative;
- changes correctness, failure, durability, ownership, or performance assumptions;
- teaches a reusable systems or low-level engineering lesson; or
- would otherwise make a future engineer ask, “Why did we do this?”

Do not use this file for current status, routine refactors, task logs, test transcripts, or unverified
performance claims. When a decision changes, preserve the old entry as **Superseded**, link the
replacement, and explain what evidence changed. Recommendations must remain visibly different from
adopted behavior.

A useful new decision entry contains:

- **Status:** adopted rule, intended architecture, engineering practice, recommendation, deferred,
  or superseded.
- **Decision:** the choice in one sentence.
- **Why:** the underlying correctness, operational, maintenance, or measured-performance reason.
- **Tradeoffs:** what became harder, slower, or more complex.
- **Rejected alternatives:** credible options and why they lost.
- **Revisit when:** concrete evidence or changed requirements that could alter the answer.
- **Authority:** the governing rule or architecture section, where applicable.

## Part I — Decision record

### D-001 — Build laptop-scale logical boundaries before distributing the system

- **Status:** Intended architecture.
- **Decision:** Keep ownership and message contracts explicit, while allowing the first complete
  exchange to run in one process on ordinary laptops.
- **Why:** Two engineers can reason about, run, and test one deterministic process more effectively
  than a premature distributed system. Logical boundaries retain a path to later separation without
  immediately adding networking, consensus, deployment, and partial-failure problems.
- **Tradeoffs:** One process offers less fault isolation and a lower independent scaling ceiling. A
  later split still requires operational work.
- **Rejected alternative:** Separate every named component into a service from the start. Names on an
  architecture diagram do not by themselves justify network boundaries.
- **Revisit when:** Profiling identifies a component bottleneck, isolation requirements demand it, or
  measured workloads no longer fit the documented reference machine.
- **Authority:** [Architecture: Scope and constraints](architecture.md#scope-and-constraints).

### D-002 — Define deterministic behavior before pursuing speed

- **Status:** Engineering practice.
- **Decision:** Specify identities, ordering, state transitions, event order, and recovery semantics
  before optimizing hot paths.
- **Why:** A fast system with ambiguous outcomes cannot be replayed, audited, or tested reliably.
  Determinism creates a reference against which later optimized implementations can be checked.
- **Tradeoffs:** Early progress spends more time on rules, types, invariants, and tests than on
  headline throughput.
- **Rejected alternative:** Optimize a plausible matcher first and define semantics from whatever the
  implementation happens to do. That turns accidents into business rules.
- **Revisit when:** Do not reverse the dependency. Optimize after the affected behavior has a stable
  deterministic contract and a reproducible baseline.
- **Authority:** [Exchange Rules](exchange-rules.md) and
  [Architecture: Architectural principles](architecture.md#architectural-principles).

### D-003 — Bound resources and make overload behavior explicit

- **Status:** Adopted rule and engineering practice.
- **Decision:** Queues, in-flight state, caches, result batches, numeric aggregates, and event counts
  have explicit bounds and defined full behavior. Accepted work is never silently dropped.
- **Why:** Unbounded growth transforms load into unpredictable memory exhaustion. Silent loss makes
  the client and exchange disagree. Explicit saturation behavior is testable and forces ownership to
  remain clear.
- **Tradeoffs:** Backpressure raises tail latency and can stall a pipeline. Pre-sequence rejection
  reduces availability during overload. Capacity values require measurement.
- **Rejected alternatives:** Unbounded containers; overwriting old committed work; logging a failed
  enqueue and continuing.
- **Revisit when:** Measurements justify different bounds or queue implementations while preserving
  no-loss and commitment guarantees.
- **Authority:** [Exchange Rules: Result capacity and event handoff](exchange-rules.md#result-capacity-and-event-handoff)
  and [Architecture: Backpressure and failure boundaries](architecture.md#backpressure-and-failure-boundaries).

### D-004 — Normalize protocols into strong integer domain values

- **Status:** Adopted rule and intended architecture.
- **Decision:** FIX and future protocols normalize into protocol-independent commands using distinct
  strong identifiers, integer price ticks, and integer quantity units. Decimal input does not pass
  through binary floating point.
- **Why:** Session fields, text spelling, protocol metadata, and floating-point rounding must not
  change matching or replay. Strong types catch accidental mixing of equal-width but different
  concepts such as `ClientId`, `OrderId`, and `CommandSequence`.
- **Tradeoffs:** Adapters need explicit parsing and instrument-version lookup. New asset classes may
  require new versioned representations.
- **Rejected alternatives:** `double` prices; generic integer aliases; sending raw FIX objects into
  matching.
- **Revisit when:** A new asset class cannot be represented by the adopted fixed-width domains. Add a
  versioned domain rather than weakening existing types.
- **Authority:** [Exchange Rules: Price and quantity representation](exchange-rules.md#price-and-quantity-representation)
  and [Architecture: Commands and events](architecture.md#commands-and-events).

### D-005 — Keep commands immutable and express outcomes as immutable events

- **Status:** Adopted rule and intended architecture.
- **Decision:** Journaled commands are immutable. Matching creates a separate complete ordered event
  batch rather than mutating the input or publishing a partial outcome.
- **Why:** Immutable commands can be replayed safely. Explicit events make transitions auditable and
  let private delivery, public market data, and recovery consume different views without sharing
  mutable book state.
- **Tradeoffs:** Batches need storage and deliberate lifetime management. Allocation and reference
  counting may cost time.
- **Rejected alternatives:** Mutating an order message into a response; enqueueing individual events
  as they occur; exposing internal book objects downstream.
- **Revisit when:** Profiling proves representation overhead is material. Change allocation or
  ownership, not immutability, correlation, or atomic batch semantics.
- **Authority:** [Exchange Rules: Business events](exchange-rules.md#business-events).

### D-006 — Begin with limit GTC, limit IOC, and cancellation

- **Status:** Adopted business rule.
- **Decision:** Complete a small state machine before adding market orders, DAY, FOK, GTD, auctions,
  expiry, or direct modification.
- **Why:** Deferred features introduce clocks, session calendars, all-or-none liquidity checks,
  auction priority, or amendment-priority policy. Adding them before deterministic recovery would
  multiply ambiguity.
- **Tradeoffs:** The feature set is deliberately narrow.
- **Rejected alternative:** Parse many FIX order types and leave their downstream semantics partial.
- **Revisit when:** Sessions, recovery, and event schemas exist and each new type has explicit priority
  and replay rules.
- **Authority:** [Exchange Rules: Initial command and TimeInForce scope](exchange-rules.md#initial-command-and-timeinforce-scope).

### D-007 — Use one logical writer per book with price-time priority and maker pricing

- **Status:** Adopted rule and intended architecture.
- **Decision:** One owner mutates each instrument book. Matching selects best price, then FIFO within
  a level, and executes at the resting maker's price.
- **Why:** Single ownership removes locks and scheduler-dependent mutation inside a book. Explicit
  priority and pricing make the same command sequence replay to the same trades.
- **Tradeoffs:** A hot instrument is serialized. Concurrency comes from independent instruments, not
  several writers racing inside one book.
- **Rejected alternatives:** Concurrent mutation of one book; lock-protected shared book state;
  scheduler-dependent ordering.
- **Revisit when:** A business rule explicitly changes priority or pricing. Do not change either as a
  performance optimization.
- **Authority:** [Exchange Rules: Matching priority and trade formation](exchange-rules.md#matching-priority-and-trade-formation).

### D-008 — Index active orders and cancel by exchange `OrderId`

- **Status:** Adopted rule and component design.
- **Decision:** Resting orders have an active-order index. FIX `OrderID(37)` supplies exact
  `TargetOrderId`; ownership and active state are checked on the specified instrument partition.
  `OrigClOrdID(41)` is only protocol correlation.
- **Why:** A stable exchange identity survives client naming and makes cancellation a direct lookup.
  Searching every book or deriving identity from a client field is slower and creates ambiguous
  cross-partition behavior.
- **Tradeoffs:** The index duplicates location metadata and must remain consistent with FIFO nodes,
  aggregates, fills, and cancellations. A wrong instrument returns `OrderNotActive` rather than doing
  a global diagnostic search.
- **Rejected alternatives:** Hashing `OrigClOrdID`; scanning all books; using a transport-session
  identity as owner.
- **Revisit when:** Cross-instrument order discovery is adopted as a separate service.
- **Authority:** [Exchange Rules: Cancellation](exchange-rules.md#cancellation).

### D-009 — Preflight capacity and cap one command at 4,096 events

- **Status:** Adopted rule.
- **Decision:** Determine exact event count and relevant resource/numeric capacity before book
  mutation. Over-limit work produces one deterministic rejection and no partial matching.
- **Why:** A single aggressive order must not allocate without bound or mutate half the book before
  discovering its complete result cannot be represented or handed off atomically.
- **Tradeoffs:** An otherwise matchable order may be rejected. Preflight may inspect liquidity before
  the mutation pass.
- **Rejected alternatives:** Grow an unbounded event vector; emit fills until the queue fills; roll
  back a partially mutated book after allocation failure.
- **Revisit when:** Measured downstream capacity justifies a new versioned limit. Historical replay
  retains the old value.
- **Authority:** [Exchange Rules: Result capacity and event handoff](exchange-rules.md#result-capacity-and-event-handoff).

### D-010 — Use one global command sequence and derive NewOrder `OrderId` (superseded scope)

- **Status:** Superseded in identity scope by D-024. One sequencer and NewOrder `OrderId` derivation
  remain adopted within an exchange run.
- **Decision:** `CommandSequence` is the durable global command identity. A NewOrder `OrderId` wraps
  the same numeric value in a separate strong type.
- **Why:** One sequence establishes deterministic order across gateways and restart recovery. Reusing
  its value for order creation avoids a second identity generator while keeping types distinct.
- **Tradeoffs:** A single ordering boundary can become a scaling limit. Independent partitions may
  publish live events concurrently even though the journal preserves global command order.
- **Rejected alternatives:** Per-port or per-shard counters; random IDs; wall-clock IDs; a separate
  order-ID generator.
- **Revisit when:** A distributed sequencer becomes necessary and can preserve the adopted durable
  order—or a new business rule explicitly replaces it.
- **Authority:** [Exchange Rules: Identifiers and retransmission](exchange-rules.md#identifiers-and-retransmission).

### D-011 — Use stable clients and exact client command identifiers

- **Status:** Adopted rule.
- **Decision:** Persistently configured protocol identities map to a stable numeric `ClientId`.
  Preserve the client's bounded `ClientCommandId` exactly; hashes may accelerate comparison but never
  determine identity.
- **Why:** Session instances and implementation-defined hashes change across reconnect, build, or
  process. A collision could suppress valid work or execute a retry twice. Exact identity is required
  for ownership and recovery.
- **Tradeoffs:** Identity configuration needs administration. Exact identifiers consume more storage
  than hash-only keys. Configuration association is not cryptographic authentication.
- **Rejected alternatives:** `std::hash` of FIX session; process counters; timestamps; hash-only
  command fingerprints.
- **Revisit when:** Authentication and client provisioning are designed. Stable exchange identity
  must survive the transport session.
- **Authority:** [Exchange Rules: Identifiers and retransmission](exchange-rules.md#identifiers-and-retransmission).

### D-012 — Admit a logical command once before sequencing

- **Status:** Adopted rule and intended architecture.
- **Decision:** One shared boundary reserves `(ExchangeRunId, ClientId, ClientCommandId)`, compares
  exact normalized business fields, coalesces identical retries, rejects conflicting reuse, and tracks
  `Reserved → Sequenced → Completed`.
- **Why:** Deduplicating after sequencing wastes identities and risks executing twice. Gateway-local
  caches race and disappear across reconnect. A state machine makes ownership at each failure point
  reviewable.
- **Tradeoffs:** Admission is shared synchronization and storage. Completed entries cannot safely
  leave RAM until a durable lookup path exists.
- **Rejected alternatives:** Per-session deduplication; sequence every retry; use only a fingerprint;
  abandon in-flight state without knowing whether it committed.
- **Revisit when:** The physical index or concurrency strategy is a measured bottleneck. Preserve
  atomic classification and exact comparison.
- **Authority:** [Exchange Rules: Validation boundary](exchange-rules.md#validation-boundary).

### D-013 — Aggregate through one FIFO sequencer before partitioning

- **Status:** Intended architecture for the current phase; the run-global sequence is adopted behavior.
- **Decision:** All admitted commands enter one process-local FIFO sequence owner. Stable instrument
  routing and parallel matching occur after authoritative journal order.
- **Why:** Earlier topic/shard counters could reuse values and let hashes or thread scheduling affect
  ordering. One owner is simple to test while durable sequencing is built.
- **Tradeoffs:** Sequence assignment is serialized and FIFO is not a sophisticated fairness policy.
- **Rejected alternative:** Several hash-selected sequencers with independent counters.
- **Revisit when:** Measured capacity requires another design that still provides the adopted
  run-global durable sequence.
- **Authority:** [Architecture: Sequencer](architecture.md#sequencer).

### D-014 — Retain one failed handoff and retry before later work

- **Status:** Component design.
- **Decision:** At bounded asynchronous boundaries, the producer retains a failed handoff and retries
  it before taking later work.
- **Why:** Dropping loses work, reading later items permits overtaking, and resequencing a retry
  duplicates identity. One explicit pending owner preserves lifetime and FIFO order.
- **Tradeoffs:** A blocked downstream path stalls later work and increases tail latency. Shutdown must
  account for the pending item.
- **Rejected alternatives:** Log and drop; dequeue around the blocked item; assign another sequence on
  retry; expose raw pointers whose lifetime is unclear.
- **Revisit when:** A different protocol proves measurably better with equivalent ownership,
  no-loss, no-duplicate, and order guarantees.
- **Authority:** [Architecture: Backpressure and failure boundaries](architecture.md#backpressure-and-failure-boundaries).

### D-015 — Hand off complete correlated result batches with explicit ownership

- **Status:** Component design.
- **Decision:** Matching transfers one immutable result batch atomically. Shared ownership keeps that
  exact batch alive across asynchronous admission completion and delivery; unsafe correlation
  mismatches fail-stop.
- **Why:** Per-event enqueue can expose half an outcome and duplicate earlier events on retry.
  Sequence-only correlation is weaker than checking client, command ID, and sequence together.
- **Tradeoffs:** Shared ownership has allocation and reference-count costs. Fail-stop favors
  correctness over availability.
- **Rejected alternatives:** Raw pointer queue APIs; rebuilding results from mutable command objects;
  treating each event enqueue as independent completion.
- **Revisit when:** Profiling proves ownership overhead material. An arena or intrusive mechanism may
  replace `shared_ptr`, but not lifetime clarity or atomic outcome ownership.
- **Authority:** [Exchange Rules: Result capacity and event handoff](exchange-rules.md#result-capacity-and-event-handoff).

### D-016 — Keep normal hot-path work quiet and avoid unconditional sleeps

- **Status:** Engineering practice and component design.
- **Decision:** Expected per-message outcomes update cheap counters rather than synchronously log.
  Workers drain available work immediately and apply bounded yield plus capped adaptive sleep only
  while idle or backpressured.
- **Why:** Formatting, locks, I/O, and unconditional sleeps add latency and distort throughput. Separate
  counters for expected rejections and internal failures preserve signal.
- **Tradeoffs:** Counters have less forensic detail and are not transactionally coherent. Polling
  backoff still trades wake latency for idle CPU.
- **Rejected alternatives:** Per-message logging; fixed sleep every loop; busy-spin forever without a
  measured CPU budget.
- **Revisit when:** Profiling compares notifications, tracing, asynchronous logs, or different spin
  budgets under representative load.
- **Authority:** [Architecture: Observability](architecture.md#observability); repository history
  `844b057` and `c4b184b`.

### D-017 — Make the normalized command journal authoritative

- **Status:** Adopted rule and intended architecture.
- **Decision:** Append the normalized command in authoritative sequence order before matching.
  Databases, indexes, materialized results, and caches are derived and rebuildable.
- **Why:** One ordered source provides a clear crash boundary and deterministic replay. Treating
  several independently updated stores as equally authoritative would require distributed
  transactions and leave recovery deciding which copy won.
- **Tradeoffs:** Recovery must replay commands and rebuild derived stores. The initial local-only
  model does not protect an unarchived segment from physical SSD loss.
- **Rejected alternatives:** Use the multicast bus as a write-ahead log; make Redis or SQL the matching
  authority; synchronously coordinate several unrelated stores for every command.
- **Revisit when:** Replication or consensus is adopted with a precise new authority and failure
  model.
- **Authority:** [Exchange Rules: Authoritative journal, durability, and replay](exchange-rules.md#authoritative-journal-durability-and-replay).

### D-018 — Use explicit binary records, immutable segments, and strict validation

- **Status:** Superseded in physical layout by D-024. Explicit encoding, CRC32C, versions, bounded
  lengths, strict validation, and continuity remain adopted; segments and a manifest do not.
- **Decision:** Journal fields use a versioned binary encoding with fixed byte order, bounded lengths,
  CRC32C, rules/configuration versions, exact sequence continuity, immutable sealed segments, and a
  manifest. Raw C++ memory layout is never the format.
- **Why:** C++ object layout varies with compiler, padding, platform, and source revision. Framing and
  checksums detect torn or corrupted records. Strict continuity prevents damaged input from silently
  changing every later book state.
- **Tradeoffs:** Codecs and compatibility tests add work. Unknown versions or middle corruption stop
  recovery instead of maximizing availability.
- **Rejected alternatives:** Raw struct dumps; newline JSON as the authoritative format; skipping a
  bad record and replaying later commands.
- **Revisit when:** A new format or checksum is justified. Preserve version dispatch and old replay
  support while those records remain retained.
- **Authority:** Historical physical-layout rule superseded by D-024. Current framing requirements
  are in [Exchange Rules: Journal record and file rules](exchange-rules.md#journal-record-and-file-rules).

### D-019 — Begin with one durable synchronization per command

- **Status:** Partially superseded by D-024. One durable synchronization per command remains adopted;
  a predesigned group-capable writer interface is no longer required.
- **Decision:** Design an ordered group-capable writer but configure one command and zero batching
  delay initially. Nothing is acknowledged before its containing group is durable.
- **Why:** Per-command synchronization makes the first commitment boundary easy to reason about.
  Designing the API around batches avoids an overhaul if measurements later justify group commit.
- **Tradeoffs:** One `fsync`/`fdatasync` per command can severely limit durable throughput. Future
  grouping adds queue delay and makes the whole unacknowledged group uncertain after sync failure.
- **Rejected alternatives:** Acknowledge before sync; accidentally depend on operating-system page
  cache; adopt a large batch without latency measurements.
- **Revisit when:** Controlled durable-throughput and latency evidence supports explicit batch values.
- **Authority:** [Exchange Rules: Authoritative journal, durability, and replay](exchange-rules.md#authoritative-journal-durability-and-replay).

### D-020 — Separate RAM capacity from historical retention

- **Status:** Superseded by D-024 because the product no longer promises indefinitely growing
  history or permanent command-key protection.
- **Decision:** Use distinct bounded in-flight RAM, a bounded recent-result cache, a disk-backed
  derived identity index, authoritative journal segments, online durable results, and archive.
  Results stay online for at least 30 days; command-key reuse remains protected indefinitely.
- **Why:** A single 100,000-entry in-memory history turns a cache bound into a lifetime exchange
  limit. Tiering keeps RAM bounded without forgetting retransmissions. The index accelerates lookup
  but can be rebuilt from authoritative records.
- **Tradeoffs:** Cache misses are slower. The identity namespace grows indefinitely. Archival and
  locator updates add operational complexity.
- **Rejected alternatives:** Never evict completed RAM entries; delete identity after a day; use a
  hash-only record; overwrite old journal records when the cache fills.
- **Eviction meaning:** Remove only the disposable RAM copy after durable discoverability. Never
  remove the journal record, durable identity, stored result, or archive copy.
- **Revisit when:** Measurements determine cache sizes, SSD watermarks, index strategy, segment
  retention, and archive service levels. SQLite is a reasonable derived-index recommendation, not an
  adopted backend.
- **Authority:** Historical retention rule superseded by D-024. Current behavior is in
  [Exchange Rules: Run capacity and retained storage](exchange-rules.md#run-capacity-and-retained-storage).

### D-021 — Fail closed on uncertain state and recover before becoming ready

- **Status:** Partially superseded by D-024. Fail-closed lifecycle, strict corruption handling,
  output-suppressed replay, and invariant validation remain adopted. Manifest, segment, snapshot,
  and fixed recovery-objective requirements do not.
- **Decision:** Startup progresses through `Starting → Recovering → Ready`. Validate versions,
  manifest, segments, snapshots, sequence continuity, replay, derived indexes, and invariants while
  suppressing external publication. Only an incomplete physical tail record is automatically
  truncatable.
- **Why:** Continuing after a checksum failure, sequence gap, escaped worker exception, uncertain
  sync, or possible partial mutation can manufacture later trades from corrupted state. Replaying
  without suppressing output can make historical events look new.
- **Tradeoffs:** Correctness wins over availability. Snapshot and old-version replay code increase
  complexity. A provisional recovery-time objective cannot weaken validation.
- **Rejected alternatives:** Skip a corrupt middle record; reset to default configuration; accept FIX
  while some partitions are rebuilding; catch every exception and continue blindly.
- **Revisit when:** Recovery benchmarks determine snapshot cadence or a replicated recovery model
  changes the failure boundary.
- **Authority:** [Exchange Rules: Corruption and recovery](exchange-rules.md#corruption-and-recovery).

### D-022 — Separate authoritative events, private views, and public market data

- **Status:** Adopted rule and intended architecture.
- **Decision:** Matching produces complete internal events. Client delivery creates private,
  counterparty-safe views; public trades and quotes belong to a separate publisher. Initially an
  identical retransmission retrieves the original result.
- **Why:** Broadcasting one internal representation can leak counterparty information and couples the
  matcher to FIX or market-data formats. Retransmission already has an idempotency key and avoids
  inventing reconnect replay before delivery tracking exists.
- **Tradeoffs:** Adapters and routing state increase. Clients initially need to retransmit after a
  disconnect, and results cease to exist after their retained run is explicitly deleted.
- **Rejected alternatives:** Let matching format FIX; expose authoritative `Trade` objects publicly;
  treat multicast forwarding as proof of private delivery.
- **Revisit when:** A reconnect replay, result-history query, or recoverable public feed is adopted.
- **Authority:** [Exchange Rules: Event visibility and result redelivery](exchange-rules.md#event-visibility-and-result-redelivery).

### D-023 — Separate deterministic correctness tests from performance benchmarks

- **Status:** Engineering practice.
- **Decision:** Behavioral tests assert exact state, identifiers, event order, saturation, and failure
  behavior. A deadline-based loop check is called a smoke test. Performance claims require a separate
  controlled harness and must state the durability policy.
- **Why:** Short timed unit tests are noisy and can reward incomplete or weaker durability. Exact tests
  catch overtaking, duplication, partial mutation, ownership, and correlation defects reliably.
- **Tradeoffs:** Correctness tests do not answer how fast the system is. A credible benchmark suite
  needs more setup and statistical care.
- **Rejected alternatives:** Print operations per second from a unit test and treat it as a supported
  throughput result; mix long nondeterministic timing gates into the deterministic suite.
- **Revisit when:** A reproducible optimized benchmark environment exists. The evidence standard
  remains.
- **Authority:** Contributor guidance and [Implementation Status](implementation-status.md).

### D-024 — Bound durability and identity by an explicit exchange run

- **Status:** Adopted rule and intended architecture. Supersedes the lifetime-global identity,
  indefinite retention, archive-tier, mandatory segmentation, mandatory snapshot, and provisional
  recovery-objective portions of D-010 and D-018 through D-021.
- **Decision:** Model the product as independently identified bounded `ExchangeRun` instances. Keep
  one authoritative journal file per run, synchronize each command before processing, replay the
  bounded journal from empty state, retain the active run plus at most one stopped run, and qualify
  command, order, event, and client retry identities with a durably allocated monotonic `uint64_t`
  `ExchangeRunId` that is never reused by one installation.
- **Why:** The product is an interactive deterministic exchange laboratory, not a permanent venue or
  regulatory archive. A supported finite run makes full replay, exact deduplication, visibility, and
  fault demonstrations tractable on ordinary laptops. It also removes permanent indexes, archive
  operations, snapshot machinery, and unbounded namespaces that users would neither see nor learn
  from.
- **Tradeoffs:** Deleted runs cannot be recovered or queried. A run has a visible hard capacity and
  must stop instead of accepting work forever. Every API and transport needs run binding, and
  exported identities need a fully qualified representation. Full replay cost grows to the maximum
  supported run size.
- **Rejected alternatives:** Keep lifetime-global numeric identifiers while deleting their history;
  archive every result forever; silently recycle a numeric `OrderId` across an unqualified FIX
  connection; implement segmentation, snapshots, SQLite, and group commit before measurements show a
  need.
- **Revisit when:** The product must retain more stopped runs, a measured maximum-capacity replay is
  too slow, run journals exceed safe file limits, or external interoperability cannot safely carry
  run-qualified identity. Any optimization must preserve the run boundary and exact replay behavior
  unless a new product requirement explicitly replaces them.
- **Authority:** [Exchange Rules: Product scope and exchange runs](exchange-rules.md#product-scope-and-exchange-runs),
  [run capacity and retained storage](exchange-rules.md#run-capacity-and-retained-storage), and
  [Architecture: Exchange-run controller](architecture.md#exchange-run-controller).

### D-025 — Separate process shutdown from explicitly ending an exchange run

- **Status:** Adopted rule and intended architecture.
- **Decision:** Clean shutdown and crash recovery leave the active run paused; only an explicit stop,
  replacement, or confirmed destructive reset ends its lifecycle or deletes its guarantees. The run
  controller is the sole lifecycle state owner, backed by crash-safe catalog metadata.
- **Why:** Process lifetime is an operational detail, while a run is the user-visible deterministic
  scenario. Automatically creating a new identity or deleting state whenever the executable exits
  would make crash recovery indistinguishable from starting over and would surprise a user who only
  closed the showcase. Recovery into `Paused` prevents generated or external clients from trading
  before the recovered state is visible and intentionally resumed.
- **Tradeoffs:** The catalog must persist active/stopped selection and capacity disposition, and the
  control plane needs explicit pause, resume, stop, recover, and reset operations. Users see one more
  lifecycle choice instead of every launch starting immediately.
- **Rejected alternatives:** Equate a process with a run; automatically stop or delete on clean exit;
  recover directly into open admission; let recovery, gateway, and journal threads publish their own
  lifecycle states.
- **Revisit when:** Usability evidence shows that automatic throwaway runs are preferable. Even then,
  destructive deletion must remain explicit and a crash must not masquerade as a clean new run.
- **Authority:** [Exchange Rules: Product scope and exchange runs](exchange-rules.md#product-scope-and-exchange-runs)
  and [Architecture: Exchange-run controller](architecture.md#exchange-run-controller).

### D-026 — Freeze a minimal portable journal V1 before implementing durability

- **Status:** Adopted rule and intended architecture.
- **Decision:** Journal V1 uses an exact little-endian binary frame, fixed type codes, bounded lengths,
  canonical command payloads, and a precisely defined CRC32C. The run header is the first checksummed
  frame and its bytes count toward run capacity.
- **Why:** “Versioned binary” is not a format. Leaving byte order, widths, enum values, checksum
  coverage, or capacity accounting to an implementation agent would create incompatible journals and
  make corruption tests depend on accidental C++ layout. A small format is easier to inspect and
  preserve than a generic serialization framework.
- **Tradeoffs:** Adding a field requires a new supported format or payload version rather than silently
  appending bytes. The codec and golden-byte tests add deliberate work before the writer exists.
- **Rejected alternatives:** Dump C++ structs; use implementation-defined enum values; adopt a schema
  framework before the command set needs it; describe only minimum fields and let the first writer
  choose the rest.
- **Revisit when:** A new command cannot be represented, exported interoperability requires another
  encoding, or measurements show codec cost is material. Old V1 journals remain readable while they
  are retained.
- **Authority:** [Exchange Rules: Journal record and file rules](exchange-rules.md#journal-record-and-file-rules).

### D-027 — Materialize one retained stopped run as a bounded read-only view

- **Status:** Intended architecture implementing adopted retained-run lookup behavior.
- **Decision:** Keep the live admission/result table for the active run and lazily reconstruct at most
  one separate immutable stopped-run view from its journal. Validate capacity for both worst-case
  views on the reference laptop.
- **Why:** Retained stopped-run lookup is a product promise, but stopped history must never re-enter
  live admission or mutate books. The one-run retention bound makes a second read-only materialization
  predictable and avoids a permanent database or repeated full replay for every UI query.
- **Tradeoffs:** Opening the stopped run has replay latency and worst-case memory approaches two run
  views. Retention replacement must invalidate the cached view safely.
- **Rejected alternatives:** Pretend the active table can answer stopped-run requests; scan and replay
  the journal for every lookup; create an unbounded multi-run cache; add a permanent result database
  before the supported capacity is measured.
- **Revisit when:** The product retains more than one stopped run, the two-view memory budget is too
  large, or measured lookup latency justifies a derived disk index.
- **Authority:** [Exchange Rules: Command identity and result retention](exchange-rules.md#command-identity-and-result-retention)
  and [Architecture: Command admission and retransmission](architecture.md#command-admission-and-retransmission).

## Part II — Superseded designs and what they taught us

### S-001 — Per-topic and per-shard sequencers

- **Status:** Superseded by D-010 and D-013.
- **Earlier idea:** Hash symbols or topics across sequencers with independent counters to maximize
  locality and parallel matching.
- **Why it looked good:** Independent instruments really are concurrency units, and avoiding shared
  state can improve locality.
- **Why it failed the larger design:** Counters were not globally unique; implementation-defined
  hashing and transport metadata influenced ownership; restart had no durable authority; and several
  ingress paths could not establish one deterministic boundary.
- **Lesson retained:** Partition instruments after durable global sequencing using persisted
  `InstrumentId`. The early scalability observation was useful; its placement was wrong.
- **Historical sources:** `archive/sequencer.md`, `archive/scattered_ideas.md`, and commit `844b057`.

### S-002 — Port/topic queues as authoritative result delivery

- **Status:** Superseded by D-015 and D-022.
- **Earlier idea:** Matching publishes to topic queues polled by FIX ports, moving routing work away
  from the matcher.
- **Why it looked good:** It reduced direct fan-out and fit the original shared-memory port design.
- **Why it failed the larger design:** Topic metadata did not guarantee exact
  client/command/sequence correlation. A multicast write could fail independently, and per-event
  output could expose a partial result.
- **Lesson retained:** Multicast can still serve non-authoritative consumers, but completion and
  private delivery require an owned, acknowledged result path.
- **Historical sources:** `archive/fixServers.md`, `archive/exchange.md`, and commit `20ca20b`.

### S-003 — Bus JSON or several unrelated stores as write-ahead authority

- **Status:** Superseded by D-017 and D-018.
- **Earlier idea:** Reuse bus serialization and/or write to a file, external database, and Redis.
- **Why it looked good:** Existing components and familiar stores appeared to provide persistence and
  redundancy quickly.
- **Why it failed the larger design:** The bus's delivery responsibility is not commit ordering. JSON
  fields did not define a stable replay schema. Several stores lacked one atomic authoritative
  durability point.
- **Lesson retained:** Derived databases, caches, and archives are useful when their authority and
  rebuild path are explicit.
- **Historical sources:** `archive/writeAhead.md`, `archive/matchingEngine.md`, and commit `c4b184b`.

## Part III — Transferable engineering lessons

### L-001 — Identity, ordering, state, and delivery are different problems

It is tempting to use one number or one queue for everything. This project separated:

- `(ExchangeRunId, ClientId, ClientCommandId)` — client retry identity;
- `CommandSequence` — authoritative processing position;
- `OrderId` — exchange identity of a created order;
- `EventId` — identity and order within a command's result;
- FIX session identifiers — transport state;
- journal position — physical durable location;
- delivery acknowledgement — knowledge that a consumer received a result.

Some values can share a numeric source, as `OrderId` does with `CommandSequence`, while remaining
different types and responsibilities. This separation prevents a reconnect, retry, reserialization,
or delivery failure from accidentally becoming a new business action.

**General lesson:** Before choosing an ID, write down its uniqueness scope, allocator, lifetime,
restart behavior, comparison rule, and whether it is business identity or transport metadata.

### L-002 — The commit point changes the meaning of every failure

The same exception means different things depending on where it occurs:

```text
Before sequence and journal durability
  → no business command committed; reservation may be safely released if invariants hold

After durable journal append but before matching
  → command exists and must be replayed; it cannot be reported as never accepted

During or after matching mutation
  → state may be uncertain; stop and reconstruct from the journal

After result creation but before client delivery
  → business result exists; retry/redelivery must not rematch
```

**General lesson:** Draw the commit boundary before designing retries or exception handling. “Try
again” is safe only when you know whether the first attempt became authoritative.

### L-003 — A cache limit is not a history-retention limit

The original 100,000-record admission map mixed a flow-control number with an unspecified lifetime.
When full, it could neither evict safely nor explain whether the exchange run itself was complete.

One valid permanent-history pattern would be:

```text
small bounded RAM cache
  → durable searchable index
  → authoritative append-only history
  → verified archive
```

That pattern is unnecessary for the bounded showcase. The adopted alternative makes the journal and
admission history coextensive with one explicitly bounded run. Deleting the stopped run ends the
published guarantee rather than disguising history deletion as cache eviction.

**General lesson:** For every cache, ask, “What is the source of truth on a miss?” If there is no
answer, it is not merely a cache.

### L-004 — A journal and an index solve opposite access patterns

The journal is optimized for ordered append, crash recovery, and sequential replay. The index is
optimized for point lookup by client command key. Forcing either one to do both jobs compromises its
strength:

- a journal scan for every retransmission becomes slow as history grows;
- a mutable B-tree database as the sole replay authority complicates exact commit order and
  reconstruction;
- keeping both authoritative creates a multi-store atomicity problem.

The project therefore makes the journal authoritative. A derived index is optional and should be
introduced only when measured bounded-run lookup or recovery cost justifies it.

**General lesson:** Separate the authoritative write pattern from derived read models. This is the
same idea behind event sourcing, write-ahead logs, and materialized views, with important differences
in each system's guarantees.

### L-005 — `fsync` is a semantic boundary, not just an expensive syscall

Writing bytes usually places them in process or operating-system buffers. A successful durable sync
is the point at which the initial design permits an external commitment. This is why benchmarks must
state whether they include synchronization.

Local durability still has a threat model. It normally covers process and operating-system crashes
when the filesystem and device honor the call. It does not automatically cover SSD destruction,
machine loss, controller lies, or site disaster. Archive and replication answer different failure
questions.

**General lesson:** Never say “persisted” without naming the acknowledged boundary and the failures it
survives.

### L-006 — Checksums detect corruption; they do not repair it

CRC32C can show that a record is not the bytes that were written, but it cannot identify the correct
replacement. Sequence continuity detects missing, duplicated, or reordered positions, but it cannot
invent the missing command. Consequently, middle corruption is a recovery failure, while an
incomplete final record can be recognized as an interrupted append and truncated.

**General lesson:** Detection, correction, replication, backup, and recovery policy are separate
capabilities. Do not imply one because another exists.

### L-007 — Single-writer ownership is often more valuable than clever lock-free sharing

One logical owner per book and one sequence owner simplify invariants, cache locality, and replay.
Lock-free containers can safely move values between owners, but they do not decide business order or
make shared mutable state deterministic.

The most useful concurrency question is not “How many threads can touch this?” but “Which state can
be partitioned so each part still has one owner?” Instruments are natural partitions; one hot
instrument is not.

**General lesson:** Use concurrency between independent state domains. Treat concurrent writers to
one invariant-rich structure as a last resort justified by measurements.

### L-008 — Queue correctness includes ownership outside the queue

A bounded `push` returning false creates a lifetime question: who owns the item now? Retaining one
pending item in the producer answers it. Retrying that exact object before later work preserves
order. The queue implementation alone does not provide these semantics.

For any asynchronous boundary, document:

- producer and consumer count;
- object ownership before and after a successful handoff;
- behavior on full, shutdown, and consumer failure;
- ordering and retry rules;
- whether the item is already committed;
- capacity metrics and client-visible consequences.

**General lesson:** A queue is a data structure. The protocol around it is the system design.

### L-009 — Low latency usually begins by removing work

The useful optimizations so far were conceptually simple:

- remove wall-clock reads and generated IDs from normalization;
- remove hashes that were not authoritative;
- remove unused message fields and serialization;
- remove synchronous normal-path logs;
- drain active work without unconditional sleeping;
- retain values rather than reconstruct or resequence them.

These changes reduce instructions, allocations, cache footprint, locks, and syscalls without
weakening behavior. More exotic changes should follow profiling.

**General lesson:** Before introducing specialized data structures, ask what work, metadata,
conversion, allocation, logging, or synchronization can be deleted entirely.

### L-010 — Busy-spin, sleep, and notification are workload tradeoffs

Busy-spinning minimizes wake latency but burns a CPU even when idle. Sleeping saves CPU but adds
scheduler latency. Notifications avoid idle polling but require synchronization and can suffer
lost-wakeup or contention bugs if designed poorly. The adaptive yield/sleep policy is a reasonable
unmeasured middle ground, not proof of low latency.

**General lesson:** Choose a waiting strategy from measured duty cycle, latency budget, core budget,
and deployment environment. A hard-coded spin count is a hypothesis to benchmark, not a universal
constant.

### L-011 — Strong types turn review comments into compiler errors

Several important values are all 64-bit integers. Without strong types, passing a client ID where an
order ID belongs compiles and may survive shallow tests. Strong wrappers make invalid combinations
harder to express and force conversions to be deliberate.

Strong types do not replace runtime validation: zero, overflow, unknown IDs, and configuration
version still need checks. They narrow the space of possible bugs.

**General lesson:** Use types for semantic distinctions and validation for value constraints.

### L-012 — Exact arithmetic and preflight prevent rollback-heavy designs

Integer ticks avoid rounding ambiguity. Checked addition/subtraction prevents wraparound. Exact event
preplanning ensures allocations and result capacity before mutation. Together they make “no partial
business state on rejection” practical without implementing a general rollback transaction inside
the matcher.

**General lesson:** If rollback is complicated, see whether validation, capacity reservation, and
checked arithmetic can prove the operation safe before mutation.

### L-013 — Business rejection and system failure must not be conflated

A malformed price, inactive cancel target, or duplicate command conflict is an expected outcome with
a defined response. A sequence contradiction, impossible internal correlation, corrupted journal,
or uncertain partial mutation means the system cannot trust itself.

Turning an invariant failure into a client rejection hides corruption and lets later commands build
on it. Crashing uncontrolled also loses diagnostic ownership. The intended response is explicit
fail-stop state followed by recovery.

**General lesson:** Define an error taxonomy before adding catch blocks. Recovery-worthy faults must
not become ordinary business outcomes.

### L-014 — Recovery is a normal lifecycle, not a startup helper

Recovery needs versions, exclusive ownership, progress state, full deterministic replay, admission
and result reconstruction, output suppression, and invariant checks. It determines whether the
service is allowed to accept work, so `Ready` is a correctness state rather than “the process is
running.” Optional snapshots or indexes may optimize replay but do not become truth.

**General lesson:** Design recovery alongside the write path. Every new durable field or business
rule creates a replay obligation.

### L-015 — Deterministic tests and benchmarks answer different questions

A deterministic test asks, “Is the exact result correct under this state and failure condition?” A
benchmark asks, “How does a defined build behave on controlled hardware under a specified workload
and durability policy?” A smoke test asks only, “Does representative work make progress before a
generous deadline?”

Mixing these creates flaky correctness gates and unsupported performance claims.

**General lesson:** Name a test by the evidence it can actually provide.

### L-016 — Security identity is not authentication

Mapping a configured FIX identity to `ClientId` stabilizes ownership and retransmission semantics.
It does not prove who controls the network connection, encrypt traffic, rotate credentials, or stop
an attacker with configuration access.

**General lesson:** Separate business identity, authentication, authorization, transport security,
and audit. Solving one does not silently solve the others.

### L-017 — Historical alternatives are valuable when their useful insight is preserved

The early sharded sequencer idea was not simply “bad.” It correctly noticed that independent
instruments enable parallelism and cache locality. The mistake was allowing that partitioning to
define command identity and sequencing too early. The revised design retains the insight after a
global durable boundary.

**General lesson:** When superseding a design, identify which assumption failed and which insight
survives. This produces better architecture than deleting history and swinging to the opposite
extreme.

### L-018 — C and C++ macros ignore ownership boundaries

A compatibility macro introduced for QuickFIX once reached standard iostream and memory headers and
caused compilation failures. A macro is lexical substitution, not a namespaced symbol; it can rewrite
tokens in every later include in the translation unit.

Useful containment techniques include:

- isolate compatibility workarounds in the smallest possible boundary;
- include unaffected standard headers before defining the macro when that is the required workaround;
- `#undef` the macro immediately after the target include;
- prefer wrapper functions, typed constants, or build-system definitions scoped to one target when
  possible; and
- compile the compatibility boundary independently so leakage is caught quickly.

**General lesson:** The preprocessor operates before C++ type and namespace rules. Treat every macro
as global mutable lexical state within its translation unit.

### L-019 — A successful image build is not a successful runtime

The sequencer image initially compiled but an in-image identity check failed because
`/app/exchange.cfg` was absent. Adding the runtime configuration to the image fixed a packaging
contract, not a matching algorithm.

A useful deployment verification ladder is:

```text
source compiles
  → target links
  → image builds
  → required files and permissions exist in the image
  → process starts with production-like arguments
  → readiness condition is reached
  → relevant end-to-end behavior succeeds
```

**General lesson:** Build correctness, packaging correctness, startup correctness, readiness, and
business-path correctness are different claims and need different checks.

### L-020 — Message fields are part of the performance and authority model

Unused identifiers, ports, topics, timestamps, nested order copies, and sequence metadata were not
free just because assignments were cheap. They increased object size, copying, cache traffic, and the
chance that later code would mistake transport metadata for authority. Removing unused fields also
made the real command contract easier to review.

Not every potentially useful future field belongs in today's hot message. Future partition metadata
can remain explicitly non-authoritative until routing actually needs it. Durable formats can evolve
through versions rather than carrying speculative fields forever.

**General lesson:** For each field crossing a hot boundary, ask who writes it, who reads it, whether it
is authoritative, whether replay needs it, and what goes wrong if it is stale. Delete fields without
a concrete owner and consumer.

### L-021 — Atomic counters are not atomic system snapshots

Relaxed atomic increments are appropriate for independent diagnostic counters when no business state
depends on their ordering. They avoid data races and provide monotonic per-counter observations with
minimal synchronization.

They do not make several counters a coherent snapshot. A reader may observe one counter before an
operation and another after it. Nor does a relaxed counter publish associated command or queue state.
Use locks, immutable snapshots, or a designed acquire/release relationship when cross-field
consistency matters.

**General lesson:** Choose memory ordering from the synchronization guarantee required, not from the
fact that a value happens to be atomic. Diagnostics and coordination are different uses.

### L-022 — Lock-free describes progress mechanics, not the whole protocol

A lock-free queue can prevent a stalled thread from blocking all data-structure progress, but it does
not automatically provide:

- the required producer/consumer topology;
- business FIFO across several upstream sources;
- durable delivery;
- ownership after a failed `push`;
- notification or efficient idle waiting;
- shutdown and drain semantics; or
- safety for pointers whose pointees may disappear.

Even methods such as `empty()` are often only advisory under concurrency; another thread can change
the queue immediately after the observation.

**General lesson:** “Lock-free” is one implementation property. Review the surrounding ownership,
ordering, lifetime, capacity, and waiting protocol separately.

### L-023 — Allocation strategy follows bounded result planning

Exact event preplanning makes it possible to reserve a contiguous result vector once, construct the
known number of events, and transfer the complete batch without repeated growth. This improves both
correctness and likely locality.

But `reserve` is not automatically a performance win everywhere. Reserving the maximum 4,096 events
for every one-event result would waste memory and cache. Shared ownership may allocate a control block,
and polymorphic or arena allocation adds lifetime constraints. These choices need profiles that
include realistic event-count distributions.

**General lesson:** Bounds enable deliberate allocation. Reserve the expected exact amount when it is
known; otherwise measure the distribution before trading memory footprint for fewer reallocations.

### L-024 — End-to-end latency is a stack of different costs

“The matching engine is fast” says little about client-observed durable latency. A useful breakdown is:

```text
network and FIX parsing
  + normalization and admission contention
  + queueing before sequence
  + journal encoding and durable sync
  + routing and matching
  + result materialization and durable lookup update
  + private protocol formatting and delivery
```

Each term responds to different techniques. Data layout may improve matching, while group commit
changes persistence throughput and queue delay. Busy-spin changes scheduling latency and CPU use.
Batching can improve throughput while making tail latency worse.

**General lesson:** Measure component service time, queueing time, and end-to-end percentiles
separately. Optimize the dominant term under the actual durability and workload contract.

## Part IV — A review checklist for future work

Before accepting a new slice, ask:

### Behavior and identity

- Is this behavior adopted, recommended, or unresolved?
- Which identity scopes are involved, and can any be reused after restart?
- Does a retry retrieve prior work or accidentally create new business work?
- Are protocol metadata and business identity still separated?

### State and concurrency

- Who is the single logical owner of each mutable invariant?
- Can two threads mutate the same book, index entry, sequence, or delivery state?
- Does a queue merely move data, or has its ownership protocol been specified too?
- What remains owned when a handoff fails?

### Failure and durability

- What is the commitment point?
- What happens if the process fails immediately before and after each step?
- Which store is authoritative, and how are all other stores rebuilt?
- Does an exception represent expected input, safe pre-commit failure, or uncertain state?
- Can recovery distinguish an incomplete tail from corruption?

### Capacity and performance

- Is every resource bounded, and what happens at the bound?
- Is a number a cache bound, flow-control bound, business rule, or storage-retention limit?
- Which work was removed before adding complexity?
- Was the suspected bottleneck profiled in an optimized representative build?
- Does the measurement include durability, saturation, and tail latency?

### Testing and documentation

- Is there a deterministic test for the behavior and each boundary condition?
- Are failure, retry, capacity-one, overflow, and restart cases covered where applicable?
- Does implementation status state only what was actually verified?
- If the “why” or tradeoff changed, was this document updated without erasing the old rationale?

## Part V — Exercises for learning the system deeply

These are useful rereading or review exercises for the project owners:

1. Trace one NewOrder from FIX text through normalization, admission, sequencing, durability,
   matching, result storage, and private delivery. Name the owner and identity at every boundary.
2. Repeat the trace while crashing immediately before and after journal synchronization. Explain
   whether the client should retry and what recovery sees.
3. Fill every bounded queue with capacity one in a thought experiment. Identify the pending owner and
   prove that no item is lost, duplicated, or overtaken.
4. Construct two commands with the same client key and different business fields. Explain why a
   fingerprint cannot be the final equality test.
5. Calculate journal growth from assumed commands per second and average encoded record size. Use it
   to choose `MaxRunCommands`, `MaxRunJournalBytes`, memory budget, and the stop-admission watermark.
6. Compare per-command sync with group sizes of 8, 64, and a 100-microsecond delay. State the expected
   throughput benefit, added latency, and uncertain-failure set before benchmarking.
7. Corrupt an imagined record at the tail and in the middle. Explain why only one case can be
   automatically truncated.
8. Explain when a bounded-run journal scan is sufficient and what measurement would justify adding a
   derived index without making it a second source of truth.
9. Design a benchmark that separates normalization, non-durable matching, durable command latency,
   and end-to-end client result latency.
10. Pick one superseded design and state the useful insight that the current architecture retained.
