# Exchange — Overview

This repository contains a performant, containerized exchange platform designed for experimenting with high-throughput order sequencing, IPC-based components, and observability. It combines a C++ sequencer and core IPC library with a lightweight Node.js API, and an OpenTelemetry → Prometheus metrics pipeline.

## Components

- **Sequencer (C++)** — assigns monotonic sequence numbers to incoming orders and exposes IPC queues for downstream consumers. See [sequencer](sequencer/).
- **Core IPC library (C++)** — header-only `SharedQueue` utilities used by the sequencer and other processes. See [core](core/).
- **Backend API (Node.js)** — REST surface for clients, validating requests and forwarding messages to the sequencer. See [backend](backend/).
- **Engine (placeholder)** — intended matching engine component; currently not implemented. See [engine](engine/).
- **Frontend (placeholder)** — Dockerfile + Nginx template for a future UI. See [frontend](frontend/).
- **Observability** — OpenTelemetry Collector configuration and Prometheus scraping for metrics. See [otel/config.yaml](otel/config.yaml) and [prometheus/prometheus.yml](prometheus/prometheus.yml).

## Tech Stack

- C++20, Boost.Interprocess (IPC)
- Node.js (CommonJS) for the API
- CMake for native builds
- Docker & Docker Compose for local stacks
- OpenTelemetry Collector (OTLP) and Prometheus for metrics
- Google Test / Node test runner / pytest for testing

## Running (Docker Compose)

Prerequisites: Docker and Docker Compose.

```bash
# Build and start all services
docker-compose up --build

# Stop services
docker-compose down
```

When running locally via Docker:
- Backend API: http://localhost:5000
- Prometheus UI: http://localhost:9090

## Native C++ Build (sequencer)

Requires a C++20 compiler, CMake 3.20+, and Boost 1.71+.

```bash
cmake -B build -S .
cmake --build build --target sequencer_app
```

## API Conventions

The backend follows strict API conventions for order messages (JSON, integer fixed-point for amounts, validation rules, and specific HTTP status codes). See `.claude/rules/api-conventions.md` for full details.

## Observability

All HTTP routes and critical operations emit OTel metrics (counters and histograms) and push to the collector at `http://otel:4318`. The collector is configured to expose a Prometheus-compatible metrics endpoint.

## Testing

- C++ unit tests use Google Test (see `sequencer/tests` and `core/tests`).
- Backend tests use the Node.js built-in test runner (`backend/tests`).
- Integration tests and simple Python clients for the sequencer live in `sequencer/tests`.

## Useful files

- [backend/index.js](backend/index.js) — Node HTTP entrypoint
- [sequencer/include/sequencer.hpp](sequencer/include/sequencer.hpp) — sequencer API
- [otel/config.yaml](otel/config.yaml) — OTel Collector config
---