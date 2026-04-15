# Code Review — Phase C: Resilience & Defensive Hardening

**Status:** Spec (design) — implementation plan produced separately via `superpowers:writing-plans`.
**Date:** 2026-04-14
**Scope:** P1 / P2 / P3 resilience and defensive-hardening items from `.docs/code-review/code-review.md` and `.docs/code-review/follow-up-findings.md`.
**Preceded by:** Phase A (memory leaks, buffer safety — merged), Phase B (thread safety, synchronization — merged).

---

## Intent

Phase C wraps up the code-review remediation work by addressing the remaining resilience items — queue sizing, timer-task stack pressure, filesystem error handling, and a handful of defensive patches. There are **no cross-cutting semantic decisions** in this phase; every task is a localized fix with bounded risk.

Two classes of items from the code review are **explicitly deferred out of Phase C** to future features:

- **Animation dispatch-failure propagation (FF-1), `std::stoi` → `strtol` migration (#14), script validation on deploy.** These are philosophically entangled: they require a cross-node panic-stop model (master fans out to padawans when any node halts internally) plus a script-corruption story. The operator-initiated panic-stop path already exists today and satisfies the "kill switch" intent. A proper design for internal panic-stop propagation and on-disk script-corruption handling will ship as a future feature.
- **Deprecated I2C driver API migration (#18).** Scheduled to coincide with an ESP-IDF 5.x bump, not a code-review patch.
- **`malloc` / `new` style convention doc (#25).** Documentation task, not a code change.

---

## Task List

| # | Source | Task | Primary files |
|---|---|---|---|
| 1 | P2 #19 | Queue depth bumps on dispatch queues | `src/main.cpp` |
| 2 | P2 #21 | Replace `animationTimer` + `esp_timer` callback with a dedicated `vTaskDelayUntil` dispatch task | `src/main.cpp` |
| 3 | P1 #13 | I2C `i2c_cmd_link_delete` on all early-return paths | `lib/I2cMaster/src/I2cMaster.cpp` |
| 4 | P2 #20 | `button_listener_task` stack: 2048 → 4096 | `src/main.cpp` |
| 5 | P2 #22 + #23 + #24 + #26 | `AstrOsStorageManager` error hardening | `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp` |
| 6 | P3 #28 | `stringFormat` `std::sprintf` → `std::snprintf` | `lib/AstrOsUtility/src/AstrOsStringUtils.hpp` |
| 7 | P3 #29 | Silent-error counters exposed via the 10 s maintenance log | `src/main.cpp` |
| 8 | — | QA test plan | `.docs/qa/code-review-phase-c.md` |

All eight are independently reversible. The only task that changes a function signature is Task 5 (`formatSdCard` → `esp_err_t` return); all others are internal.

---

## Task 1 — Queue depth bumps

### Problem

All nine FreeRTOS queues in `src/main.cpp` are created with `QUEUE_LENGTH = 5`. The animation dispatch path can enqueue one message per channel per frame, and bursty producers (maestro servo traffic, OLED display coinciding with animation frames) can fill a depth-5 queue faster than the consumer task drains it.

### Design

Raise depth on the five dispatch queues. Leave the four control/response queues (`animationQueue`, `serviceQueue`, `interfaceResponseQueue`, `espnowQueue`) at 5 — they carry low-rate coordination traffic.

| Queue | Current | New | Rationale |
|---|---|---|---|
| `servoQueue` | 5 | 20 | Highest churn — fan-in from animation frames plus Maestro traffic; two PCA9685 boards produce bursts. |
| `i2cQueue` | 5 | 16 | Shared by OLED display writes *and* animation I2C commands; display updates can coincide with animation frames. |
| `serialCh1Queue` | 5 | 10 | Doubled for slack; at most one serial channel dispatched per frame. |
| `serialCh2Queue` | 5 | 10 | Same. |
| `gpioQueue` | 5 | 10 | Low frequency but cheap to double. |
| Other queues | 5 | 5 | Unchanged. |

### Risk

Deeper queues mean an `xQueueSend` success no longer guarantees prompt consumer service. For animation sequencing this is acceptable because commands are already time-ordered by the dispatch task at fixed intervals; a deep queue just adds buffering headroom. Called out in the QA plan so tuning gets real-world validation.

### Verification

Smoke test multi-channel animation scripts and confirm no `"Send X queue fail"` warnings across repeated playbacks.

---

## Task 2 — Animation dispatch refactor: drop `esp_timer`

### Problem

The `animationTimerCallback` runs in the shared `esp_timer` service task (fixed ~4 KB stack). The callback performs multiple `malloc` / `memcpy` / string-format / `xQueueSend` operations per tick — contrary to the `esp_timer` idiom of returning in microseconds and risking stack overflow in the shared timer task.

### Design

Replace the timer + callback entirely with a dedicated FreeRTOS task:

- **Delete** `animationTimer` (`esp_timer_handle_t`) and `animationTimerCallback()` from `src/main.cpp`.
- **Create** `animationDispatchTask`, pinned to **core 1**, stack size **4096**, same priority tier as other I/O consumer tasks. High-water-mark check follows the project convention (warn at 500 bytes remaining).
- **Loop shape:**

  ```cpp
  TickType_t lastWake = xTaskGetTickCount();
  while (true) {
      // Body of the former animationTimerCallback — minus esp_timer_start_once.
      // Reads AnimationCtrl.scriptIsLoaded(), calls getNextCommandPtr(),
      // switches on MODULE_TYPE, builds + enqueues downstream message.
      // Free payload on xQueueSend failure (ownership convention unchanged).

      uint32_t delayMs = AnimationCtrl.msTillNextServoCommand();
      if (delayMs < 10) delayMs = 10;   // minimum wake interval; matches idle-poll granularity
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(delayMs));
  }
  ```

- **`vTaskDelayUntil`** is deliberate: it schedules against an absolute last-wake timestamp, so the dispatch cadence doesn't drift by the command-build duration each iteration.
- **Idle state** (no script loaded): the loop still runs, reads `scriptIsLoaded() == false`, takes the else branch, and sleeps on a minimum wake interval. Matches current callback behavior for the null-cmd recovery path from Phase B.
- **Startup:** spawn `animationDispatchTask` in `setup()` alongside the other task creations. Remove the `esp_timer_create` + `esp_timer_start_once` calls for the animation timer.
- **Shutdown:** `vTaskDelete` on the task handle if teardown is ever needed (not used today, listed for completeness).

### Rationale for dropping `esp_timer`

Considered keeping `esp_timer` + adding a dispatch queue + new task (tick-enqueue pattern). Rejected because:

1. It preserves two state machines (timer + consumer task) to solve a problem that collapses into one (the task itself).
2. The tick-enqueue dispatch queue is a new leak/ownership surface for no behavioral benefit — `vTaskDelayUntil` provides the cadence directly.
3. Ownership of animation dispatch code consolidates into one location instead of being split between timer re-arm (in the callback) and state reads (in the controller).

Tradeoffs accepted:

- `vTaskDelay` resolution is 1 ms at `CONFIG_FREERTOS_HZ=1000`; `esp_timer` is microsecond-resolution. Not material for animation delays in the 10+ ms range.
- The task is blocked in `vTaskDelayUntil` rather than `xQueueReceive`, so an external "wake immediately" pre-emption would need `xTaskNotify`. Not needed today; can be added later if required.

### Risk

Medium — animation dispatch timing is safety-relevant; any cadence regression is observable during real-world playback. Mitigated by:

- `vTaskDelayUntil` honors absolute cadence.
- Ownership / malloc / free semantics carry over verbatim from the current callback — no new allocation sites or frees.
- Panic-stop latency is bounded by one `delayMs` cycle, same as today's `esp_timer_start_once` pattern — no regression vs. current behavior.

### Verification

QA plan includes timing observation via log timestamps, panic-stop mid-script, and script chaining (queue → run → queue → run). Dispatch latency should be below ~2 ms on top of the scripted delay.

---

## Task 3 — I2C `cmd` handle leaks

### Problem

`lib/I2cMaster/src/I2cMaster.cpp` uses the legacy ESP-IDF 4.x API pattern: `i2c_cmd_handle_t cmd = i2c_cmd_link_create()`, chain operations, `i2c_master_cmd_begin(cmd)`, `i2c_cmd_link_delete(cmd)`. An inspection of the file surfaced three distinct problems, more severe than the original review described:

1. **`DeviceExists` (line 50)** — `cmd` is created but `i2c_cmd_link_delete(cmd)` is never called. Leaks on every I2C device-detection call (peer bringup, bus scan).
2. **`ReadWord` (lines 209–215)** — `cmd` is re-assigned (line 215) without deleting the handle produced by the previous `i2c_cmd_link_create()` (line 209). Then at line 233 the code calls `i2c_cmd_link_delete(cmd)` a second time on a handle already deleted at line 222 → **double-free / use-after-free**.
3. **`ReadTwoWords` (lines 264–270 and 306)** — identical pattern to `ReadWord`: a leaked `cmd` followed by a double-delete.

Early-return paths in these functions also skip `i2c_cmd_link_delete` on the current handle, which is the originally reviewed issue.

### Design

Fix the three concrete bugs above, then audit the remaining `i2c_cmd_link_create` call sites for early-return paths that skip delete.

1. **`DeviceExists`** — add `i2c_cmd_link_delete(cmd)` before `xSemaphoreGive`.
2. **`ReadWord`** — remove the stray double-create on lines 209–210; keep only the `cmd = i2c_cmd_link_create()` at line 215 (which becomes the second handle). Remove the duplicate `i2c_cmd_link_delete(cmd)` at line 233.
3. **`ReadTwoWords`** — identical shape: remove the stray create at 264–265 and the duplicate delete at line 306.

For every other function with an `i2c_cmd_link_create`, confirm each early-return branch has `i2c_cmd_link_delete(cmd)` + `xSemaphoreGive(i2cBusMutext)` before returning, in that order.

No RAII wrapper is introduced — the branch count per function after the fixes above is 1–2 early returns, within the "plain cleanup is clearer" threshold from the spec.

**Note:** Lines 318+ of `I2cMaster.cpp` are wrapped in `/* ... */` — an incomplete migration to the new ESP-IDF 5.x `i2c_master_bus_*` API. This Phase C work does not touch the commented-out code.

### Risk

Low. All edits are additive cleanup — no logic shifts.

### Verification

Hardware test: force I2C errors (disconnect a PCA9685), observe logs for repeated errors, confirm `esp_get_free_heap_size()` stays stable across ~5 minutes of induced failures.

---

## Task 4 — `button_listener_task` stack bump

### Problem

`xTaskCreatePinnedToCore(button_listener_task, "BTN", 2048, NULL, 5, NULL, 1)` — 2 KB is tight. Task does GPIO reads, `ESP_LOGW`, and queue sends; formatted logging alone consumes several hundred bytes of stack. The existing high-water-mark warning at 500 bytes remaining suggests this is already close to the edge.

### Design

Change the stack size argument from `2048` to `4096`. Aligns with the other I/O tasks in the codebase (servo / i2c / gpio consumer tasks).

### Risk

Effectively zero. +2 KB RAM — irrelevant on either board.

### Verification

Flash and observe the `BTN` high-water-mark log line over a typical session; confirm no warnings.

---

## Task 5 — `AstrOsStorageManager` error hardening

Four review items collapse into one consolidated pass on `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp`.

### 5a — File handle validation (#22)

Every `fopen()` needs a `NULL` check before use, and no code should call `fclose(NULL)`. Mechanical audit: grep for `fopen`, confirm each call site has `if (fd == NULL) { ESP_LOGE(...); return error; }`, confirm no `fclose` is reachable with a potentially-null `fd`.

### 5b — Path sanitization (#23)

File paths built from external input (script names and peer names delivered via serial / ESP-NOW) must be rejected if they attempt to escape the intended mount point. Add a helper:

```cpp
bool isPathSafe(const char *path);
```

that returns false + logs the rejection reason when:

- `path` contains `..` anywhere
- `path` starts with `/` (scripts/configs are expected to be mount-relative)
- `path` contains `//`
- `path` length exceeds the downstream buffer (mount prefix + path + null terminator fits)

Call `isPathSafe` at every site that constructs a filesystem path from externally-supplied data (load/save/delete script, load/save config, etc.). Reject with a logged error and propagate the failure upstream — callers already handle a bool-false return from storage ops.

### 5c — `mkdir` return value checks (#24)

Every `mkdir()` call must check the return. Success is either `0` or `errno == EEXIST` (pre-existing directory is fine). Any other `errno` gets logged with `ESP_LOGE` and propagated as a failure to the caller.

### 5d — `formatSdCard` error distinction (#26)

Today `formatSdCard` returns `bool false` for both "format failed" and "work-buffer OOM". Change:

- Return type: `bool` → `esp_err_t`
- Return values: `ESP_OK` on success, `ESP_ERR_NO_MEM` on work-buffer allocation failure, pass-through of `esp_vfs_fat_*` error codes on format failure.
- Update the single caller in `main.cpp`'s service queue task to accept `esp_err_t` and surface the distinct errors to the interface response.

### Scope guard

No refactors, no other API changes. If a fifth issue surfaces during the work it gets logged as a follow-up finding, not absorbed into this task.

### Risk

Low-to-medium. 5d is a signature change with a single call site (tracked in lockstep). 5b could reject a path the web UI currently relies on — confirm during QA that realistic deploys still succeed.

### Verification

- Deploy a script with `..` in its name — NAK expected.
- Deploy a script with a path over buffer length — NAK expected.
- Deploy to a full SD — meaningful log + propagated failure to caller.
- Format SD with no card present — `ESP_ERR_NO_MEM` (or equivalent) surfaces distinctly from a format-failed error.
- Regression: confirm normal deploy/load/delete flows still work.

---

## Task 6 — `stringFormat` `std::sprintf` → `std::snprintf`

### Problem

On inspection, `lib/Uuid/guid.h` already uses `snprintf` (review item #28 was stale). The remaining `std::sprintf` in the codebase is at `lib/AstrOsUtility/src/AstrOsStringUtils.hpp:155` in the `stringFormat` template:

```cpp
template <typename... Args> static std::string stringFormat(const std::string &format, Args &&...args) {
    auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
    std::string output(size + 1, '\0');
    std::sprintf(&output[0], format.c_str(), std::forward<Args>(args)...);
    return output;
}
```

Although the preceding `std::snprintf(nullptr, 0, ...)` computes the exact needed size, using `std::sprintf` here is still a defense-in-depth issue — if `size` or `output` is ever mis-computed by a future edit, `sprintf` gives no bounds protection.

### Design

Replace `std::sprintf(&output[0], format.c_str(), ...)` with `std::snprintf(&output[0], size + 1, format.c_str(), ...)`. One-line change.

### Risk

Zero. Semantics unchanged when the existing `size` is correct; a safety net when it is not.

### Verification

Build both board environments and run native tests (`stringFormat` is used across `lib/AstrOsMessaging`, which has native test coverage).

---

## Task 7 — Silent-error counters

### Problem

Two sites silently drop errors:

- **`astrosRxTask()`** — on buffer overflow, resets `bufferIndex = 0` and discards the partial message with only a warning log.
- **`espnowRecvCallback()`** — on `malloc` failure, returns without propagating the error.

These are observable only if an operator happens to be watching the serial monitor when they occur. A running tally is cheaper to monitor.

### Design

Two atomic counters in `main.cpp`, exposed via the existing 10-second maintenance timer that already logs free heap:

```cpp
static std::atomic<uint32_t> astrosRxOverflowCount{0};
static std::atomic<uint32_t> espnowMallocFailureCount{0};
```

Increment with `.fetch_add(1, std::memory_order_relaxed)` at each of the offending sites. In the maintenance timer callback, if either counter is non-zero, emit:

```
ESP_LOGI(TAG, "err-counters rx-overflow=%u espnow-malloc-fail=%u", ...)
```

Skip the log line entirely when both counters are zero — avoids noise in a healthy system.

### Rationale for `std::atomic` over mutex

Single-word increments are atomic on ESP32 with `<atomic>`. `espnowRecvCallback` runs in the Wi-Fi driver task — a mutex would add unnecessary latency on a hot ISR-adjacent path. Relaxed memory ordering is sufficient because the counters are read-only observation; no happens-before relationship is needed with other state.

### Rationale for not building a general framework

YAGNI. If a third site surfaces, add a third counter and a third tally in the log line. A registry / dynamic counter framework would be design speculation.

### Risk

Essentially zero. Additive code, log line is skipped when counters are zero.

### Verification

Force a buffer overflow in `astrosRxTask` (send oversized message), confirm counter increments in the next maintenance log cycle.

---

## Task 8 — QA test plan

Deliverable: `.docs/qa/code-review-phase-c.md` with sections for each of the above tasks. Coverage:

- **Queue depth** — multi-channel animation script, 10+ playbacks, no `"Send X queue fail"` warnings.
- **Animation dispatch refactor** — timing vs. expected script delays, panic-stop responsiveness mid-script, script chaining, idle-to-active transitions.
- **I2C leak** — induced-error heap-stability test over ~5 min.
- **Button stack** — high-water-mark watch over a typical session.
- **Storage hardening** — path-traversal deploys (expect NAK), oversized path deploys, full-SD deploy, format-with-no-card, regression on normal deploy/load/delete.
- **GUID snprintf** — covered by existing GUID-exercising paths.
- **Error counters** — induced buffer overflow, counter visible in 10 s maintenance log.
- **Regression sweep** — full animation playback across all five dispatch channels, peer registration + polling, OLED updates.

No new native unit tests — no Phase C work lands in `lib/AstrOsMessaging` or `lib/AstrOsUtility`. All verification is hardware-in-the-loop.

---

## Out of scope (deferred)

| Item | Reason | Future home |
|---|---|---|
| FF-1 dispatch-failure propagation | Needs cross-node halt design; philosophically tied to script corruption | "Internal panic-stop + corruption handling" feature |
| P1 #14 `std::stoi` → `strtol` | Parse-failure semantics are entangled with script validation and internal panic-stop | Same feature as above |
| Script validation on deploy (implied by #14) | Proper feature scope — grammar, NAK messaging, web UI feedback | Same feature as above |
| P1 #18 Deprecated I2C driver API | Paired with ESP-IDF 5.x bump | IDF bump project |
| P2 #25 malloc/new style doc | Documentation, not a code change | Standalone doc PR |

---

## Commit and review

Plan file committed to `.docs/plans/` before any implementation code lands, per the `CLAUDE.md` planning rule. Implementation plan with checklist produced separately by the `superpowers:writing-plans` skill once this spec is approved.
