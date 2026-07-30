# API Conventions Adapter

No independent HTTP or FIX business behavior is authoritative in this file.

- Adopted order, identifier, numeric, validation, rejection, and event behavior belongs in
  `docs/exchange-rules.md`.
- Gateway responsibilities and boundaries belong in `docs/architecture.md`.
- Existing routes and protocol support belong in `docs/implementation-status.md`.
- Required contribution and reporting practices belong in `AGENTS.md`.

Do not implement a gateway schema by silently resolving an unresolved exchange rule. A protocol
adapter must map external messages losslessly into the adopted normalized command model and translate
the resulting typed events without changing matching behavior.
