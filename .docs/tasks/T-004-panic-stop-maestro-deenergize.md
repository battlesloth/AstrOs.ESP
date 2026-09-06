# T-004: Make panic stop de-energize every configured Maestro channel

<!-- File: .docs/tasks/T-004-panic-stop-maestro-deenergize.md. Branch: feature/T-004-panic-stop-maestro-deenergize.
     PR title: "T-004: Make panic stop de-energize every configured Maestro channel".
     Depends on: T-002 (per-instance channel state). If T-003 lands first, Panic() takes
     stateMutex per T-003's rules. -->

## Context

Found 2026-09-06 during the T-002 PR-toolkit review: `MaestroModule::Panic()` has no caller.
`handlePanicStop` (`src/main.cpp:2140`) calls only `AnimationCtrl.panicStop()`, which clears the
script state and stops dispatching. A servo move already sent to the Maestro completes to its
target and stays energized until the normal release deadline (20 s floor, T-001). Panic stop is
the operator's kill-switch (see the hold-tension comment above `servoShutdownTimerCallback` in
`src/main.cpp`); it must reach the hardware, not just the script.

Panic already reaches every node through one handler: the master decodes serial `PANIC_STOP`
into `AstrOsInterfaceResponseType::PANIC_STOP` → `handlePanicStop`, and also emits
`SEND_PANIC_STOP` over ESP-NOW; padawans decode that packet
(`lib_native/AstrOsEspNowProtocol`, `handlePanicStop`) into the same `PANIC_STOP` interface
message → the same `handlePanicStop`. So the wiring is one place.

The existing `Panic()` body is not usable as-is. It builds a 74-byte Pololu *Set Multiple
Targets* (`0x9F`) frame with a channel number every third byte. The real format is
`0x9F, count, firstChannel, then two target bytes per consecutive channel`, so a Maestro would
read target 0 for channel 0, target 1 (0.25 µs → clamped to the channel's minimum) for channel
1, and so on, then 23 trailing bytes as garbage commands. It has never run, which is why this
was never seen. `0x9F` is also Mini Maestro 12/18/24 only, and a count past the physical channel
count is a serial protocol error. The per-channel off (`setServoOff` → `0x84 ch 0 0`) is the
path `CheckServos` exercises on every release, so panic uses that.

Known limitation, accepted: a servo command already dequeued into `servoQueue` at the instant
panic fires still executes after the off; that one servo re-energizes and releases on the
normal deadline. Flushing the queue is out of scope.

## Contract (pinned — do not change)

- Serial and ESP-NOW `PANIC_STOP` wire formats unchanged; `lib_native/AstrOsSerialProtocol`
  and `lib_native/AstrOsEspNowProtocol` untouched.
- Maestro wire protocol: release stays `SET_SERVO_COMMAND` (`0x84`) with target 0 per channel
  via the existing private `setServoOff`. No `0x9F` frame is sent.
- Public `MaestroModule` API unchanged: `void Panic()` keeps its signature.
- `handlePanicStop` never holds `maestroModulesMutex` across `Panic()` — `sendQueueMsg` takes
  the per-module mutex and can block up to 500 ms on `xQueueSend`. Use the snapshot pattern
  documented above `servoShutdownTimerCallback` in `src/main.cpp` (bounded take, `ESP_LOGW`
  on timeout, copy the `shared_ptr`s, release, then call).
- Queue-message ownership unchanged: `sendQueueMsg` mallocs per message; the serial task frees.
- Order inside `handlePanicStop`: `AnimationCtrl.panicStop()` first (stop dispatching), then
  the Maestro offs.
- Depends on T-002 merged. If T-003 is merged first, `Panic()` takes `stateMutex` around its
  `channels[]` writes and sends outside the lock, per T-003's Contract.

## Task

1. Rewrite `MaestroModule::Panic()`: for every channel with `enabled == true` (servo **and**
   GPIO-type — panic means "no signal on every configured output"), set `on = false` and
   `currentPos = 0`, then call `setServoOff(i)`. One `ESP_LOGI` per module
   (`"Panic: de-energizing module %d"`), not per channel. Delete the `0x9F` frame construction.
   Leave the `SET_MULTIPLE_SERVOS_COMMAND` define alone.
2. `handlePanicStop` in `src/main.cpp`: after `AnimationCtrl.panicStop()`, snapshot
   `maestroModules` under `maestroModulesMutex` (`pdMS_TO_TICKS(100)`, warn and skip on
   timeout), release the mutex, then call `Panic()` on each module in the snapshot.
3. Update QA: rewrite case 7 in `.docs/qa/maestro-servo-release.md` to the new behavior and
   add the GPIO-channel and padawan cases below.

## Acceptance criteria

- [ ] `Panic()` sends exactly one `0x84 ch 0 0` frame per enabled channel, marks each
      `on = false`, and sends nothing for disabled channels. No `0x9F` byte leaves the module.
- [ ] `handlePanicStop` reaches every module in `maestroModules` without holding the map mutex
      across a send (verified by reading the code against the snapshot pattern).
- [ ] `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build
      clean; clang-format clean.
- [ ] Bench, master (human-gated): start a slow scripted move (speed 5); send panic stop from
      the server → script halts and the servo goes slack immediately (Maestro Control Center
      shows target 0); no `Turning off servo N on module M` for that channel afterward.
- [ ] Bench, padawan (human-gated): same via ESP-NOW from the master.
- [ ] Bench, GPIO channel (human-gated): GPIO-type channel on → panic → output drops.
- [ ] Bench, recovery (human-gated): after panic, a script or slider move re-energizes and
      moves the servo normally, and it releases on the normal deadline.
- [ ] QA plan updated (case 7 rewritten; GPIO + padawan + recovery cases added).

## Out of scope

- Flushing `servoQueue` / serial queues on panic (queue-ownership change; see Context
  limitation).
- PCA9685 (I²C) servo channels — panic does not reach them today either. Backlog.
- Locking between timer and command paths — T-003.
- `lastPos` staleness on re-arm after an off — PLAN.md Backlog.
- Hold-tension option — future feature.

## Verification

```bash
pio test -e test
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: master — slow script move, panic from server; monitor shows
# "Panic: de-energizing module 0"; servo slack at once; no later release log for it.
# bench: padawan — same, panic relayed over ESP-NOW.
# bench: GPIO channel on → panic → off. Then a normal move works and releases at ~20 s.
```

## Implementation checklist

<!-- Added when work STARTS, not at authoring time. Check off + commit as work proceeds. -->
