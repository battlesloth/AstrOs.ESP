# T-006: Re-energize released Maestro servos at their last commanded position

<!-- File: .docs/tasks/T-006-maestro-reenergize-at-last-position.md. Branch: feature/T-006-maestro-reenergize-at-last-position.
     PR title: "T-006: Re-energize released Maestro servos at their last commanded position".
     Everything above "Implementation checklist" is written and committed BEFORE implementation code.
     Sizing rules and rationale: .docs/agentic-workflow.md §1. -->

## Context

Bench observation 2026-09-14: an inverted Maestro servo commanded 0 → 100 (its `maxPos` →
`minPos`) jumps at the start of the move instead of travelling at the scripted speed.

Every scripted move (`MaestroModule::QueueCommand` → `setServoPosition`,
`lib/Modules/src/MaestroModule.cpp:553-616`) puts four frames on the Maestro UART:
`SET_TARGET lastPos` → `SET_SPEED` → `SET_ACCEL` → `SET_TARGET target`. The first frame exists
because a released servo (target 0) has no position as far as the Maestro is concerned — Pololu
user's guide §5.e: *"as soon as the servo is turned off, the Maestro has no way of knowing where
it is, so it will immediately move to any new target"* — so the servo must be re-energized
*where it physically is* before speed/accel can shape the real move. Since T-005 every servo is
released 5 s after its last move, so nearly every scripted move starts from the off state and
**the servo jumps instantly to wherever frame 1 says**. Frame 1 is wrong in two ways:

1. **Units.** `setServoPosition` sends `lastpos` raw (`:574-575`) but scales the real target
   `ms * 4` (`:600`, Maestro targets are quarter-µs). `lastPos` is stored in µs like `home` /
   `minPos` / `maxPos`, so a home of 1500 goes out as `0x84 ch 0x5C 0x0B` = 375 µs — below any
   servo's range; the Maestro clamps it to the channel's Control-Center Min (default 992 µs) or
   emits it as-is, and either way the servo slams to its **low-µs end**. Both lines date from the
   initial Maestro commit (`4d67e56`, 2025-01-01).
2. **Stale value.** `lastPos` is written only by config load (`minPos`,
   `lib_native/AstrOsUtility/src/AstrOsFileUtils.hpp:211`) and `HomeServos` (`home`, `:418`).
   Neither `QueueCommand` nor the slider path `SetServoPosition` records the target it just sent,
   so even with the units fixed frame 1 would say "go to home", not "stay put". (PLAN.md Backlog
   item, found 2026-08-31.)

Why inverted 0 → 100 is the visible case: inverted position 0 = `maxPos`, so the servo is parked
at the high-µs end and frame 1 yanks it the full range to the low end; the real target
(`minPos`) is then right where it landed, so speed/accel never shape anything — the jump *is*
the move. Non-inverted 100 → 0 should show the identical jump; the opposite directions show at
most a small twitch toward the low end followed by a smooth move.

Once `lastPos` is correct, frame 1 is a no-op when the servo is still on (the Maestro is already
holding, or heading to, that target under the previous limits) and a zero-motion re-energize
when it is off. After a release the servo may have been moved by hand or by load; the last
commanded target is the best estimate available without reading the Maestro back (see Out of
scope).

GPIO-type channels: `QueueCommand` passes `lastPos` for them too, so today every GPIO command
sends a raw-`minPos` frame (always < 6000 quarter-µs → LOW) ahead of the real level. Scaling it
would turn that into a HIGH blip whenever `minPos ≥ 1500`. The Maestro applies no speed or
acceleration to digital outputs (§5.e, Set Speed), so the pre-position frame has no purpose on a
GPIO channel and this task stops sending it there.

## Contract (pinned — do not change)

- Maestro wire protocol (Pololu compact protocol) unchanged: `0x84 ch lo hi`, target in
  quarter-µs, 7 bits per byte, 0 = off. This task makes frame 1 *conform* to it.
- `setServoPosition` frame order (pre-position → speed → accel → target), its `-1` (skip the
  pre-position frame) and `0` (explicit off first, `HomeServos`) sentinels, and the `SendStage`
  return semantics unchanged; `reconcileLimits` unchanged.
- T-003 locking discipline: lock order send → state; `stateMutex` never held across a blocking
  enqueue; `CheckServos` zero-wait only. Any new state write happens under `stateMutex` with the
  bounded `STATE_MUTEX_WAIT_MS` take and a log on timeout.
- Release tracking untouched: `on`, `currentPos`, `speed`, `acceleration`, `CheckServos`,
  `ServoReleaseDeadlineMs`, `Panic`. `lastPos` is read by `QueueCommand` only.
- `servo_channel` layout (`lib_native/AstrOsUtility/src/AstrOsStructs.h`) unchanged; `lastPos`
  stays an `int` in µs.
- Script command format (`MaestroCommand` pipe template) and the servo config file format
  (`id|set|min|max|home|inverted`) unchanged.
- PURE-lib purity for the new encoder in `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp`.

## Task

1. **PURE encoder.** `EncodeMaestroTarget(int us, uint8_t &lo, uint8_t &hi)` in
   `AstrOsServoUtils.hpp`: µs → quarter-µs, split into two 7-bit bytes; 0 encodes as `0, 0`.
   Native tests first (RED): 0 → `{0x00, 0x00}`; 500 → `{0x50, 0x0F}`; 1500 → `{0x70, 0x2E}`;
   2500 → `{0x10, 0x4E}`.
