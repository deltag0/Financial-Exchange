# AGENTS.md

This file defines how Work, Codex, and future contributors should operate in this repository.

## Project context

This is a C++20 financial exchange being developed incrementally by two engineers for ordinary laptops. Each stage should fit the intended architecture and remain useful as
the system gains instruments, complete matching behavior, deterministic events, recovery, and
measured performance.

Do not describe the project as production-ready, ultra-low-latency, or high-frequency without
reproducible evidence.

## Sources of truth

- Exchange behavior: `docs/exchange-rules.md`
- Intended component design and ownership: `docs/architecture.md`
- Current implementation and test coverage: `docs/implementation-status.md`

Follow adopted exchange rules. Never treat a recommendation or unresolved decision as adopted
behavior.

Verify current behavior using source code, tests, and `implementation-status.md`. Never infer that a
feature exists because it appears in `architecture.md`.

## Required working practices

- Distinguish confirmed implementation facts, intended architecture, adopted behavior,
  recommendations, unresolved decisions, and assumptions.
- Make narrow, reviewable changes that address the requested behavior.
- Avoid unrelated refactoring and preserve unrelated worktree changes.
- Add or update deterministic tests for behavioral changes.
- Do not optimize primarily for minimum short-term development effort. Prefer quality, simplicity,
  robustness, scalability, and long-term maintainability while keeping work scoped to the current
  phase.
- Profile before optimizing. Support performance claims with reproducible before-and-after evidence.
- Use bounded resources and define backpressure rather than silently dropping accepted work.
- Keep ownership and object lifetime explicit, especially across threads and queues.
- Do not commit, push, open a pull request, deploy, stash, reset, or remove user work unless the user
  requested that action.

## Documentation-update rules

| Change | Required documentation |
|---|---|
| Implement an existing adopted exchange rule | Update code, deterministic tests, and `docs/implementation-status.md` |
| Add or change exchange behavior | Update `docs/exchange-rules.md`, tests, and implementation status where applicable |
| Change component responsibilities, ownership, or data flow | Update `docs/architecture.md` |
| Change current completeness, limitations, or verified test coverage | Update `docs/implementation-status.md` |
| Change only internal implementation without affecting behavior, architecture, or documented status | No documentation update is normally required |
| Improve performance without changing those areas | Record profiling and benchmark evidence in the pull request or development report; do not create a permanent performance document |

Do not duplicate current implementation descriptions outside `docs/implementation-status.md`.

## Repository guidance

Before touching a layer, read any relevant tool-specific rules that still apply:

- `.claude/rules/code-style.md`
- `.claude/rules/testing.md`
- `.claude/rules/api-conventions.md`

If a tool-specific instruction conflicts with the sources of truth above or with the current
repository, report the conflict and follow the higher-level project documentation. Tool-specific
files must not invent exchange behavior.

## Build and test commands

Commands that build, test, run services, or generate artifacts can change the workspace or local
runtime state. State what will run and why before using them.

### Native C++

Requires a C++20 compiler, CMake 3.20+, Boost 1.71+, QuickFIX, and Google Test.

```bash
cmake -B build -S .
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Docker Compose

```bash
docker-compose up --build
docker-compose down
```

Do not assume the full stack is operational merely because containers start. Verify the relevant
behavior and report missing endpoints, health checks, or wiring as failures or unverified gaps.

### Backend

```bash
cd backend && npm start
```

Consult `docs/implementation-status.md` before assuming backend routes or tests exist.

## Reporting requirements

At handoff, report:

- files changed;
- behavior changed and the governing exchange rule;
- commands run;
- tests and checks passed;
- failures;
- checks skipped and why;
- remaining assumptions or unresolved decisions;
- any unrelated worktree changes that were preserved.

Never claim a build, test, benchmark, deployment, or runtime behavior was verified if it was not run.
