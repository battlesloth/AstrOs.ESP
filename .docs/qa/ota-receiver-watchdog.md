# OtaReceiver idle-activity watchdog — manual QA

Verifies that `OtaReceiver` auto-aborts a stuck transfer after 10 s of no chunk activity, so a subsequent FW_TRANSFER_BEGIN succeeds without rebooting the master. Closes the "BEGIN accepted, then silence" failure mode that previously required a physical reboot.

## Preconditions

- Master ESP flashed with this branch (`feature/ota-phase3-wire-up`) so `OtaReceiver::Init(otaQueue)` runs and the watchdog timer is created at boot.
- `pio device monitor -e <env>` attached so master serial output is visible.
- AstrOs.Server running with a recent build that emits valid FW_CHUNK frames (post-CRC fix in `feature/firmware-allow-downgrade`).

## Test cases

### 1. Stuck-transfer recovery (the failure mode that motivated this work)

Reproduces the "BEGIN accepted, chunks never land" scenario the bench hit after the CRC fix landed mid-flow.

1. From `/firmware`, pick an upload, click Flash, confirm. Master log shows: `FW_TRANSFER_BEGIN accepted: transferId=N totalSize=... chunks=...`.
2. **Immediately kill the server** (`Ctrl-C` the API process) BEFORE the first FW_CHUNK arrives. The master is now in the "stuck after BEGIN" state — `active_=true`, no chunks coming, no END coming.
3. Wait 10 s while watching the master log.
4. **Pass:** within ~10 s of the BEGIN, master logs:
   `W (...) OtaReceiver: OTA watchdog fired: transferId=N idle >10000ms — aborting transfer`
5. **Pass:** restart the server, retry the flash. The next BEGIN should be **accepted** (NOT replied with `busy`):
   `I (...) OtaReceiver: FW_TRANSFER_BEGIN accepted: transferId=N+1 ...`
6. **Fail:** watchdog log doesn't appear, OR the next BEGIN returns `busy`. Either indicates the timer didn't fire or the abort handler didn't clear `active_`.

### 2. Happy-path transfer is unaffected by the watchdog

Verifies that a normal flash flow doesn't trip the watchdog (chunks arrive faster than 10 s, so the watchdog keeps getting restarted and never fires).

1. Trigger a normal flash (no kill). Watch the master log.
2. **Pass:** the master logs successive `FW_CHUNK` events as they arrive AND `FW_TRANSFER_END OK: transferId=N totalChunks=...` at the end. No `OTA watchdog fired` line appears at any point.
3. **Pass:** the next flash starts cleanly (no `busy` reply).
4. **Fail:** watchdog fires mid-transfer (would indicate either a too-tight idle threshold OR a bug in the restart path missing a code path in `handleChunk`).

### 3. Slow-bus tolerance smoke test

Flashes during nominal bench load and confirms the watchdog tolerates legitimate inter-chunk gaps.

1. Run a flash while the bench is doing other ESP-NOW traffic (poll cycles, status broadcasts).
2. **Pass:** flash completes without the watchdog firing. The ~500 ms theoretical-best per chunk leaves 9.5 s of headroom — bus contention should never push gaps that high in normal operation.
3. If you observe a watchdog fire during apparently-normal operation, capture the master log + the surrounding 30 s and treat as an idle-threshold tuning question (10 s is conservative but a heavily-loaded master could conceivably exceed it).

### 4. Repeated stuck/recover cycles

Stress the lifecycle to surface any timer-state leak.

1. Repeat test case 1 three times consecutively (each iteration: start flash, kill server before first chunk, wait 10 s for watchdog, restart, retry).
2. **Pass:** every iteration sees the watchdog fire and the next BEGIN accepted. Master memory stable (no leak from the timer create/start/stop cycles).
3. **Fail:** any iteration's BEGIN returns `busy` OR the master crashes / behaves abnormally after multiple cycles.

### 5. Watchdog disabled gracefully on timer-create failure (no easy bench repro)

Defensive case the code handles but isn't bench-reproducible without modifying the firmware. Documented for code review: if `esp_timer_create` fails at `Init()`, `watchdog_` stays `nullptr`, all watchdogStart/Restart/Stop become no-ops, and the receiver still handles happy-path transfers. Stuck-recovery falls back to physical reboot. Verify by inspection that every `watchdog*()` helper guards `watchdog_ == nullptr` at the top.

## Notes

- Idle threshold (`kWatchdogIdleUs = 10s`) is a constexpr in `OtaReceiver.hpp`. If bench measurements show legitimate transfers needing more headroom, bump the constant — single edit point.
- No server-side coordination needed. The server's chunk_streamer has its own retry/timeout that surfaces `chunk_retry_exhausted` / `transfer_timeout` to the operator on its end; the watchdog's job is just to make the master ready for the next BEGIN.
- The watchdog does NOT fire during the wait between FW_TRANSFER_END_ACK and FW_DEPLOY_BEGIN — those messages are different kinds, and `handleEnd` stops the timer before `handleDeployBegin` runs. If the server stalls between END and DEPLOY_BEGIN, the master is in idle state, not stuck.
