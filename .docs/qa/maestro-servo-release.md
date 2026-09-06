# QA: Maestro servo release (auto de-energize)

Covers the servo-shutdown dead-reckoning in `MaestroModule::CheckServos` (T-001), the
per-instance channel state that backs it (T-002), and the locking between the shutdown timer
and the command paths (T-003).

Deadline model: `ServoReleaseDeadlineMs(speed, accel)` = max(`WorstCaseTravelMs`, 20 s floor).
`WorstCaseTravelMs` is the physical trapezoid over the 0–3000 µs guard range using true
Maestro units: cruise = 120000 / speed ms (speed 0 → 255); accel adds a ramp of
80 × speed / accel ms (accel is speed-units per 80 ms, **not** a speed cap); when the cap is
never reached the move is a triangle, sqrt(38 400 000 / accel) ms. No multiplier — the guard
range (3000 µs vs a real sweep of ≤2000 µs) is the margin.
The shutdown timer checks every 300 ms and counts its first tick after a command as a full
300 ms, so observed release lands between deadline − 300 ms and deadline + one tick, plus a
small late drift (measured 2026-09-06: 19.81–20.04 s for the 20 s floor).

Reference deadlines (model, floored at 20 s):

| speed / accel | model | deadline | note |
|---|---|---|---|
| 0 / 0 (slider, boot homing) | 0.5 s | **20 s** | floor; matches ~19 s pre-T-001 |
| 0 / 1 | 6.2 s | **20 s** | floor |
| 20 / 0 | 6 s | **20 s** | floor |
| 20 / 2 | 6.8 s | **20 s** | floor; the reported bug case, never released pre-T-001 |
| 10 / 2 | 12.4 s | **20 s** | floor |
| 5 / 0 | 24 s | 24 s | model wins |
| 5 / 1 | 24.4 s | 24.4 s | model wins; speed 5 is ~11°/s |
| 1 / 0 | 120 s | 120 s | speed 1 is 2.25°/s; 80 s physical sweep |

## Preconditions

- Board flashed with a build containing T-001 (amended: trapezoid model + floor) and T-002;
  serial monitor attached (115200). Log lines carry ms-since-boot; subtract
  `Setting servo N …` from `Turning off servo N` to get the observed release time. Both lines
  carry the module id right after the servo number (T-002): `Setting servo N on module M
  (min: …` and `Turning off servo N on module M`. The short forms quoted in cases 1–7 are
  prefixes of the full lines.
- Maestro module configured with ≥1 enabled servo channel and ≥1 GPIO (non-servo) channel.
- At least one script on the SD card that moves a servo with explicit speed/accel values.
- For T-003 cases 9–11 in one run: the **`T-003 QA`** script in the server's local database
  (`AstrOs.Server/.data/database.sqlite3`, id `s1788696QAq`, four body-Maestro servos):
  phase A (0–35 s) moves all four at t=0, then hammers channel 1 every 0.5 s while channels 2–4
  come due together at ~20 s; phase B (40–123 s) moves all four at 40 s, then each channel
  again every 20.0 / 20.1 / 20.2 / 20.3 s (ch1–ch4) so a move lands inside the release tick each
  round; home at 123 s. Deploy it to the body location and run it with the monitor attached.
  Pass: no `Turning off servo N on module 1` *after* a newer `Setting servo N on module 1` for
  that channel (a release 19.7–20.1 s after the latest move is normal; the body Maestro is
  module idx 1); channels 2–4 release once at ~20 s during phase A; no `state mutex timeout`
  line anywhere; all four home at 123 s and release ~20 s later. First run 2026-09-06:
  89 moves, 22 releases, gaps 19.81–20.04 s, 0 stale releases, 0 WARN/ERROR — pass.
- Optional but recommended: Maestro USB to a laptop with Maestro Control Center open on the
  Status tab. When the release lands, the channel's target drops to 0 and its Enabled box
  unchecks — proof the Maestro received the off command, not just that the ESP sent it.

