# T-003: Synchronize Maestro channel state between timer and command paths

<!-- File: .docs/tasks/T-003-maestro-channel-state-sync.md. Branch: feature/T-003-maestro-channel-state-sync.
     PR title: "T-003: Synchronize Maestro channel state between timer and command paths".
     Depends on: T-002 (per-instance channel state). -->

## Context

Maestro channel state is mutated from the serial-command path (`QueueCommand`, `HomeServos`,
`LoadConfig` — task context) and read-modify-written from `servoShutdownTimerCallback` →
`CheckServos` (esp_timer task) with no synchronization. Concrete races:

- `QueueCommand` resets the release accumulator and sets `on = true`; an interleaved
  `CheckServos` read-modify-write can lose the reset → premature `setServoOff` mid-move.
- Multi-field updates (`speed` / `acceleration` / `requestedPos` / `on`) are not atomic →
  `CheckServos` can compute a deadline from mixed old/new state.

Same class as the cross-core-globals findings from the April 2026 code review. Found 2026-08-31
during the servo-release investigation (see T-001 Context).

## Contract (pinned — do not change)

- Never hold a state lock across `sendQueueMsg` — it takes `this->mutex` (non-recursive) and can
  block up to 500 ms on `xQueueSend`; the esp_timer task must not block (see the snapshot pattern
  documented above `servoShutdownTimerCallback` in `src/main.cpp`).
- Existing `this->mutex` (serial-send mutex) semantics unchanged.
- Public `MaestroModule` API unchanged; Maestro wire protocol unchanged.
- Prerequisite: T-002 merged (state is per-instance, so the lock is per-instance).

## Task

Add a per-instance `stateMutex` (`SemaphoreHandle_t`) guarding the `channels` member array:

1. `CheckServos`: take `stateMutex` with `pdMS_TO_TICKS(50)` (skip cycle + `ESP_LOGW` on
   timeout — never block the esp_timer task); advance accumulators and mark due channels off
   under the lock while collecting their indices; release; then send the off commands via
   `setServoOff` outside the lock.
2. `QueueCommand` / `HomeServos` / `LoadConfig`: take `stateMutex` around their channel
   read-modify-write sections; issue UART sends outside the lock where practical.
3. Follow the repo convention: bounded takes with a log-on-failure path, no `portMAX_DELAY`.

## Acceptance criteria

- [ ] Every `channels` access in both paths is under `stateMutex`; no path holds it across
      `sendQueueMsg`/`setServoOff`/`setServoPosition` sends.
- [ ] `pio test -e test` green; both board environments build clean.
- [ ] Bench (human-gated): rapid repeated commands to one servo while the 300 ms shutdown timer
      runs — no premature release mid-move, no task-watchdog or mutex-timeout warnings.
- [ ] QA plan `.docs/qa/maestro-servo-release.md` gains the rapid-command + shutdown-timer
      interaction case.

## Out of scope

- CheckServos release math — T-001. Global→member move — T-002.
- Cross-core globals in `main.cpp` (`displayTimeout`, `discoveryMode`, `isMasterNode`, `rank`) —
  separate backlog items in the code-review catalog.

## Verification

```bash
pio test -e test
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: hammer one servo with slider commands for ~30 s with the shutdown timer
# active; verify no premature release and no warnings on the monitor.
```

## Implementation checklist

<!-- Added when work starts. -->
