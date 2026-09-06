# T-001: Fix CheckServos release math so scripted moves de-energize servos

<!-- File: .docs/tasks/T-001-checkservos-release-timeout.md. Branch: feature/T-001-checkservos-release-timeout.
     PR title: "T-001: Fix CheckServos release math so scripted moves de-energize servos". -->

## Context

Bench report (2026-08-31): servos stay energized (holding tension) after script moves. Root cause
traced in-session:

- `MaestroModule::CheckServos` (`lib/Modules/src/MaestroModule.cpp:190`) dead-reckons worst-case
  travel into `servo_channel.currentPos` and sends the Maestro off command (target 0 via
  `setServoOff`) once the accumulator crosses 3000 µs.
- `currentPos` is `int` (`lib_native/AstrOsUtility/src/AstrOsStructs.h`). The per-check increment
  `(.25 * speed * (msSinceLastCheck / 100)) / 4` works out to `0.1875 × effectiveSpeed` for the
  300 ms timer period. Assigning the double sum back into the int truncates toward zero, so any
  effective speed ≤ 5 adds 0 every check — the accumulator never advances and the servo is never
  turned off.
- Effective speed is substituted with acceleration when `0 < accel < speed`
  (`MaestroModule.cpp:209`). Scripts routinely use accel 1–5, so most scripted moves land in
  never-release territory. Full-speed moves (speed 0 → treated as 255) still release, which is why
  slider/bench testing doesn't reproduce it.
- Secondary: even without truncation the modeled rate is ~40× slower than physical travel — the
  Maestro speed unit is 0.25 µs per **10 ms** per unit, but the code divides ms by 100 and then by
  4 again. A speed-20 move would hold ~5 minutes before release.
- Latent footgun: `msSinceLastCheck / 100` integer division yields 0 for any period < 100 ms.

**Amended 2026-09-05** (first implementation reviewed before PR):

- The accel-for-speed substitution the first pass preserved is a dimensional error, not a
  conservative choice. Maestro acceleration is 0.25 µs / 10 ms / **80 ms** per unit — i.e. speed
  grows by `accel` speed-units every 80 ms. Accel 2 reaches speed 25 within a second; it only adds a
  short ramp at each end of a move and never caps cruise speed. Modeling it as a speed cap made a
  speed-20/accel-2 move (physically ~6.8 s over the guard range) wait 240 s, and any accel-1 move
  wait 480 s. The correct model is the standard trapezoid (ramp–cruise–ramp) with the triangle case
  when the cap is never reached.
- The first pass also cut the full-speed deadline from ~19 s (old broken math) to ~1.9 s. The
  ~19 s figure is what the linear actuators had been living with, so a 20 s floor is added to keep
  full-speed behavior where it was while the slow cases get fixed. The floor is a stand-in until a
  per-channel release setting exists (PLAN.md Backlog).
- Second pass, same day: the ×4 slack multiplier inherited from the old `/ 4` is dropped. Every
  real error source is a fixed few hundred ms (timer tick, serial latency, Maestro 80 ms accel
  steps), so a multiplier over-penalizes slow moves (speed 1: 480 s for an 80 s sweep) while
  adding nothing where it matters. The model already carries a proportional margin — the guard
  range is 3000 µs against a real sweep of ≤2000 µs — and the 20 s floor covers everything at
  speed ≥ 10. Deadline is simply `max(model, floor)`.
