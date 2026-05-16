# Pause master polling while OTA is active

## Problem

OTA transfers over the 115200-baud serial link to the master should run at wire-rate (~480 ms per ~5.5 KB FW_CHUNK = ~2 chunks/sec), but bench measurements show effective throughput of ~1 chunk/sec — half wire capacity. ESP-side chunk-arrival timing reveals the cause:

```
seq=0 → seq=1: 430 ms  (wire-rate)
seq=1 → seq=2: 1660 ms (gap)
seq=2 → seq=3: 520 ms  (wire-rate)
...
seq=5 → seq=6: 2430 ms (gap)
seq=7 → seq=8: 3650 ms (gap, master Heartbeat + POLL_ACKs fired in this window)
```

Chunks arrive in pairs at wire-rate, with multi-second gaps in between. The gap durations align with the 2 s `pollingTimer` cycle — every 2 s the master's `pollingTimerCallback` (`src/main.cpp:409`) queues a POLL_PADAWANS ESP-NOW message AND sends its self POLL_ACK (~99 bytes) over the serial wire, and the resulting CPU + UART-TX contention with `astrosRxTask` / `astrosSerialDrainTask` / `otaReceiverTask` stalls inbound chunk processing for 1-3 s.

Concrete consequence: 294 chunks × ~1 s/chunk ≈ 295 s — right at the host's whole-transfer watchdog edge. We hit `transfer_timeout: transfer exceeded 300000ms watchdog` at chunk 195 / 294 on the 2026-05-16 bench (server log line 4596). A separate fix bumped the watchdog to 10 min to ride out the slow rate, but the underlying interference is the real bug.

## Approach

Gate the master-side polling work in `pollingTimerCallback` on `!AstrOs_OtaReceiver.isActive()`. While an OTA transfer is in flight, the timer still fires (preserving the heartbeat log + display-timeout countdown) but skips:

- Master self POLL_ACK serial TX (`sendPollAckNak`)
- POLL_PADAWANS / EXPIRE_POLLS ESP-NOW queue dispatch
- Padawan SEND_REGISTRAION_REQ branch (impossible to hit during master OTA — padawan's OtaReceiver is inactive — but gating is cheap and centralizes the rule)

Polling resumes automatically when the OTA finishes — `OtaReceiver::handleEnd` and `handleWatchdogFire` already set `active_ = false`, so the next 2-s tick takes the normal path.

`OtaReceiver::active_` is currently a plain `bool` written from `otaReceiverTask` (via `process()`) and only read from the same task. The polling timer's callback fires on `esp_timer`'s dispatch task — adding a read from there crosses a thread boundary and would be a data race under the C++ memory model. Convert `active_` to `std::atomic<bool>` and expose `isActive() const noexcept` as a public accessor. Default memory order is acquire/release (via the implicit conversion operators), which is sufficient for the visibility we need here — the polling-skip is a coarse optimization, not a correctness invariant.

## Tasks

- [x] **Make `OtaReceiver::active_` atomic + expose `isActive()`.** Changed `bool active_ = false;` → `std::atomic<bool> active_{false};` in `OtaReceiver.hpp`, added `<atomic>` include, added `isActive() const noexcept` public accessor with comment noting cross-task safety. Refreshed the class-level threading comment to flag `active_` as the one exception to the otaReceiverTask-only access invariant. No `.cpp` changes needed — `std::atomic<bool>`'s implicit conversion handles all existing `if (active_)` / `active_ = ...` sites.

- [x] **Gate the master polling branch in `pollingTimerCallback`.** Added `const bool otaActive = AstrOs_OtaReceiver.isActive();` after the existing `master`/`discovery` snapshot. Extended the master polling branch's condition from `if (master && !discovery)` to `if (master && !discovery && !otaActive)`, and the padawan-discovery branch from `else if (!master && discovery)` to `else if (!master && discovery && !otaActive)`. Heartbeat log + display-timeout decrement unchanged.

- [x] **Build both boards + native tests.** `pio run -e metro_s3` SUCCESS, `pio run -e lolin_d32_pro` SUCCESS, `pio test -e test` 309/309 PASSED.

- [x] **Bench validation.** Confirmed on the post-`f913d6e` flash run: ESP-side chunk arrivals at perfectly uniform ~480 ms intervals — the 1.5-3.5 s polling-cycle gaps from the prior bench are gone. POLL_ACK TX lines absent during the upload phase, resume after END_ACK. Heartbeat lines keep firing throughout. Host-side observed inter-ACK gap is ~1535 ms — the polling pause closed the master-side stalls but didn't reach the predicted ~500 ms cycle, because the residual ~1 s per chunk now lives on the HOST side (USB-CDC buffering / Node worker IPC / SerialPort write latency between "ACK arrives" and "next chunk hits the wire"). That's an independent bottleneck — separate speed-investigation plan.

## Files touched

- `lib/OtaReceiver/include/OtaReceiver.hpp` — `std::atomic<bool> active_` + `isActive()` public accessor
- `src/main.cpp` — `pollingTimerCallback` gate on the master-mode branch

## Out of scope

- **Stopping `pollingTimer` entirely during OTA (vs. gating its callback body).** Stopping requires lifecycle management (call `esp_timer_stop` on BEGIN, `esp_timer_start_periodic` on END / abort) and a guarantee we re-arm on every exit path (FLASH_FULL NAK, watchdog idle abort, OOM during handleBegin, …). Gating the callback keeps the timer ownership simple — if any exit path misses clearing `active_`, the worst case is "polling stays suppressed" (operator-observable, easy to debug) rather than "polling never resumes" (silent regression).
- **Pausing `maintenanceTimer` (10 s RAM logger), `animationTimer`, or `servoMoveTimer`.** Maintenance is too infrequent / cheap to matter. Animations and servo movement during an OTA are operator-driven — if the operator wants to run a script while flashing, that's their call; the OTA path shouldn't unilaterally disable other features.
- **Padawan OTA optimization.** The protocol today only ships OTA chunks over the host UART to the master; padawans receive flash payloads via ESP-NOW from the master (Phase 4 work). The gate on padawans' polling is a no-op (their `OtaReceiver::isActive()` is always false during a master flash). Revisit when padawan-OTA lands.
- **Switching `OtaReceiver::active_` to `std::atomic_flag` instead of `std::atomic<bool>`.** `atomic_flag` would be slightly cheaper but doesn't support direct boolean read of current state without a CAS — the polling timer needs a non-mutating peek. `std::atomic<bool>` is the right tool.
