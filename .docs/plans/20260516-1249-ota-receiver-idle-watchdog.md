# OtaReceiver — idle-activity watchdog (Phase 5 forward-port)

## Problem

`OtaReceiver` sets `active_ = true` on FW_TRANSFER_BEGIN accept and clears it only on FW_TRANSFER_END. When a transfer is abandoned mid-flight (server crash, all chunks parse-failing, link drops between BEGIN and the first chunk), `active_` stays `true` forever. Every subsequent FW_TRANSFER_BEGIN is rejected with `busy` and the master is unreachable for new flashes until rebooted.

Surfaced on the bench after the FW_CHUNK CRC fix landed server-side: a previous flash attempt left the master stuck because all chunks were parse-rejected (placeholder CRC), so the post-fix retry hit `busy`. Reboot is the current workaround.

The Phase 3 wire-up plan explicitly defers the watchdog to Phase 5 ("Phase 5 hardens failure modes and adds a watchdog if measurements show it's needed"). Bringing it forward because measurements now show it IS needed — a single parse-time bug requires a physical reboot to recover.

## Approach

Idle-activity watchdog backed by `esp_timer`:

- Timer is created in `OtaReceiver::Init()` with a callback that posts an `OTA_MSG_WATCHDOG_FIRE` message back to the otaQueue. The callback runs on esp_timer's dispatch task, but the abort itself happens on otaReceiverTask via the normal `process()` switch — matches the existing "all OtaReceiver state mutation runs on otaReceiverTask" invariant.
- Timer is **started** at the end of `handleBegin` (success path) and **restarted** at the end of `handleChunk` (every chunk activity, ACK or NAK).
- Timer is **stopped** at the end of `handleEnd` (transfer completed normally) and inside the watchdog handler itself (defensive — esp_timer doesn't auto-stop after a one-shot fire, but `esp_timer_stop()` on an idle timer is a documented no-op).
- Threshold: **10 seconds** of idle. At 115200 baud with a 16-frame window, the inter-chunk gap is well under 1s in healthy operation. 10s leaves generous headroom for slow links / bursty bus contention while still surfacing the abandoned-transfer case within a single operator's retry window.

No FW_* reply on watchdog fire — the server's chunk_streamer has its own timeout that surfaces `chunk_retry_exhausted` / `transfer_timeout` to the operator. The master's job is just to recover so the next BEGIN works; staying silent avoids racing the server's own failure surface.

## Tasks

- [ ] **Add `OTA_MSG_WATCHDOG_FIRE` to the union.** Extend `ota_msg_kind_t` in `lib/OtaReceiver/include/OtaQueueMessage.h` with the new kind (value 4). No union arm needed — the message carries only the kind byte; `transferId` is nullptr. Update the per-kind owned-pointers comment to note "WATCHDOG_FIRE: none."

- [ ] **Wire the queue handle through OtaReceiver.** Add `QueueHandle_t otaQueue_ = nullptr;` member; extend `Init()` to take the queue handle so the timer callback can post to it. Update `main.cpp` line 276 (or wherever Init runs) to pass the otaQueue. Without the handle, the timer callback has nothing to post to.

- [ ] **esp_timer lifecycle in OtaReceiver.** New private member `esp_timer_handle_t watchdog_ = nullptr;`. `Init()` creates the timer (one-shot, callback queues the WATCHDOG_FIRE message). `handleBegin` (after `active_ = true`) calls `esp_timer_start_once(watchdog_, kWatchdogIdleMs * 1000)`. `handleChunk` end (after either ACK or NAK branch) calls `esp_timer_stop(watchdog_)` then `esp_timer_start_once(...)` (the "restart" pattern; esp_timer doesn't have a single restart call for one-shot timers). `handleEnd` calls `esp_timer_stop(watchdog_)`. Constant: `static constexpr uint64_t kWatchdogIdleMs = 10'000;`.

- [ ] **`handleWatchdogFire()` handler.** Logs `ESP_LOGW(TAG, "OTA watchdog fired: transferId=%s idle >%ums — aborting transfer", transferIdStr_.c_str(), kWatchdogIdleMs)`. Then: `bulk_.reset(); active_ = false; transferIdStr_.clear(); beginMsgId_.clear();`. No FW_* reply (see Approach above). Update `process()` switch to route OTA_MSG_WATCHDOG_FIRE; update the default-case abort to ignore the new kind cleanly.

- [ ] **QA + manual verification.** Add `.docs/qa/ota-receiver-watchdog.md`: simulate the abandoned-transfer scenario (BEGIN accepted, no chunks within 10s) and verify the LOGW fires + a subsequent BEGIN is accepted. Real-world repro: revert the server's CRC fix temporarily, attempt a flash, wait 10s, restore the CRC fix, retry without rebooting. Build + manual flash via `pio device monitor`.

## Files touched

- `lib/OtaReceiver/include/OtaQueueMessage.h` — new kind
- `lib/OtaReceiver/include/OtaReceiver.hpp` — esp_timer member, queue handle, new handler decl
- `lib/OtaReceiver/src/OtaReceiver.cpp` — timer create/start/stop, new handler, process() switch
- `src/main.cpp` — pass otaQueue to OtaReceiver Init
- `.docs/qa/ota-receiver-watchdog.md` (new)

## Out of scope

- **Whole-transfer (5-min) backstop watchdog.** The original Phase 5 design notes mentioned an "optional 5-min whole-transfer watchdog" alongside the idle watchdog. Idle-only covers the failure mode that actually surfaced; add the whole-transfer backstop only if a future scenario demonstrates the idle watchdog is insufficient.
- **Server-side FW_TRANSFER_CANCEL command.** Considered as Option C in the diagnostic; not pursued — protocol surface bloat for a scenario the firmware-side watchdog handles end-to-end. The server-side chunk_streamer already has its own timeout that surfaces the failure to the operator.
- **Notifying the server on watchdog fire.** Could send FW_CHUNK_NAK with a `transfer_timeout` reason, but the server has its own watchdog and the master's job is to recover, not narrate. Silent cleanup is the simpler contract.
- **Persistent state for `active_` across reboots.** Not relevant — reboot is the current escape hatch AND clears `active_` naturally. Watchdog covers the no-reboot recovery case.

## Threading note

The esp_timer callback runs on esp_timer's dispatch task, NOT otaReceiverTask. The callback only does `xQueueSend(otaQueue_, &msg, 0)` (with `pdMS_TO_TICKS(0)` to avoid blocking the dispatch task). The actual state mutation happens on otaReceiverTask via `process(OTA_MSG_WATCHDOG_FIRE)`, preserving the existing single-task-state-mutation invariant. If the queue is full (otaReceiverTask is wedged), the xQueueSend returns errQUEUE_FULL — log it and move on; if otaReceiverTask is wedged, the watchdog firing won't help anyway, and the next flash attempt will surface the wedge through its own failure path.
