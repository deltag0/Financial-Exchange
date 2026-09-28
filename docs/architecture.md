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

## Showcase process boundary

The initial end-state deployment is one C++ exchange application process plus the browser. The same
process hosts the application event gateway, exchange-run controller, deterministic command path,
and optional FIX adapter. Built browser assets are static inputs to the C++ application gateway; a
Node backend and a separate Nginx frontend service are not part of the target runtime. This keeps
browser delivery on the same side of the process boundary as the existing projections and JSON
serializers and avoids a second command, control, and result protocol between local services.

The process assigns each responsibility to one component:

| Responsibility | Owner |
|---|---|
| HTTPS termination and static browser assets | C++ application gateway |
| Configured participant authentication, secure session cookies, and the session-to-`ClientId` binding | C++ application gateway identity/session state |
| WebSocket upgrade, connection state, and connection-to-`ExchangeRunId` binding | C++ application gateway |
| Browser order ingress and browser-protocol validation | C++ application gateway; it derives `ClientId` from the authenticated session and submits one normalized command to the controller boundary |
| Run creation, recovery, resume, pause, stop, retained-run lookup, and every lifecycle state change | `ExchangeRunController`; participant sessions expose none of these operations until a separate authorization policy is adopted |
| Public and participant-private JSON serialization | The pure C++ application-event serializers, called only by the application gateway after it obtains a projected value |
| Public broadcast and recipient-private routing | C++ application gateway, using the connection's run binding and server-side participant identity |
| Exchange-side result-handoff capacity | Controller-owned private and public queues |
| Post-pop routing state, bounded serialized-byte buffers, disconnect cleanup, and socket writes | C++ application gateway |
| Startup and coordinated shutdown order | C++ application composition root; it coordinates owners but does not duplicate controller or gateway state |

The command path crosses no process boundary: the C++ gateway validates and normalizes a participant
request, then submits it to controller-owned admission and command processing. Browser lifecycle
control remains unavailable until a separate authorization policy is adopted; only the controller
changes lifecycle state. On the result path, the controller owns each private or public handoff item
until the gateway successfully pops it, after which the gateway serializes and routes it. The optional
FIX adapter remains a separate in-process protocol adapter.