## Test cases

1. **Home-and-release on boot (floor)**
   - Power the node; wait for homing.
   - Expected: servos move home; ~20.3 s after `Homing Servos` each enabled servo logs
     `Turning off servo N` and is free to move by hand (no holding torque).

2. **Slider move on an already-released channel re-arms release**
   - Wait for a servo to log `Turning off servo N` (e.g. ~20 s after boot homing). Then send a
     single slider move to that servo.
   - Expected: servo moves; `Turning off servo N` ~20.3 s after the slider message; servo free
     by hand. Pre-T-001 (third pass) the slider path never armed tracking, so a released
     servo stayed energized indefinitely after a slider move. **On linear actuators:** the
     stroke must complete well before release — if it is cut short, that is the tuning signal
     for the floor / per-channel setting (PLAN.md Backlog).

2b. **Continuous slider drag (noisy stream)**
   - Drag the slider back and forth for ~30 s (well past the 20 s floor), then stop.
   - Expected: no `Turning off servo N` for that channel during the drag — every message
     resets the clock; exactly one `Turning off servo N` ~20.3 s after the *last* slider
     message; no per-message log lines from `MaestroModule` during the drag (only the
     interface-level ones that already existed).

3. **Scripted move with low accel (the reported bug)**
   - Run a script move with speed 20, accel 2.
   - Expected: `Turning off servo N` ~20.3 s after the command (floor); servo free by hand.
     Pre-T-001 this NEVER released. Accel only adds a short ramp; if release lands minutes
     later, the accel-as-speed model has crept back in.

4. **Slow scripted move uses the model (floor vs model crossover)**
   - Run a script move with speed 5, accel 1.
   - Expected: `Turning off servo N` ~24.7 s after the command — above the floor, so the
     model is what fired. Together with case 3 this proves the max() is wired.
     (Pre-T-001: never released for speed ≤ 5.)

5. **New command resets the clock**
   - Send a second move to the same servo ~10 s after a speed-5 move.
   - Expected: servo stays energized across the second move; single
     `Turning off servo N` ~24.3 s after the *second* command, not the first.

6. **GPIO channels are never auto-released**
   - Toggle a non-servo (GPIO) channel on.
   - Expected: no `Turning off servo N` for that channel at any point; output holds.

7. **Panic stop does not reach the Maestro (documents current behavior)**
   - Start a slow scripted move, then send panic stop.
   - Expected today: the script halts (no further commands dispatched), but the in-flight
     servo move completes to its target and releases on the normal deadline.
     `handlePanicStop` only calls the animation controller's panic; `MaestroModule::Panic()`
     (all channels off) has no caller. Verified 2026-09-06 during T-002 review. T-004 wires
     panic to the Maestro; it rewrites this case when it lands.

8. **Two Maestro modules keep independent channel state** (T-002; human-gated on a second
   Maestro being wired to serial channel 2)
   - **Run this on a padawan.** The master reserves UART 1 for the server link, and
     `loadMaestroConfigs` rejects a Maestro module on UART 1 there — on a master only module 1
     would be created and the case would prove nothing.
   - Configure two Maestro modules (idx 0 on serial 1, idx 1 on serial 2) with different servo
     configs — e.g. module 0 channel 0 as a servo with home 1500, module 1 channel 0 as a servo
     with home 2000 and a different min/max.
   - Power the node; wait for homing.
   - Expected: each module homes *its own* channel 0 to *its own* home value. Pre-T-002, the
     second `LoadConfig` overwrote the shared array, so both modules homed to the last-loaded
     config.
   - **Wait for the boot-homing releases to land on both modules** (`Turning off servo 0 on
     module 0` and `… on module 1`, ~20 s after homing — case 1). Only then continue;
     otherwise the boot release will be mistaken for a failure below.
   - Send a script move to module 0 channel 0 only.
   - Expected: `Setting servo 0 on module 0 …`; only module 0's servo moves. **Exactly one**
     `Turning off servo 0 on module 0` ~20 s after the script move, and no
     `Turning off servo 0 on module 1` at all; module 1's servo neither moves nor
     re-energizes. Pre-T-002, both modules' `CheckServos` advanced the same accumulator, so
     release came in half the time, and `HomeServos` on one module flipped state observed by
     the other.
   - Send a slider move to module 1 channel 0.
   - Expected: only module 1's servo moves; exactly one `Turning off servo 0 on module 1`
     ~20 s after; module 0's servo stays released.

