# Code Review — Follow-up Findings

Findings discovered while implementing remediations from `code-review.md`. These were not in the original review but surfaced during focused work on related code.

---

## FF-1 — `animationTimerCallback` silently advances past dispatch failures

**Discovered:** 2026-04-12 during Phase B Task 3 (AnimationController finite timeouts)
**Severity:** P0 — safety (mechanical damage risk on sequenced operations)
**File:** `src/main.cpp` — `animationTimerCallback` around lines 495-570

### The Bug

When the animation timer callback dispatches a command via `xQueueSend` to `serialCh1Queue`, `serialCh2Queue`, `servoQueue`, `i2cQueue`, or `gpioQueue`, and the send fails (queue full, timeout), the callback logs a warning, frees the payload, and falls through to dispatch the NEXT command on the next timer tick.

Example path (lines ~503-506):
```cpp
if (xQueueSend(serialCh1Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
{
    ESP_LOGW(TAG, "Send serial queue fail");
    free(serialMsg.data);
}
// Falls through — cmd is deleted, timer restarts, next tick dispatches command N+1
```

### Why This is Dangerous

Animation scripts encode physical sequences where ordering matters for mechanical safety. Example: "open dome panel" (command N) followed by "extend greeblie through panel" (command N+1). If command N's queue send fails silently and the timer advances to command N+1, the greeblie attempts to extend through a closed panel. Mechanical damage, service cost, possible injury if a person is nearby.

The current code has this bug for every `xQueueSend` failure path in `animationTimerCallback` — 5 dispatch sites across serial, servo, I2C, and GPIO.

### Recommended Fix (Follow-up task)

On any `xQueueSend` failure during animation dispatch, call `AnimationCtrl.panicStop()` to halt the sequence. This is consistent with the Phase B Task 3 treatment of mutex timeouts in `getNextCommandPtr` and `parseScript`, which also call `panicStop`-equivalent behavior (set `scriptLoaded=false`) on failure.

Additionally, the sender should ideally distinguish between "transient queue full, retry OK" and "terminal failure, abort sequence" — but the simplest safe behavior is abort-on-any-failure, since sequenced integrity matters more than throughput.

### Suggested Phase

Not covered by Phase B (scope is thread safety on AnimationController itself, not the timer callback's dispatch logic). Candidates:
- **Phase C** — Resilience + defensive hardening. Task C5 already addresses `animationTimerCallback` refactor (deferring heavy work off the `esp_timer` task). Dispatch-failure handling fits there.
- **Standalone small task** before Phase C, since the safety implications don't warrant waiting on the broader timer-callback refactor.

### Related — upstream null-cmd path already addressed

A related case in the same callback — `getNextCommandPtr` returning `nullptr` after a mutex timeout — is handled as part of Phase B Task 3 (commit `a4f8a5b`) and a follow-up commit that re-arms the animation timer on the null-cmd early-return path. With `getNextCommandPtr` setting `scriptLoaded=false` on timeout, the re-armed timer takes the "no script loaded" else-branch on its next tick and keeps the subsystem alive for graceful recovery via future `queueScript` calls. A reboot would otherwise be required, which is undesirable because third-party hardware state on power-cycle is unpredictable — operators need the ability to dispatch recovery scripts that gently return the droid to a safe state.

The dispatch-failure case (FF-1 above) should adopt the same pattern: on any `xQueueSend` failure, halt the current script via `AnimationCtrl.panicStop()` but keep the animation timer alive for recovery scripts.
