# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

### Docker (primary workflow)
```bash
docker-compose up --build    # Build and start all services
docker-compose down          # Stop services
docker-compose up --build backend   # Rebuild a single service
```

### Native C++ build (requires C++20 compiler, CMake 3.20+, Boost 1.71+)
```bash
# From repo root (bash)
cmake -B build -S .
cmake --build build --target sequencer_app --parallel
```

### Backend (Node.js)
```bash
cd backend && npm start      # Start backend server
```

## Architecture

This is an early-stage containerized exchange system. The core design separates order processing (C++) from the API layer (Node.js), with an observability stack baked in.

**Component responsibilities:**
- `sequencer/` — C++20 order sequencer; receives orders and assigns sequence numbers. Uses Boost.Interprocess shared memory queues to communicate with other processes.
- `core/` — Header-only IPC library (`shared_queue.hpp`). Wraps `boost::interprocess::message_queue` in the `exchange::core::SharedQueue` class. All inter-process communication goes through this.
- `engine/` — Placeholder for the matching engine (not yet implemented, excluded from CMake build).
- `backend/` — Minimal Node.js HTTP server (port 5000). Intended to be the REST API layer between clients and the exchange.
- `frontend/` — Dockerfile template only; no frontend code implemented yet. Served by Nginx on port 5173.

**Observability pipeline:**
```
App → OTLP push → OTel Collector (port 4318) → exposes metrics (port 8889)
Prometheus (port 9090) → scrapes OTel at otel:8889 every 15s
```
The OTel Collector config lives in `otel/config.yaml`; Prometheus config in `prometheus/prometheus.yml`.

**IPC / shared memory:**
The sequencer container sets `ipc: shareable` in `docker-compose.yml` so other containers can attach to its shared memory queues. The `SharedQueue` class provides `send()`, `receive()` (non-blocking), and `receive_blocking()` methods.

**Key data structure** (`sequencer/lib/sequencer.hpp`):
```cpp
struct sequenceMessage {
    int id, sequence_number, port;
    double price, quantity;
    char symbol[10];
    orderType type;  // BUY, SELL, CANCEL, CANCELREJ
};
```

## Rules

Modular instruction files live in `.claude/rules/`. Read the relevant one before touching that layer:

- `.claude/rules/code-style.md` — C++, Node.js, Docker/YAML conventions
- `.claude/rules/testing.md` — unit, integration, and load testing expectations
- `.claude/rules/api-conventions.md` — REST endpoint design, request/response format, OTel requirements

## Custom Commands

- `/project:deploy` — full build → health check → observability verification sequence
- `/project:review` — SRE-focused code review (reliability, observability, security)
- `/project:fix-issue <N>` — fetch GitHub issue #N and produce a minimal, tested fix