There is no application-gateway IPC contract in this topology. Introducing a separate browser
gateway process later requires a separate adopted architecture decision. Gateway ownership and
lifecycle composition are defined under
[Application event gateway](#application-event-gateway); externally observable authentication,
delivery, backpressure, disconnect, and shutdown behavior remains authoritative in
[Exchange Rules: Browser application event delivery](exchange-rules.md#browser-application-event-delivery).

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
  |                                  |
  v                                  v
Private result handoff       Public-trade handoff
  |                                  |
  +----------------+-----------------+
                   |
                   v
        Application event gateway
                   |
                   v
      Authenticated browser WebSocket
```

Commands never travel backward through this path. Results are represented as events rather than by
mutating and returning the original command. The journal is authoritative for recovery; replay sends
its commands through the same matching path to regenerate events.

The intended showcase flow is: create or resume a bounded run, optionally start its deterministic
generated participants, observe a clearly labelled educational projection, submit and cancel orders
through the same normalized command path, pause or resume the run, and explicitly stop, replay, or
reset it. UI and operator controls use a control plane around the exchange-run controller; they do
not call the matcher directly. Participant-private output, public market data, and the
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
the recovered run paused until an explicit resume. `CapacityReached`, `FailStopped`, and `Stopped` are terminal for
new business submissions. Recovery components validate and report outcomes; they do not
independently publish lifecycle transitions.

Normal existing-run startup selects only the canonical catalog's active `Open` or `Paused` run
under exclusive catalog and journal ownership. Both startup operations refuse when the controller
already owns a run; neither silently replaces those owners. Normal startup verifies the selected run
ID on the descriptor being recovered before any journal repair or writer transfer. After
reconstruction, the controller persists `Paused` before exposing read-only matching state and
completed-result lookup; it does not
open admission. Other dispositions require explicit operations, and neither `CapacityReached` nor
`FailStopped` is downgraded by normal startup. Catalog replacement uncertainty leaves runtime state unavailable even
if a canonical reload observes the proposed paused snapshot. Recovery failure reports
`RecoveryFailed` and preserves evidence rather than selecting or creating another run.

Explicit new-run startup refuses before reservation or journal creation if the controller already
owns a run or the canonical catalog selects an active run. Otherwise, it composes the existing
new-run preparation, empty-state construction, and durable activation boundaries, transferring one
`RunStateV1` into controller ownership and reporting `Ready` only after committed `Open` activation.
Refusal preserves existing owners and evidence; failures retain the exact underlying stage details
and never expose the proposed run as ready. Reserved IDs and journal evidence are not rolled back
after a later failure. This operation does not replace, stop, or resume another run.
Invalid or unreadable preflight catalogs stop new-run startup immediately with the captured load
result, without entering preparation or retrying that read. Missing-catalog journal-evidence checks
remain delegated to preparation.

Both startup operations resolve the catalog binding to an absolute path before their first read and
use that same path throughout startup and later resume, so working-directory changes cannot select
another installation. Explicit resume operates only on a controller-owned `Paused` run and that
captured binding. It revalidates the active run ID, persisted `Paused` disposition,
and remaining configured command/byte capacity against the same writer's immutable header and
committed position. Exact next-frame fit remains the writer's append check. Resume neither replays
nor reconstructs state: the journal writer, matching state, admission index, immutable completed
results, and next sequence remain owned in place. It checked-increments catalog generation and
persists `Open` before reporting `Ready`. Failure or uncertainty retains the owned paused state and
read-only lookup without reopening business admission, even if reload observes `Open`; that
observation is not proof of directory-entry durability. Other dispositions, especially
`CapacityReached`, are not downgraded. After committed `Open` activation or resume, the controller
opens the shared admission index before reporting `Ready`. Run-bound indexes start closed, including
those constructed by standalone startup composition or recovery. The unqualified executable index
remains an explicitly legacy, non-durable path that starts open without controller authority.

The index serializes gate transitions and reservation decisions with its existing mutex. It checks
existing records before gate availability, preserving identical in-flight/completed retransmissions
and distinguishable conflicts while closed. Closing the gate preserves reservations and immutable
completed results. Only the controller opens production run-bound admission; its explicit
`closeAdmission` operation closes new reservations without changing catalog disposition or performing
the adopted ordered pause/drain barrier. Deterministic worker installation is a separate composition
boundary from the controller's explicit single-threaded pause operation.

The controller may install one concrete command-processing path for its owned Ready or Paused run.
The installation borrows the existing writer, matching state, and admission index in place, without
moving owners, reconstructing books/results, or replaying commands. It does not open admission or
change startup/resume state. An explicit run ID accompanies each already normalized submission and
is checked before existing-record lookup. For an unseen key, the controller validates the prospective
journal command through the writer and existing codec, then combines its exact frame size with the
writer's committed position and one direct count/byte total for accepted but unappended commands.
The shared index still performs existing-history classification first and only then applies the
controller's capacity decision while atomically reserving and publishing to the bounded Sequencer
ingress. No reserve-then-abandon capacity path, second queue, or duplicate serialization-size formula
is used.

The controller owns ingress and matching command queues, the bounded result boundary, and a bounded
internal bus with no delivery readers. These precede the Sequencer, MatchingEngine, and
AdmissionCompletionConsumer in construction order; all workers are destroyed before the queues and
controller-owned run state. Installation is one-time, and startup cannot replace an owned run.
Submission, advancement, lifecycle calls, and inspection require one driving thread or external
quiescence. Mutable run access remains private; this composition exposes no mutable controller
getters and creates no background threads.

One explicit advancement cycle drains sequencing until backpressure, checks its append evidence,
drains matching until result backpressure, and completes one FIFO result through the existing
consumer. The Sequencer's pending command and matcher's pending immutable result keep their existing
retry-before-next-work semantics. Matching also retains a dequeued command if processing throws,
without retrying an uncertain mutation. The workers expose narrow nonblocking advancement methods used by
both this driver and their existing run loops. Normal gate closure prevents new submissions while
this Ready path can still advance accepted work; closure alone does not publish `Paused`.

When accepting a command exactly exhausts either run bound, or an unseen command cannot fit the
remaining projected capacity, the controller closes admission and marks a capacity transition
pending. The rejected command receives `RunCapacityReached` without a reservation, queue entry,
sequence, or append. Existing identical and conflicting keys remain classified from retained history.
The same driver drains all work accepted before the barrier, reconciles its direct pending totals
from the writer's committed count and bytes, and publishes catalog `CapacityReached` only after every
queue and pending slot is empty. Only a committed replacement changes runtime state to terminal
`CapacityReached`; pause and resume cannot reopen it.

For an installed Ready path, explicit pause closes admission first, then drives the same ordered
component chain until ingress, matching, result queue, and every worker pending slot are empty.
Only then does it replace the bound catalog's `Open` snapshot with a durably committed `Paused`
snapshot and report runtime `Paused`. Completed-result lookup remains available through the closed
index. Explicit resume durably restores `Open` before reopening the gate and uses the same run
owners, books, results, and next sequence. The caller serializes pause, submission, advancement,
inspection, and resume on one driving thread or under external quiescence.

Any non-committed append closes admission before matching or completion can run in that cycle. The
exact append result and candidate stay with the Sequencer; earlier committed matching-queue work and
later staged submissions remain owned. An unexpected matching or completion exception similarly
retains its exception and any in-progress command, pending immutable result, or completion batch.
The controller immediately makes runtime processing unavailable, validates that the bound catalog
still selects the same run in `Open` with an incrementable generation, and attempts one replacement
to `FailStopped`. Only a committed replacement reports terminal runtime `FailStopped`. Catalog load,
validation, generation, replacement failure, or replacement uncertainty leaves runtime
`Unavailable`, even if the uncertain replacement observes `FailStopped`; inspection retains the
load and replacement evidence beside the original processing failure. No advancement, append retry,
drain, replacement installation, pause, or resume clears the latch. Existing identical, completed,
and conflicting command lookup still uses the retained closed index, while unseen commands receive
`ExchangeRunUnavailable`. Normal startup refuses persisted `FailStopped`.

Explicit failed-run recovery accepts a catalog path and selects only the active run durably marked
`FailStopped` or `RecoveryFailed`. It resolves the catalog binding to an absolute normalized path and
validates the selected run, failed disposition, and incrementable generation before reconstruction.
This lets a fresh controller recover after restart without relying on retained process state. When
the controller still owns an installed runtime `FailStopped` path, the supplied catalog must match
its original binding and selected run; the operation copies the original processing inspection,
then destroys the borrowing path before releasing its writer, matching state, and admission index.
It uses the ordinary journal preparation, tail repair, replay, and admission reconstruction
composition against the authoritative journal and temporary owners. An immediate-write failure
contributes no frame, an incomplete final frame is repaired under the journal rule, and a complete
frame left by sync uncertainty is replayed. Queued commands without complete journal frames are not
reconstructed.

Successful reconstruction from either failed disposition is installed with admission closed only
after a checked catalog replacement commits `Paused`; the processing path is not reinstalled, and
explicit resume remains a separate operation. Validation, replay, or admission reconstruction
failure from `FailStopped` publishes `RecoveryFailed` before reporting that runtime state and exposes
no reconstructed run owners. A failed retry already durably in `RecoveryFailed` remains there without
a redundant catalog replacement. Any required catalog replacement failure or uncertainty leaves
runtime `Unavailable`, even if reload observes the proposed disposition, while the result retains
the original processing inspection when present, recovery stages, and replacement evidence.
Preflight refusal preserves installed failed owners and files. This composition does not wire the
executable/FIX startup, client delivery, or coordinated shutdown.

Explicit stop accepts only a controller-owned `Ready`, `Paused`, or `CapacityReached` run. It first
loads the controller's bound catalog and validates the selected run ID, the disposition corresponding
to the runtime state, sufficient generation space, and any existing retained stopped-run selection.
Replacing that selection requires exact confirmation of its `ExchangeRunId`; missing or mismatched
confirmation returns before admission, owners, journals, or catalog bytes change. A `Ready` stop
delegates its ordered barrier and FIFO drain to the ordinary pause operation, so its catalog advances
through `Paused` before the stop snapshot. `Paused` and `CapacityReached` already have closed
admission and require no processing drain.

The stop snapshot checked-increments the generation, clears the active run and disposition, and
selects the former active run as the one retained stopped run. Runtime reports `Stopped` only after
that replacement commits. Definite or uncertain replacement failure leaves runtime unavailable,
keeps both journals and all active owners, and returns the nested catalog evidence. After commitment,
the controller destroys the processing path before its borrowed writer, matching state, and admission
index. If an older stopped run was replaced, only then does it unlink that older journal and
synchronize the containing directory. Cleanup reports its own typed outcome and system error; a
cleanup failure does not undo or obscure the already committed stop. The newly retained journal is
never deleted by this operation. The same committed selection invalidates any cached view of the
replaced run before cleanup, including when cleanup later fails.

Pause drain or catalog
publication failure leaves admission closed, owned work and nested evidence retained, and runtime
unavailable; it never reports a successful pause, even if an uncertain catalog replacement is later
observed as `Paused`. Capacity drain or publication failure has the same fail-closed rule and never
reports runtime `CapacityReached`, including when an uncertain replacement reload observes it.

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
| `Ready` | Pause or clean shutdown requested | `Paused` | Close admission, drain every accepted reservation through completion, then commit the catalog |
| `Paused` | Explicit resume | `Ready` | Revalidate active binding and capacity before admission opens |
| `Ready` | A run limit is exhausted | `CapacityReached` | Reject unseen work before sequencing, close admission, drain accepted work, then commit the catalog before reporting terminal state |
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
- linearized reservation and publication to the gateway's bounded staging queue;
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
records for the active run. The table owns its single `ExchangeRunId` once and keys records by exact
`(ClientId, ClientCommandId)` within that run. Its required maximum size is bounded by
`MaxRunCommands`, and recovery reconstructs it and its exact original results from the run journal.
A fingerprint may accelerate a lookup, but exact canonical comparison determines retransmission
behavior.

Gateway producers use one concrete admission-and-staging operation under the index's existing
mutex. Existing-record lookup precedes gate and capacity checks and never stages retransmissions or
conflicts. A closed gate normally returns `ExchangeRunUnavailable`; the controller-backed path passes
its already computed capacity decision so a new key at the terminal capacity barrier instead returns
`RunCapacityReached`. For an available new key, record allocation succeeds before the nonblocking
fixed-size queue push; payload copying cannot throw. A full staging queue returns `GatewayBusy` and
erases the unexposed reservation before mutex release, without incrementing accepted-first-submission
statistics. Successful admission leaves one record and one staged command. Gate closure linearizes
before or after both effects; it cannot split them. This mutex boundary performs no waiting for queue
space, journal I/O, or matching and is not a pause/drain barrier.

The retained stopped run never enters live admission. The controller's trusted read-only result
operation takes an explicit catalog path, `ExchangeRunId`, and normalized command. An owned
active-run lookup uses its existing admission index without replay. Otherwise, the operation loads
the named catalog and derives a journal path only after that catalog selects the requested ID as its
retained stopped run; active-but-unowned, unknown, replaced, and deleted IDs fail before journal
recovery. A participant-facing operation delegates to that same lookup, derives the recipient only
from the normalized command's `ClientId`, validates the returned run/client/command correlation, and
returns only the recipient-safe projection. It never returns the authoritative batch or `Trade`.

The first retained lookup validates and reconstructs the journal through the same recovery, replay,
matching-state, and admission composition used for active recovery. It closes the temporary recovered
writer and installs one private optional view containing only the reconstructed matching state and
closed completed-admission index. Cache hits reuse that view without replay. Recovery failure returns
the nested evidence and installs no partial view. The active owners and retained view may coexist, but
the view cannot reserve commands, append, install workers, publish replay events, or mutate books.
Supported capacity configuration must fit the worst-case active table and one maximum-capacity
stopped-run view concurrently on the reference laptop; the implementation may not make a promised
retained-run lookup fail merely because this documented memory was not budgeted.

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

The journal-backed sequencer borrows one caller-owned writer and an admission index bound to the
same run. The writer's immutable header supplies run/rules context and its exact next sequence
supplies the candidate; existing command codecs and writer checks govern the append. Admission is
bound as sequenced and matching handoff is permitted only after committed synchronization. One
pending slot retains committed work under matching-queue saturation, retrying only handoff before
consuming another command. A non-committed append retains the candidate and exact append result,
blocks further ingress consumption, and never retries writes automatically. Lifecycle handling must
close admission and interpret capacity, definite failure, or uncertainty; this is not performed by
the sequencer. Inspection of pending work and failures requires the consumer thread or external
quiescence. An admission-binding invariant failure also retains work and cannot release it to
matching; the production worker's existing uncaught invariant exception is fail-stop.

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

The synchronous matching state core owns the books and applies one command transition without queue,
bus, task, or worker dependencies. The run owner retains the matching state; the live matching engine
borrows exactly one non-null state by reference and wraps it with its command queue and
result/publication boundaries. The caller constructs state before the worker and keeps it alive at
its original address until the worker is destroyed and all worker invocations have ended. The worker
is the exclusive live mutator; the owner must not concurrently inspect or mutate books, replace the
state, or destroy it while the worker can access it. Matching retains its own pending-result retry
state, independently of book ownership. The legacy executable's composition root owns its state;
controller-backed deterministic workers borrow the private run state under the same lifetime
contract. Any later replacement requires quiescence before replacing run state. No mutable controller inspection API is implied by this contract.
Prepared-journal replay invokes the same core directly while suppressing all external publication,
and a worker can borrow that reconstructed state without transferring ownership, rebuilding or
copying books, or replaying commands again. Given
the same instrument configuration, initial state, and sequenced commands, the core produces the same
final state and ordered business events.

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

The result dispatcher completes admission once, derives the participant-private and public views
once, and retains the immutable authoritative batch until both projections are empty or accepted.
Private and public acceptance are separate atomic operations, so one may succeed before the other.
Each channel preserves command and event order independently, and a pending earlier batch blocks that
channel from overtaking it. The dispatcher consumes no later authoritative result until both
projections for the held result are empty or accepted. Saturation cannot change a durable business
result, cause repeated admission completion or projection, or silently discard an event.

### Private execution delivery

#### Purpose

Route acceptance, rejection, order-state, fill, and cancellation events to the correct client-facing
gateway.

#### Owns

- client/event routing;
- complete accepted `RecipientResults` fan-out batches;
- recipient-safe projection and handoff ordering;
- preservation of a complete fan-out until the downstream application event gateway owns it.

#### Does not own

- matching state;
- event creation;
- public market data;
- sequencing policy.

Initially, identical command retransmission within a retained run is the required result-retrieval
path. It waits for or returns the original committed result with the original identifiers. The
browser application stream does not automatically replay after reconnect. A deleted or unknown run
cannot be used to retrieve a result or create work in the active run.

The private view of a trade contains the recipient's order identity, role or side, execution price
and quantity, and own remaining quantity. It excludes the counterparty's client identity, order
identity, and remaining quantity. Rest, cancellation, and command-rejection events are private to
the affected client. The adopted public-trade projection and any future quote feed remain separate
market-data concerns.

The exchange-side dispatcher projects each immutable command result into one complete ordered
`RecipientResults` batch. An empty projection bypasses the handoff. The bounded FIFO measures its
capacity in contained private events and, using checked arithmetic, must hold at least
`2 × MaxEventsPerCommand` events when empty. This covers the worst valid expansion in which every
authoritative event is a self-trade and therefore produces maker then taker private views. Recipient
envelopes are ordered by first event appearance; events within each envelope preserve authoritative
order.

Queue insertion is all-or-nothing for the entire command fan-out. A full boundary retains exactly
one projected batch and prevents later authoritative results from being consumed until retry accepts
that same batch. Queue acceptance transfers ownership to private delivery. The application event
gateway implements that downstream role for the browser surface and routes envelopes by stable
`ClientId`; the exchange-side queue does not select a connection or imply transport acknowledgement.

Private and public handoffs accept independently. The dispatcher may release one projection after
that channel accepts it, but it retains the authoritative result and the other projection until both
channels are empty or accepted. Admission completion occurs once before retryable delivery progress;
retry cannot recomplete admission, reproject a view, duplicate a batch, or let either channel
overtake its own pending work.

Identical completed retransmission and retained-run lookup read the existing authoritative result and
do not create unsolicited queue entries. Replay reconstructs the same private values without live
publication. The queue itself is not crash-persistent: after restart, the journal and reconstructed
admission record remain authoritative for explicit retrieval. Pause and stop include pending and
queued private batches in their quiescence check and report retryable backpressure until the
private-delivery consumer drains them.

### Market-data publisher

#### Purpose

Derive the public view adopted for a particular rules version, accept complete public-trade batches
through a bounded handoff, and transfer them to the application event gateway. Public trade values,
the exchange-side handoff contract, and the browser transport are adopted; a recoverable public feed
is not.

#### Owns

- public event filtering and projection;
- accepted public-trade batches after exchange-side handoff;
- any later-adopted market-data sequence, snapshot, or consumer gap-recovery protocol.

#### Does not own

- private client execution state;
- matching decisions;
- client order entry;
- authoritative order-book mutation.

The exchange-side dispatcher projects each immutable command result into one complete ordered public
trade batch. An empty projection bypasses the handoff. The bounded FIFO accounts capacity in queued
trade records and can hold at least one maximum-sized command projection when empty. Acceptance is
all-or-nothing for the batch. A full boundary does not drop or overwrite records: the dispatcher
retains one pending batch and stops consuming later command results until the same batch is accepted.

Public acceptance is independent of the participant-private handoff. Either channel may accept its
projection first, but the dispatcher retains the authoritative result until both projections are
empty or accepted, and each channel blocks overtaking behind its own pending earlier batch.

Queue acceptance transfers handoff ownership from the exchange-side dispatcher to the market-data
publisher role. The application event gateway implements that role for the browser surface and owns
the batch after popping it. Transport success, retry, and client delivery do not alter matching or
the accepted batch. `EventId` identifies the authoritative source event but is not a gap-free
market-data sequence. Replay and recovery may regenerate the same projection for internal validation
while suppressing external publication.

An installed public handoff participates in lifecycle quiescence. Pause and stop cannot clear or
discard its pending or queued batches. If the publisher cannot consume enough records to accept the
pending batch, deterministic lifecycle advancement remains retryable and cannot report the boundary
drained. No snapshot, reconnect replay, gap recovery, top-of-book, or depth is selected here.

### Application event gateway

#### Purpose

Own the browser-facing application event stream after the exchange-side private and public handoffs.

#### Inputs

- pop access to the controller-owned private-result and public-trade handoff queues, borrowed in the
  in-process composition or consumed through the controller's pull operations;

#### Owns

- at most one popped item from each input while routing it;
- post-pop serialization, recipient authorization, connection routing, and browser delivery;
- one bounded serialized-byte queue per connection and a configured maximum connection count.

#### Does not own

- matching, sequencing, admission, or exchange-run lifecycle state;
- authoritative result storage or retained-run reconstruction;
- FIX delivery;
- a public snapshot, feed sequence, gap-recovery service, or replay store.

The gateway combines the downstream private-delivery and market-data-publisher roles without
combining their value types or handoff queues. Each successful pop transfers one complete item to the
gateway, which retains it until routing succeeds or the adopted disconnect rule resolves delivery;
it does not pop later work from that input first. Pure JSON V1 serializers at this boundary convert a
popped public batch or recipient-private envelope into gateway-owned bytes. The per-connection
outbound byte capacity must hold the largest valid controller-produced V1 handoff after serialization.

The outer lifecycle composition continues consuming both handoffs while the controller pauses or
stops. Controller quiescence ends when those queues transfer their items to the gateway; gateway-owned
delivery state remains separate. Run replacement and coordinated shutdown close bindings and flush
or disconnect gateway-owned work according to the adopted rules. The exact JSON schema,
authentication, ordering, reconnect, authorization, buffering, and slow-consumer behavior are defined
in [Exchange Rules: Browser application event delivery](exchange-rules.md#browser-application-event-delivery).

#### Participant identity and session ownership

The composition root loads and validates one bounded startup credential configuration before any
listener opens. The application gateway's identity/session state then owns that immutable credential
configuration and the process-local sessions bounded by its configured client count. The core
boundary accepts typed `ClientId` and 32-byte digest records; selecting and parsing an external file,
environment, or command-line representation remains a composition concern rather than a second
credential model.

HTTPS passes one bounded credential attempt to that owner and receives a server-derived `ClientId`
only on success. WebSocket handling resolves the session and passes that `ClientId` plus the requested
`ExchangeRunId` to the existing gateway binding boundary. Revocation tells the network owner to
remove affected gateway bindings and close their sockets. All observable acceptance, authorization,
cookie, lifetime, failure, and secrecy behavior remains in
[Exchange Rules: Browser participant authentication and sessions](exchange-rules.md#browser-participant-authentication-and-sessions).

The identity/session owner uses the event loop's monotonic clock for authorization deadlines; timer
callbacks only remove expired state. A random-source failure or generated-identifier collision
latches login unavailable without revoking an existing session. HTTP owns the exact V1 parsing,
status, header, cookie, and body mappings, while the identity/session owner returns typed outcomes and
does not own Beast messages or sockets.

#### Network and execution boundary

The C++ application gateway uses Boost.Beast for HTTP/1.1 and WebSocket framing, Boost.Asio for
asynchronous TCP and timer operations, and Boost.Asio SSL over OpenSSL for TLS. No second HTTP
framework, WebSocket library, JSON library, reverse proxy, or plaintext listener participates in the
target runtime.

One main application thread runs one `boost::asio::io_context`. On that thread, the gateway network
runtime exclusively owns the TLS listener, HTTP/WebSocket socket sessions, timers, and each current
asynchronous write. `ApplicationEventGateway` owns the logical connection bindings and queued
serialized bytes; after `tryPopOutbound` succeeds, the corresponding WebSocket session owns that one
immutable message until its write completes or the connection closes. The same thread is the sole
caller of `ApplicationEventGateway` and, after composition startup, every `ExchangeRunController`
submission, advancement, inspection, lookup, and lifecycle operation. Participant browser commands
therefore call the controller directly after protocol validation and authorization; no
cross-thread controller call or new ingress/control/result queue exists. Each event-loop turn
performs bounded protocol work and bounded deterministic controller/gateway advancement before
returning to network dispatch. A journal synchronization can still block that thread; this is an
explicit initial tradeoff rather than hidden concurrency.

HTTPS serves built static assets and the eventual login/session endpoints. After authentication, the
same WebSocket is bidirectional: complete inbound messages carry participant commands, while complete
outbound messages carry the existing public and participant-private JSON envelopes. The inbound
command schema remains a separate implementation slice; it may not bypass normalization or derive a
participant from message content. Browser lifecycle control remains unavailable until a separate
authorization policy is adopted.

One validated startup network configuration supplies the listen endpoint, static-asset root and
maximum asset size, TLS certificate-chain and private-key paths, the one allowed HTTPS `Origin`,
accept backlog, maximum connections, maximum HTTP header and body bytes, maximum WebSocket
inbound-message bytes, TLS/WebSocket handshake timeout, connection idle timeout, per-connection
outbound-byte capacity, shutdown flush deadline, bounded participant credential configuration, and
positive finite session lifetimes.
Deployment chooses the positive finite values; architecture does not invent universal defaults. The
outbound capacity must also satisfy the gateway's controller-handoff minimum. HTTP parsing admits one
request at a time per connection, each WebSocket session has at most one read and one write in
progress, and the existing gateway queue preserves complete outbound message boundaries. Container
deployment mounts certificate and key files read-only and supplies their paths through that
configuration; key material is never stored in the repository.
The configured static-asset tree remains immutable for the server's lifetime because containment and
file-type validation occur before the validated path is opened.

#### Startup and shutdown ownership

The composition root validates all finite resource settings and loads a matching certificate chain
and private key before opening a listener. It then resolves controller startup/recovery, installs the
deterministic processing path when the owned run permits it, constructs the borrowing application
event gateway and the identity/session owner, constructs the network runtime, and finally binds the
TLS listener. Readiness requires successful configuration and TLS loading, a completed controller
startup decision, constructed owners and borrowers, and a bound listener; run lifecycle state is
reported separately and need not be `Ready`.

Signal handling runs through the same Asio event loop. Coordinated shutdown stops accepts, upgrades,
new commands, and new control requests; closes admission; and continues bounded controller and
gateway advancement until the active run is durably paused or an exact failure is retained. It then
allows queued writes and WebSocket/TLS close handshakes until the configured shutdown deadline.
Expiry applies the adopted disconnect rule, after which the event loop stops. Destruction proceeds
in reverse ownership order: network sessions and listener, identity/session state, application event
gateway, controller processing borrowers, and controller-owned run state. A pause or TLS-close
failure is reported and must not be described as a clean shutdown.

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

- each gateway owns its connection and protocol session state;
- the sequencer owns command ordering state;
- the journal owns its append position;
- an instrument partition owns its books and active-order index;
- the application event gateway owns popped public/private batches and browser delivery state.

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
