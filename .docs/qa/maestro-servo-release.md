# QA: Maestro servo release (auto de-energize)

Covers the servo-shutdown dead-reckoning in `MaestroModule::CheckServos` (T-001), the
per-instance channel state that backs it (T-002), the locking between the shutdown timer
and the command paths (T-003), panic stop reaching the Maestro (T-004), and the other half
of the lifecycle — re-energizing a released servo where it is so the next scripted move
honours speed/accel (T-006, cases 12–16), and the per-channel release time (T-010, cases 17–23).

Re-energize (T-006): a released servo has no position as far as the Maestro is concerned, so
it jumps straight to the first target it receives. Every scripted move therefore sends a
pre-position frame at the channel's `lastPos` before the speed/accel frames and the real
target. Pre-T-006 that frame went out in raw µs where the Maestro expects quarter-µs (a
1500 home was a 375 µs pulse, i.e. a slam to the low end) and `lastPos` was only ever the
home position; since T-006 both target frames share one encoder and `lastPos` is the last
target sent on that channel (script or slider). GPIO-type channels get no pre-position frame.

Deadline model: `ServoReleaseDeadlineMs(speed, accel, releaseMs)` = max(the channel's release time,
`WorstCaseTravelMs` + 10 %). The release time is per channel since T-010 (5 s by default, the T-005
floor); before T-005 the floor was 20 s.
`WorstCaseTravelMs` is the physical trapezoid over the 0–3000 µs guard range using true
Maestro units: cruise = 120000 / speed ms (speed 0 → 255); accel adds a ramp of
80 × speed / accel ms (accel is speed-units per 80 ms, **not** a speed cap); when the cap is
never reached the move is a triangle, sqrt(38 400 000 / accel) ms. No multiplier — the guard
range (3000 µs vs a real sweep of ≤2000 µs) is the margin.
The shutdown timer checks every 300 ms and counts its first tick after a command as a full
300 ms, so observed release lands between deadline − 300 ms and deadline + one tick, plus a
small late drift (measured 2026-09-06 on the old 20 s floor: 19.81–20.04 s).

Reference deadlines for a channel at the default release time (max of 5 s and model + 10 %):

| speed / accel | model | deadline | note |
|---|---|---|---|
| 0 / 0 (slider, boot homing) | 0.5 s | **5 s** | floor |
| 0 / 1 | 6.2 s | 6.8 s | model + 10 % |
| 20 / 0 | 6 s | 6.6 s | model + 10 % |
| 20 / 2 | 6.8 s | 7.5 s | the reported bug case, never released pre-T-001 |
| 10 / 2 | 12.4 s | 13.6 s | |
| 5 / 0 | 24 s | 26.4 s | speed 5 is ~11°/s |
| 5 / 1 | 24.4 s | 26.8 s | |
| 1 / 0 | 120 s | 132 s | speed 1 is 2.25°/s; 80 s physical sweep |
| 0 / 2 | 4.4 s | **5 s** | floor (4.8 s with margin) |

First run on the T-005 formula (2026-09-07, scripted via the server API): boot releases ~5 s after
homing; `T-005 QA` deadline-race script (id `s1788789HJY`) 40 moves / 40 releases, gaps
4.82–5.11 s, 0 stale, 0 WARN/ERROR; `T-001 QA` margin cases: speed 11 → 12.13 s (12.0 s deadline),
speed 5 → 26.25 s (26.4 s), speed 5 / accel 1 → 26.89 s (26.84 s) — pass.

## Preconditions

- Board flashed with a build containing T-001 (amended: trapezoid model + floor) and T-002,
  built with `PLATFORMIO_BUILD_FLAGS="-D ASTROS_VERBOSE_COMMANDS"` (T-011): without it the per-command lines (`… command val`, `Sending Command` (GPIO, I2C), `Queueing servo command`, `Setting servo …`, `I2C Command received on queue`) are compiled out;
  serial monitor attached (115200). Log lines carry ms-since-boot; subtract
  `Setting servo N …` from `Turning off servo N` to get the observed release time. Both lines
  carry the module id right after the servo number (T-002): `Setting servo N on module M
  (min: …` and `Turning off servo N on module M`. The short forms quoted in cases 1–7 are
  prefixes of the full lines. Since T-006 the `Setting servo` line ends in `lastPos: P` — the
  µs position the pre-position frame re-energizes the servo at (`-1` on a GPIO channel: none).
