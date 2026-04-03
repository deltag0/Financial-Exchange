---
description: Auto-invoked security review. Runs whenever new dependencies, Dockerfiles, or API endpoints are added.
---

# Security Review Skill

This skill runs automatically when changes are detected in:
- `**/Dockerfile`
- `**/package.json`
- `docker-compose.yml`
- `backend/**` (new routes)

## Trigger Behavior

When invoked, use the `security-auditor` agent to review the changed files and produce a security
report before the changes are committed or deployed.

## Steps

1. Identify the changed files in scope.
2. Run `npm audit` if `package.json` was modified:
   ```bash
   cd backend && npm audit --json
   ```
3. Check for hardcoded secrets with a pattern scan:
   ```bash
   git diff HEAD | grep -iE '(password|secret|token|api_key)\s*=\s*\S+'
   ```
4. For new Dockerfile layers, check base image provenance and any `RUN curl | bash` patterns.
5. For new API routes, verify input validation against `.claude/rules/api-conventions.md`.
6. Produce the security report using the `security-auditor` agent persona.
7. If any CRITICAL or HIGH findings exist, halt and present them before proceeding.
