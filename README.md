# Exchange — Overview

This repository contains a containerized financial exchange platform designed for deterministic,
multi-instrument order processing. It combines a C++20 exchange core with FIX connectivity, order
sequencing, instrument-partitioned matching engines, durable recovery, a Node.js API, a web
interface, and an OpenTelemetry and Prometheus observability pipeline.

## Components

- **FIX gateway (C++)** — manages FIX sessions, validates protocol messages, and translates client
  requests into normalized exchange commands. See [core/fix](core/fix/).
- **Sequencer and journal (C++)** — establishes an authoritative order for accepted commands,
  records them durably, and routes them to the correct instrument partition. See
  [sequencer](sequencer/).
- **Matching engine (C++)** — maintains price-time-priority order books and deterministically
  processes orders for each instrument. See [matching_engine](matching_engine/).
- **Core libraries (C++)** — provide bounded queues, event distribution, task abstractions, and
  exchange component wiring. See [core](core/).
- **Backend API (Node.js)** — exposes client-facing HTTP endpoints and connects external
  applications to exchange services. See [backend](backend/).
- **Frontend** — provides a web interface for exchange data and services. See [frontend](frontend/).
- **Recovery and replay** — restores order books and exchange state from journals and snapshots
  while preserving deterministic processing results.
- **Observability** — exports operational metrics through OpenTelemetry for collection and
  Prometheus monitoring. See [otel/config.yaml](otel/config.yaml) and
  [prometheus/prometheus.yml](prometheus/prometheus.yml).

## Tech Stack

- C++20, Boost, and QuickFIX
- Node.js using CommonJS
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

The services are orchestrated with Docker Compose:

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