- Third pass (PR #52 review, Copilot): slider/direct moves go through `SetServoPosition`, which
  never armed release tracking — `on`, `currentPos`, `speed`, `acceleration` were untouched — so
  a slider move on an already-released channel was never turned off. Pre-existing, but this
  task's acceptance criteria claim slider release, so it is fixed here: arm in
  `SetServoPosition` with the same block the script and homing paths use. Slider input is a
  stream (tens of messages per second during a drag); each re-arm only resets the clock, so the
  servo releases 20 s after the *last* message and never mid-drag. No INFO log on this path (it
  would spam at drag rate). `SetServoPosition` previously never indexed `channels[]`, so a bad
  channel was harmless; with arming it would be an out-of-bounds write, so it is bounds-checked
  first, like `QueueCommand`.

## Contract (pinned — do not change)

- Maestro wire protocol unchanged: release remains `SET_SERVO_COMMAND` with target 0
  (`setServoOff`); no new Maestro commands, no position polling.
- `CheckServos(300)` continues to be called from `servoShutdownTimerCallback` (esp_timer task,
  `src/main.cpp`); it must not block.
- `servo_channel` struct layout unchanged. (Persisted servo config is text-parsed via
  `AstrOsFileUtils::parseServoConfig`, so layout is not a persistence contract — but this task
  doesn't need to touch it: `currentPos` is runtime-only and is repurposed as an integer
  elapsed-ms accumulator, same type.)
- PURE-lib purity: the new helpers live in `lib_native/AstrOsUtility` (`AstrOsServoUtils.hpp`),
  no ESP-IDF/FreeRTOS includes.
- One explicit named allowance: the floor (`MAESTRO_RELEASE_FLOOR_MS`) for loads whose slew the
  model does not describe. No multiplier. The model's own margin is the 0–3000 µs guard range
  (a real sweep is ≤2000 µs), stated in the helper's comment, not buried in unit math.

## Task

1. Rework `WorstCaseTravelMs(int speed, int acceleration)` in
   `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp` to the physical trapezoid model over the
   0–3000 µs guard range, using true Maestro units (speed: 0.25 µs / 10 ms per unit; accel:
   0.25 µs / 10 ms / 80 ms per unit), speed 0 → 255, inputs clamped to 0–255:
   - accel 0: `cruise = ceil(120000 / speed)` ms.
   - cap reached (`speed² ≤ 1500 × accel`): `cruise + ramp`, where `ramp = ceil(80 × speed / accel)` ms.
   - cap never reached (triangle): `ceil(sqrt(38 400 000 / accel))` ms (= 2·√(3000 µs / a)).
   - no multiplier; the guard range is the margin.
2. Add `ServoReleaseDeadlineMs(int speed, int acceleration)` =
   `max(WorstCaseTravelMs(speed, acceleration), MAESTRO_RELEASE_FLOOR_MS)` with the floor at
   20 000 ms. This is the policy layer `CheckServos` calls; `WorstCaseTravelMs` stays pure physics.
3. `CheckServos` accumulates **elapsed integer milliseconds** into `currentPos`
   (`currentPos += msSinceLastCheck`) and turns the channel off when
   `currentPos >= ServoReleaseDeadlineMs(channels[i].speed, channels[i].acceleration)`. No new
   struct fields; no `/ 100` or `/ 4` divisions anywhere on the path.
4. `CheckServos` comment block describes the ms-accumulator model and points at the helpers.
5. `SetServoPosition` (slider/direct path): reject `channel > 23`, then set `currentPos = 0`,
   `speed = 0`, `acceleration = 0`, `on = true` before sending the move. No INFO log.

## Acceptance criteria

- [x] Native tests in `test/test_native/astros_servo_utils_tests.cpp` cover `WorstCaseTravelMs`:
      speed-only (0→255, 1, 10, 20), trapezoid (20/2, 20/1, 10/2, 5/1, 10/50, 1/1), triangle
      (0/1, 0/2, 0/5, 200/1), the regime boundary (255/43 vs 255/44 continuous), and clamping —
      each asserting the exact expected ms.
- [x] Native tests cover `ServoReleaseDeadlineMs`: floor applied when the model is below 20 s
      (0/0, 20/2, 10/2), model wins when above (5/1, 5/0, 1/0).
- [x] Formerly-never-release cases are finite and physical: accel 1 with speed 200 → 6 197 ms
      (was 480 000); speed 20 / accel 2 → 6 800 ms (was 240 000); both then floored to 20 s.
- [x] `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build clean.
- [x] QA plan `.docs/qa/maestro-servo-release.md` updated with the new reference deadlines and a
      floor case.
- [x] Bench (human-gated, 2026-09-05): scripted move with speed 20 / accel 2 (the reported bug)
      → "Turning off servo N" ~20 s after the command (floor) and the servo is free by hand.
- [x] Bench (human-gated, 2026-09-05): scripted move with speed 5 / accel 1 → release ~24.7 s
      (model above the floor), proving the max() is wired.
- [ ] Bench (human-gated): slider move on a channel that has **already been released** turns
      off ~20 s after the last slider message; a continuous drag produces no release mid-drag
      and no per-message log lines; linear actuators complete their stroke before release.

## Out of scope

- File-scope `channels[24]` shared across module instances — T-002.
- Locking between the timer and command paths — T-003 (the slider path is now a third writer
  of the same fields, same pattern as `QueueCommand`; no new race class).
- `lastPos` never updated after moves (stale re-arm position in `QueueCommand`) — PLAN.md Backlog.
- Per-channel release-time setting (removes the global floor) — PLAN.md Backlog.
- A hold-tension option (see comment above `servoShutdownTimerCallback`) — future feature.

## Verification

```bash
pio test -e test                      # WorstCaseTravelMs + ServoReleaseDeadlineMs cases
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: script move with speed 20 / accel 2; serial monitor shows
# "Turning off servo N" ~20 s after "Setting servo N ..." (floor); servo de-energized after.
# bench: script move with speed 5 / accel 1; release ~24.7 s after (model above floor).
# bench: slider move (speed 0); "Turning off servo N" ~20 s after (floor).
```

## Implementation checklist

- [x] RED: native tests for `WorstCaseTravelMs` written and observed failing
- [x] GREEN: helper implemented in `AstrOsServoUtils.hpp`, tests pass
- [x] `CheckServos` reworked to integer-ms accumulation + deadline compare; comment updated
- [x] `pio test -e test` fully green
- [x] `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build clean
- [x] QA plan `.docs/qa/maestro-servo-release.md` written
- [x] clang-format clean on changed C++ files
- [x] PLAN.md status updated

Amendment (2026-09-05):

- [x] RED: tests updated to the trapezoid/triangle model + new `ServoReleaseDeadlineMs` floor tests; observed failing
- [x] GREEN: `WorstCaseTravelMs` reworked, `ServoReleaseDeadlineMs` added, `CheckServos` switched to it
- [x] `pio test -e test` fully green; both boards build clean
- [x] QA plan reference deadlines + floor case updated
- [x] clang-format clean on changed C++ files
- [x] PLAN.md: Backlog item for per-channel release setting; Status + Log updated

Amendment, second pass (2026-09-05, drop the multiplier):

- [x] RED: test expectations reduced to model-only values; deadline tests re-split around the floor; observed failing
- [x] GREEN: `MAESTRO_RELEASE_SLACK` deleted; `WorstCaseTravelMs` returns the physical model
- [x] `pio test -e test` fully green; both boards build clean; clang-format clean
- [x] QA plan reference table + cases updated; PLAN.md Status + Log updated

Third pass (2026-09-05, PR #52 review — slider arming):

- [ ] `SetServoPosition` bounds-checks the channel and arms release tracking; no INFO log
- [ ] both boards build clean; clang-format clean
- [ ] QA plan: released-channel slider case + noisy-drag case added
- [ ] PLAN.md Status + Log updated
