---
description: Auto-invoked deploy workflow. Runs the full build → health check → observability verification sequence.
---

# Deploy Skill

Orchestrates a full deployment and post-deploy verification. Delegates to the `/project:deploy` command
for the step-by-step execution, then performs the post-deploy checks below.

## When to Invoke

- Manually via `/project:deploy`
- After a successful merge to `main` (CI triggers this via GitHub Actions)

## Post-Deploy Verification Checklist

After `docker-compose up -d` completes, verify each of the following. Stop and report on first failure.

### 1. All containers healthy
```bash
docker-compose ps --format json | jq '.[] | {name, State, Health}'
```
Expected: every service shows `"Health": "healthy"` or has no health check configured yet.

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
Expected: `200`

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
