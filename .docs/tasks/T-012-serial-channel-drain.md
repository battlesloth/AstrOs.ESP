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
- [ ] Fresh-context code review
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
