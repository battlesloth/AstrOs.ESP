# T-003: Synchronize Maestro channel state between timer and command paths

<!-- File: .docs/tasks/T-003-maestro-channel-state-sync.md. Branch: feature/T-003-maestro-channel-state-sync.
     PR title: "T-003: Synchronize Maestro channel state between timer and command paths".
     Depends on: T-002 (per-instance channel state). -->

## Context

Maestro channel state is mutated from task context (`QueueCommand` and `SetServoPosition` —
the script and slider command paths; `HomeServos` / `LoadConfig` on the boot and
RELOAD_CONFIG path; `Panic` writes too but has no caller today) and read-modify-written from
`servoShutdownTimerCallback` →
`CheckServos` (esp_timer task) with no synchronization. Concrete races:

- `QueueCommand` resets the release accumulator and sets `on = true`; an interleaved
  `CheckServos` read-modify-write can lose the reset → premature `setServoOff` mid-move.
- Multi-field updates (`speed` / `acceleration` / `requestedPos` / `on`) are not atomic →
  `CheckServos` can compute a deadline from mixed old/new state.

Same class as the cross-core-globals findings from the April 2026 code review. Found 2026-08-31
during the servo-release investigation (see T-001 Context).

**Design amended 2026-09-06 (PR #56 review).** The original plan — mark due channels under a
state lock, unlock, then send the offs — still lets a new command's target frame land *before*
the stale off: the off then turns off the new move (premature release). Root cause, shared with
the T-004 panic race: state updates and frame sends live in separate critical sections, and a
multi-frame send takes the send mutex once *per frame*, so any two paths can interleave between
a state write and its frames. The fix is structural: a command-path operation holds the send
mutex once across "update state + enqueue every frame", and the timer path decides *and*
enqueues its off while still holding the state lock, using only non-blocking primitives. One
design covers both races, so T-004 depends on this task.

## Contract (pinned — do not change)

- **The esp_timer task never blocks.** `CheckServos` may hold `stateMutex` only around
  non-blocking work. Any send from the timer path is a zero-wait try-take of `this->mutex`
  followed by a zero-timeout `xQueueSend`; if either fails the channel is skipped this tick
  (WARN) and re-checked next tick. No blocking call of any kind on that path (see the snapshot
  pattern documented above `servoShutdownTimerCallback` in `src/main.cpp`).
- **Lock order for blocking paths is `this->mutex` (send) → `stateMutex`.** No path ever waits
  on `this->mutex` while holding `stateMutex`. The timer's inverted order (state → *try-lock*
  send) is permitted only because the try-lock never waits, which breaks the deadlock cycle.
- **`this->mutex` is held once per operation, not per frame** (amends the 2026-08-31 "semantics
  unchanged" line). Today `sendQueueMsg` takes it around each 4-byte frame; after this task a
  command-path operation (`QueueCommand`, `SetServoPosition`, `HomeServos`, and `Panic` via
  T-004) holds it across its state update and all of its frames. Frame order on the wire is
  unchanged.
- `servo_channel` layout unchanged (no generation/epoch fields; T-002 Contract).
- Public `MaestroModule` API unchanged; Maestro wire protocol unchanged.
- Prerequisite: T-002 merged (state is per-instance, so both locks are per-instance).

## Task

Add a per-instance `stateMutex` (`SemaphoreHandle_t`) guarding the `channels` member array, and
restructure sends so one operation holds `this->mutex` once:

1. Split `sendQueueMsg`: a private lock-free `enqueueFrame(const uint8_t *cmd, size_t size,
   TickType_t wait)` that mallocs, `xQueueSend`s with the given timeout, and frees + returns
   `false` on failure. `sendQueueMsg` keeps its signature for any remaining single-frame caller
   and becomes "take `this->mutex` (retry loop as today) → `enqueueFrame(…, 500 ms)` → give".
2. Command paths — `QueueCommand`, `SetServoPosition`, `HomeServos` (and `Panic` in T-004):
   take `this->mutex` (existing bounded-retry loop) → take `stateMutex` (`pdMS_TO_TICKS(50)`,
   WARN + abort the operation on timeout) → write the channel fields → give `stateMutex` →
   `enqueueFrame(…, 500 ms)` for every frame of the operation → give `this->mutex`.
   `LoadConfig` copies under `stateMutex` only; its `HomeServos` call uses the pattern above.
3. `CheckServos` (esp_timer task): take `stateMutex` (`pdMS_TO_TICKS(50)`; skip the whole cycle +
   WARN on timeout). For each servo channel that is `on`: advance the accumulator; if due,
   `xSemaphoreTake(this->mutex, 0)`; if acquired, `enqueueFrame(off, 0)`; if that returned
   `true`, set `on = false`, `currentPos = 0`; give `this->mutex`. If the try-take or the
   enqueue fails, leave the channel `on` (it is re-checked next tick) and WARN. Give
   `stateMutex` at the end. Nothing on this path may block.
4. Repo convention: bounded takes with a log-on-failure path, no `portMAX_DELAY`. The existing
   unbounded retry loop in `sendQueueMsg` is not this task's to fix (Backlog).

## Acceptance criteria

- [ ] Every `channels` access is under `stateMutex`.
- [ ] Each command-path operation performs its state update and all of its frames under one
      hold of `this->mutex`; `CheckServos` sends only via zero-wait try-take + zero-timeout
      enqueue and calls no blocking primitive (verified by reading every path).
- [ ] Lock order verified by reading: no path waits on `this->mutex` while holding `stateMutex`.
- [ ] `pio test -e test` green; both board environments build clean; clang-format clean.
- [ ] Bench (human-gated): hammer one servo with slider commands for ~30 s while the 300 ms
      shutdown timer runs — no premature release mid-move, no task-watchdog warning.
      Occasional `CheckServos: send busy, retry next tick` WARNs are acceptable.
- [ ] Bench (human-gated, the PR #56 scenario): issue a move exactly as a release is due (a
      slider move ~20 s after the previous one, repeated a dozen times) — the servo always
      completes the new move and stays energized for a fresh 20 s; never a
      `Turning off servo N on module M` within 20 s after a `Setting servo N on module M`.
- [ ] QA plan `.docs/qa/maestro-servo-release.md` gains both cases above.

## Out of scope

- CheckServos release math — T-001. Global→member move — T-002. Panic wiring — T-004.
- Cross-core globals in `main.cpp` (`displayTimeout`, `discoveryMode`, `isMasterNode`, `rank`) —
  separate backlog items in the code-review catalog.
- A per-channel command-generation stamp on frames (drop-stale-frames scheme). Considered and
  rejected 2026-09-06 as the primary design — it still needs the per-operation send mutex to be
  airtight — but it is the fallback if the bench shows the timer's try-lock skip rate is too
  high.
- Bounding the retry loop in `sendQueueMsg` — PLAN.md Backlog.

## Verification

```bash
pio test -e test
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: hammer one servo with slider commands for ~30 s with the shutdown timer
# active; verify no premature release and no watchdog warning on the monitor.
# bench: slider move at ~20 s after the previous one, x12; never an off within 20 s
# of a "Setting servo" line for that channel.
```

## Implementation checklist

<!-- Added when work starts. -->
