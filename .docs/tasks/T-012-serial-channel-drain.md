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
- [ ] **Bench, before anything else:** `maestro-servo-release.md` cases 12, 13 and 14, with 15 as the control, show no jump (Review C1). If one jumps, stop: the fix changes frame pacing, which the Contract pins. **Failed 2026-11-23: see Bench results.**
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
- Homing on T-012 took 90 ms for eight channels, against ~320 ms by the old pacing, and all seven servos released on time.
- **Stopped here, per the checklist.** The fix changes frame pacing, which the Contract pins: Jeff's decision. The other bench cases were not run. The master is on develop (`4c0fd65`, verbose).
- Noticed, not investigated: the T-012 image logged `App version: 1.3.0-dev.154` in `esp_app_desc` but `1.3.0-dev.156` from the firmware, after an incremental build. Those must match for an OTA version confirm; check before using a local incremental build for OTA.
