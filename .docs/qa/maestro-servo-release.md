# QA: Maestro servo release (auto de-energize)

Covers the servo-shutdown dead-reckoning in `MaestroModule::CheckServos` (T-001).
Deadline model: `WorstCaseTravelMs(speed, accel)` = ceil(480000 / effectiveSpeed) ms,
where effectiveSpeed = speed (0 → 255), replaced by accel when `0 < accel < speed`.
The shutdown timer checks every 300 ms, so observed release ≈ deadline + ≤300 ms.

Reference deadlines: speed 0/accel 0 → ~1.9 s · speed 20/accel 0 → 24 s ·
speed 10/accel 0 → 48 s · any speed/accel 2 → 240 s (4 min, deliberately conservative).

## Preconditions

- Board flashed with a build containing T-001; serial monitor attached (115200).
- Maestro module configured with ≥1 enabled servo channel and ≥1 GPIO (non-servo) channel.
- At least one script on the SD card that moves a servo with explicit speed/accel values.

## Test cases

1. **Home-and-release on boot**
   - Power the node; wait for homing.
   - Expected: servos move home; within ~2.2 s each enabled servo logs
     `Turning off servo N` and is free to move by hand (no holding torque).

2. **Full-speed move releases in seconds**
   - Send a slider/direct move (speed 0, accel 0).
   - Expected: move completes; `Turning off servo N` within ~2.2 s of the command;
     servo free by hand. (Pre-T-001 this took ~19 s.)

3. **Scripted move with speed limit (regression for the truncation bug)**
   - Run a script move with speed 20, accel 0.
   - Expected: `Turning off servo N` ~24 s after the command; servo free by hand.
     (Pre-T-001: never released for speed ≤ 5; minutes for mid speeds.)

4. **Scripted move with low accel (the reported bug)**
   - Run a script move with accel 2 (any speed).
   - Expected: `Turning off servo N` at ~4 min; servo then free by hand.
     Pre-T-001 this NEVER released. Note: the accel-substitution deadline is
     deliberately conservative; if 4 min proves too long on the bench, tune
     `MAESTRO_RELEASE_SLACK` / the accel model (see T-001 notes) — do not
     hand-edit deadlines elsewhere.

5. **New command resets the clock**
   - Send a second move to the same servo ~10 s after a speed-20 move.
   - Expected: servo stays energized across the second move; single
     `Turning off servo N` ~24 s after the *second* command, not the first.

6. **GPIO channels are never auto-released**
   - Toggle a non-servo (GPIO) channel on.
   - Expected: no `Turning off servo N` for that channel at any point; output holds.

7. **Panic overrides the timer**
   - Start a slow scripted move, then send panic stop.
   - Expected: all channels off immediately; no stray `Turning off servo N`
     afterward for the panicked channels.

## Edge cases / negative tests

- **Out-of-range speed/accel** (hand-crafted command with speed > 255 or negative):
  servo still releases within the speed-255 deadline (~2 s) — inputs are clamped.
- **Servo commanded to its current position** (no physical motion): still releases
  on the same deadline — the model is time-based, not motion-based.
