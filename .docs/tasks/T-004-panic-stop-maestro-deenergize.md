# T-004: Make panic stop de-energize every configured Maestro channel

<!-- File: .docs/tasks/T-004-panic-stop-maestro-deenergize.md. Branch: feature/T-004-panic-stop-maestro-deenergize.
     PR title: "T-004: Make panic stop de-energize every configured Maestro channel".
     Depends on: T-002 (per-instance channel state) and T-003 (per-operation send mutex +
     stateMutex). Panic() is one more command-path operation under T-003's pattern. -->

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
was never seen. `0x9F` is also Mini Maestro 12/18/24 only, and the ESP does not know the physical
channel count, so the count byte would be a guess. The per-channel off (`setServoOff` →
`0x84 ch 0 0`) is the path `CheckServos` exercises on every release, so panic uses that.

Queued work must be dropped, not just future dispatch. `servoQueue` holds 20 entries and
`servoQueueTask` drains one per pass, so a script event that moves several servos can leave a
burst queued at the instant panic fires; each entry would be encoded and sent *after* the off
frames and re-energize its channel (PR #56 review). `handlePanicStop` therefore drains
`servoQueue` before sending the offs. Frames already in the serial queues are harmless: the
off frames enter the same FIFO behind them, so the off wins.

A command that is already past the drain — dequeued by `servoQueueTask` just before it, or a
producer already blocked inside `xQueueSend` — is serialized against `Panic()` by T-003's
per-operation send mutex: it either completes entirely *before* the offs (offs win, channel
`on = false`) or runs entirely *after* them with its state update intact (the servo
re-energizes with `on = true` and releases on the normal 20 s deadline; visible as a
`Setting servo` line after the `Panic:` line). Without T-003 the command's frames each took the
send mutex separately, so panic could slip its off between the speed frame and the target
frame and leave a servo energized with `on = false` forever (PR #56 review). That is why this
task depends on T-003.

## Contract (pinned — do not change)

- Serial and ESP-NOW `PANIC_STOP` wire formats unchanged; `lib_native/AstrOsSerialProtocol`
  and `lib_native/AstrOsEspNowProtocol` untouched.
- Maestro wire protocol: release stays `SET_SERVO_COMMAND` (`0x84`) with target 0 per channel
  via the existing private `setServoOff`. No `0x9F` frame is sent.
- Public `MaestroModule` API unchanged: `void Panic()` keeps its signature.
- `handlePanicStop` never holds `maestroModulesMutex` across `Panic()` — the per-module send
  mutex take is bounded by T-003's `takeSendMutex()` (~2.2 s) and each frame can then block up
  to 500 ms on `xQueueSend`, so `Panic()` can take seconds. Use the snapshot pattern
  documented above `servoShutdownTimerCallback` in `src/main.cpp` (bounded take, `ESP_LOGW`
  on timeout, copy the `shared_ptr`s, release, then call).
- Queue-message ownership unchanged: `sendQueueMsg` mallocs per message; the serial task frees.
  The drain in `handlePanicStop` becomes the consumer for every `servoQueue` message it removes
  and frees its payload exactly as `servoQueueTask` does after processing. No other queue is
  touched (serial queues self-heal by FIFO order; `i2cQueue` is the PCA9685 path, out of scope).
- Order inside `handlePanicStop`: `AnimationCtrl.panicStop()` first (stop dispatching), then
  drain `servoQueue`, then the Maestro offs.
- `Panic()` follows T-003's command-path locking: hold `this->mutex` once across its state
  reads, all of its off frames, and its state clear, with `stateMutex` nested briefly inside
  (lock order send → state) and never held across an enqueue. It runs on
  `interfaceResponseQueueTask`, never on the esp_timer task, so it may block on the sends.
- Tracking state is cleared only for channels whose off frame was enqueued. A channel whose
  off could not be queued stays `on` so the timer retries it (servo) or the error is the
  signal (GPIO). Never clear state ahead of a send that can fail.
- Depends on T-002 and T-003 merged.

## Task

1. Rewrite `MaestroModule::Panic()` as a T-003 command-path operation: `takeSendMutex()` (T-003's
   bounded take; on `false`, `ESP_LOGE("Panic: send mutex timeout on module %d — offs NOT
   sent")` and return — the serial path is wedged and nothing could reach the Maestro anyway)
   → take `stateMutex` (50 ms) → copy `enabled` for all 24 channels into a local flag array
   (servo **and** GPIO-type — panic means "no signal on every configured output") → give
   `stateMutex` → `enqueueFrame(0x84 ch 0 0, 500 ms)` for each enabled channel, recording
   success per channel → take `stateMutex` (50 ms) → for each channel whose enqueue
   **succeeded** set `on = false`, `currentPos = 0` → give `stateMutex` → give `this->mutex`.
   State is cleared only for channels whose off frame actually went out. A channel whose
   enqueue failed stays exactly as it was: `ESP_LOGE("Panic: off NOT queued for channel %d on
   module %d")`; a servo channel is then retried by `CheckServos` within one deadline because
   it is still `on`; a GPIO-type channel has no timer retry (they are never auto-released), so
   the error line is the operator's signal. This ordering differs from the other T-003
   command paths (which write state first) because panic's failure direction — output
   energized while tracking says off — is the one that must never happen. The single
   send-mutex hold and the no-blocking-under-`stateMutex` rule still apply: the enqueues run
   with `stateMutex` released.
   **Fallback if the `stateMutex` take times out** (practically unreachable under T-003's
   lock order — the only other holders are `CheckServos` and `LoadConfig`, for microseconds):
   WARN, read `enabled` for each channel *without* the lock — it is config-only, written by
   `LoadConfig` alone, a single byte so it cannot tear — send the offs for those channels, and
   leave `on` / `currentPos` untouched (the post-send clear is skipped too). `CheckServos` then
   sends one redundant off per channel within a deadline and clears the state itself; nothing
   is left energized or untracked.
   One `ESP_LOGI` per module (`"Panic: de-energizing module %d"`), not per channel. Delete the
   `0x9F` frame construction. Leave the `SET_MULTIPLE_SERVOS_COMMAND` define alone.
2. `handlePanicStop` in `src/main.cpp`: after `AnimationCtrl.panicStop()`, drain `servoQueue`
   with a zero-timeout `xQueueReceive` loop, freeing each message's payload; log the count
   dropped at INFO. Then snapshot `maestroModules` under `maestroModulesMutex`
   (`pdMS_TO_TICKS(100)`, warn and skip on timeout), release the mutex, and call `Panic()` on
   each module in the snapshot.
3. Update QA: rewrite case 7 in `.docs/qa/maestro-servo-release.md` to the new behavior and
   add the GPIO-channel and padawan cases below.
4. Update the `channels` comment in `lib/Modules/include/MaestroModule.hpp`: `Panic()` now
   writes on `interfaceResponseQueueTask`; drop "has no caller today".

## Acceptance criteria

- [ ] `Panic()` sends exactly one `0x84 ch 0 0` frame per enabled channel and sends nothing
      for disabled channels — all under a single hold of `this->mutex`. It marks `on = false`
      only for channels whose frame was enqueued; a failed enqueue leaves the channel `on`
      and logs an error naming it (verified by reading the code — the clear happens after the
      enqueue result). In the `stateMutex`-timeout fallback it leaves state alone and
      `CheckServos` clears it with a redundant off within one deadline. No `0x9F` byte leaves
      the module.
- [ ] `handlePanicStop` drains `servoQueue` (freeing payloads) before any off is sent, and
      reaches every module in `maestroModules` without holding the map mutex across a send
      (verified by reading the code against the snapshot pattern).
- [ ] `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build
      clean; clang-format clean.
- [ ] Bench, master (human-gated): start a slow scripted move (speed 5); send panic stop from
      the server → script halts and the servo goes slack immediately (Maestro Control Center
      shows target 0); no `Turning off servo N on module M` for that channel afterward.
- [ ] Bench, padawan (human-gated): same via ESP-NOW from the master.
- [ ] Bench, GPIO channel (human-gated): GPIO-type channel on → panic → output drops.
- [ ] Bench, queued burst (human-gated): run a script whose first event moves ≥4 servos at
      speed 5; send panic within ~1 s. All four go slack; the monitor shows
      `Panic: dropped N queued servo commands` and at most one `Setting servo` line after the
      `Panic:` line. If one appears, that channel logs `Turning off servo N on module M`
      ~20 s later — its state survived, so the normal release still fires.
- [ ] Bench, recovery (human-gated): after panic, a script or slider move re-energizes and
      moves the servo normally, and it releases on the normal deadline.
- [ ] QA plan updated (case 7 rewritten; GPIO + padawan + recovery cases added).

## Out of scope

- Flushing the serial queues (`serialCh1Queue` / `serialCh2Queue`) — FIFO order already makes
  the off frames win; see Context.
- The single in-flight message residual described in Context (fail-safe under T-003; a
  zero-residual frame-generation scheme is T-003's documented fallback).
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

- [ ] `Panic()` rewritten as a T-003 command-path operation: bounded send take → copy `enabled`
      under `stateMutex` → per-channel `0x84 ch 0 0` via `setServoOff(ch, 500 ms)` → clear
      `on`/`currentPos` under `stateMutex` only for channels whose off was queued; `stateMutex`
      timeout fallback reads `enabled` unlocked and skips the clear; 0x9F frame deleted
- [ ] `handlePanicStop`: `AnimationCtrl.panicStop()` → drain `servoQueue` (free payloads, log
      count) → snapshot `maestroModules` (100 ms, WARN on timeout) → `Panic()` per module
- [ ] `channels` comment in `MaestroModule.hpp` updated (Panic has a caller on
      `interfaceResponseQueueTask`, follows the command-path pattern)
- [ ] `pio test -e test` green; both boards build clean, no new warnings; clang-format clean
- [ ] QA plan: case 7 rewritten; GPIO, padawan, queued-burst, recovery cases added
- [ ] PLAN.md Status updated; bench (human-gated) pending
