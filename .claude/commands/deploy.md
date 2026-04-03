# Deploy

Run through the full deployment sequence for this project. Follow these steps in order, stopping and
reporting any failure rather than continuing past it.

## 1. Pre-flight checks

- Confirm the working tree is clean (`git status`). If there are uncommitted changes, ask whether to
  stash them or abort.
- Confirm the current branch and the target environment (default: local Docker Compose stack).
- Check that Docker is running.

## 2. Build images

Build all Docker images from source. Do not pull cached images for the sequencer or backend — rebuild
them to pick up any code changes.

```bash
docker-compose build --no-cache sequencer backend
docker-compose build frontend
```

Report the image sizes after building.

## 3. Run CI checks locally

Before deploying, verify the build is clean:

```bash
# C++ compile check (inside the sequencer image that was just built)
docker-compose run --rm sequencer cmake --build /app/build --target sequencer_app

# Node.js tests
docker-compose run --rm backend node --test tests/**/*.test.js
```

If any check fails, stop here and report what failed.

## 4. Start the stack

```bash
docker-compose up -d
```

## 5. Health check verification

Wait for all containers to report healthy. Poll every 3 seconds, timeout after 60 seconds.

```bash
docker-compose ps
```

Expected: all services show `healthy` or `running` (once health checks are wired up). Report any service
that is `unhealthy` or has restarted.

## 6. Verify observability pipeline

Check that the metrics pipeline is live end-to-end:

```bash
# OTel Collector is up
curl -sf http://localhost:8889/metrics | head -20

# Prometheus has active targets
curl -sf http://localhost:9090/api/v1/targets | python3 -m json.tool | grep '"health"'
```

All targets should show `"health": "up"`. Report any that are `"down"`.

## 7. Smoke test

Hit the backend to confirm it's serving traffic:

```bash
curl -sf http://localhost:5000/
```

Expected: `200 OK`. Report the response body and status code.

## 8. Post-deploy summary

Report:
- Which services are running and their mapped ports
- Grafana URL if the grafana service is present: `http://localhost:3000`
- Prometheus URL: `http://localhost:9090`
- Any warnings (e.g., services using `latest` image tags, missing health checks)

## Rollback

If the deploy fails after step 4, run:

```bash
docker-compose down
```

Then investigate logs before re-attempting:

```bash
docker-compose logs --tail=50 <failing-service>
```