9. **Rapid slider hammering with the timer active** (T-003)
   - Drag a slider continuously for ~30 s, then hold it still; repeat three times.
   - Expected: the servo never goes slack mid-drag; exactly one `Turning off servo N on
     module M` ~20 s after the last message each time; no task-watchdog warning. Occasional
     `CheckServos: state busy on module M, skipping tick` or `CheckServos: send busy on module
     M, channels 0x… retry next tick` WARNs are acceptable — they are the timer yielding to a
     command, not a fault. A `SetServoPosition: serial queue full, move … dropped` WARN means
     the drag out-ran the UART; the next message re-sends everything.

10. **Move issued exactly as a release is due** (T-003; the PR #56 review scenario)
   - Send a slider move; wait ~20 s watching the monitor; send another slider move just as
     the release is expected (within a second either side). Repeat a dozen times.
   - Expected: every move completes and the servo stays energized for a fresh 20 s. Never a
     `Turning off servo N on module M` within 20 s *after* a move to that channel. Pre-T-003
     a stale off could land after the new target and drop the servo mid-move.

11. **All channels come due on the same tick while a slider is active** (T-003)
   - Boot (or `RELOAD_CONFIG`) with ≥4 enabled servo channels; ~15 s after homing start
     dragging a slider on one of them and keep dragging through the 20 s mark.
   - Expected: one `Turning off servo N on module M` line per *other* channel, all on the same
     tick; the dragged servo stays energized; no `QueueCommand`/`SetServoPosition: state mutex
     timeout` line — the timer logs only after releasing the lock, so the burst cannot starve
     the command paths.

## Edge cases / negative tests

- **Out-of-range speed/accel** (hand-crafted command with speed > 255 or negative):
  inputs are clamped; servo releases at the floor (~20 s).
- **Slider message with channel outside 0–23** (hand-crafted: 24, -1, and 256 — the last two
  would wrap to 255 and 0 if narrowed to a byte before the check): `Invalid channel N` error
  logged with the value as sent, no command sent, no crash. The check runs on the parsed `int`
  and the cast to the wire byte happens only after it passes.
- **Servo commanded to its current position** (no physical motion): still releases
  on the same deadline — the model is time-based, not motion-based.
- **Tuning knob** lives in `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp` only:
  `MAESTRO_RELEASE_FLOOR_MS` (slow loads). Do not hand-edit deadlines elsewhere.
- **Command during config reload** (T-003): send a *script* move while a `RELOAD_CONFIG` is in
  progress. Expected: either the move applies normally or the monitor shows
  `QueueCommand: state mutex timeout, dropping command for channel N on module M` — a dropped
  move is visible, never a silent half-applied one. No crash, no watchdog. A *slider* move in
  the same window is ignored silently by the pre-existing `loading` guard (no log line) and
  the next slider message after the reload applies.
- **Dropped frame inside a move** (T-003; needs the UART saturated, e.g. a hard slider drag):
  `QueueCommand: serial queue full, move for channel N on module M incomplete (still tracked
  on)` or the `SetServoPosition … dropped` WARN. The channel stays tracked as on and releases
  on its normal deadline; a move never continues past a dropped frame, so the servo cannot run
  at a stale speed against a new deadline.
- **Wedged serial path** (T-003, hard to provoke): if the send mutex cannot be taken for ~2 s
  the operation logs `Send mutex timeout on module M after 20 attempts` and drops the command
  without touching channel state. Pre-T-003 the caller spun forever.
