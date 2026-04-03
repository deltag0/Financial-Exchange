# Observability

A containerized exchange sequencer system with a built-in observability stack. The project provides a high-performance C++ order sequencer communicating over shared memory, a lightweight Node.js backend, and full metrics collection via OpenTelemetry and Prometheus.

## Services

| Service | Description | Port |
|---------|-------------|------|
| `backend` | Node.js HTTP server | 5000 |
| `frontend` | Nginx static server | 5173 |
| `sequencer` | C++ order sequencer (IPC) | — |
| `otel` | OpenTelemetry Collector | 4318 (OTLP), 8889 (metrics) |
| `prometheus` | Metrics storage & UI | 9090 |

## Stack

- **C++20** — sequencer and core IPC library (Boost.Interprocess)
- **Node.js 20** — backend API
- **Nginx** — frontend server
- **OpenTelemetry Collector** — metrics pipeline
- **Prometheus** — metrics storage and visualization
- **Docker Compose** — service orchestration
- **CMake 3.20+** — C++ build system

## Getting Started

**Prerequisites:** Docker and Docker Compose.

```bash
# Build and start all services
docker-compose up --build

# Stop services
docker-compose down
```

Once running:
- Backend API: http://localhost:5000
- Prometheus UI: http://localhost:9090

## Building the Sequencer Natively

Requires a C++20 compiler, CMake 3.20+, and Boost 1.71+.

```bash
mkdir build && cd build
cmake -B . -S ..
cmake --build . --target sequencer_app -j$(nproc)
```

## Project Structure

```
backend/        Node.js backend service
frontend/       Nginx frontend (Dockerfile only, app not yet implemented)
sequencer/      C++ sequencer service
core/           Shared header-only IPC library (SharedQueue)
engine/         Placeholder for the matching engine (not yet implemented)
otel/           OpenTelemetry Collector configuration
prometheus/     Prometheus configuration
```