- Maestro module configured with ≥1 enabled servo channel and ≥1 GPIO (non-servo) channel.
  For cases 12–13 one servo channel configured **inverted** and one not, ideally with a wide
  min–max (≥ 1000 µs) so the travel is visible; speed 10 / accel 0 moves at 0.25 µs/ms, so a
  1000 µs range takes ~4 s and the deadline is 13.2 s.
- At least one script on the SD card that moves a servo with explicit speed/accel values.
- **Regression scripts on the droid's server instance** (Raspberry Pi, http://192.168.40.76/,
  created 2026-09-07 through the API; same module/channel ids as the dev DB):
  `T-003/T-005 QA release race` (`s1788805wBm`: hammered channel + 5.0–5.3 s deadline race,
  home at 65 s), `T-004 QA panic` (`s1788805iuD`: four servos at speed 5 + relay on; send
  panic ~3 s in), `T-001/T-005 QA deadlines` (`s1788805Jym`: speeds 11 / 5 / 5+accel 1 / 1 →
  releases ~12.0 / 26.4 / 26.8 / 132 s). First run 2026-09-07 against v1.2.1-equivalent
  firmware with the master console captured: race 75 moves, gaps 4.81–5.10 s, 0 stale, burst of
  three releases on one tick under load; panic `7 off(s) queued, 0 failed`, no release after;
  deadlines 12.03 / 26.11 / 26.96 s; 0 WARN/ERROR — all pass.
- For T-003 cases 9–11 in one run (written against the pre-T-005 20 s floor; the deadline-race
  half is superseded by the `T-005 QA` script, id `s1788789HJY`, which uses 5.0–5.3 s periods):
  the **`T-003 QA`** script in the server's local database
  (`AstrOs.Server/.data/database.sqlite3`, id `s1788696QAq`, four body-Maestro servos):
  phase A (0–35 s) moves all four at t=0, then hammers channel 1 every 0.5 s while channels 2–4
  come due together at ~5 s (~20 s when written); phase B (40–123 s) moves all four at 40 s, then each channel
  again every 20.0 / 20.1 / 20.2 / 20.3 s (ch1–ch4) so a move lands inside the release tick each
  round; home at 123 s. Deploy it to the body location and run it with the monitor attached.
  Pass: no `Turning off servo N on module 1` *after* a newer `Setting servo N on module 1` for
  that channel (a release one deadline after the latest move is normal — 19.7–20.1 s on the
  20 s floor that script was written for, 4.7–5.1 s since T-005; the body Maestro is
  module idx 1); channels 2–4 release once at ~5 s during phase A; no `state mutex timeout`
  line anywhere; all four home at 123 s and release ~5 s later. First run 2026-09-06 (20 s floor):
  89 moves, 22 releases, gaps 19.81–20.04 s, 0 stale releases, 0 WARN/ERROR — pass.
- Optional but recommended: Maestro USB to a laptop with Maestro Control Center open on the
  Status tab. When the release lands, the channel's target drops to 0 and its Enabled box
  unchecks — proof the Maestro received the off command, not just that the ESP sent it.

## Test cases

1. **Home-and-release on boot (floor)**
   - Power the node; wait for homing.
   - Expected: servos move home; ~5.3 s after `Homing Servos` each enabled servo logs
     `Turning off servo N` and is free to move by hand (no holding torque).

2. **Slider move on an already-released channel re-arms release**
   - Wait for a servo to log `Turning off servo N` (e.g. ~5 s after boot homing). Then send a
     single slider move to that servo.
   - Expected: servo moves; `Turning off servo N` ~5.3 s after the slider message (was ~20.3 s
     before T-005); servo free by hand. Pre-T-001 (third pass) the slider path never armed
     tracking, so a released servo stayed energized indefinitely after a slider move. **On linear actuators:** the
     stroke must complete well before release — if it is cut short, that is the signal to raise
     that channel's release time (case 17; `.docs/protocol.md` § D).

