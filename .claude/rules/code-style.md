# Code Style

Follow `AGENTS.md` and the authoritative project documents it identifies. This file contains only
language and formatting guidance; it does not define exchange behavior, architecture, or current
implementation status.

## C++ (sequencer, core, engine)

- **Standard:** C++20. Use modern features (concepts, ranges, `std::span`) where they clarify intent.
- **Formatter:** `clang-format` with the project `.clang-format` (Google base). Run before every commit touching C++.
- **Indent:** 4 spaces. **Line limit:** 120 columns.
- **Namespaces:** Follow the existing `exchange::<component>` hierarchy (e.g., `exchange::sequencer`, `exchange::core`). Never `using namespace` at file scope.
- **Header guards:** `#pragma once` only — no `#ifndef` guards.
- **Includes:** Group in order: related header, C++ stdlib, Boost, project headers. Blank line between groups.
- **Types:** Prefer fixed-width integers (`uint64_t`, `int32_t`) for all wire-format and IPC structs. Never use `int`/`long` in `sequenceMessage` or `SharedQueue` signatures.
- **Naming:**
  - Types and classes: `PascalCase`
  - Functions and variables: `camelCase`
  - Constants and enumerators: `UPPER_SNAKE_CASE`
  - Private members: `trailingUnderscore_`
- **Comments:** Write comments that explain *why*, not *what*. Concurrency and queue ownership are
  non-obvious — document ownership, lifetime, and synchronization assumptions.
- **Error handling:** Use return codes or `std::expected` for recoverable errors. `assert` only for invariants that can never fail in correct code. No exceptions in hot paths (order processing).

## Node.js (backend)

- **Module system:** CommonJS (`require`/`module.exports`) — do not mix ESM `import` syntax.
- **Formatting:** 2-space indent, single quotes, semicolons required.
- **HTTP:** Use the built-in `http` module for new routes; do not add Express unless complexity clearly warrants it.
- **Environment variables:** All config (port, downstream URLs) via `process.env` with a documented default. Never hardcode ports or hostnames.
- **Metrics:** Any new endpoint or operation that touches the exchange must emit an OTel metric (counter or histogram) before the PR is merged. The OTel SDK push target is `otel:4318`.
- **Logging:** `console.log` is acceptable for now; prefix structured fields: `[component] message key=value`.

## Docker / YAML

- **Indent:** 2 spaces for all YAML files (`docker-compose.yml`, OTel config, Prometheus config).
- **Ports:** Always comment what each published port is for.
- **Secrets:** Never hardcode credentials in `docker-compose.yml`. Use environment variable references (`${VAR}`).
- **Health checks:** Every service that exposes an HTTP endpoint must have a `healthcheck:` block.
- **Image tags:** Pin to a specific version tag in production configs (e.g., `prom/prometheus:v2.52.0`), not `latest`.

## General

- No trailing whitespace. Newline at end of every file.
- Commit messages: imperative mood, present tense (`Add health check for backend`, not `Added`).
- Do not commit generated build artifacts (`build/`, `CMakeFiles/`, `node_modules/`).
