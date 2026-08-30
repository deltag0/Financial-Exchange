> **Historical note:** This file is preserved for background only. It is not authoritative and may
> contradict `../exchange-rules.md`, `../architecture.md`, or `../implementation-status.md`.

The original design proposed threads for:
- QuickFIX receiving FIX messages
- FIX ports handling messages
- Sequencer
- Matching engine

Each port has private shared queues:
- fixMessageQueue (between QuickFix and FIX port)
- internalMessageQueue (between ME and FIX port)

That design also proposed per-port queues feeding the sequencer. It has been superseded by the
single process-local sequencing boundary documented in `../architecture.md` and
`../implementation-status.md`.
