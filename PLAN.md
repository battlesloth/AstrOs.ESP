# AstrOs ESP Firmware — Plan

Workflow rules: `CLAUDE.md` (Workflow section). Rationale and templates: `.docs/agentic-workflow.md`. Task IDs here are independent of AstrOs.Server's.

## Status

Active:  standalone tasks — Maestro servo-release fixes
Now:     T-002 — in progress on `feature/T-002-maestro-channels-per-instance` (channels moved to a private member; native tests + both builds green; PR-toolkit review done, doc findings fixed; bench single-module regression + PR pending)
Next:    T-003 (state locking) or T-004 (panic → Maestro off, authored 2026-09-06); both depend on T-002, independent of each other
Blocked: none
Last:    2026-09-06 — T-001 complete: merged to develop via PRs #52, #54, #55; bench-verified

## Standalone tasks

- [x] **T-001** — Fix CheckServos release math so scripted moves de-energize servos (`.docs/tasks/completed/T-001-checkservos-release-timeout.md`) — done 2026-09-06
- [ ] **T-002** — Move Maestro channel state into MaestroModule instances (`.docs/tasks/T-002-maestro-channels-per-instance.md`)
- [ ] **T-003** — Synchronize Maestro channel state between timer and command paths (`.docs/tasks/T-003-maestro-channel-state-sync.md`) — depends on T-002
- [ ] **T-004** — Make panic stop de-energize every configured Maestro channel (`.docs/tasks/T-004-panic-stop-maestro-deenergize.md`) — depends on T-002; independent of T-003

## Backlog (unscheduled candidates)

- Known-fragile catalog: `.docs/code-review/code-review.md` (P0–P3) — headline items also listed in CLAUDE.md "Known-fragile areas" (timer-callback leaks/stack pressure, `peers` vector mutex, `AnimationController` unlocked reads, `setKeyId` peer-index assumption, cross-core globals). Promote individually as tasks.
- Queue consumers that fail to `free()` embedded pointers (same review catalog) — sweepable as one task or per-consumer.
- Upgrade to espressif32 7.x / ESP-IDF 6.x deliberately: migrate both `sdkconfig.<env>` files (IDF 6.1 kconfgen crashes on the committed IDF-5-era files — the 2026-08-31 CI outage), then lift the `espressif32@6.13.0` pin in `platformio.ini`. Own task; touches both boards.
- `MaestroModule::QueueCommand` passes a stale `lastPos` when re-arming a released servo — `lastPos` is only ever set by `HomeServos`, so the pre-speed/accel position command replays the home position, not the last commanded one. Found 2026-08-31 during the servo-release investigation; needs its own investigation before a fix task.

- **Panic stop does not reach PCA9685 (I²C) servo channels.** T-004 covers the Maestro; the I²C servo path has the same gap. Found 2026-09-06.
- **`MaestroModule` hygiene** (all pre-existing, found by T-002 review 2026-09-06): destructor never `vSemaphoreDelete`s `mutex` (leaks when `loadMaestroConfigs` erases a dropped module); `LoadConfig` ignores `loadMaestroServos`'s return and overlays without clearing, so entries past the parsed count keep stale state on reload (fix belongs with T-003's lock — clearing opens a window against the timer); `QueueCommand` lacks the `loading` gate `SetServoPosition` has and treats a pre-config channel as GPIO; six hard-coded `24`s including the `Panic` frame math (`cmd[74]`) want one named channel-count constant.

Cross-repo: AstrOs.Server's `PLAN.md` Backlog holds the server-side serial findings from the 2026-08-06 bench log (e.g., server discards master-emitted POLL_NAK). Wire-format changes, if any come out of that, pin their contract in both repos first.

## Completed projects

- **OTA upgrade pipeline** (2026-04 → 2026-08) — padawan + master OTA over ESP-NOW/serial, recovery via USB, receiver watchdog, master self-flash (stack overflow fixed in PR #47), progress reporting (PR #49). Shipped in rel_1.2. Plans archive: `.docs/completed-plans/`.

## Log

- 2026-09-06 T-001 complete — CheckServos release math (PRs #52, #54, #55 → develop)
  - `CheckServos` accumulates integer ms per channel; deadline = max(physical trapezoid/triangle model of the Maestro speed + accel units, 20 s floor). No multiplier — the 3000 µs guard range vs ≤2000 µs real sweep is the margin
  - slider path (`SetServoPosition`) now arms release tracking (was never turned off after a release) and validates the channel as the parsed `int` before narrowing to the wire byte
  - bench: speed 20 / accel 2 releases ~20 s (floor), speed 5 / accel 1 ~24.7 s (model), slider re-arm on a released channel; QA plan `.docs/qa/maestro-servo-release.md`
  - declined: measured timer delta in `servoShutdownTimerCallback` (constant 300 ms undercounts → late release only, accepted)

- 2026-09-05 T-001 amendment (pre-PR review)
  - the accel-for-speed substitution carried over from the old code was a dimensional error: Maestro accel is speed-units per 80 ms, not a speed cap. Accel 2 reaches speed 25 in 1 s; modeling it as speed 2 made a speed-20/accel-2 move (physically ~6.8 s) wait 240 s
  - replaced with the physical trapezoid/triangle model (`WorstCaseTravelMs`) and added `ServoReleaseDeadlineMs` = max(model × slack, 20 s floor). The floor keeps full-speed release at ~20 s (old broken math gave ~19 s, which the linear actuators were living with; first-pass T-001 had cut it to 1.9 s)
  - per-channel release-time setting (absolute ms, floor semantics) added to Backlog; removes the global floor when it lands
  - second pass: dropped the ×4 slack multiplier inherited from the old `/ 4`. Error sources are additive and sub-second; the 3000 vs ≤2000 µs guard range already gives 1.5×; the floor covers everything at speed ≥ 10. Deadline = max(model, 20 s)
  - PR #52 review (Copilot) found the slider path (`SetServoPosition`) never armed release tracking — pre-existing; a slider move on a released channel was never turned off. Fixed by arming with the script-path block (bounds-checked, no INFO log so drags don't spam). Second review item — timer callback passes a constant 300 ms rather than a measured delta, so the accumulator undercounts (late release only) — declined: late release is acceptable, constant stays
  - PR #52 merged 2026-09-06 (8b54d77) at 55c3128, before the slider fix. Follow-up PR #54 from the same branch carries slider arming + a review fix: `SetServoPosition` validated after narrowing to `uint8_t`, so channel 256 wrapped to 0 and passed — now validates the parsed `int` first

- 2026-08-31 servo-release investigation
  - bench symptom: servos stay energized after scripted moves. Root cause: `CheckServos` accumulates a fractional double into the int `currentPos` — increments < 1 (effective speed ≤ 5, which includes any scripted accel 1–5 via the accel-substitution clamp) truncate to zero, so the Maestro off command never fires; modeled rate is also ~40× slower than physical travel
  - authored T-001 (fix math via pure `lib_native` helper + native tests), T-002 (de-globalize `channels[24]`), T-003 (timer-vs-command-path locking); noted stale-`lastPos` re-arm issue in Backlog

- 2026-08-16 workflow bootstrap
  - adopted the task-file workflow (`.docs/agentic-workflow.md`), ported from AstrOs.Server; `PLAN.md` is now the authoritative status view — agent memory is a cache
  - `.docs/plans/` retired for new work in favor of `.docs/tasks/`; historical plans remain in `.docs/completed-plans/`
