# QA: Maestro servo release (auto de-energize)

Covers the servo-shutdown dead-reckoning in `MaestroModule::CheckServos` (T-001).

Deadline model: `ServoReleaseDeadlineMs(speed, accel)` = max(`WorstCaseTravelMs`, 20 s floor).
`WorstCaseTravelMs` is the physical trapezoid over the 0–3000 µs guard range using true
Maestro units, times a slack of 4: cruise = 120000 / speed ms (speed 0 → 255); accel adds a
ramp of 80 × speed / accel ms (accel is speed-units per 80 ms, **not** a speed cap); when the
cap is never reached the move is a triangle, sqrt(38 400 000 / accel) ms.
The shutdown timer checks every 300 ms, so observed release ≈ deadline + ≤300 ms.

Reference deadlines (model × 4, floored at 20 s):

| speed / accel | model × 4 | deadline | note |
|---|---|---|---|
| 0 / 0 (slider, boot homing) | 1.9 s | **20 s** | floor; matches ~19 s pre-T-001 |
| 0 / 1 | 24.8 s | 24.8 s | triangle; model wins |
| 0 / 2 … 5 | 17.5 … 11 s | **20 s** | floor |
| 20 / 0 | 24 s | 24 s | |
| 20 / 2 | 27.2 s | 27.2 s | the reported bug case; never released pre-T-001 |
| 10 / 2 | 49.6 s | 49.6 s | |
| 5 / 1 | 97.6 s | 97.6 s | speed 5 is ~11°/s — genuinely slow |
| 1 / 0 | 480 s | 480 s | speed 1 is 2.25°/s; 120 s physical |

## Preconditions

- Board flashed with a build containing T-001 (amended: trapezoid model + floor); serial monitor
  attached (115200). Log lines carry ms-since-boot; subtract `Setting servo N …` from
  `Turning off servo N` to get the observed release time.
- Maestro module configured with ≥1 enabled servo channel and ≥1 GPIO (non-servo) channel.
- At least one script on the SD card that moves a servo with explicit speed/accel values.
- Optional but recommended: Maestro USB to a laptop with Maestro Control Center open on the
  Status tab. When the release lands, the channel's target drops to 0 and its Enabled box
  unchecks — proof the Maestro received the off command, not just that the ESP sent it.

## Test cases

1. **Home-and-release on boot (floor)**
   - Power the node; wait for homing.
   - Expected: servos move home; ~20.3 s after `Homing Servos` each enabled servo logs
     `Turning off servo N` and is free to move by hand (no holding torque).

2. **Full-speed move releases at the floor**
   - Send a slider/direct move (speed 0, accel 0).
   - Expected: move completes; `Turning off servo N` ~20.3 s after the command; servo free by
     hand. Same as pre-T-001 (~19 s). **On linear actuators:** the stroke must complete well
     before release — if it is cut short, that is the tuning signal for the floor / per-channel
     setting (PLAN.md Backlog).

3. **Scripted move with speed limit**
   - Run a script move with speed 20, accel 0.
   - Expected: `Turning off servo N` ~24.3 s after the command; servo free by hand.
     (Pre-T-001: never released for speed ≤ 5; minutes for mid speeds.)

4. **Scripted move with low accel (the reported bug)**
   - Run a script move with speed 20, accel 2.
   - Expected: `Turning off servo N` ~27.5 s after the command; servo free by hand.
     Pre-T-001 this NEVER released. Accel only adds a short ramp; if release lands minutes
     later, the accel-as-speed model has crept back in.

5. **Floor vs model crossover**
   - Send a full-speed move with accel 1, then another with accel 2.
   - Expected: accel 1 releases at ~25 s (model above floor); accel 2 releases at ~20.3 s
     (floor). Proves the max() is wired, not just one of the two.

6. **New command resets the clock**
   - Send a second move to the same servo ~10 s after a speed-20 move.
   - Expected: servo stays energized across the second move; single
     `Turning off servo N` ~24.3 s after the *second* command, not the first.

7. **GPIO channels are never auto-released**
   - Toggle a non-servo (GPIO) channel on.
   - Expected: no `Turning off servo N` for that channel at any point; output holds.

8. **Panic overrides the timer**
   - Start a slow scripted move, then send panic stop.
   - Expected: all channels off immediately; no stray `Turning off servo N`
     afterward for the panicked channels.

## Edge cases / negative tests

- **Out-of-range speed/accel** (hand-crafted command with speed > 255 or negative):
  inputs are clamped; servo releases at the floor (~20 s).
- **Servo commanded to its current position** (no physical motion): still releases
  on the same deadline — the model is time-based, not motion-based.
- **Tuning knobs** live in `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp` only:
  `MAESTRO_RELEASE_SLACK` (model uncertainty) and `MAESTRO_RELEASE_FLOOR_MS` (slow loads).
  Do not hand-edit deadlines elsewhere.
