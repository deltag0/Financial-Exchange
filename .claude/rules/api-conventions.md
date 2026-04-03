# API Conventions

The backend exposes a REST API over HTTP on port 5000. All routes that interact with the exchange
must follow these conventions.

## URL Structure

```
POST   /orders              Submit a new order
GET    /orders/:id          Get order status by ID
DELETE /orders/:id          Cancel an order
GET    /healthz             Health check (used by Docker and Prometheus)
GET    /metrics             Internal metrics endpoint (optional; OTel is the primary path)
```

## Request Format

- Content-Type: `application/json` for all request bodies.
- Order submission body:
  ```json
  {
    "symbol": "AAPL",
    "type": "BUY",
    "price": 18250,
    "quantity": 10
  }
  ```
- `price` and `quantity` are integers representing fixed-point values (cents / whole units). Never use
  floating-point on the wire — the sequencer stores them as `uint64_t`.

## Response Format

All responses are JSON. Successful responses:
```json
{ "id": 42, "sequence_number": 1001, "status": "ACCEPTED" }
```

Error responses always include `error` and `code`:
```json
{ "error": "symbol must be 1-9 characters", "code": "INVALID_SYMBOL" }
```

## HTTP Status Codes

| Situation                  | Code |
|---------------------------|------|
| Order accepted             | 201  |
| Order found                | 200  |
| Invalid request body       | 400  |
| Order not found            | 404  |
| Sequencer unavailable      | 503  |
| All other server errors    | 500  |

Never return 200 for an error, and never return a non-2xx for success.

## Health Check

`GET /healthz` must respond within 1 second with:
```json
{ "status": "ok", "uptime": 123.4 }
```
This endpoint is used by Docker health checks and Prometheus blackbox monitoring. It must not depend
on the sequencer being up — it only signals that the Node process itself is alive.

## Observability Requirements

Every route handler must:
1. Record a counter metric `http_requests_total` with labels `method`, `path`, `status_code`.
2. Record a histogram metric `http_request_duration_ms` with the same labels.
3. On sequencer errors, increment `sequencer_errors_total` with label `reason`.

Push metrics to the OTel Collector at `http://otel:4318/v1/metrics`.

## Validation Rules

- `symbol`: 1–9 uppercase ASCII letters (matches `sequenceMessage.symbol[10]` buffer).
- `type`: one of `BUY`, `SELL`, `CANCEL`.
- `price`: positive integer, max `uint64_t`.
- `quantity`: positive integer ≥ 1.

Reject and return 400 before the request reaches the sequencer.
