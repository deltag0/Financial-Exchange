# Testing

## Philosophy

The goal is confidence that the system behaves correctly under the conditions that matter in production:
real IPC queues, real Docker networks, real Prometheus scrapes. Prefer integration tests that hit live
components over unit tests with mocked boundaries whenever the boundary is non-trivial (e.g., shared
memory, HTTP handlers).

## C++ (sequencer, core)

**Framework:** Google Test (`gtest`) + Google Mock (`gmock`). Add a `tests/` subdirectory under `sequencer/`
with a `CMakeLists.txt` that adds a `sequencer_tests` target.

**What to test:**
- `SharedQueue`: `send()` / `receive()` round-trip on a named queue; verify `receive()` returns `false`
  when queue is empty (non-blocking path); verify `receive_blocking()` unblocks when a message arrives.
- `Sequencer` logic: sequence numbers increment monotonically; `BUY` and `SELL` messages are passed
  through unchanged; `CANCEL` on an unknown ID produces a `CANCELREJ`.

**What not to test:** The Boost.Interprocess primitives themselves. Trust the library; test your usage of it.

**Running tests:**
```bash
cmake -B build -S . -DBUILD_TESTING=ON
cmake --build build --target sequencer_tests
ctest --test-dir build --output-on-failure
```

## Node.js (backend)

**Framework:** Node.js built-in `node:test` runner — no extra dependencies. Test files live in `backend/tests/`.

**What to test:**
- HTTP handler returns `200` and correct `Content-Type` for the happy path.
- Any route that proxies to the sequencer: verify it sends the correct OTel metric on success and on error.
- Health check endpoint (`/healthz`): returns `200 { status: "ok" }` when the service is up.

**Running tests:**
```bash
cd backend && node --test tests/**/*.test.js
```

## Integration Tests

Integration tests verify cross-service behavior using the real Docker Compose stack.

**What to test:**
- Prometheus can reach `otel:8889` and has at least one active target after `docker-compose up`.
- The backend health endpoint returns `200` within 10 seconds of container start.
- Posting an order via the backend results in a new metric data point visible in Prometheus within 30s.

**Tooling:** Write integration tests as shell scripts or use [k6](https://k6.io/) for HTTP scenarios.
Place them in `tests/integration/`.

**Running integration tests:**
```bash
docker-compose up -d
./tests/integration/smoke_test.sh
docker-compose down
```

## Load Testing

Used to validate the scalability story. Tool: **k6**.

- Scripts live in `tests/load/`.
- Default scenario: ramp from 1 to 50 virtual users over 60s sending orders to the backend.
- Pass criteria: p99 latency < 100ms, error rate < 0.1%.
- Run during hackathon demo with Grafana open to show live metrics.

```bash
k6 run tests/load/order_flood.js
```

## CI

All unit and integration tests run on every pull request via GitHub Actions (`.github/workflows/ci.yml`).
The pipeline must be green before merging to `main`.
