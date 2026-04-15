# Phase C QA Test Plan

Covers the Phase C code-review remediation work landing on branch `fix/code-review-phase-c`.

**Prerequisites:**
- A flashed master node and at least one padawan node.
- SD card mounted with deployed scripts covering all five channel types (serial, servo/Maestro, I2C, GPIO, NONE).
- Serial monitor open on both boards, `esp32_exception_decoder` filter active.
- Web UI accessible for script deploy / panic-stop.

---

## 1. Queue-depth smoke test (Task 1)

**Goal:** Confirm bumped queue depths eliminate transient "Send X queue fail" warnings under normal animation load.

1. Deploy and run a multi-channel animation script that exercises serial + servo + I2C + GPIO simultaneously.
2. Play the script back-to-back 10 times.

**Expected:** Zero `Send serial queue fail`, `Send servo queue fail`, `Send i2c queue fail`, `Send gpio queue fail` warnings across the 10 runs on either node.

---

## 2. Animation dispatch refactor (Task 2)

**Goal:** Confirm dispatch cadence, panic-stop responsiveness, and script chaining behave equivalently to the pre-refactor timer-driven implementation, now that dispatch runs from a dedicated FreeRTOS task using `vTaskDelay`.

1. **Cadence timing.** Run a script with known-interval commands (e.g. servo moves every 200 ms for 30 iterations). Timestamp each `ESP_LOGI "Maestro command val"` and compute actual inter-command deltas.
   - **Expected:** 200 ms ± 10 ms per step. No progressive drift across the 30 iterations.
2. **Long-delay scripted pause.** Run a script with a single command followed by a ≥ 5 second scripted delay, then several commands at 100 ms intervals.
   - **Expected:** First command fires, task sleeps for the 5 s, then the subsequent commands fire at their scripted 100 ms intervals — NOT a catch-up burst where the post-delay commands arrive simultaneously. (This validates the `vTaskDelay` choice over `vTaskDelayUntil`.)
3. **Panic-stop mid-script.** Start a long-running script (30+ s). Issue panic-stop from the web UI mid-playback.
   - **Expected:** Dispatch stops within one delay cycle (≤ 250 ms + the current scripted delay). No queued servo/serial/I2C/GPIO commands dispatched after the panic-stop log line.
4. **Script chaining.** Queue script A (short). When it completes, queue script B. Repeat A then C with no gap.
   - **Expected:** Each script plays to completion. No missed first command on the second script.
5. **Idle to active.** Let the device sit idle (no script) for 2 minutes. Queue a script.
   - **Expected:** Script begins within 250 ms of the queueScript call (the idle wake interval).
6. **Animation Dispatch Stack HWM.** Monitor for `Animation Dispatch Stack HWM:` warnings throughout.
   - **Expected:** No warnings (high-water-mark stays above 500 bytes). If warnings appear, bump the task's stack from 4096 to 5120 and re-test.

---

## 3. I2C leak fixes (Task 3)

**Goal:** Confirm no heap regression when I2C operations fail repeatedly, and that no heap corruption occurs from the removed double-free in `ReadWord` / `ReadTwoWords`.

1. Power the node with a PCA9685 board physically disconnected on the bus.
2. Let it run for 5 minutes.
3. Observe `RAM left` log from the maintenance timer at 10 s intervals.

**Expected:** `RAM left` value stays within ±200 bytes of its starting value across the run. No unbounded decline. No `CORRUPT HEAP` or `LoadProhibited` crash logs (which the prior double-free could have triggered intermittently).

---

## 4. Button task stack (Task 4)

**Goal:** Confirm 4 KB stack is adequate for the reset-button path.

1. Flash the firmware. Leave the device powered with no further actions for 2 minutes.
2. Press the reset button briefly (< 3 s).
3. Press and hold for 4 s (medium press).
4. Press and hold for 11 s (long press).

**Expected:** No `button_listener_task Stack HWM:` warnings in any log. Each press produces its expected side effect (reboot, etc.).

---

## 5. Storage manager error hardening (Task 5)

**Goal:** Confirm path sanitization, mkdir checks, and format error codes behave as specified.

1. **Path traversal rejection.** Deploy a script named `../foo.scr`.
   - **Expected:** Deploy NAK. Serial log shows `isPathSafe: traversal component rejected` at log level W (warning, not error). No file written outside `/sdcard/scripts/`.
