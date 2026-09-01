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

## Contract (pinned — do not change)

- Maestro wire protocol unchanged: release remains `SET_SERVO_COMMAND` with target 0
  (`setServoOff`); no new Maestro commands, no position polling.
- `CheckServos(300)` continues to be called from `servoShutdownTimerCallback` (esp_timer task,
  `src/main.cpp`); it must not block.
- `servo_channel` struct layout unchanged. (Persisted servo config is text-parsed via
  `AstrOsFileUtils::parseServoConfig`, so layout is not a persistence contract — but this task
  doesn't need to touch it: `currentPos` is runtime-only and is repurposed as an integer
  elapsed-ms accumulator, same type.)
- PURE-lib purity: the new helper lives in `lib_native/AstrOsUtility` (`AstrOsServoUtils.hpp`),
  no ESP-IDF/FreeRTOS includes.
- The intentional extra allowance for slow linear actuators is preserved as one explicit named
  slack multiplier, not buried in unit math.

## Task

1. Add a pure helper to `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp`:
   `int WorstCaseTravelMs(int speed, int acceleration)` — worst-case time to cover the 0–3000 µs
   guard range using the true Maestro unit (0.25 µs per 10 ms per speed unit), mapping speed 0 →
   255 (no limit), substituting accel for speed when `0 < accel < speed` (keeps today's
   conservative intent), and applying a named slack multiplier (`MAESTRO_RELEASE_SLACK`, initial
   value 4 — tune on bench with the linear actuators).
2. Rework `CheckServos` to accumulate **elapsed integer milliseconds** into `currentPos`
   (`currentPos += msSinceLastCheck` — integral, no truncation loss possible) and turn the channel
   off when `currentPos >= WorstCaseTravelMs(channels[i].speed, channels[i].acceleration)`.
   Deadline is computed per check from the stored per-channel speed/accel — no new struct fields.
3. Remove the `/ 100` and `/ 4` divisions entirely; no path where a sub-period call zeroes out.
4. Update the misleading comment block in `CheckServos` to describe the ms-accumulator model.

## Acceptance criteria

- [x] Native tests in `test/test_native/astros_servo_utils_tests.cpp` cover `WorstCaseTravelMs`:
      speed 0 (→255), speed 1, speed 255, high speed + accel 1 (substitution), accel ≥ speed
      (no substitution), accel 0 (no substitution) — each asserting the exact expected ms.
- [x] Formerly-broken case is finite and correct: speed 1 → 120 000 ms × slack; accel 1 with any
      speed → same.
- [x] `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build clean.
- [x] QA plan `.docs/qa/maestro-servo-release.md` created with release-timing cases.
- [ ] Bench (human-gated): scripted move with accel 1–5 → "Turning off servo N" appears on the
      monitor within the computed deadline and the servo is free to move by hand.
- [ ] Bench (human-gated): full-speed slider move releases in a few seconds, not ~19 s.

## Out of scope

- File-scope `channels[24]` shared across module instances — T-002.
- Locking between the timer and command paths — T-003.
- `lastPos` never updated after moves (stale re-arm position in `QueueCommand`) — PLAN.md Backlog.
- A hold-tension option (see comment above `servoShutdownTimerCallback`) — future feature.

## Verification

```bash
pio test -e test                      # includes new WorstCaseTravelMs cases
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: run a script move with speed ~20 / accel 2; serial monitor shows
# "Turning off servo N" within WorstCaseTravelMs; servo de-energized after.
```

## Implementation checklist

<!-- Added when work starts. -->

- [x] RED: native tests for `WorstCaseTravelMs` written and observed failing
- [x] GREEN: helper implemented in `AstrOsServoUtils.hpp`, tests pass
- [x] `CheckServos` reworked to integer-ms accumulation + deadline compare; comment updated
- [x] `pio test -e test` fully green
- [x] `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build clean
- [x] QA plan `.docs/qa/maestro-servo-release.md` written
- [x] clang-format clean on changed C++ files
- [x] PLAN.md status updated
