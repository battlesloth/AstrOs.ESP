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

- **The esp_timer task never blocks inside `CheckServos`.** Every take on that path is
  zero-wait: `xSemaphoreTake(stateMutex, 0)` (on failure skip the whole tick, WARN, re-check
  next tick), and per due channel a zero-wait try-take of `this->mutex` followed by a
  zero-timeout `xQueueSend` (on failure skip that channel this tick, WARN). No blocking
  primitive of any kind on that path. Command paths hold `stateMutex` for microseconds, so
  skips are rare, and a skipped tick only delays a release — never advances one. (The
  pre-existing 100 ms `maestroModulesMutex` take in `servoShutdownTimerCallback` is in
  `src/main.cpp` and outside this task.)
- **Lock order for blocking paths is `this->mutex` (send) → `stateMutex`.** No path ever waits
  on `this->mutex` while holding `stateMutex`. The timer's inverted order (state → *try-lock*
  send) is permitted only because the try-lock never waits, which breaks the deadlock cycle.
- **`this->mutex` is held once per operation, not per frame** (amends the 2026-08-31 "semantics
  unchanged" line). Today `sendQueueMsg` takes it around each 4-byte frame; after this task a
  command-path operation (`QueueCommand`, `SetServoPosition`, `HomeServos`, and `Panic` via
  T-004) holds it across its state update and all of its frames. Frame order on the wire is
  unchanged.
- **The send-mutex take is bounded.** Today `sendQueueMsg` retries a 100 ms take forever
  (`MaestroModule.cpp` `while (!sent)`). This task bounds it: at most 20 attempts (≈2 s), then
  `ESP_LOGE` and the operation aborts *before* touching channel state, so an aborted operation
  leaves state consistent. Repo convention: bounded takes with a log-on-failure path.
- `servo_channel` layout unchanged (no generation/epoch fields; T-002 Contract).
- Public `MaestroModule` API unchanged; Maestro wire protocol unchanged.
- Prerequisite: T-002 merged (state is per-instance, so both locks are per-instance).

## Task

Add a per-instance `stateMutex` (`SemaphoreHandle_t`) guarding the `channels` member array, and
restructure sends so one operation holds `this->mutex` once:

1. Split `sendQueueMsg`: a private lock-free `enqueueFrame(const uint8_t *cmd, size_t size,
   TickType_t wait)` that mallocs, `xQueueSend`s with the given timeout, and frees + returns
   `false` on failure; and a private `takeSendMutex()` that retries `xSemaphoreTake(this->mutex,
   pdMS_TO_TICKS(100))` at most 20 times (10 ms delay between attempts, as today) and returns
   `false` after `ESP_LOGE`. `sendQueueMsg` keeps its signature for any remaining single-frame
   caller and becomes "`takeSendMutex()` → `enqueueFrame(…, 500 ms)` → give".
2. Command paths — `QueueCommand`, `SetServoPosition`, `HomeServos` (and `Panic` in T-004):
   `takeSendMutex()` (abort the operation on `false`; no state has been touched yet) → take
   `stateMutex` (`pdMS_TO_TICKS(50)`; WARN + give `this->mutex` + abort on timeout) → write the
   channel fields → give `stateMutex` → `enqueueFrame(…, 500 ms)` for every frame of the
   operation → give `this->mutex`. `LoadConfig` copies under `stateMutex` only; its
   `HomeServos` call uses the pattern above.
3. `CheckServos` (esp_timer task): `xSemaphoreTake(stateMutex, 0)`; on failure WARN and return
   (whole tick skipped). For each servo channel that is `on`: advance the accumulator; if due,
   `xSemaphoreTake(this->mutex, 0)`; if acquired, `enqueueFrame(off, 0)`; if that returned
   `true`, set `on = false`, `currentPos = 0`; give `this->mutex`. If the try-take or the
   enqueue fails, leave the channel `on` (it is re-checked next tick) and WARN. Give
   `stateMutex` at the end. Nothing on this path may block.
4. Repo convention: bounded takes with a log-on-failure path, no `portMAX_DELAY`.

## Acceptance criteria

- [ ] Every `channels` access is under `stateMutex`.
- [ ] Each command-path operation performs its state update and all of its frames under one
      hold of `this->mutex`, acquired via the bounded `takeSendMutex()`; an aborted operation
      touches no channel state. `CheckServos` uses only zero-wait takes and a zero-timeout
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
- The 100 ms `maestroModulesMutex` take in `servoShutdownTimerCallback` (`src/main.cpp`) —
  pre-existing, outside `MaestroModule`; Backlog if it ever shows up as timer jitter.

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