2b. **Continuous slider drag (noisy stream)**
   - Drag the slider back and forth for ~30 s (well past the channel's release time), then stop.
   - Expected: no `Turning off servo N` for that channel during the drag — every message
     resets the clock; exactly one `Turning off servo N` ~5.3 s after the *last* slider
     message; no per-message log lines from `MaestroModule` during the drag (only the
     interface-level ones that already existed).

3. **Scripted move with low accel (the reported bug)**
   - Run a script move with speed 20, accel 2.
   - Expected: `Turning off servo N` ~7.8 s after the command (6.8 s model + 10 %, one tick);
     servo free by hand. Pre-T-001 this NEVER released. Accel only adds a short ramp; if release lands minutes
     later, the accel-as-speed model has crept back in.

4. **Slow scripted move uses the model (floor vs model crossover)**
   - Run a script move with speed 5, accel 1.
   - Expected: `Turning off servo N` ~27 s after the command (24.4 s model + 10 %, one tick).
     Together with case 1 (floor) this proves the max() is wired.
     (Pre-T-001: never released for speed ≤ 5.)

5. **New command resets the clock**
   - Send a second move to the same servo ~10 s after a speed-5 move.
   - Expected: servo stays energized across the second move; single
     `Turning off servo N` ~27 s after the *second* command, not the first.

6. **GPIO channels are never auto-released**
   - Toggle a non-servo (GPIO) channel on.
   - Expected: no `Turning off servo N` for that channel at any point; output holds.

7. **Panic stop de-energizes every configured Maestro channel** (T-004)
   - Start a slow scripted move (speed 5, ~24 s), then send panic stop from the server a few
     seconds in.
   - Expected: the script halts and the servo goes slack *immediately* (Maestro Control Center
     shows target 0; the horn moves freely by hand). Monitor shows, in this order:
     `Panic: dropped N queued servo commands` (N is usually 0 here), then
     `Panic: module M complete, K off(s) queued, 0 failed` once per configured module. **No**
     `Turning off servo N on module M` for that channel afterward — panic cleared its tracking,
     so the timer has nothing to release. Pre-T-004 the move completed and stayed energized
     until the normal deadline.

7a. **Panic on a padawan** (T-004)
   - Same as 7 on a servo owned by a padawan; send panic from the server (master relays it
     over ESP-NOW).
   - Expected: identical behavior on the padawan's monitor; the master's monitor also shows its
     own `Panic:` lines for its modules. **Note the delay** between the master's `Panic:` lines
     and the padawan's: the master relays panic to padawans from the same single-threaded
     queue that runs its own offs, so if the server lists the master's record first the
     padawan's kill waits for the master's offs (healthy: ~150 ms per master module; wedged
     serial path: seconds). Record the observed delay — it decides whether the Backlog item
     "panic offs on a dedicated task" gets scheduled.

7b. **Panic holds a GPIO-type channel** (T-004; panic is a stop, not a reset)
   - Turn a GPIO-type Maestro channel on (script or slider), then send panic.
   - Expected: the output does **not** change — the relay stays closed / LED stays on. Panic
     sends offs to enabled *servo* channels only; a GPIO output "stops" by holding its state,
     since driving it anywhere could itself move something. The per-module line counts servo
     channels only (7 on the bench Maestro: channels 1–7).

7c. **Panic against a queued burst** (T-004)
   - Run a script whose first event moves ≥4 servos at speed 5; send panic within ~1 s.
   - Expected: all four go slack. Monitor shows `Panic: dropped N queued servo commands` with
     N ≥ 1 if any command was still queued, and at most one `Setting servo N on module M` line
     *after* the `Panic:` lines (a command `servoQueueTask` had already dequeued when panic
     fired; a command the dispatch task enqueued late is caught by the second drain and shows
     as `Panic: dropped 1 late servo command(s) after the offs`). A late servo residual logs
     `Turning off servo N on module M` on its normal deadline (~27 s at speed 5) — its state
     survived, so the normal release still fires. A late GPIO residual applies once and then
     holds. Never a servo left
     energized with no later release.

7d. **Recovery after panic** (T-004)
   - After 7, run a script or slider move on the same servo.
   - Expected: the servo re-energizes and moves normally, then releases on its normal deadline.
     Panic leaves nothing latched.

   First run 2026-09-07 (scripted: `T-004 QA` script in the server DB, id `s1788783HSD`; run and
   panic sent via `GET /api/scripts/run` and `POST /api/panicStop`, both consoles captured):
   7 panics, each `dropped 0`, `8 off(s) queued, 0 failed`, no release in the following 30 s,
   0 WARN/ERROR; recovery normal — pass. Re-run after panic was narrowed to servo channels:
   `7 off(s) queued, 0 failed` (relay ch0 excluded), no release after — pass. Observed on the
   hardware: the servos stopped at the panic. 7a not coverable (padawan has no Maestro); relay to
   the padawan measured at 40 ms. Note ~1.0 s passes between the HTTP call and the board for both
   run and panic. The 2026-11-22 bench traced it to the master's own serial read: `uart_read_bytes`
   asks for 1024 bytes and returns only after a 1 s timeout following the last byte. So at least
   1 s of it is firmware, not the server; 2026-11-22 measured 1.09 s in all (`PLAN.md` Backlog).

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
     module 0` and `… on module 1`, ~5 s after homing — case 1). Only then continue;
     otherwise the boot release will be mistaken for a failure below.
   - Send a script move to module 0 channel 0 only.
   - Expected: `Setting servo 0 on module 0 …`; only module 0's servo moves. **Exactly one**
     `Turning off servo 0 on module 0` on its deadline after the script move, and no
     `Turning off servo 0 on module 1` at all; module 1's servo neither moves nor
     re-energizes. Pre-T-002, both modules' `CheckServos` advanced the same accumulator, so
     release came in half the time, and `HomeServos` on one module flipped state observed by
     the other.
   - Send a slider move to module 1 channel 0.
   - Expected: only module 1's servo moves; exactly one `Turning off servo 0 on module 1`
     ~5 s after; module 0's servo stays released.

9. **Rapid slider hammering with the timer active** (T-003)
   - Drag a slider continuously for ~30 s, then hold it still; repeat three times.
   - Expected: the servo never goes slack mid-drag; exactly one `Turning off servo N on
     module M` ~5 s after the last message each time; no task-watchdog warning. Occasional
     `CheckServos: state busy on module M, skipping tick` or `CheckServos: send busy on module
     M, channels 0x… retry next tick` WARNs are acceptable — they are the timer yielding to a
     command, not a fault. A `SetServoPosition: frame not queued (serial queue full or no memory)
     after stage N of 3, move for channel C on module M dropped` WARN means the drag out-ran the
     UART; the next message re-sends everything.

10. **Move issued exactly as a release is due** (T-003; the PR #56 review scenario)
   - Send a slider move; wait ~5 s watching the monitor; send another slider move just as
     the release is expected (within a second either side). Repeat a dozen times — or run the
     `T-005 QA` script, which does this on four channels at 5.0/5.1/5.2/5.3 s periods.
   - Expected: every move completes and the servo stays energized for a fresh 5 s. Never a
     `Turning off servo N on module M` within 5 s *after* a move to that channel. Pre-T-003
     a stale off could land after the new target and drop the servo mid-move.

11. **All channels come due on the same tick while a slider is active** (T-003)
   - Boot (or `RELOAD_CONFIG`) with ≥4 enabled servo channels; ~2 s after homing start
     dragging a slider on one of them and keep dragging through the 5 s mark.
   - Expected: one `Turning off servo N on module M` line per *other* channel, all on the same
     tick; the dragged servo stays energized; no `QueueCommand`/`SetServoPosition: state mutex
     timeout` line — the timer logs only after releasing the lock, so the burst cannot starve
     the command paths.

12. **Inverted servo 0 → 100 after a release starts where it was** (T-006; the reported bug)
   - Script or slider the inverted channel to position 0 (its `maxPos`); wait for
     `Turning off servo N on module M`; then run a script move to 100 at speed 10, accel 0.
   - Expected: `Setting servo N on module M (min: …) to <minPos>, cmd: 100. speed: 10. accel:
     0. inverted: 1. lastPos: <maxPos>`; the servo departs from `maxPos` and travels smoothly to
     `minPos` over ~4 s per 1000 µs of range, with no initial jump. Pre-T-006 the servo
     slammed to its low end the instant the command arrived and the "move" was over before the
     speed frame mattered.

13. **Non-inverted mirror: 100 → 0 after a release** (T-006)
   - Same as case 12 on a non-inverted channel: park it at 100 (`maxPos`), wait for the
     release, then script 0 at speed 10.
   - Expected: `… to <minPos>, cmd: 0 … inverted: 0. lastPos: <maxPos>`; smooth travel, no
     jump. (Pre-T-006 this direction jumped exactly like case 12 — the bug was never
     inversion-specific; the opposite directions only twitched because the servo was already
     near the low end.)

14. **Slider hand-off** (T-006)
   - Drag a servo to roughly mid-range and let go; wait for the release; then script 100 at
     speed 10.
   - Expected: `lastPos:` in the `Setting servo` line is the last slider µs (within the
     slider's resolution), and the servo departs from where the slider left it. This proves
     the slider path records `lastPos` too — pre-T-006 the line would have replayed the home
     position.

15. **Move while still energized** (T-006)
   - Script 0 at speed 10, then within 5 s script 100 at speed 10.
   - Expected: smooth reversal from wherever the first move had reached, no jump — the
     pre-position frame is a no-op while the Maestro still knows the position. Second line
     shows `lastPos: <first move's target>`.

16. **GPIO channel gets no pre-position frame** (T-006; human-gated on a relay or LED)
   - Command the GPIO channel on, then on again, then off → on → off.
   - Expected: `lastPos: -1` on every line; the output holds steady across the repeated "on"
     (no blip) and toggles normally on the off/on/off. Pre-T-006 every GPIO command was
     preceded by a LOW frame; with the encoder fix that would have become a HIGH blip for any
     channel whose configured min was ≥ 1500 µs, so the frame is no longer sent.

### Per-channel release time (T-010)

**Preconditions:**
- AstrOs.Server with its T-014, which sends `releaseMs` as the 8th channel field (`.docs/protocol.md` § D). Set the values by SQL (`UPDATE maestro_channels SET release_ms = 15000 WHERE id = '<channel id>'`) until its T-015 UI lands.
- **Flash every controller with a T-010 build before the first sync.** A `1.3.0-dev` build without T-010 passes the server's gate, drops 8-field entries, and loses its Maestro channels until reflashed.
- Master console captured (`pio device monitor -e metro_s3`, or a timestamping logger).
- Firmware built with `PLATFORMIO_BUILD_FLAGS="-D ASTROS_VERBOSE_COMMANDS"` once T-011 is in it, so `Queueing servo command` is logged (cases 17–21 time releases from it). The boot line reads `Verbose command logs: on`.
- Every config load logs `Maestro module M release times: …` (e.g. `ch1 15000 ms; others 5000 ms`), the release times the board applied.
- After case 19, reset ch1 to 5000 (`UPDATE maestro_channels SET release_ms = 5000 …`) and sync: cases 1–16 assume default channels.

17. **A channel's own release time** (T-010)
   - Set Right Pan (ch1) to 15000, sync, and run a scripted move at speed 0, accel 0.
   - Expected: after the sync, `Maestro module 1 release times: ch1 15000 ms; others 5000 ms`; then `Turning off servo 1 on module 1` ~15 s after the move's `Queueing servo command`, not at 5 s. The config's entry for ch1 ends `…:15000` in the `AstrOs RX: Read N bytes: '…'` line.

18. **Default channel unchanged** (T-010)
   - With ch1 at 15000, run a speed-0, accel-0 move on ch6 (left at the default 5000).
   - Expected: ch6 releases ~5 s after its move, as in case 1. This is the only case that tells a per-channel time from a single global one: don't skip it.

19. **Travel beyond the release time** (T-010)
   - Set ch1 to 10000 and run a speed-5, accel-0 move (models 24000 ms + 10 %; accel would change the number).
   - Expected: the release comes ~26.4 s after the move: the model wins over a smaller setting, and the setting is never a cap.

20. **1.2.x SD files after the OTA update** (T-010)
   - On a board synced under 1.2.x (7-field `/sdcard/maestro/<idx>.cfg`), flash 1.3.0 over OTA and **do not sync**.
   - Expected: it boots, loads every Maestro channel (`Maestro module M release times: all 5000 ms (default)`), homes them, and releases them ~5 s after `Homing Servos` (an OTA image is a normal build, so time from that line), the same as before the flash.

21. **Padawan** (T-010; log-only works: a Maestro configured with nothing attached)
   - Repeat case 17 on a padawan's channel.
   - Expected: the padawan's console shows `Maestro module M release times: ch… 15000 ms; …` after the sync, and the release ~15 s after the move.

22. **A bad release field keeps the channel** (T-010)
   - Hand-edit a channel entry's 8th field on the SD card to `abc` (or `999`), then reboot. (Not `RELOAD_CONFIG`: the only path that sends it, a config deploy, rewrites the SD file first.)
   - Expected: one warning on its own line, `W (<ms>) AstrOsFileUtils: Maestro channels with an invalid release time (accepted 1000-120000 ms), using 5000 ms: ch <id> 'abc'`. The `W (…) AstrOsFileUtils:` prefix and the line break are the logger-adapter fix. Several bad channels share that one line (up to four listed, then `(+N more)`). The channel stays enabled and releases at 5 s.
   - An entry with more than 8 fields keeps its channel, with the extras ignored and one `Maestro channel entries with more than 8 fields (extra fields ignored): N` warning.

23. **A reload releases a servo it no longer tracks** (T-010, amended; Jeff)
   - Set ch1 to 30000 and run a move, so ch1 stays energized for 30 s. Within those 30 s, disable ch1 on the server and sync.
   - Expected: soon after `Homing Servos` (homing's frames, plus up to one ~300 ms `CheckServos` tick; homing logs no end line), `CheckServos: module 1 sent offs to channels 0x000002 left energized by a reload`, and the servo goes limp. Homing's frames can fill the 10-frame serial queue, so one `CheckServos: serial queue full on module 1, channels 0x000002 retry next tick` WARN just before the INFO line still passes. Before T-010 (and with any channel that was energized when a reload disabled it), it stayed powered until a power-cycle.
   - Re-enable ch1 and reset it to 5000 afterwards.

## Edge cases / negative tests

- **Out-of-range speed/accel** (hand-crafted command with speed > 255 or negative):
  inputs are clamped; servo releases at the channel's release time (~5 s by default).
- **Panic with a wedged serial path** (T-004; hard to provoke): if the module's send mutex
  cannot be taken within ~2.2 s the monitor shows `Panic: send mutex timeout on module M - offs
  NOT sent` at ERROR and no off frames go out — the serial path could not have delivered them
  anyway. If an individual off cannot be queued (`Panic: off NOT queued for servo N on module
  M (serial queue full|no memory) - stays energized until the timer retry`), that channel keeps
  its tracking and is released by the timer within one deadline. If the module map cannot be
  snapshotted within 1 s, `handlePanicStop:
  maestroModulesMutex timeout - Maestro offs NOT sent on any module` at ERROR: the script is
  stopped and the queue drained but no hardware was de-energized.
- **Panic concurrent with a config reload or module init** (T-004, accepted limitation): if a
  `RELOAD_CONFIG` overlaps the panic within ~1 s, channels may be homed *after* the offs and
  stay energized until their normal release; panic may also miss a module whose config
  was being loaded at that instant. Not a bench case — the servos still release on the normal
  deadline, and the ordering fix is a Backlog enhancement. The same goes for a servo a reload
  disabled while it was energized (T-010): panic doesn't stop it, and `CheckServos` sends its
  off about one tick after the panic releases the send mutex (later if the panic overlaps the
  reload itself).
- **A script keeps commanding a channel the sync just disabled** (T-010): a script still
  running during the sync moves ch1 again after the reload. Either order passes, since the
  script's next move races the first `CheckServos` tick after loading (a ch1 command during the
  SD read is dropped after the 50 ms state wait and doesn't re-arm it):
  - the move lands first: no `left energized by a reload` line names ch1;
  - the tick wins: one such line names ch1, and the next move re-energizes it.
  Either way no move stops early (a reload off never cuts a move short), and ch1 releases on
  its normal deadline after its last move.
- **…and the sync made that channel a disabled digital output** (T-010, PR #66 review): on the
  server set ch1 to GPIO, then to disabled (disabling keeps the last type), and sync while ch1 is
  energized and a script keeps commanding it. `CheckServos` never releases a digital channel, so
  the reload off goes out whichever comes first: a `left energized by a reload` line always names
  ch1, and it cuts the level a command set just before it. A command after that line drives the
  output again and it stays driven (pre-existing: the command paths don't check `enabled`;
  Backlog).
- **Slider message with channel outside 0–23** (hand-crafted: 24, -1, and 256 — the last two
  would wrap to 255 and 0 if narrowed to a byte before the check): `Invalid channel N` error
  logged with the value as sent, no command sent, no crash. The check runs on the parsed `int`
  and the cast to the wire byte happens only after it passes.
- **Servo commanded to its current position** (no physical motion): still releases
  on the same deadline — the model is time-based, not motion-based.
- **Tuning knob** is the per-channel release time (T-010), set on the server and sent as the
  channel entry's 8th field (`.docs/protocol.md` § D). Its default comes from
  `ASTROS_MAESTRO_RELEASE_DEFAULT_MS` (`AstrOsStructs.h`), and its range and margin from
  `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp`. The firmware default only applies to a
  7-field entry or a rejected value: the server always sends field 8 (its column defaults to 5000),
  so changing the firmware default doesn't change synced channels. Do not hand-edit deadlines elsewhere.
- **Command during config reload** (T-003): send a *script* move while a `RELOAD_CONFIG` is in
  progress. Expected: either the move applies normally or the monitor shows
  `QueueCommand: state mutex timeout, dropping command for channel N on module M` — a dropped
  move is visible, never a silent half-applied one. No crash, no watchdog. A *slider* move in
  the same window is ignored silently by the pre-existing `loading` guard (no log line) and
  the next slider message after the reload applies.
- **Dropped frame inside a move** (T-003; needs the UART saturated, e.g. a hard slider drag):
  `QueueCommand: frame not queued (serial queue full or no memory) after stage N of 4, move for
  channel C on module M incomplete (still tracked on)` or the `SetServoPosition … after stage N
  of 3 … dropped` WARN. The channel stays tracked as on and releases on its normal deadline; a
  move never continues past a dropped frame, and the tracked speed/accel are reconciled to the
  stage reached (a frame that did not go out leaves the Maestro's previous limit in force), so
  the deadline always models the limits the servo is really moving under.
- **Wedged serial path** (T-003, hard to provoke): if the send mutex cannot be taken for ~2 s
  the operation logs `Send mutex timeout on module M after 20 attempts` and drops the command
  without touching channel state. Pre-T-003 the caller spun forever.
- **`lastPos` not recorded** (T-006; needs a `RELOAD_CONFIG` overlapping the end of a move):
  `recordLastPos: state mutex timeout, next move for channel N on module M re-energizes at the
  previous target` WARN. The move itself completed; only the *next* move on that channel starts
  with a jump to the previous target (the pre-T-006 behaviour), and that move records normally.
- **Partial send leaves `lastPos` alone** (T-006, with the T-003 dropped-frame case above): a
  move that stops before its target frame does not update `lastPos` — the servo is either still
  at the old value (the pre-position frame went out) or untouched. The next `Setting servo` line
  shows the older `lastPos`, which is correct.
- **Servo moved by hand or by load while released** (T-006, accepted limitation): `lastPos` is
  the last *commanded* target, not a reading from the Maestro. If the servo is physically moved
  while off, the next move begins with a jump back to `lastPos` before the smooth travel. The
  fix, if it ever matters, is reading the position back (`GET_SERVO_POSITION 0x90`) — Backlog.
- **Boot / reload** (T-006): `HomeServos` sets `lastPos = home` and sends the home target, so
  the first scripted move after boot shows `lastPos: <home>` and departs from home. A home
  target frame that fails to queue (`HomeServos: frames not queued …`) leaves `lastPos` claiming
  home while the servo sits released wherever it was — pre-existing, boot/reload only.
- **A slower serial device on the Maestro's channel** (T-012, known limit, not a failure): an HCR at
  9600 sharing the channel can miss sounds while a slider is dragged (12 of 16 played; 32 of 32
  without one, scripted moves included). Several devices on one channel are best effort, at the
  user's own risk (Jeff). The servos must still move and release normally.