2. **`setServoPosition`.** Rename the parameters to `targetUs` / `lastPosUs`; both `SET_TARGET`
   frames go through `EncodeMaestroTarget`. Sentinels and stages unchanged. Update the header
   comment (`MaestroModule.hpp`) and the in-function comment to say the pre-position frame is in
   µs like the target.
3. **`QueueCommand`.** GPIO-type channels (`!isServo`) call `setServoPosition` with
   `lastPosUs = -1` (no pre-position frame); servo channels pass `lastPos` as today. Add
   `lastPos` to the existing `Setting servo …` INFO line so the bench can see what the servo is
   re-energized at. After `setServoPosition` returns `SendStage::Target`, record the sent target
   as the channel's `lastPos` (step 5). On any earlier stage leave `lastPos` alone — the servo is
   either still at the old `lastPos` (frame 1 went out) or untouched (it did not).
4. **`SetServoPosition` (slider).** After `SendStage::Target`, record `ms` as `lastPos` the same
   way. No pre-position frame on this path (unchanged); no INFO log (unchanged).
5. **`recordLastPos(int channel, int us)`.** Private; caller holds the send mutex; bounded
   `stateMutex` take (`STATE_MUTEX_WAIT_MS`), sets `channels[channel].lastPos`, gives. On timeout
   `ESP_LOGW` that the next move on this channel re-energizes at the previous target. Documented
   in the header's locking comment next to `reconcileLimits` (its success-path twin).
6. **QA + plan.** Add the cases below to `.docs/qa/maestro-servo-release.md` (re-energize is the
   other half of the release lifecycle; no new plan file). `PLAN.md`: T-006 checkbox under
   Standalone tasks; drop the stale-`lastPos` Backlog line.

## Acceptance criteria

- [ ] Native tests for `EncodeMaestroTarget` (the four vectors above) pass; `pio test -e test`
      green.
- [ ] `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build clean; clang-format clean.
- [ ] `QueueCommand` for a servo channel logs `lastPos` and, after a complete send, the next
      command on that channel logs the previous command's target as its `lastPos`.
- [ ] Bench, the reported bug: inverted servo at position 0 (`maxPos`), wait for
      `Turning off servo N`, then script `100` at speed 10 / accel 0 → the servo departs from
      `maxPos` and travels smoothly to `minPos` over ~(range / 0.25) ms (≈ 4 s for a 1000 µs
      range); no initial jump. Console shows `… to <minPos> … lastPos: <maxPos>`.
- [ ] Bench, mirror: non-inverted servo at 100 (`maxPos`), released, then `0` at speed 10 →
      same smooth travel, no jump.
- [ ] Bench, slider hand-off: drag a servo to roughly mid-range, wait for release, script `100`
      at speed 10 → departs from the slider position (log `lastPos` ≈ the last slider µs).
- [ ] Bench, still-on: script `0`, then within 5 s script `100` at speed 10 → smooth reversal,
      no jump (frame 1 is a no-op while on).
- [ ] Bench, GPIO (human-gated): a relay/LED channel commanded on twice in a row holds its state
      with no blip; commanded off → on → off toggles as before.

## Out of scope

- Reading the true position back from the Maestro (`GET_SERVO_POSITION_COMMAND 0x90`) instead
  of estimating from the last target — needs the Maestro RX path; its own task if the estimate
  ever proves insufficient (servo moved by hand/load while released).
- `HomeServos` sets `lastPos = home` before its frames go out, so a dropped home target leaves
  `lastPos` claiming home while the servo sits off at its old position. Pre-existing, boot /
  reload path only; note for the `MaestroModule` hygiene Backlog item.
- The rest of the `MaestroModule` hygiene Backlog item (reload semantics, `loading` gate on
  `QueueCommand`, `UpdateConfig` spin, channel-count constant).
- PCA9685 (I²C) servo path — no inversion, no release, no pre-position frame.
- Any change to what `lastPos` means for GPIO channels (they no longer consume it).

## Verification

```bash
pio test -e test                      # EncodeMaestroTarget vectors + existing suites green
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench (master console captured, an inverted servo channel configured, speed 10 / accel 0 scripts):
#   position 0 → wait for "Turning off servo N" → position 100: smooth maxPos → minPos, no jump;
#     console "Setting servo N … lastPos: <maxPos>"
#   non-inverted servo: 100 → release → 0: same
#   slider to mid → release → script 100: departs from the slider position
#   script 0 → (within 5 s) script 100: smooth reversal
#   GPIO channel: on, on → holds; off → on → off toggles
```

## Implementation checklist

- [x] RED: `EncodeMaestroTarget` tests (0, 500, 1500, 2500) added to
      `test/test_native/astros_servo_utils_tests.cpp`, observed failing
- [x] GREEN: `EncodeMaestroTarget` in `AstrOsServoUtils.hpp`; `pio test -e test` green (495/495)
- [ ] `setServoPosition`: `targetUs` / `lastPosUs`, both `SET_TARGET` frames via the encoder;
      comments updated (.cpp + .hpp)
- [ ] `recordLastPos` added; `QueueCommand` (servo channels only, GPIO passes -1, `lastPos` in
      the INFO line) and `SetServoPosition` record after `SendStage::Target`
- [ ] both boards build clean; clang-format clean on changed files
- [ ] QA plan cases added to `.docs/qa/maestro-servo-release.md`
- [ ] PLAN.md Status updated; PR opened
- [ ] bench: reported bug, mirror, slider hand-off, still-on, GPIO (human-gated)
