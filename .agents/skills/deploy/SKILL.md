---
description: Auto-invoked deploy workflow. Runs the full build → health check → observability verification sequence.
---

# Deploy Skill

Follow `AGENTS.md`. This file is a tool-specific operational adapter and does not define exchange
behavior, architecture, or implementation status.

This is a local development-stack workflow, not evidence that the exchange is production-ready.
Read `docs/exchange-rules.md` before treating an order-path or observability result as supported
behavior. The project is intended to run on ordinary laptops; do not infer production, colocated, or
ultra-low-latency guarantees from this workflow.

Orchestrates a full deployment and post-deploy verification. Delegates to the `/project:deploy` command
for the step-by-step execution, then performs the post-deploy checks below.

## When to Invoke

- Manually via `/project:deploy`
- After a successful merge to `main` only when an explicitly configured CI workflow invokes it

Do not run this workflow merely because files changed. Deployment, shutdown, cleanup, and external
network actions require the user's request or an explicitly authorized workflow.

## Post-Deploy Verification Checklist

After `docker-compose up -d` completes, verify each of the following. Stop and report on first failure.

### 1. All containers healthy
```bash
docker-compose ps --format json | jq '.[] | {name, State, Health}'
```
Expected: every service that is supposed to expose health information shows `"Health": "healthy"`.
If a health check is missing, report that as a repository gap rather than treating the service as
verified.

### 2. Observability pipeline live
```bash
# OTel exporting metrics
curl -sf http://localhost:8889/metrics | grep -c '^[^#]'
# Prometheus has scraped at least one target successfully
curl -sf 'http://localhost:9090/api/v1/query?query=up' | jq '.data.result[] | select(.value[1]=="1") | .metric.job'
```

### 3. Backend serving traffic
```bash
curl -sf -o /dev/null -w "%{http_code}" http://localhost:5000/healthz
```
Expected: `200`. If `/healthz` is not implemented, report the missing endpoint; do not substitute a
different response and call the health check successful.

### 4. No recent container restarts
```bash
docker-compose ps | grep -v "Up"
```
Expected: no output (all containers stable).

## Rollback Procedure

If any check fails:
1. Capture logs: `docker-compose logs --tail=100 > /tmp/deploy-failure.log`
2. Stop stack: `docker-compose down`
3. Report the failing check and the captured logs.
4. Do not auto-retry. Wait for human intervention.
