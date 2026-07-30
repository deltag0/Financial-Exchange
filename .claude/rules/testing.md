# Testing Adapter

Follow `AGENTS.md` for required working practices, commands, reporting, and documentation updates.

- Exchange behavior comes only from adopted rules in `docs/exchange-rules.md`.
- Existing coverage and known gaps come only from `docs/implementation-status.md`.
- Intended components in `docs/architecture.md` do not prove that a target or test exists.
- Behavioral changes require deterministic tests at the smallest state-owning boundary.
- Integration tests verify real boundaries after deterministic state-machine tests establish business
  behavior.
- Benchmark names and claims must match the code path actually measured.
- Report the exact command, result, failures, skipped checks, assumptions, build configuration, and
  relevant environment.

Do not invent a test target, endpoint, CI workflow, load scenario, or pass threshold that is not
present and verified.
