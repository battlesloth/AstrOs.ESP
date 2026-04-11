# Implementation Plans

This directory is the canonical location for implementation plans referenced by the **Planning** section of `CLAUDE.md`. Plans here are the durable, in-repo record of what was done and why — they survive session crashes, context loss, and handoffs between Claude Code sessions.

## When to add a plan

See `CLAUDE.md` — the short version is:

- **Quick changes** (typo fixes, logging tweaks, copy changes, stack-size bumps driven by high-water-mark warnings): no plan needed.
- **Light plans** (3–5 tasks, 2–3 files, self-contained bug fix or small feature): short plan file committed before implementation.
- **Full plans** (multi-layer features, anything that touches wire format + queues + hardware): brainstorming first, then a full plan file committed before implementation.

## Naming convention

`YYYYMMDD-HHmm-feature-name.md` using the `YYYYMMDD-HHmm` format — for example:

```
20260327-1500-ota-upgrades-phase1-api.md
20260330-0915-maestro-hotplug.md
```

The timestamp component is based on when the plan was written, not when implementation finishes.

## Workflow

1. Write the plan file in this directory with a checklist of discrete tasks (`- [ ]`).
2. **Commit the plan file before touching implementation code.** This is the critical step — it makes the plan durable across sessions.
3. As each task is completed, check off its box (`- [x]`) and commit the update.
4. If a session is interrupted, the next session reads the plan file to determine what has been done and what remains.

## Relationship to `~/.claude/plans/`

Claude Code's in-session plan mode writes ephemeral working drafts to `~/.claude/plans/<session>.md`. Before leaving plan mode and starting implementation, copy the finalized plan here and commit it. The session file is scratch; this directory is the source of truth.

## Phasing large work

For features that span more than ~8 discrete tasks, break the work into phases with separate plan files. For firmware features, the natural phasing follows queue/task seams:

- **Phase 1** — wire format + native tests (`lib/AstrOsMessaging`, `test/test_native/`)
- **Phase 2** — queue producer/consumer wiring (`src/main.cpp`, `lib/Modules`)
- **Phase 3** — hardware integration + QA plan

Each phase should compile, ship, and be testable on its own.
