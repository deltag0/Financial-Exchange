# SRE Code Review

Review the current staged diff or the files passed as arguments through the lens of a Production Engineer.
If no argument is given, run `git diff HEAD` to get the current changes.

Check each of the following areas and report findings grouped by severity: **BLOCKER**, **WARNING**, **SUGGESTION**.

## Reliability

- Does every new Docker service have a `healthcheck:` block and a `restart:` policy?
- Are all error paths handled? A 500 with no log line is a silent failure.
- Are there any unbounded retry loops or missing timeouts on HTTP/IPC calls?
- Does the code behave correctly if the sequencer is down? (Backend must degrade gracefully.)
- Are any new `assert` / `panic` paths reachable in production?

## Observability

- Does every new HTTP route emit `http_requests_total` and `http_request_duration_ms` OTel metrics?
- Are new error conditions counted with a dedicated metric (e.g., `sequencer_errors_total`)?
- Is there a log line at INFO or ERROR level for every significant state transition?
- Would an on-call engineer be able to diagnose a failure from the logs and metrics alone?

## Security

- Are any secrets, tokens, or credentials hardcoded? (Flag even in test code.)
- Is user input validated before it reaches the sequencer or any system call?
- Are Docker images pinned to a digest or version tag (not `latest`)?

## Performance

- Are there any synchronous blocking calls on the Node.js event loop that should be async?
- Are any hot-path C++ operations allocating on the heap where they could use stack or pre-allocated buffers?
- Does any new Prometheus scrape target have a scrape interval that is too aggressive (< 5s)?

## Code Style

- Does C++ code pass `clang-format` with the project's `.clang-format`?
- Are new Node.js files using CommonJS (`require`) and 2-space indent?
- Are all new environment variables documented with a default in `CLAUDE.md` or the relevant Dockerfile?

## Final Output

Produce a markdown report with:
1. A one-line summary: **LGTM** / **Needs changes** / **Blocked**
2. Bulleted findings by severity
3. For each BLOCKER, a concrete suggested fix
