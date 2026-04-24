# Plan — Fix low stack HWM on service & interface queue tasks

## Context

Bench testing of the AstrOs.ESP firmware (driven by `astros_smoke_test`'s `full-happy-path` scenario, run four times with stable heap — no leak) surfaced two FreeRTOS high-water-mark warnings that fire whenever remaining stack drops below 500 bytes:

```
W AstrOs-esp32: Service Queue Stack HWM: 144
W AstrOs-esp32: Server Response Queue Stack HWM: 96
```

HWM 96 bytes on a 4096-byte stack is well into red-zone territory — a single deeper error path (e.g., an unexpected `ESP_LOGW`, a nested retry) could tip it into overflow. `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` is enabled on every board (`sdkconfig.lolin_d32_pro:1325`, `sdkconfig.metro_s3:1509`), so overflow would panic rather than silently corrupt — but we want to avoid that panic entirely.

CLAUDE.md §Conventions: *"Every task has a high-water-mark check that warns at 500 bytes remaining — if you see that warning in logs, bump the stack rather than chasing the symptom."* Quick-feedback mode explicitly permits HWM-driven stack bumps without a plan, but the investigation covered both a memory-efficiency audit and the stack-size question, so this light plan captures the findings.

## Findings

### A. Memory efficiency audit — no meaningful waste

Audited both tasks' dispatch loops and the handlers reached from their switch statements:

- **No oversized stack buffers.** No `char buf[256+]` on the hot path in either task function or its callees.
- **No recursion, no deep format strings.** `sprintf`/`snprintf` buffers are small (≤64 B) and scoped to single calls.
- **HWM self-logging is gated.** The `ESP_LOGW` fires only when `highWaterMark < 500`, so on the happy path it costs only the `uxTaskGetStackHighWaterMark(NULL)` call (~8 B local). When the warning *does* fire, the log formatter itself uses ~400 B of stack — a minor pathology when stack is already tight, but not the root cause.
- **Depth is inherent to the handlers.** `handleFormatSD` descends into FatFS; `handleRegistrationSync` / `handleSaveScript` descend into NVS + ESP-NOW. Refactoring those chains would be weeks of work for marginal return.

**Conclusion:** The HWM warnings reflect legitimate peak stack use, not wasted memory. No efficiency fixes justified as a prerequisite for stack bumps.

### B. Stack sizes — bump both tasks

Observed max stack use:
- `serviceQueueTask`: 3072 B stack − 144 B HWM = **~2928 B peak**
- `interfaceResponseQueueTask`: 4096 B stack − 96 B HWM = **~4000 B peak**

Both peaks exceed 90% of their allocated stack. The fix is exactly what CLAUDE.md prescribes: widen the stack so the happy path sits comfortably above the 500 B warning threshold with room for error-path excursions.

## Recommended change

Edit two lines in `src/main.cpp`:

| Task | File:Line | Current | New | Expected new HWM |
|---|---|---|---|---|
| `serviceQueueTask` | `src/main.cpp:208` | 3072 | **4096** | ~1168 B |
| `interfaceResponseQueueTask` | `src/main.cpp:211` | 4096 | **6144** | ~2144 B |

Cost: ~3 KB additional SRAM reservation on a board with 320 KB. Negligible.

Why these specific targets over the tighter values considered (3584 / 5120):
- After bump, both HWMs should be well above the 500 B warning threshold, not just marginally above it. A 10-15% safety cushion over peak use is the standard sizing heuristic.
- Deeper error paths (NAK construction, malloc-fail fallbacks, log formatting during error conditions) haven't been exercised yet by the smoke test — real worst-case is likely 200–400 B deeper than what we've observed.

## Task checklist

- [x] Bump `serviceQueueTask` stack 3072 → 4096 at `src/main.cpp:208`
- [x] Bump `interfaceResponseQueueTask` stack 4096 → 6144 at `src/main.cpp:211`
- [x] Build both board envs: `pio run -e metro_s3` and `pio run -e lolin_d32_pro`
- [ ] Flash bench master; run `npm run smoke -- full-happy-path --confirm` 3–4 times; confirm no HWM warnings

## Out of scope

Three adjacent items considered and *not* bundled:

1. **Moving HWM monitoring out of the per-task loop into `maintenanceTimerCallback`** (`src/main.cpp:475`). Would require storing `TaskHandle_t` for each task (currently the 6th arg to `xTaskCreatePinnedToCore` is `NULL`) and iterating in the 10 s timer. Separate refactor; not required to fix the warnings.
2. **`handleServoTest()` unsafe `std::stoi`** — flagged P0 in `.docs/code-review/code-review-20260415.md` #2. Crash hazard, but unrelated to these HWM warnings. Existing `safeStoi()` utility in `lib_native/AstrOsUtility` already available if fixed separately.
3. **`button_listener_task` 2048 B stack** — flagged P2 in the same code review (#20, recommending 3072+). Hasn't fired an HWM warning yet in bench runs. Worth a separate quick-feedback-mode bump if/when it does.

## Verification

After the edit:

1. **Rebuild both boards:** `pio run -e metro_s3` and `pio run -e lolin_d32_pro`.
2. **Flash the bench master:** `pio run -e metro_s3 -t upload`.
3. **Run the smoke test full-happy-path scenario** from `astros_smoke_test`:
   ```
   npm run smoke -- full-happy-path --confirm --port /dev/ttyUSB0
   ```
4. **Inspect serial logs** (`pio device monitor -e metro_s3`) during the run:
   - **Expected:** No `W AstrOs-esp32: Service Queue Stack HWM` or `Server Response Queue Stack HWM` warnings.
   - **Expected:** `I AstrOs-esp32: RAM left <N>` from `maintenanceTimerCallback` continues to report stable free heap (≥180 KB typical).
5. **Repeat the scenario 3–4 times** to confirm stability — matches existing bench-test practice.
6. **If warnings still fire on either task**, bump that one again by +1024 B; the handler paths are deeper than measured.
