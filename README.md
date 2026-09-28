# Exchange — Overview

This repository develops a bounded, deterministic financial-exchange showcase. The intended runtime
is one C++20 application process containing the exchange and browser application gateway. Current
implementation completeness, including legacy stubs and unverified container configuration, is
tracked in [Implementation Status](docs/implementation-status.md).

## Components

- **FIX adapter (C++)** — parses the currently supported FIX messages and translates them into
  normalized exchange commands. The existing executable still uses a legacy non-durable
  composition. See [core/fix](core/fix/).
- **Sequencer and journal (C++)** — provides process-local sequencing in the legacy executable and a
  separately tested journal-backed durability path. See [sequencer](sequencer/).
- **Matching engine (C++)** — maintains deterministic price-time-priority books keyed by instrument.
  Partition workers are not yet implemented. See [matching_engine](matching_engine/).
- **Core libraries (C++)** — provide bounded queues, event distribution, task abstractions, and
  exchange component wiring. See [core](core/).
- **Application-event boundary (C++)** — projects participant-private and public results and provides
  deterministic JSON serialization. Browser transport and executable integration are not yet
  implemented.
- **Legacy Node backend stub** — returns a plain-text greeting and is not part of the intended target
  runtime. See [backend](backend/).
- **Legacy Nginx frontend template** — contains no frontend application and is not a target runtime
  service. Future built browser assets belong to the C++ application gateway. See
  [frontend](frontend/).
- **Recovery and replay** — reconstructs bounded run state and completed results from the
  authoritative journal.
- **Observability configuration** — contains OpenTelemetry Collector and Prometheus configuration;
  the applications do not yet emit the documented exchange metrics. See
  [otel/config.yaml](otel/config.yaml) and [prometheus/prometheus.yml](prometheus/prometheus.yml).

## Tech Stack

- C++20, Boost, and QuickFIX
- Node.js using CommonJS for the retained legacy backend stub
- CMake and Google Test
- Python and pytest
- Docker and Docker Compose
- OpenTelemetry Collector and Prometheus

## Build

Native builds require a C++20 compiler, CMake 3.20+, Boost 1.71+, QuickFIX, and Google Test.

```bash
cmake -B build -S .
cmake --build build --parallel
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

The repository retains an unverified Docker Compose definition containing the legacy backend and
frontend services alongside monitoring and sequencer services. It does not represent the adopted
target topology or a verified operational stack:

```bash
docker-compose up --build
docker-compose down
```

## Documentation

- [Product Direction](docs/product-direction.md) — educational simulation vision, experience, and feature priorities
- [Exchange Rules](docs/exchange-rules.md) — order handling and matching behavior
- [Architecture](docs/architecture.md) — component responsibilities, ownership, and data flow
- [Implementation Status](docs/implementation-status.md) — implementation and test coverage record
- [Contributor and Agent Guidance](AGENTS.md) — required working and documentation practices
