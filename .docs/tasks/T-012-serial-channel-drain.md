# T-012: Send every queued frame per tick in the serial-channel tasks

<!-- File: .docs/tasks/T-012-serial-channel-drain.md. Branch: feature/T-012-serial-channel-drain. PR title: "T-012: Send every queued frame per tick in the serial-channel tasks". -->

## Context

**Found by the T-011 bench** (2026-11-22, QA `random-span-events.md` case 11). On a normal (flag-off) build, scripted GPIO commands executed 10–20 ms after dispatch, but the scripted servo took **310 ms in every run**.

**The cause is not logging.**
- `serialCh1QueueTask` and `serialCh2QueueTask` (`src/main.cpp`) handle one queue message per loop, then `vTaskDelay(10 ms)`.
- `MaestroModule` sends every frame as its own message (`enqueueFrame`). A servo or digital change is three frames (speed, accel, target), four when a released servo gets its pre-position frame (T-006). So each change costs ~30–40 ms of serial-task time.
- When the 10-slot serial queue is full, `servoQueueTask` blocks in `enqueueFrame`, so `servoQueue` drains at the same rate. A scripted command behind ten queued random changes waits ~310 ms.
- The Maestro link runs at 115200 baud (~1.4 ms for a change's bytes), so the UART is not the limit.

**Jeff, 2026-11-22:** fix this before v1.3.0, as its own task. T-011 closed on its measured numbers.

**Relates to:** PLAN.md § Per-channel servo release time, and the v1.3.0 hold.

## Contract (pinned — do not change)

- **Each loop, both serial-channel tasks** receive and handle every message already in their queue, then `vTaskDelay(pdMS_TO_TICKS(10))`. Receives use a 0 wait.
  - At most one queue length (10) per loop, so a producer refilling the queue can't keep the task from yielding.
- **Per-message handling is unchanged:**
  - `message_id` 0 → `SendCommand(msg.data)`
  - `message_id` 1 → `SendBytes(msg.baudrate, msg.data, msg.dataSize)`
  - `free(msg.data)` after each
  - FIFO order kept
  - the stack high-water-mark check once per loop
- **Both channels share one loop body.** `serialCh1QueueTask` and `serialCh2QueueTask` call it with their queue and `SerialModule`, so the two can't drift.
- **Unchanged:** queue lengths, task priorities and stacks, `MaestroModule`'s frames and `enqueueFrame` waits, the generic-serial and Kangaroo message formats, and every other queue task.

**Amended 2026-11-23 (Jeff, option A, after the first bench):** the drain holds after a pre-position frame.
- `MaestroModule` sends its pre-position frame (`lastPosUs != -1` in `setServoPosition`: every scripted servo move, and homing's explicit off) as `message_id` 2: send the bytes, then hold the channel for **at least 30 ms** before its next message. 30 ms is the gap the one-frame-per-tick loop gave, and the one T-006's bench passed with.
- Everything else drains at full speed: digital channels and slider moves have no pre-position frame. A servo move therefore costs about what it does today (30–40 ms), so a queue of servo changes drains no faster than before; on/off changes do.
- `queue_serial_msg_t` keeps its fields. Ids 0 and 1, the frames themselves, their order and `enqueueFrame`'s waits are unchanged.
- A hold yields (`vTaskDelay`), so the cap and the yield rules above still stand. Up to three holds can sit ahead of a panic's offs in a full queue (~120 ms, against 100 ms today).
- **Acceptance, added:** the jump check (Bench results), 16 moves from released on this build, none fast.

## Task

- **The shared loop body:** e.g. `static void serialChannelQueueLoop(QueueHandle_t queue, SerialModule &channel, const char *name)` in `src/main.cpp`. It's the task loop, so it stays in `main.cpp`, consistent with the "thin consumer loops" item in § main.cpp decomposition. Both task entry points call it.
- **QA `random-span-events.md` case 11:** replace the normal-build servo number (310 ms) with the new measurement, and drop the "(ESP T-012 drains them per tick)" note.
- **The drain comment** in `lib_native/AstrOsRandomEffects/include/RandomSpanAdmission.hpp` is updated the same way.

## Acceptance criteria

- [ ] Both boards build; `pio test -e test` green; clang-format clean on touched files.
- [ ] **Bench, normal build:** case 11 ×3 with the T-011 throwaway instrumentation (the scripted-only WARN lines, rebased onto this branch and never merged).
  - The scripted servo's dispatch → execution is **≤ 150 ms in every run** (expected ~100 ms: ten changes × (10 ms tick + ~1 ms)).
  - GPIO and the same-time offset are no worse than T-011's (10–20 ms, +40–50 ms).
  - 0 queue fails.
- [ ] **Bench, verbose build:** case 11 still meets its verbose bounds (GPIO ≤ 120, servo ≤ 450).
- [ ] **Regression:**
  - `maestro-servo-release.md` case 7 (panic) and cases 9–11 (T-003 slider hammering and release races) pass on the master.
  - A generic-serial or Kangaroo script command on a serial channel still goes out, if one is configured on the bench; otherwise noted as not covered.
- [ ] Case 11 and the drain comment are updated.

## Out of scope

- Coalescing a change's frames into one message, or skipping an unchanged speed/accel frame. That's a possible further step; Backlog it if the bench shows it's needed.
- The servo random share (`RandomChangeHasRoom`).
- `servoQueueTask`'s own one-message-per-tick loop: the bench decides whether it matters once the serial drain is fixed. Note the result.
- Other queue tasks.

## Verification

- `pio test -e test`; `pio run -e lolin_d32_pro` and `pio run -e metro_s3`; clang-format.
- The bench runs above, on the master with AstrOs.Server driving case 11 by `curl` (`random-span-events.md` § Bench preconditions / Measuring). Stop any console logger before each flash: it holds the USB port.

## Failure-mode inventory

- **Concurrency:**
  - The serial tasks run at priority 9 on core 1. A full drain is at most ten frames, about 40 bytes, so ~3.5 ms of UART time at 115200 baud.
  - `SendCommand` / `SendBytes` block on the UART driver's TX buffer. A burst of ten frames fits it: check the driver's TX buffer size in `SerialModule`, and if it's smaller, the burst blocks briefly, which is harmless at priority 9.
  - Priority 10 (servo queue, interface) can still preempt between messages.
- **Ordering:** a single consumer per queue, FIFO, so the frame order inside a change (speed → accel → target) is unchanged.
- **Resource lifecycle:** each message's `data` is freed after handling, exactly as before. The loop can't skip a `free`: one path per message.
- **Starvation:** the per-loop cap (one queue length) keeps a producer that refills the queue from pinning the task; `vTaskDelay` still runs every loop.

## Implementation checklist

- [x] `SerialModule`: track the baud last applied, and wait for TX to finish before changing it (Ruling 1; its own commit, before the drain)
- [x] `serialChannelQueueLoop` in `src/main.cpp`; both task entry points call it; one constant for the queue length and the per-loop cap
- [x] `pio test -e test` 631/631; both boards build; clang-format clean
- [x] Fresh-context code review (see Review notes)
- [x] Option A (Contract amendment): hold after a pre-position frame; jump check 16 of 16 slow
- [x] **Bench, before anything else:** `maestro-servo-release.md` cases 12, 13 and 14, with 15 as the control, show no jump (Review C1). If one jumps, stop: the fix changes frame pacing, which the Contract pins. **Failed 2026-11-23 on the plain drain; passes on option A (2026-11-24): see Bench results.**
- [ ] Bench: normal build, case 11 ×3, plus the Maestro Control Center Errors check (Ruling 1)
- [ ] Bench: verbose build, case 11
- [ ] Bench: regression (`maestro-servo-release.md` 7, 9–11; generic serial or Kangaroo if configured)
- [ ] Case 11 and the drain comment updated with the measured numbers

## Rulings (2026-11-23, before implementation)

**Ruling 1: `SerialModule::SendData` changes too, although it isn't in the Task list.** The drain isn't safe without it, so it ships in this task, as its own commit.
- **What the 10 ms tick was hiding.** On every port but the master's server link, `SendData` compares the requested baud with `uart_get_baudrate`, which returns the divider's rounded rate. By the IDF 5.5.3 formulas (80 MHz APB, both chips), 115200 reads back as 115201, as do 76800 and 230400 (76803, 230423). So it calls `uart_set_baudrate` before **every** Maestro frame. From source, not yet observed on a board.
- `uart_set_baudrate` doesn't wait for TX. The port has no TX ring buffer (`uart_driver_install(…, 0, …)`), so `uart_write_bytes` returns with the bytes still in the 128-byte hardware FIFO. A 4-byte frame needs 347 µs at 115200.
- With the drain, each frame of a burst would reprogram the divider while the previous frame is on the wire. Whether rewriting the same value corrupts a byte isn't documented. Two modules at different rates on one channel (the UI allows it) would corrupt for certain.
- **The change:** remember the baud last applied, and compare with that. When it really changes, `uart_wait_tx_done` first (bounded, WARN on timeout, then change it anyway, as today). `message_id` handling, formats and the master's server link are unchanged.
- **Check:** after bench case 11, the Maestro's Errors tab (Maestro Control Center) shows no serial framing, overrun or protocol error.
- **Cost if wrong:** one extra int per port, and a short wait on a real baud change. Dropping the commit restores today's behavior.

**Ruling 2: the high-water-mark check runs once per loop that handled a message,** after the drain. An idle loop doesn't check, as today, so a low stack can't log at 100 Hz.

**Ruling 3 (2026-11-24, Jeff's policy, after the mixed-baud bench): a short quiet gap when a port changes speed, best effort, one try.**
- **Policy (Jeff):** several devices on one serial channel are allowed but discouraged, at the user's own risk. The firmware makes a best effort; serial was never designed for this on one pair of wires. If a fix isn't quick and easy, it isn't worth the effort.
- **What happened:** 2 of 8 HCR sounds (9600) were lost while slider frames (115200) streamed on the same channel. See Bench results.
- **Likely cause, not proven:** a device at one speed hears the other speed's bytes as noise, and needs the line idle for about a character (1 ms at 9600) to find the next start bit. The old loop left about 10 ms between any two messages. The drain can send them back to back.
- **The change:** in `SendData`'s speed-change branch, wait at least 10 ms (the old spacing) after TX is done, before changing. A port that stays at one speed never waits.
- **If it doesn't fix it:** revert it and record the limit. No second try.
- **Outcome (2026-11-24): it didn't fix it** (still 6 of 8, see Bench results). **Kept anyway (Jeff):** it does no harm, and it gives back the spacing the old loop had around a speed change. No second try.
- **Cost:** 10–20 ms per speed change, with the shared mutex held, so latency only. A panic's offs behind a sound wait for at most two of them.

## Found while reading (not in this task)

- **Both channels share one mutex.** `serialMutex` in `SerialModule.cpp` is file-static, and the second `Init` overwrites (leaks) the first. While channel 1 waits for FIFO space it holds off channel 2. The drain makes that up to ten messages instead of one; it is still bounded, and `SendData` retries, so it costs latency only. Backlog.
- **`ASTROS_INTERFACE_MESSAGE` sends the wrong struct.** It queues a `queue_msg_t` to `serialCh1Queue`, whose items are the larger `queue_serial_msg_t`. Nothing posts that command today. Backlog.

## Review notes (2026-11-23, fresh context, before the bench)

No defect found in the loop or the baud tracking beyond M2. Checked against the code before acting:

- **C1 (Critical, unproven): the drain removes the gap T-006's pre-position frame may rely on.**
  - `QueueCommand` queues pre-position, speed, accel and target together. The old loop put 10 ms between frames, so the target followed the pre-position frame by at least 30 ms; now it follows by about 1 ms (16 bytes contiguous at 115200).
  - If the Maestro applies a released channel's target only on its own update, the pre-position frame is overwritten before it acts, and the move jumps as before v1.2.2. The Maestro firmware is closed, and the reviewer's forum sources disagree.
  - The acceptance criteria would pass with the bug present: case 11's servo runs at speed 0. Hence cases 12–15 above, first.
- **I1 (Important): case 11's load no longer fills the queue.** Two 20–40 ms spans ask for ~60 changes/s and `servoQueueTask` now serves 100/s, so "expected ~100 ms" and the servo half-full lines won't appear. The load or the expected number must change before the number goes into case 11. Open: Jeff's call.
- **I2 (Important): the baud-change branch gets little coverage** with only a Maestro on the channel. The droid's body has an HCR at 9600 on the Maestro's channel 2, or a USB-serial adapter can read the bytes. Don't judge that run by the Maestro's Errors tab.
- **M2, fixed:** a rate outside 300–5000000 is refused with an ERROR, and the message goes out at the current rate.
  - The HAL accepts −1 and anything above 80 MHz as divider 0. The old code then divided by it on readback (a reboot), and Ruling 1's version could have stalled both channels. The first fix (54f0beb) caught only `baud <= 0`; the commit security review found the upper end.
  - Not dropped: `SendData` carries Maestro frames too, so a drop would lose a panic's offs on a Maestro with a bad configured baud.
- **M3 (watch):** homing and multi-servo events now start servos ~10 ms apart or less, where the old loop spread them 40 ms apart. Watch the supply on the first homing.
- **M4:** take a baseline of the Maestro's Errors tab after boot, and re-read it after a sync and a panic, not only after case 11.
- **Corrects the Context:** every scripted servo move is four frames, since `lastPos` is sent whether or not the servo is released. Three is digital channels and sliders.
- **M1 (note):** a Maestro at 9600 can now fill the TX FIFO, so a channel can hold the shared mutex for up to ~123 ms. Latency only; same Backlog item.

## Bench results (2026-11-23): the drain brings the jump back (C1 confirmed)

Master (metro_s3), verbose builds, left pan (ch4: inverted, 1300–1830), moved by one-event scripts at speed 10, accel 0, each from released. A 530 µs move should take ~2.1 s. Jeff judged each move by eye; commands were logged with identical speed, accel and `lastPos`, and no warnings.

| Firmware | Moves, alternating out / back | Fast |
|---|---|---|
| T-012 (`ccabfd6`) | slow, fast, slow, slow, slow, fast, fast, fast | 4 of 8 |
| develop (`4c0fd65`, no drain) | slow × 8 | 0 of 8 |

- **The speed varies move to move, in both directions,** for the same command. That fits the mechanism in C1: the target now follows the pre-position frame by ~1 ms, and whether the Maestro acts on the pre-position frame first is a matter of timing. The gap on develop is at least 30 ms.
- **Small sample** (one-tailed p ≈ 0.04), but predicted before the run, and it also matches the v1.2.2 bug's symptom.
- **Not covered:** the Maestro's Errors tab (no Maestro Control Center available). The servos were already home at both boots, so homing put no load on the supply (M3 open).
- Homing took 90 ms on T-012, against 860 ms on develop, and all seven servos released on time. The droid's SD config has all 24 channels enabled (left over from an earlier bench; the server's database has 8), so homing is 96 frames.
- **Stopped here, per the checklist.** The fix changes frame pacing, which the Contract pins: Jeff's decision. The other bench cases were not run. The master is on develop (`4c0fd65`, verbose).
- Noticed: the T-012 image logged `App version: 1.3.0-dev.154` in `esp_app_desc` but `1.3.0-dev.156` from the firmware, after an incremental build. A clean build (2026-11-24) has the same version in both, so it's incremental builds only. They must match for an OTA version confirm: use a clean build for a local OTA.

### Option A (`5817749`, verbose): passes the jump check

| Firmware | Moves, alternating out / back | Fast |
|---|---|---|
| Option A (`5817749`) | slow × 16 | 0 of 16 |

- Same servo, scripts and method as above; each move from released, no warnings, released on time after every move. At the drain's 4-in-8 rate, 16 slow moves would happen about once in 65,000.
- Homing took 970 ms: every channel's first frame is a pre-position frame, so the servos start about 40 ms apart, as on develop. That closes M3 for homing.
- This covers case 12 (inverted).

**Cases 13–15, 2026-11-24, same build** (the master hadn't restarted: up 9.9 h). Right tilt (ch2, not inverted, 1105–2300; position 0 is up), speed 10, accel 0. Jeff judged each move by eye; no warnings in the log.

| Case | Moves | Fast |
|---|---|---|
| 13, scripted end to end from released | 8 | 0 |
| 14, slider to 1700, released, then scripted to 2300 | 4 | 0 |
| 15, reversed while energized (the control) | 2 | 0 (both smooth) |

- **Option A in all: 28 moves from released, none fast** (16 + 8 + 4).
- Case 14: `lastPos: 1700` in all four rounds, exactly the last slider value. Slider moves were sent over the server's websocket as the web page does (20 values, 100 ms apart).

### Mixed baud (2026-11-24): sounds lost under a slider stream, with and without the gap

HCR (9600) and Maestro (115200) on the body's channel 2. Jeff counted the sounds; every one was logged as sent, with no warnings. The first try played nothing: a bad power connector, fixed, and not the firmware.

| Run | Sounds heard | Servos |
|---|---|---|
| Sounds alone (m2), `5817749` | 8 of 8 | none moved |
| Sounds between two scripted servo moves (m1), `5817749` | 8 of 8 | 16 of 16 moves good |
| Sounds during a 40/s slider stream (left pan, 32 s, 1,272 messages), `5817749` | **6 of 8** (6 and 8 lost) | good throughout; one release, after the end |
| The same, with the gap (`c1f1143`) | **6 of 8** (3 and 4 lost) | good throughout; one release, after the end |
| Sounds alone (m2) twice, `c1f1143` | 16 of 16 | none moved |

- Slider messages reach the Maestro in bursts of about 12, with 150–190 ms of quiet between bursts. Sounds 1–4 went out 40–120 ms after the last one, and all played. Sounds 5–8 went out in the same tick or so as one, and two of the four were lost.
- In the gap run all 8 sounds went out in the middle of a burst, and still 2 were lost. The gap leaves at least 10 ms quiet on both sides of a sound, so a collision with the Maestro's bytes doesn't explain these.
- **What the numbers say:** no slider traffic, 32 of 32; slider traffic, 12 of 16. A speed change alone isn't enough (m1 made 16). Cause not found.
- **Known limit:** an HCR sharing a channel with a Maestro can miss sounds while a slider is dragged. Not run on develop, so whether the drain makes it worse isn't known. Ruling 3's policy applies.

### Panic (2026-11-24, `c1f1143`, verbose): pass

Panic sent with `POST /api/panicStop`, as the panic button does.

- **Case 7:** both pans and both tilts moving at speed 5, panic 1.7 s in. All four stopped partway. `7 off(s) queued, 0 failed` within 20 ms; no release in the next 30 s.
- **Panic during the holds (7c's shape, 4 tries):** panic 110, 150 and 220 ms after the first of four servo commands (one try landed before them). In the closest, 30 ms after the last command, with frames still behind the holds, the offs took 40 ms to queue. 7 queued, 0 failed every time; nothing moved after. `dropped` was 0 each time, so no servo command was still in `servoQueue`: that part of 7c isn't covered.
- **Case 7d, recovery:** each servo snapped to the interrupted move's target, then went home at its speed. `lastPos` is the last commanded target, and panic doesn't change it, so the pre-position frame jumps there. T-006 left `Panic` out of scope; not from T-012. Added to the existing `lastPos` item in the Backlog.

### To resume (bench continued 2026-11-24)

**State.** The master runs option A plus the gap (`c1f1143`, verbose). It still has bench leftovers: the earlier bench's Maestro config (24 channels enabled) and scripts `sT012j1`–`sT012j5`, `sT012m1`–`sT012m2` and `sT012p1`–`sT012p3` on the SD card. **Ports swapped** when the droid was power-cycled: the master's console is now `/dev/ttyACM0`. Find it by its ID (`80:65:99:BB:32:EC`), and give PlatformIO the resolved `/dev/ttyACM*` path (it can't use the by-id one). Nothing on this branch is pushed.

**Setup, confirmed with Jeff:**
- The master's server link is on this laptop (`/dev/ttyUSB0`); its console is `/dev/ttyACM1`.
- Server: a throwaway build of AstrOs.Server `develop` (2cb15025) against a copy of `.data/database.sqlite3` (`DATABASE_PATH` override, port 3000), approved. The real database and the T-016 checkout aren't touched. It was in the session scratchpad, so rebuild it if `/tmp` was cleared.
- No web page on the throwaway server, so no panic button: Jeff cuts servo power by hand.
- The HCR is connected on channel 2 (9600) and plays sounds. Maestro outputs 8–11 are empty. Maestro Control Center isn't available, so the Errors tab can't be checked.

**Jump-check scripts** (one event at 0 unless noted, `speed` 10, `acceleration` 0): j1 ch4 → 0, j2 ch4 → 100, j3 ch2 → 100, j4 ch2 → 0, j5 ch2 → 0 then 100 at 2 s. **Mixed-baud scripts:** m1 eight rounds 4 s apart (ch2 move, HCR sound, ch4 move at the same time), m2 the eight sounds alone (the baseline).

**Left to run, in order:**
1. ~~Cases 13, 14 and 15~~ (pass, 2026-11-24).
2. ~~Mixed baud~~ (done, 2026-11-24: Ruling 3).
3. ~~Panic, `maestro-servo-release.md` case 7~~ (pass, 2026-11-24).
4. Cases 9–11 (slider hammering, release races).
5. Case 11 of `random-span-events.md`: verbose, then flag-off with the timing lines (`tmp/t012-instrumentation`, rebase onto this branch) ×3. Use four Maestro digital spans on outputs 8–11, so the queue really fills (Review I1), and settle the expected number with Jeff.
6. Put back Jeff's real config by syncing it, confirm 8 enabled channels, and remove the test scripts.

**Then:** case 11 and the drain comment with the measured numbers, a review of the option A change, and the PR.
