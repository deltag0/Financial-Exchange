# Fix GitHub Issue

Fix the GitHub issue whose number is given as the argument (e.g., `/project:fix-issue 42`).

## Steps

1. **Fetch the issue** using `gh issue view $ARGUMENTS` to get the title, body, and any linked comments.
   Read the full description carefully before touching any code.

2. **Understand the blast radius.** Before making changes, identify:
   - Which component owns the problem (sequencer, backend, otel, prometheus, docker-compose)?
   - Is this a reliability issue (service crash), observability gap (missing metric), or a feature request?

3. **Reproduce the issue** if possible:
   - For backend issues: `docker-compose up -d backend` then `curl` the failing endpoint.
   - For sequencer issues: check `docker-compose logs sequencer`.
   - For metrics issues: check `curl localhost:8889/metrics` and `localhost:9090/api/v1/targets`.

4. **Make the minimal fix.** Do not refactor surrounding code. Do not add unrelated improvements.
   Follow `.claude/rules/code-style.md` for the component being changed.

5. **Add or update a test** in the appropriate location (see `.claude/rules/testing.md`):
   - Unit test if the fix is in a single function.
   - Integration test if the fix spans services.

6. **Verify the fix:**
   ```bash
   docker-compose up --build -d
   # run the relevant test or curl command that previously failed
   ```

7. **Check observability:** if the issue was caused by a missing metric or alert, add it as part of the fix.

8. **Commit** with the message format: `Fix #<issue-number>: <short description>`
   Example: `Fix #42: Return 503 when sequencer queue is full`

9. **Open a PR** with `gh pr create` and link it to the issue. Include:
   - What was broken and why
   - What the fix does
   - How to verify it (the curl command or test name)