2. **Absolute path rejection.** Deploy a script named `/etc/passwd`.
   - **Expected:** Same as above, with `isPathSafe: absolute path rejected`.
3. **Double-slash rejection.** Deploy a script named `scripts//foo.scr`.
   - **Expected:** `isPathSafe: double-slash rejected` at log level W.
4. **Empty-path rejection.** Deploy with an empty filename (if the web UI allows, or inject via serial).
   - **Expected:** `isPathSafe: empty path rejected` at log level W.
5. **Overlong path.** Deploy a script with a name of 200 characters.
   - **Expected:** `isPathSafe: path too long (...)` log line at level W, with the path truncated to 40 chars followed by `...` in the log output. Deploy NAKs.
6. **Full-SD deploy.** Fill the SD card to near capacity (within 1 KB). Deploy a new script.
   - **Expected:** `fwrite` or `fopen` failure logged; storage layer returns error; caller surfaces a meaningful NAK to the web UI.
7. **Format with no card present.** Remove the SD card and issue a format command from the web UI.
   - **Expected:** `formatSdCard failed:` log line from `handleFormatSD` showing a distinct `ESP_ERR_NO_MEM` (work-buffer alloc) or `ESP_FAIL` (f_mkfs) error name.
8. **Normal deploy regression.** Deploy, run, delete a valid script.
   - **Expected:** All three operations succeed with no warnings.

---

## 6. `stringFormat` snprintf (Task 6)

**Goal:** Native unit tests still pass; no observable runtime regression.

1. Run `pio test -e test`.

**Expected:** All tests pass.

---

## 7. Error counters (Task 7)

**Goal:** Confirm counters increment on induced errors and show up in the maintenance log.

1. **RX overflow.** From the connected host (via the AstrOs interface UART), send a message longer than 2000 bytes without a newline terminator.
   - **Expected:** `AstrOs RX Buffer overflow` warning immediately. Within 10 s, a maintenance log line of the form `err-counters rx-overflow=1 espnow-malloc-fail=0` at log level W.
2. **ESP-NOW malloc failure.** Difficult to induce deliberately; verify indirectly by observing the counter stays at 0 during normal operation.
   - **Expected:** `espnow-malloc-fail=0` in any counter log line during an un-stressed session.
3. **No-error idle.** Leave the device idle with no induced errors for 60 s.
   - **Expected:** No `err-counters` log lines emitted (both counters zero → log suppressed to avoid noise).

---

## 8. Regression sweep

Run these after all Phase C commits land, before merging:

1. Full animation playback across all five dispatch channels.
2. Master node polling of a padawan for 5 minutes, confirming no poll NAK escalations.
3. OLED display updates during an active animation (shows timeout / countdown as expected).
4. Fresh peer registration from the web UI.
5. Panic-stop → queueScript recovery cycle.
6. **Concurrent storage-reject + active animation.** While a long script is actively playing (30+ s), deploy a script with a path-traversal name (`../malicious.scr`) from the web UI. Confirm:
   - The deploy NAKs (storage layer rejects) without perturbing the running animation.
   - Dispatch cadence of the running script stays within its normal ±10 ms envelope.
   - No `Animation Dispatch Stack HWM:` warnings triggered by the concurrent storage activity.
   - The `isPathSafe: ... rejected` log line appears at level W (not E).

**Expected:** All behavior matches pre-Phase-C reference baseline. No new error logs, no heap decline over 10 minutes of mixed activity.

---

## Rollback

If any P0/P1 regression is identified during QA, the Phase C branch is reverse-proof as a sequence of independent commits. The revert order (most-likely-source-first) is:

1. Animation dispatch refactor (commits `0f23180` + `ee94c17`) — safety-critical; first suspect if animation behavior differs.
2. Storage manager error hardening (commits `e6e9685` + `b806450`) — first suspect if deploy / format flows regress.
3. I2C leak fixes (commit `6b0cbc8`) — first suspect if servo / OLED behavior degrades.
4. Error counters / queue depths / stack bump / stringFormat — low-risk; unlikely to cause a regression.

Use `git revert <sha>` rather than reset-hard, since the branch is already pushed or will be.
