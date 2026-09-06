# AstrOs ESP Firmware — Plan

Workflow rules: `CLAUDE.md` (Workflow section). Rationale and templates: `.docs/agentic-workflow.md`. Task IDs here are independent of AstrOs.Server's.

## Status

Active:  standalone tasks — Maestro servo-release fixes
Now:     T-001 — PR #52 open on develop; script-move bench cases passed 2026-09-05; review fix (slider arming) in progress, slider re-bench + timer-delta review item pending
Next:    T-002 (channels per-instance), then T-003 (state locking; depends on T-002)
Blocked: none
Last:    2026-09-05 — T-001 amended: accel-as-speed model was a dimensional error (4-min deadlines); replaced with physical trapezoid + floor, slack multiplier dropped

## Standalone tasks

- [ ] **T-001** — Fix CheckServos release math so scripted moves de-energize servos (`.docs/tasks/T-001-checkservos-release-timeout.md`)
- [ ] **T-002** — Move Maestro channel state into MaestroModule instances (`.docs/tasks/T-002-maestro-channels-per-instance.md`)
- [ ] **T-003** — Synchronize Maestro channel state between timer and command paths (`.docs/tasks/T-003-maestro-channel-state-sync.md`) — depends on T-002

## Backlog (unscheduled candidates)

- Known-fragile catalog: `.docs/code-review/code-review.md` (P0–P3) — headline items also listed in CLAUDE.md "Known-fragile areas" (timer-callback leaks/stack pressure, `peers` vector mutex, `AnimationController` unlocked reads, `setKeyId` peer-index assumption, cross-core globals). Promote individually as tasks.
- Queue consumers that fail to `free()` embedded pointers (same review catalog) — sweepable as one task or per-consumer.
- `MaestroModule::QueueCommand` passes a stale `lastPos` when re-arming a released servo — `lastPos` is only ever set by `HomeServos`, so the pre-speed/accel position command replays the home position, not the last commanded one. Found 2026-08-31 during the servo-release investigation; needs its own investigation before a fix task.

Cross-repo: AstrOs.Server's `PLAN.md` Backlog holds the server-side serial findings from the 2026-08-06 bench log (e.g., server discards master-emitted POLL_NAK). Wire-format changes, if any come out of that, pin their contract in both repos first.

## Completed projects

- **OTA upgrade pipeline** (2026-04 → 2026-08) — padawan + master OTA over ESP-NOW/serial, recovery via USB, receiver watchdog, master self-flash (stack overflow fixed in PR #47), progress reporting (PR #49). Shipped in rel_1.2. Plans archive: `.docs/completed-plans/`.

## Log

- 2026-09-05 T-001 amendment (pre-PR review)
  - the accel-for-speed substitution carried over from the old code was a dimensional error: Maestro accel is speed-units per 80 ms, not a speed cap. Accel 2 reaches speed 25 in 1 s; modeling it as speed 2 made a speed-20/accel-2 move (physically ~6.8 s) wait 240 s
  - replaced with the physical trapezoid/triangle model (`WorstCaseTravelMs`) and added `ServoReleaseDeadlineMs` = max(model × slack, 20 s floor). The floor keeps full-speed release at ~20 s (old broken math gave ~19 s, which the linear actuators were living with; first-pass T-001 had cut it to 1.9 s)
  - per-channel release-time setting (absolute ms, floor semantics) added to Backlog; removes the global floor when it lands
  - second pass: dropped the ×4 slack multiplier inherited from the old `/ 4`. Error sources are additive and sub-second; the 3000 vs ≤2000 µs guard range already gives 1.5×; the floor covers everything at speed ≥ 10. Deadline = max(model, 20 s)
  - PR #52 review (Copilot) found the slider path (`SetServoPosition`) never armed release tracking — pre-existing; a slider move on a released channel was never turned off. Fixed by arming with the script-path block (bounds-checked, no INFO log so drags don't spam). Second review item — timer callback passes a constant 300 ms rather than a measured delta, so the accumulator undercounts (late release only) — pending decision

- 2026-08-31 servo-release investigation
  - bench symptom: servos stay energized after scripted moves. Root cause: `CheckServos` accumulates a fractional double into the int `currentPos` — increments < 1 (effective speed ≤ 5, which includes any scripted accel 1–5 via the accel-substitution clamp) truncate to zero, so the Maestro off command never fires; modeled rate is also ~40× slower than physical travel
  - authored T-001 (fix math via pure `lib_native` helper + native tests), T-002 (de-globalize `channels[24]`), T-003 (timer-vs-command-path locking); noted stale-`lastPos` re-arm issue in Backlog

- 2026-08-16 workflow bootstrap
  - adopted the task-file workflow (`.docs/agentic-workflow.md`), ported from AstrOs.Server; `PLAN.md` is now the authoritative status view — agent memory is a cache
  - `.docs/plans/` retired for new work in favor of `.docs/tasks/`; historical plans remain in `.docs/completed-plans/`
