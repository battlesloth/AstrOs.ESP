# AstrOs ESP Firmware — Plan

Workflow rules: `CLAUDE.md` (Workflow section). Rationale and templates: `.docs/agentic-workflow.md`. Task IDs here are independent of AstrOs.Server's.

## Status

Active:  none — between projects
Now:     —
Next:    promote a Backlog item to T-001, or start the next project with a seam-discovery session
Blocked: none
Last:    2026-08-16 — agentic workflow bootstrap; rel_1.2 shipped, develop bumped to 1.3.0

## Standalone tasks

(none open)

## Backlog (unscheduled candidates)

- Known-fragile catalog: `.docs/code-review/code-review.md` (P0–P3) — headline items also listed in CLAUDE.md "Known-fragile areas" (timer-callback leaks/stack pressure, `peers` vector mutex, `AnimationController` unlocked reads, `setKeyId` peer-index assumption, cross-core globals). Promote individually as tasks.
- Queue consumers that fail to `free()` embedded pointers (same review catalog) — sweepable as one task or per-consumer.

Cross-repo: AstrOs.Server's `PLAN.md` Backlog holds the server-side serial findings from the 2026-08-06 bench log (e.g., server discards master-emitted POLL_NAK). Wire-format changes, if any come out of that, pin their contract in both repos first.

## Completed projects

- **OTA upgrade pipeline** (2026-04 → 2026-08) — padawan + master OTA over ESP-NOW/serial, recovery via USB, receiver watchdog, master self-flash (stack overflow fixed in PR #47), progress reporting (PR #49). Shipped in rel_1.2. Plans archive: `.docs/completed-plans/`.

## Log

- 2026-08-16 workflow bootstrap
  - adopted the task-file workflow (`.docs/agentic-workflow.md`), ported from AstrOs.Server; `PLAN.md` is now the authoritative status view — agent memory is a cache
  - `.docs/plans/` retired for new work in favor of `.docs/tasks/`; historical plans remain in `.docs/completed-plans/`
