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

### Part B — ESP-NOW TX credit pacing (`src/main.cpp` + `lib/AstrOsEspNow`)
Add a counting semaphore ("TX credits") sized a couple below the ESP-NOW internal
queue depth so the master never outruns the radio:
- Create `otaTxCredits` (FreeRTOS counting semaphore, max=N, initial=N; N≈5).
- `sendOtaFrame`: `xSemaphoreTake(credits, timeout)` BEFORE `esp_now_send`. If
  `esp_now_send` returns non-OK (NO_MEM etc.), **give the credit back
  immediately** — no send-done callback will fire for a send that didn't
  enqueue. On take-timeout, return a busy error so the tick retries later.
- `espnowSendCallback` (main.cpp): on each completion, `xSemaphoreGive(credits)`
  (ISR/WiFi-task-safe give). Fires once per `esp_now_send` that returned ESP_OK.

**Accounting invariant (the sharp edge):** every successful `esp_now_send`
(ESP_OK) produces exactly one callback → exactly one give. Every take is paired
with either (a) a give in the callback (send enqueued) or (b) an immediate give
on the non-OK return (send not enqueued). Get this wrong and credits leak →
deadlock. **Scope the credit gate to the OTA path only** (sendOtaFrame), not all
12 `esp_now_send` sites — but the single shared callback gives unconditionally,
so non-OTA sends would over-give. Resolve by either: (i) a dedicated OTA-only
send wrapper whose completions are distinguishable, or (ii) gate ALL sends with
the credit sem (simplest, uniform accounting) — DECIDE during implementation
after checking whether the callback can distinguish OTA frames. Default lean:
gate all sends uniformly (one sem, one give per callback, one take per send).

## Tasks

- [x] Part A: bump `kAckTimeoutMs` 400→1500, `kWindowSize` 8→4 in OtaForwarder.hpp.
- [ ] Part B: add TX-credit counting semaphore; take in the send path, give in
      `espnowSendCallback`, give-back on non-OK `esp_now_send` return. Decide
      OTA-only vs all-sends gating and document the accounting invariant in code.
- [ ] `pio run -e metro_s3` and `-e lolin_d32_pro` build clean.
- [ ] `pio test -e test` green (BulkTransport native tests unaffected — constants
      are runtime args; no PURE-layer change).
- [ ] Bench: re-run the padawan-only deploy; confirm NO_MEM bursts gone (or rare)
      and OUT_OF_ORDER NAK storm gone; transfer completes without manual abort.
      Capture before/after in `.docs/qa/ota-upgrade-pr-set-2.md`.

## Risk / notes

- Touches the **shared** ESP-NOW send path + the WiFi-task send callback — a
  known-fragile, brick-adjacent area (CLAUDE.md). Credit-accounting bugs
  deadlock the mesh, so the take/give pairing must be exhaustively correct
  across every `esp_now_send` return path.
- Part A alone may calm the storm enough to ship; if bench after A is clean,
  Part B can be evaluated separately. Keep the commits split so A can land
  independently of B.
- Separate from the server-side "reboot failed" bug
  (`debug/serial-rx-frame-diag`) and the version-confirm fix
  (`fix/ota-version-confirm-project-ver`). This branch is mesh-transport only.
