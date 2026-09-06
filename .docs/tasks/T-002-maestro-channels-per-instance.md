# T-002: Move Maestro channel state into MaestroModule instances

<!-- File: .docs/tasks/T-002-maestro-channels-per-instance.md. Branch: feature/T-002-maestro-channels-per-instance.
     PR title: "T-002: Move Maestro channel state into MaestroModule instances". -->

## Context

`servo_channel channels[24]` at `lib/Modules/src/MaestroModule.cpp:19` is a file-scope global
shared by every `MaestroModule` instance (instances live in the `maestroModules` map in
`src/main.cpp`). With more than one Maestro module configured:

- `LoadConfig()` for one module overwrites every module's channel config.
- Each module's `CheckServos` pass advances the same release accumulators — N modules advance
  them N× per timer cycle.
- `QueueCommand` / `HomeServos` on one module mutate state observed by all.

Single-module deployments mask all of this, which is why it hasn't bitten on the bench yet.
Found 2026-08-31 during the servo-release investigation (see T-001 Context).

Update 2026-09-06 (T-001 merged): `SetServoPosition` (the slider/direct path) now also writes
`currentPos` / `speed` / `acceleration` / `on` — a third writer alongside `QueueCommand` and
`HomeServos`. It indexes the same array and moves with it; nothing extra to do here, but T-003
must cover it when it adds the lock.

## Contract (pinned — do not change)

- `servo_channel` layout and the persisted text config format unchanged.
- Maestro wire protocol unchanged.
- Public `MaestroModule` API unchanged (`LoadConfig`, `QueueCommand`, `SetServoPosition`,
  `Panic`, `HomeServos`, `CheckServos`, `UpdateConfig`, constructor signature).

## Task

Make `channels` a private zero-initialized member array of `MaestroModule`; update all references
in `MaestroModule.cpp`. Pure ownership refactor — no behavior change for single-module setups.

## Acceptance criteria

- [ ] No file-scope channel state remains in `MaestroModule.cpp`; each instance owns its array.
- [ ] `pio test -e test` green; both board environments build clean.
- [ ] Bench regression (single module, human-gated): home-on-boot, scripted move, and release
      behave exactly as before.
- [ ] QA plan `.docs/qa/maestro-servo-release.md` gains a multi-module config case (execution
      human-gated on second-module hardware availability).

## Out of scope

- CheckServos release math — T-001.
- Locking around channel state — T-003 (builds on this task).

## Verification

```bash
pio test -e test
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: single-module regression per .docs/qa/maestro-servo-release.md
```

## Implementation checklist

- [ ] `channels` becomes a private zero-initialized member of `MaestroModule` (header gains the
      `servo_channel` include); file-scope global removed
- [ ] every reference in `MaestroModule.cpp` resolves to the member (no code change needed
      beyond the declaration move — verify by diff)
- [ ] `pio test -e test` green; `pio run -e lolin_d32_pro` + `pio run -e metro_s3` clean, no
      new warnings in changed files; clang-format clean
- [ ] QA plan: multi-module config case added (human-gated on second-module hardware)
- [ ] PLAN.md Status updated; bench single-module regression (human-gated)
