# Fix: OTA transfer NAK storm — ESP-NOW TX flow control + ack-timeout tuning

## Problem

Bench OTA of a ~1.27 MB image (metro_s3) produces a self-reinforcing congestion
storm on the master→padawan ESP-NOW link. Observed (operator aborted the run):

- **Master** (`OtaForwarder` / `AstrOsEspNow`): bursts of
  `OTA_DATA seq=N sendOtaFrame returned ESP_ERR_ESPNOW_NO_MEM; tick will retry`
  + `sendOtaFrame: esp_now_send returned ESP_ERR_ESPNOW_NO_MEM for type=28`.
- **Padawan** (`OtaWriter`): bursts of
  `handleData: xferId=1 seq=111 NAK reason=3 (hcs=113 nes=114 wr=8)` —
  `reason=3` = `NakReason::OUT_OF_ORDER`; the padawan is at hcs=113/next=114 but
  the master keeps re-sending 110/111/112 (chunks it already committed).

### Root cause (read from source)

1. **No TX flow control.** `AstrOsEspNow::sendOtaFrame` (AstrOsEspNowService.cpp
   ~1249) calls `esp_now_send` and returns immediately; it does not gate on the
   send-done callback. The ESP-NOW internal TX queue (~6 buffers) fills, then
   every further send returns `ESP_ERR_ESPNOW_NO_MEM`. The registered send
   callback `espnowSendCallback` (main.cpp ~1920) is a **no-op** today
   (comment: "Reserved for future flow-control / send-confirmation work").
2. **Ack-timeout too tight for flash cadence.** `kAckTimeoutMs = 400`
   (OtaForwarder.hpp:249) with `kWindowSize = 8`. The padawan writes each chunk
   to OTA flash (slow erase+program) and pauses other ESP-NOW traffic during
   writes, so its cumulative ACKs lag > 400 ms under load. The 50 ms tick
   (`BulkSender::tick` → `chunksToRetransmit`) then retransmits in-flight slots
   the padawan has already received → every dup triggers an OUT_OF_ORDER NAK
   (`BulkReceiver::onChunk`: `seq != nextSeq_`).
3. The redundant chunks + NAKs add airtime → more NO_MEM → more ack lag → more
   spurious retransmits. Tail risk: `kMaxRetries = 3` in `tick()` can trip
   `data_retry_exceeded` and abort the padawan under a sustained spike.

## Fix (user-selected scope: constants + TX send-done pacing)

### Part A — ack-timeout / window tuning (`lib/OtaForwarder/include/OtaForwarder.hpp`)
- Raise `kAckTimeoutMs` 400 → 1500 so the master stops retransmitting chunks the
  padawan is still flashing. (Directly kills the spurious-retransmit half.)
- Lower `kWindowSize` 8 → 4 to bound in-flight bytes and airtime pressure.
- Leave `kChunkSize=128`, `kMaxRetries=3` unchanged for now (revisit after bench).

### Part B — ESP-NOW TX in-flight counter (`src/main.cpp` + `lib/AstrOsEspNow`)

**Design choice: non-blocking atomic counter, NOT a counting semaphore.** A
blocking semaphore `take` in front of every send means any credit-accounting leak
deadlocks the whole mesh (all 9 send sites share one callback). An atomic counter
whose only consumer is a *poll-and-decline* check in the OTA drain degrades a leak
to "OTA throttles a bit more than it should" (slow), never "mesh hangs." The
window+tick machinery already polls-and-declines on `WINDOW_FULL`, so this fits
the existing control flow with no new blocking primitive.

**Centralize accounting in ONE wrapper.** All 9 `esp_now_send(mac,data,size)`
calls in `AstrOsEspNowService.cpp` are uniform, so route them through a single
file-static helper rather than counting at each site (per-site counting is the
leak hazard). Count ALL sends (poll, registration, servo-test, OTA) — they share
the same radio TX buffers, so counting them all is physically accurate, not a
compromise.

- `static std::atomic<int> espnowTxInFlight_{0};` (file-static in the .cpp).
- `espnowSendCounted(mac,data,size)`: `fetch_add(1)` → `esp_now_send` →
  `fetch_sub(1)` if it returns non-OK (no send-done callback fires for a frame
  that never enqueued). Replace all 9 raw `esp_now_send` calls with this helper.
- `AstrOsEspNow::notifyTxComplete()`: `fetch_sub(1)`. Called once, unconditionally
  (before the NULL-arg guard), at the top of `espnowSendCallback` in main.cpp —
  the send-done callback fires exactly once per enqueued frame, SUCCESS or FAIL.
- `AstrOsEspNow::espnowTxAtCapacity()`: `load() >= kEspnowTxInFlightCap`
  (cap = 6, under the 32 WiFi TX buffers, matching TX_BA_WIN=6).
- `OtaForwarder::streamDrain`: at the top of each loop iteration, if
  `AstrOs_EspNow.espnowTxAtCapacity()` then stop draining this tick (same exit
  as `WINDOW_FULL`); the 50 ms tick resumes when the radio drains.

**Accounting invariant:** every `fetch_add` is matched by exactly one
`fetch_sub` — either the immediate non-OK give-back (frame not enqueued) or the
one callback (frame enqueued). Centralizing both the add and the non-OK sub in
`espnowSendCounted`, and the callback sub in the single `notifyTxComplete`, makes
the pairing auditable in two functions. **Failure direction is safe:** if a
sub is ever missed (e.g. pending callbacks dropped on ESP-NOW teardown / abort),
the counter drifts HIGH → OTA throttles more → slower, never hung. It self-heals
as in-flight callbacks arrive. `memory_order_relaxed` throughout — the counter is
a throttle hint, not a data guard (mirrors `espnowMallocFailureCount`).

## Tasks

- [x] Part A: bump `kAckTimeoutMs` 400→1500, `kWindowSize` 8→4 in OtaForwarder.hpp.
- [x] Part B: add `espnowTxInFlight_` atomic + `espnowSendCounted` wrapper (route
      all 9 send sites through it), `notifyTxComplete` (called from
      `espnowSendCallback`), `espnowTxAtCapacity` accessor, and the cap check in
      `streamDrain`. Non-blocking — leak degrades to slow, never deadlock.
- [x] `pio run -e metro_s3` and `-e lolin_d32_pro` build clean.
- [x] `pio test -e test` green (BulkTransport native tests unaffected — constants
      are runtime args; no PURE-layer change).
- [ ] Bench: re-run the padawan-only deploy; confirm NO_MEM bursts gone (or rare)
      and OUT_OF_ORDER NAK storm gone; transfer completes without manual abort.
      Capture before/after in `.docs/qa/ota-upgrade-pr-set-2.md`.

## Risk / notes

- Touches the **shared** ESP-NOW send path + the WiFi-task send callback — a
  known-fragile, brick-adjacent area (CLAUDE.md). Credit-accounting bugs can
  over-throttle OTA or disable throttling, so the add/sub pairing must be
  exhaustively correct across every `esp_now_send` return path.
- Part A alone may calm the storm enough to ship; if bench after A is clean,
  Part B can be evaluated separately. Keep the commits split so A can land
  independently of B.
- Separate from the server-side "reboot failed" bug
  (`debug/serial-rx-frame-diag`) and the version-confirm fix
  (`fix/ota-version-confirm-project-ver`). This branch is mesh-transport only.
