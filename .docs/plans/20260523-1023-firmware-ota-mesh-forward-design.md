# Firmware OTA — Mesh forward + validate on padawan (design)

## Context

The cross-repo Firmware OTA feature is decomposed in
`AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md`
into five sub-projects. As of 2026-05-23 the Server-side sub-projects (c0
JobLock, c orchestrator, d Vue Firmware view) have shipped, and the ESP-side
**serial-receive milestone** has shipped through Phase 4 (PR #33) plus the
software-SHA-256 fix (PR #34, merged commit `51d6d5b`). The end-to-end serial
path is working: server uploads a 1.2 MB `.bin` to the master, master writes
`/sdcard/firmware/staging.bin`, verifies SHA-256, atomically renames to
`/sdcard/firmware/<sha-prefix>.bin`. The receiver then emits `FW_DEPLOY_DONE`
with all targets `FAILED("not_implemented")` — a stub from Phase 3.

This design covers the **next phase**: actually forwarding the staged `.bin`
to padawans over ESP-NOW. It is deliberately split from the *commit-and-boot*
work that follows, in two PR sets:

> **PR set 1 (this design)** — Master reads the staged `.bin` from SD and
> forwards it to padawans over ESP-NOW. Padawans write to their inactive OTA
> partition, verify SHA-256 (streaming + read-back-rehash), and emit
> `OTA_END_ACK OK` **without committing the boot partition**. No reboot.
> Bricking risk is firewalled out of this PR set entirely.
>
> **PR set 2 (separate plan, not in this design)** — Padawan calls
> `esp_ota_set_boot_partition`, reboots, master observes
> `VERSION_CONFIRMED` via the existing 2 s heartbeat. Master self-flashes
> last. `OTA_COMMIT` / `OTA_COMMIT_ACK` wire types added.

PR set 1 itself decomposes into 5 milestones (M1–M5), each one shippable PR.

### What's already shipped that this builds on

- **Serial-receive milestone** (PRs #28–#34): `OtaReceiver` lands verified
  firmware at `/sdcard/firmware/<sha-prefix>.bin` on master. PR set 1 reads
  this file as input.
- **`AstrOsBulkTransport` PURE lib** (PR #29): `BulkReceiver` + CRC-16/CCITT-FALSE
  helper, fully native-tested. PR set 1 extends with `BulkSender` (M2) and
  reuses `BulkReceiver` on the padawan side (M4).
- **Padawan partition layout**: both `partition_8mb.csv` (lolin_d32_pro) and
  `partition_16mb.csv` (metro_s3) define `ota_0` + `ota_1` slots with plenty
  of room for a 1.2 MB image (2 MB and 6.25 MB respectively).
- **Polling-pause pattern**: `OtaReceiver::isActive()` gates the master's
  `pollingTimerCallback`. PR set 1 extends the gate to also include
  `OtaForwarder::isActive()`, and adds a symmetric `OtaWriter::isActive()`
  gate on the padawan side.
- **Server-side orchestrator + UI**: Sub-projects (c) and (d) already produce
  `FW_DEPLOY_BEGIN` and consume `FW_PROGRESS` / `FW_DEPLOY_DONE`. No
  server-side or Vue changes required for PR set 1.
- **All ESP nodes carry SD cards** (master and padawans). Padawans load and
  run scripts locally; "master" only designates the Raspberry Pi serial
  interface, not an SD-vs-no-SD split.

## Decisions (confirmed with user 2026-05-23)

- **Padawan storage**: write to **inactive OTA partition** via
  `esp_ota_begin` / `esp_ota_write` / `esp_ota_end`. **Do not** call
  `esp_ota_set_boot_partition` in PR set 1. The dual-partition design is the
  natural firewall — bytes get fully committed to flash, but the running app
  is untouched until the boot pointer flips in PR set 2.
- **Read-back-and-rehash**: padawan re-reads the inactive partition after
  `esp_ota_end`, recomputes SHA-256, compares to the streamed digest. Adds
  one extra safety layer beyond streaming SHA (catches the rare "flash
  write returned OK but stored corrupted bytes" mode). Cost: ~1–3 s per
  padawan.
- **Sender architecture**: new PURE `BulkSender` class in
  `lib_native/AstrOsBulkTransport`, mirroring the existing PURE
  `BulkReceiver`. New MIXED `lib/OtaForwarder` wraps it with file I/O,
  ESP-NOW TX, and FreeRTOS timer-driven tick. Symmetric to the
  `OtaReceiver` / `BulkReceiver` split that worked for serial.
- **Sub-phasing**: 5 sub-PRs (M1 wire format · M2 BulkSender PURE · M3
  master OtaForwarder · M4 padawan OtaWriter · M5 orchestration +
  FW_PROGRESS). Each ~5–8 tasks, each ships independently.
- **FW_PROGRESS cadence**: stage transitions + every ~5% of `totalBytes`
  during SENDING. ~22 progress events per padawan for a 1.2 MB image.
  Server's existing 4 Hz throttle smooths any bursts.
- **Master entry in `FW_DEPLOY_BEGIN` order list**: record as
  `FAILED("not_implemented_in_pr_set_1")`. Honest signal to server vs
  silent skip (false-green checkmark).
- **`OTA_COMMIT` / `OTA_COMMIT_ACK` wire types**: deferred to PR set 2.
  Adding to the enum but leaving handlers stubbed would muddy the
  scope-boundary.
- **Sequential per-padawan delivery**: walk the order list one padawan at
  a time, 0 ms inter-padawan idle (per frozen contract). Parallel /
  broadcast delivery is explicitly out of scope — ESP-NOW topology and
  per-padawan ACK demux would massively complicate the state machine.
- **Resume-after-error policy**: v1 = abort that padawan, mark FAILED,
  advance to next in order list. Resume-from-seq-N is a future extension
  the wire format already supports via `next-expected-seq`.

## Architecture

### Lib classification

| Lib | Class | Summary |
|---|---|---|
| `lib_native/AstrOsBulkTransport` (extend) | PURE | Add `BulkSender` class alongside existing `BulkReceiver`. Sliding-window tracking, retransmit decisions, retry counts, abandonment policy. No I/O, no FreeRTOS, no ESP-IDF. |
| `lib_native/AstrOsMessaging` (extend) | PURE | Add `OTA_BEGIN`/`OTA_BEGIN_ACK`/`OTA_BEGIN_NAK`/`OTA_DATA`/`OTA_DATA_ACK`/`OTA_DATA_NAK`/`OTA_END`/`OTA_END_ACK` to `AstrOsPacketType` enum. Binary-frame builders + parsers. |
| `lib_native/AstrOsEspNowProtocol` (extend) | PURE | POD record types for parsed frames; decode/dispatch routing with role gating. |
| `lib/OtaForwarder` (new, master-only) | MIXED | Wraps `BulkSender`. Reads `/sdcard/firmware/<sha>.bin`. Drives ESP-NOW unicast TX. Walks order list. Emits `FW_PROGRESS` to `interfaceResponseQueue`. Drains `otaForwarderQueue`. |
| `lib/OtaWriter` (new, padawan-only) | MIXED | Wraps existing `BulkReceiver`. Owns `esp_ota_handle_t` (no `set_boot_partition`). Streaming SHA via mbedtls. Read-back-and-rehash. `std::atomic<bool> active_` for polling pause. Drains `otaWriterQueue`. |
| `lib/AstrOsEspNow` (extend) | MIXED | New binary-frame TX overload: `send(mac, AstrOsPacketType, const uint8_t *payload, size_t len)`. Existing string-payload path unchanged. Dispatch new `OTA_*` arrivals into the role-appropriate queue. |

`OtaReceiver` (the existing master-side serial-receive lib) is untouched.

### New queues

`otaForwarderQueue` (master only):

```c
typedef enum {
    OTA_FWD_DEPLOY_BEGIN,  // from AstrOsSerialMsgHandler on FW_DEPLOY_BEGIN
    OTA_FWD_BEGIN_ACK,     // from AstrOsEspNow on OTA_BEGIN_ACK / NAK arrival
    OTA_FWD_DATA_ACK,      // from AstrOsEspNow on OTA_DATA_ACK / NAK arrival
    OTA_FWD_END_ACK,       // from AstrOsEspNow on OTA_END_ACK arrival
    OTA_FWD_TICK           // from 50 ms esp_timer for BulkSender::tick
} ota_forwarder_msg_kind_t;
```

`otaWriterQueue` (padawan only):

```c
typedef enum {
    OTA_WRT_BEGIN,         // from AstrOsEspNow on OTA_BEGIN arrival
    OTA_WRT_DATA,          // from AstrOsEspNow on OTA_DATA arrival
    OTA_WRT_END,           // from AstrOsEspNow on OTA_END arrival
    OTA_WRT_WATCHDOG       // from idle watchdog timer
} ota_writer_msg_kind_t;
```

Both queues carry POD messages with the standard producer-allocates /
consumer-frees convention. Producer (`AstrOsEspNow` callback) `malloc`s the
payload buffer, copies the frame bytes, calls `xQueueSend`. If send fails,
producer frees. Consumer task (`otaForwarderTask` / `otaWriterTask`) frees
after processing.

### Task placement

- `otaForwarderTask` pinned to **core 1**, master only (gated on
  `isMasterNode`). Stack: 4 KB to start, HWM warning at 500 bytes (existing
  pattern).
- `otaWriterTask` pinned to **core 1**, padawan only (gated on
  `!isMasterNode`). Stack: 4 KB to start; M4 bench validates against the
  on-stack `mbedtls_sha256_context` overhead.
- ESP-NOW RX dispatch (`astrosEspNowRxTask` on core 0) routes `OTA_*`
  arrivals into the role-appropriate queue. New role-gated branch in the
  existing dispatch switch.

### Wire-format binding (PR set 1)

The contract is frozen in
`AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md`
section B. PR set 1 binds these packet types:

| Type | Direction | Frame layout (binary, packed) |
|---|---|---|
| `OTA_BEGIN` | master → padawan | `u8 xferId · u32 totalSize · u16 chunkSize · u32 totalChunks · u8[32] sha256Expected · u8 flags` (~44 B) |
| `OTA_BEGIN_ACK` | padawan → master | `u8 xferId` (1 B) |
| `OTA_BEGIN_NAK` | padawan → master | `u8 xferId · u8 reason` (2 B); reason ∈ {BUSY, NO_PARTITION, BEGIN_FAILED} |
| `OTA_DATA` | master → padawan | `u8 xferId · u32 seq · u16 payloadLen · u16 crc16 · u8[chunkSize] payload` (~137 B with chunkSize=128) |
| `OTA_DATA_ACK` | padawan → master | `u8 xferId · u32 highestContiguousSeq · u32 nextExpectedSeq · u8 windowRemaining` (10 B) |
| `OTA_DATA_NAK` | padawan → master | `u8 xferId · u32 highestContiguousSeq · u32 nextExpectedSeq · u8 windowRemaining · u8 reason` (11 B); reason ∈ {CRC, SIZE, OUT_OF_ORDER, WRITE} |
| `OTA_END` | master → padawan | `u8 xferId · u32 totalChunksSent · u8[32] sha256Final` (37 B) |
| `OTA_END_ACK` | padawan → master | `u8 xferId · u8 status · u8[32] sha256Computed` (34 B); status ∈ {OK, HASH_MISMATCH, WRITE_ERROR} |

ESP-NOW frame budget is 250 B (20 B AstrOs header + 180 B max payload). Every
frame above fits in a single ESP-NOW transmission — no message-service
fragmentation needed. `static_assert` on each payload struct's `sizeof` is
M1's compile-time gate.

**`FW_PROGRESS`** (master → server, serial): already exists in the
`AstrOsSerialMessageType` enum at value 38 from Phase 1. PR set 1 (M5) adds
the `getFwProgress(...)` builder if not already present and wires the
emission path.

**Deferred to PR set 2**: `OTA_COMMIT` and `OTA_COMMIT_ACK` are NOT added to
the `AstrOsPacketType` enum in this PR set.

## State machines

### Master side — `BulkSender` (PURE) + `OtaForwarder` (MIXED)

**`BulkSender` state** (single transfer to single padawan):

- `transferId`, `totalChunks`, `chunkSize`, `windowSize`
- `highestConfirmedSeq` — advances on `OTA_DATA_ACK` cumulative
- `nextSeqToSend` — advances each TX
- In-flight set: `[highestConfirmedSeq+1 .. nextSeqToSend-1]`, capped at
  `windowSize` (= 8 per frozen contract)
- Per-seq: `sendTimestamp`, `retryCount`
- Status: `IDLE | AWAITING_BEGIN_ACK | STREAMING | AWAITING_END_ACK | DONE_OK | ABANDONED`

**PURE entry points exposed to the MIXED layer**:

- `begin(xferId, totalChunks, chunkSize, windowSize)` → `BeginSenderResult`
- `onBeginAck(xferId)` → `Decision::OK | WRONG_XFER_ID`
- `onDataAck(xferId, cumulativeSeq)` → list of freed seqs (caller refills
  window)
- `onDataNak(xferId, nextExpectedSeq, reason)` → seq to retransmit
- `tick(nowMs)` → `TickResult { retransmitSeqs[], abandon }`
- `nextChunkToSend()` → seq, or `WINDOW_FULL`
- `onEndAck(xferId, status)` → state advances to `DONE_OK` or `ABANDONED`

**`OtaForwarder` (MIXED) owns**:

- `FILE *firmwareFile_` from `/sdcard/firmware/<sha>.bin`
- `BulkSender bulk_`
- `mbedtls_sha256_context` (defensive self-validation on file read)
- `esp_timer_handle_t tickTimer_` firing every 50 ms to call
  `BulkSender::tick`
- Order-list iterator + per-padawan result records for `FW_DEPLOY_DONE`
- `std::atomic<bool> active_` + `isActive()` for polling-pause gate

### Padawan side — `BulkReceiver` (PURE, existing) + `OtaWriter` (MIXED)

**`OtaWriter` (MIXED) owns**:

- `esp_ota_handle_t otaHandle_`
- `const esp_partition_t *inactivePartition_` from
  `esp_ota_get_next_update_partition(NULL)`
- `mbedtls_sha256_context shaCtx_` — streaming, accumulated across
  `OTA_DATA` arrivals
- `BulkReceiver bulk_` (existing PURE lib)
- `std::atomic<bool> active_` + `isActive()` accessor
- Idle watchdog timer (10 s, mirrors `OtaReceiver` pattern)

**`handleBegin`**:

1. Busy check → `OTA_BEGIN_NAK BUSY` if `active_`
2. `inactivePartition_ = esp_ota_get_next_update_partition(NULL)` → `NAK
   NO_PARTITION` if null
3. Size check (`totalSize ≤ inactivePartition_->size`) → `NAK NO_PARTITION`
   if too big
4. `esp_ota_begin(inactivePartition_, totalSize, &otaHandle_)` → `NAK
   BEGIN_FAILED` on error
5. `mbedtls_sha256_init(&shaCtx_); mbedtls_sha256_starts(&shaCtx_, 0)`
6. `bulk_.begin(xferId, totalSize, totalChunks, chunkSize, windowSize)` —
   if `!BeginResult.valid`, abort cleanly (`esp_ota_abort` + free SHA) and
   `NAK BEGIN_FAILED`
7. `active_ = true`, reply `OTA_BEGIN_ACK`, start idle watchdog

**`handleData`**:

```cpp
auto cr = bulk_.onChunk(xferId, seq, payloadLen, crc16, payload);
if (cr.decision == Decision::NAK) {
    sendOtaDataNak(xferId, cr.highestContiguousSeq, cr.nextExpectedSeq,
                   cr.windowRemaining, cr.reason);
    return;
}
esp_err_t werr = esp_ota_write(otaHandle_, cr.payload, cr.payloadLen);
if (werr != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write failed: %s — aborting", esp_err_to_name(werr));
    sendOtaDataNak(xferId, cr.highestContiguousSeq, cr.nextExpectedSeq,
                   cr.windowRemaining, NakReason::WRITE);
    resetOtaHandleAndSha();
    bulk_.reset();
    active_ = false;
    watchdogStop();
    return;
}
mbedtls_sha256_update(&shaCtx_, cr.payload, cr.payloadLen);
sendOtaDataAck(xferId, cr.highestContiguousSeq, cr.nextExpectedSeq, cr.windowRemaining);
watchdogReset();
```

**`handleEnd`** (after `bulk_.onEnd` returns OK):

```cpp
uint8_t streamedDigest[32];
mbedtls_sha256_finish(&shaCtx_, streamedDigest);
mbedtls_sha256_free(&shaCtx_);

if (memcmp(streamedDigest, sha256Expected, 32) != 0) {
    sendOtaEndAck(xferId, OtaEndStatus::HASH_MISMATCH, streamedDigest);
    resetOtaHandleAndSha();  // calls esp_ota_abort on handle
    return;
}

esp_err_t eerr = esp_ota_end(otaHandle_);
if (eerr != ESP_OK) {
    sendOtaEndAck(xferId, OtaEndStatus::WRITE_ERROR, streamedDigest);
    otaHandle_ = 0;  // esp_ota_end already released
    active_ = false;
    return;
}
otaHandle_ = 0;

// Read-back-and-rehash
mbedtls_sha256_context rbCtx;
mbedtls_sha256_init(&rbCtx);
mbedtls_sha256_starts(&rbCtx, 0);
uint8_t buf[4096];
for (size_t off = 0; off < totalSize_; off += sizeof(buf)) {
    size_t chunk = std::min(sizeof(buf), totalSize_ - off);
    if (esp_partition_read(inactivePartition_, off, buf, chunk) != ESP_OK) {
        sendOtaEndAck(xferId, OtaEndStatus::WRITE_ERROR, streamedDigest);
        mbedtls_sha256_free(&rbCtx);
        active_ = false;
        return;
    }
    mbedtls_sha256_update(&rbCtx, buf, chunk);
}
uint8_t readbackDigest[32];
mbedtls_sha256_finish(&rbCtx, readbackDigest);
mbedtls_sha256_free(&rbCtx);

if (memcmp(readbackDigest, streamedDigest, 32) != 0) {
    ESP_LOGE(TAG, "Read-back hash mismatch — flash write corrupted bytes");
    sendOtaEndAck(xferId, OtaEndStatus::WRITE_ERROR, readbackDigest);
} else {
    sendOtaEndAck(xferId, OtaEndStatus::OK, streamedDigest);
}
active_ = false;
watchdogStop();
// NB: inactive partition holds verified bytes. Boot table unchanged.
// PR set 2 will add the `esp_ota_set_boot_partition` call here.
```

**`resetOtaHandleAndSha()`**: idempotent cleanup. Calls `esp_ota_abort` if
`otaHandle_ != 0`, frees SHA if initialized, sets `active_ = false`. Invoked
from data-write-fail abandon path, end-ack failure paths, watchdog fire, and
destructor.

## Data flow (happy path, single padawan)

```
Server                  Master (OtaForwarder)            Padawan (OtaWriter)
  FW_DEPLOY_BEGIN
   order=[pad1]
  ─────────────────►  walk order list → pad1
                      open /sdcard/firmware/<sha>.bin
                      build OTA_BEGIN frame (~44 B)
                      ───────────────────────────────► esp_ota_get_next_update_partition
                                                       esp_ota_begin
                                                       bulk_.begin
                                                       mbedtls_sha256_starts
                                                  ◄──── OTA_BEGIN_ACK
                      bulk_.begin
                      ── fill window: send seq 0..7 ──► esp_ota_write × 8
                                                       sha256_update × 8
                                                  ◄──── OTA_DATA_ACK cum=7
                      onDataAck → free 0..7
                      ── send seq 8..15 ──►            (loop continues, ~12 KB/s
                                                       effective on ESP-NOW for
                                                       128 B chunks)
                      ─ FW_PROGRESS pad1 SENDING 5% ──►
  ◄─────────────────  (every ~5% of bytes)
                      ...
                      after last cum ACK:
                      build OTA_END (sha=<expected>)
                      ───────────────────────────────► bulk_.onEnd
                                                       sha256_finish → streamedDigest
                                                       compare to expected → match
                                                       esp_ota_end (no boot change)
                                                       ── read-back-and-rehash ──
                                                       for 4 KB chunks in partition:
                                                         esp_partition_read
                                                         sha256_update
                                                       sha256_finish → readbackDigest
                                                       compare → match
                                                  ◄──── OTA_END_ACK OK sha=<streamed>
                      ─ FW_PROGRESS pad1 VERIFYING ──►
  ◄─────────────────  record pad1 = OK
                      close file, advance order list
                      (if more padawans, repeat from BEGIN)
                      after last padawan:
  ◄────  FW_DEPLOY_DONE  ── pad1=OK, ..., master=FAILED("not_implemented_in_pr_set_1")
```

`esp_ota_end()` finalizes the OTA write (with optional image-signature
checks if secure boot is enabled — it is not in this project) but does
**not** change the boot partition table. That's the firewall PR set 1
lives behind.

## Polling-pause symmetry

The existing master-side gate (`OtaReceiver::isActive()` blocking
`pollingTimerCallback`'s master branch in `src/main.cpp`) extends:

- **Master polling** gated on `OtaReceiver::isActive() ||
  OtaForwarder::isActive()`
- **Padawan polling** gated on `OtaWriter::isActive()` (new gate)

Both `active_` flags are `std::atomic<bool>` for cross-task safety
(`pollingTimer` fires on the `esp_timer` dispatch task; `active_` is
written from the OTA worker tasks). Both gates are coarse optimizations,
not correctness invariants.

## FW_PROGRESS emission cadence

Master emits `FW_PROGRESS` on these events, per padawan in the order list:

| Trigger | stage | bytesSent | detail |
|---|---|---|---|
| File opened, OTA_BEGIN sent | `QUEUED` | 0 | empty |
| OTA_BEGIN_ACK received | `SENDING` | 0 | empty |
| Every ~5% of totalBytes during streaming | `SENDING` | running count | empty |
| OTA_END sent (awaiting OTA_END_ACK) | `VERIFYING` | totalBytes | empty |
| OTA_END_ACK OK | (no FW_PROGRESS — captured in FW_DEPLOY_DONE) | — | — |
| Any failure | `FAILED` | last known bytesSent | reason string |

Skipped in PR set 1: `REBOOTING`, `VERSION_CONFIRMED` (PR set 2 stages).
For a 1.2 MB image: ~20 SENDING messages + 2 stage transitions = ~22
progress events per padawan, all on the (idle during mesh forward) serial
channel.

## Timeouts (from frozen contract)

| Timeout | Value | Action on fire |
|---|---|---|
| `OTA_BEGIN_ACK` (master waiting) | 2 s | abandon padawan, mark FAILED("begin_ack_timeout") |
| `OTA_DATA_ACK` per seq (master waiting) | 400 ms | retransmit seq, retry++ |
| Per-seq retry count exceeded | 3 retries | abandon padawan, mark FAILED("data_retry_exceeded") |
| `OTA_END_ACK` (master waiting) | 5 s | abandon padawan, mark FAILED("end_ack_timeout") |
| Padawan idle watchdog (no OTA_DATA arriving) | 10 s | cleanup, `active_ = false`, no reply |

`BulkSender::tick(nowMs)` is the PURE entry point for time-based
decisions. The MIXED layer's 50 ms `esp_timer` calls it; tick returns
retransmit-list + abandonment signal.

## Error handling matrix

| Trigger | Side | Wire response | State action |
|---|---|---|---|
| OTA_BEGIN while padawan busy | padawan | `OTA_BEGIN_NAK BUSY` | existing transfer continues |
| OTA_BEGIN, no inactive partition or too small | padawan | `OTA_BEGIN_NAK NO_PARTITION` | no state change |
| OTA_BEGIN, `esp_ota_begin` fails | padawan | `OTA_BEGIN_NAK BEGIN_FAILED` | no state change |
| OTA_BEGIN, BulkReceiver rejects | padawan | `OTA_BEGIN_NAK BEGIN_FAILED` | `esp_ota_abort`, no state change |
| OTA_BEGIN_ACK timeout (2 s) | master | — | abandon padawan, FAILED("begin_ack_timeout") |
| OTA_DATA bad CRC | padawan | `OTA_DATA_NAK CRC` | no state change |
| OTA_DATA out of order | padawan | `OTA_DATA_NAK OUT_OF_ORDER` | no state change |
| OTA_DATA payload-len mismatch | padawan | `OTA_DATA_NAK SIZE` | no state change |
| OTA_DATA `esp_ota_write` fails | padawan | `OTA_DATA_NAK WRITE` | full cleanup, `active_ = false` |
| OTA_DATA_ACK timeout per seq (400 ms) | master | — | retransmit seq, retry++ |
| Per-seq retry count > 3 | master | — | abandon padawan, FAILED("data_retry_exceeded") |
| OTA_END chunk count mismatch | padawan | `OTA_END_ACK WRITE_ERROR` | cleanup |
| OTA_END streamed SHA mismatch | padawan | `OTA_END_ACK HASH_MISMATCH sha=<streamed>` | cleanup |
| OTA_END read-back-rehash mismatch | padawan | `OTA_END_ACK WRITE_ERROR sha=<readback>` | cleanup (rare; SPI flash corruption) |
| OTA_END_ACK timeout (5 s) | master | — | abandon padawan, FAILED("end_ack_timeout") |
| Padawan idle watchdog (10 s) | padawan | — (silent) | cleanup, `active_ = false` |
| "master" entry in FW_DEPLOY_BEGIN order list | master | — | record FAILED("not_implemented_in_pr_set_1") |

All padawan-side failures cleanly release `esp_ota_handle_t` via
`resetOtaHandleAndSha()`. All master-side per-padawan failures advance to
the next entry in the order list — never abort the whole transfer.

## Per-milestone deliverables

Each milestone ships as one PR. Each leaves the codebase shippable. Each
has its own implementation plan written via `superpowers:writing-plans`
when work starts on it.

### M1 — ESP-NOW OTA wire format (PURE, native-testable)

Files (~5):

- `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.{hpp,cpp}` —
  extend `AstrOsPacketType` enum (OTA_BEGIN, OTA_BEGIN_ACK, OTA_BEGIN_NAK,
  OTA_DATA, OTA_DATA_ACK, OTA_DATA_NAK, OTA_END, OTA_END_ACK); extend
  `AstrOsENC` string table; add **binary** builders for each frame
  (`build(type, mac, const uint8_t *payload, size_t len)` overload).
- `lib_native/AstrOsEspNowProtocol/{include,src}` — POD record types
  (`OtaBeginRecord`, `OtaDataRecord`, `OtaEndRecord`, ACK/NAK records);
  decode/dispatch routing with role gating (master-only vs padawan-only
  packet types enforced at decode time).
- `test/test_native/astros_espnow_messages_tests.cpp` — round-trip per
  builder, reject paths per parser, `static_assert(sizeof(...) == N)` for
  each packed payload struct.

Approx 5–6 files, ~6 discrete tasks. No firmware behavior change.

### M2 — `BulkSender` PURE (state machine + native tests)

Files (~3):

- `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` — add
  `BulkSender` class; result types (`BeginSenderResult`, `AckResult`,
  `NakResult`, `TickResult`).
- `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp` —
  implementation.
- `test/test_native/bulk_transport_tests.cpp` (extend) — happy fill-window-drain,
  NAK-driven retransmit, timeout-driven retransmit, retry exhaustion →
  abandonment, cumulative ACK eviction, stale-ACK rejection. Small
  `FakePeer` helper for scripted responses.

Approx 3 files, ~5 discrete tasks. No firmware behavior change.

### M3 — Master `OtaForwarder` (MIXED) + ESP-NOW binary TX path

Files (~6):

- `lib/OtaForwarder/{include,src,README}` — new MIXED lib.
- `lib/AstrOsEspNow/src/AstrOsEspNowService.{hpp,cpp}` — binary-frame TX
  overload + dispatch new `OTA_*` arrival types into the role-appropriate
  queue (master receives ACKs/NAKs only; in M3, padawan-side queue routing
  is a no-op until M4 lands).
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` — replace
  Phase 3's "all-FAILED stub" for `FW_DEPLOY_BEGIN` with a route into
  `otaForwarderQueue`.
- `src/main.cpp` — create `otaForwarderQueue` (size 16, matching the
  existing `interfaceResponseQueue` convention); spawn `otaForwarderTask`
  pinned to core 1 (master only); extend polling-pause gate to
  `OtaReceiver::isActive() || OtaForwarder::isActive()`.

Approx 6 files, ~7 discrete tasks.

**Merge bar**: master correctly emits `OTA_BEGIN` and `OTA_DATA` frames
over the air when given `FW_DEPLOY_BEGIN` (verified via ESP-NOW sniffer or
a temporary padawan-side log on the `espnow_recv` callback). Since
padawan code doesn't exist yet, master times out after 2 s `OTA_BEGIN_ACK`
window and emits `FW_DEPLOY_DONE FAILED("begin_ack_timeout")`. **No
end-to-end round trip in M3** — that's M4's bar.

### M4 — Padawan `OtaWriter` (MIXED) + first end-to-end bench validation

Files (~6):

- `lib/OtaWriter/{include,src,README}` — new MIXED lib. Wraps existing
  `BulkReceiver`. Owns `esp_ota_handle_t`, `mbedtls_sha256_context`, idle
  watchdog. Read-back-and-rehash via `esp_partition_read`.
- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — dispatch `OTA_BEGIN` /
  `OTA_DATA` / `OTA_END` arrivals on padawans into `otaWriterQueue`.
- `src/main.cpp` — create `otaWriterQueue` (size 16); spawn
  `otaWriterTask` pinned to core 1 (padawan only); add
  `OtaWriter::isActive()` to padawan polling-pause gate.

Approx 6 files, ~7 discrete tasks.

**Merge bar (first end-to-end bench checkpoint)**: server uploads 1.2 MB
`.bin` via UI → master mesh-forwards to one padawan → padawan writes
inactive partition + passes read-back-rehash → emits `OTA_END_ACK OK` →
master emits `FW_DEPLOY_DONE pad1=OK`. USB-attach padawan, dump inactive
partition via `esptool.py read_flash`, `sha256sum` matches source.

### M5 — `FW_DEPLOY_BEGIN` orchestration + `FW_PROGRESS` emission

Files (~5):

- `lib/OtaForwarder/src/OtaForwarder.cpp` — order-list iteration
  (multi-padawan sequential loop), per-padawan result tracking,
  `FW_PROGRESS` emission at stage transitions + 5% byte intervals,
  `FW_DEPLOY_DONE` assembly with all per-padawan results.
- `lib_native/AstrOsMessaging/...` — confirm/add `getFwProgress(...)`
  builder.
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` —
  `sendFwProgress(...)` thin wrapper.
- `test/test_native/astros_serial_messages_tests.cpp` — round-trip
  `FW_PROGRESS`.
- `.docs/qa/ota-mesh-forward.md` — new QA plan covering M3/M4/M5 bench
  cases (single-padawan, 2-padawan, abandonment scenarios).

Approx 5 files, ~6 discrete tasks.

**Merge bar (final bench checkpoint)**: 1 master + 2 padawans on bench,
full deploy driven from `AstrOs.Server` Firmware view. Server UI shows
`QUEUED → SENDING (with byte counter animating) → VERIFYING → ...` per
padawan in sequence. Both padawans `= OK` in `FW_DEPLOY_DONE`. Master
shows `FAILED("not_implemented_in_pr_set_1")` per the agreed
honest-signal behavior.

## Testing

### Native tests (`pio test -e test`)

| Milestone | Coverage |
|---|---|
| **M1** | Round-trip per builder, reject paths per parser, byte-offset `static_assert` on packed structs |
| **M2** | `BulkSender` state machine: happy fill-window-drain, NAK retransmit, timeout retransmit, retry exhaustion → abandonment, cumulative ACK eviction, stale ACK rejection. Includes a `FakePeer` test harness for scripted ACK/NAK responses |
| **M5** | Round-trip `getFwProgress` (extends existing `astros_serial_messages_tests.cpp`) |

Existing 309+ native tests must continue to pass. M2 adds ~6–8 new
`BulkSender` tests.

### Bench tests

| Milestone | Bench checkpoint |
|---|---|
| **M3** | ESP-NOW sniffer (or temporary padawan-side log on `espnow_recv`) confirms master emits `OTA_BEGIN` + `OTA_DATA` frames. Master `FW_DEPLOY_DONE` after 2 s with `FAILED("begin_ack_timeout")` since no padawan replies. |
| **M4** | First end-to-end: server UI uploads → master mesh-forwards → 1 padawan writes inactive partition + read-back-rehash → `OTA_END_ACK OK`. USB-attach padawan, `esptool.py read_flash` of inactive partition, `sha256sum` matches source. |
| **M5** | 2-padawan bench, full deploy from server UI. Both `= OK`. FW_PROGRESS UI animates per padawan. Negative-path bench: one padawan offline (master times out gracefully, marks FAILED, moves to next); one padawan power-cycled mid-transfer (idle watchdog fires, padawan recovers cleanly on next BEGIN). |

### Per-milestone verification-before-completion gates

1. `pio test -e test` 100% pass.
2. `pio run -e metro_s3` and `pio run -e lolin_d32_pro` build clean.
3. `clang-format` clean on changed files.
4. Native-purity CI guard passes (M1, M2).
5. M3–M5: the milestone-appropriate bench case has been exercised against
   the live `AstrOs.Server` `develop` branch.

## Cross-repo coordination

- **Wire-format contract is frozen** in
  `AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md`
  section B. PR set 1 binds the contract; does not amend it.
- **No Server-side changes required.** Sub-project (c) already produces
  `FW_DEPLOY_BEGIN` and consumes `FW_PROGRESS` / `FW_DEPLOY_DONE`.
- **No Vue changes required.** The Firmware view from sub-project (d)
  already renders per-controller progress stages.
- The serial-receive milestone's `/sdcard/firmware/<sha>.bin` staging is
  PR set 1's input. No new dependency from server.
- `.docs/protocol.md` is shared with `AstrOs.Server`. M1's plan confirms
  current state and amends only if a real gap is found (decomposition
  section B is canonical; protocol.md may need a copy of the binary-frame
  byte offsets).

## Out of scope (deferred to PR set 2 or beyond)

- `esp_ota_set_boot_partition` on either master or padawan
- Reboot orchestration + `OTA_COMMIT` / `OTA_COMMIT_ACK` wire types
- `esp_ota_mark_app_valid_cancel_rollback` discipline (rollback heuristic
  per decomposition open item #2)
- `VERSION_CONFIRMED` + `REBOOTING` `FW_PROGRESS` stage emission
- Master self-flash from `/sdcard/firmware/<sha>.bin`
- Resume-after-error for a single padawan (v1 = abort-and-skip per frozen
  contract)
- Cleanup/eviction of staged `<sha>.bin` files on master SD (owned by PR
  set 2 when bytes are finally consumed)
- `FW_BACKPRESSURE` master→server if mesh write is slower than serial
  upload (likely not needed; sequential per-padawan throttles naturally)
- Parallel / broadcast delivery to multiple padawans (v1 = sequential)

## Open questions (deferred to per-milestone plans)

1. **`OTA_DATA` exact byte alignment** — frozen contract specifies fields
   but not alignment. M1's plan locks layout with `static_assert`s on each
   payload struct's `sizeof`.
2. **`BulkSender::tick` cadence** — design proposes 50 ms. M3's plan
   validates 50 ms vs 100 ms against the 400 ms ACK timeout granularity.
3. **`OtaForwarder` task stack size** — start at 4 KB matching
   `OtaReceiver`. M3 bench reveals if `BulkSender` state needs more
   (standard 500-byte HWM warning).
4. **Padawan idle watchdog timeout** — 10 s mirrors serial-receive. M4's
   plan confirms appropriateness for mesh chunk-arrival burstiness.
5. **`OTA_BEGIN_ACK` 2 s timeout sufficiency** — assumes padawan partition
   erase fits the budget. M4 bench measures actual erase time on both
   boards (lolin_d32_pro 2 MB partition vs metro_s3 6.25 MB partition may
   differ meaningfully).
6. **Read-back chunk size for `esp_partition_read`** — proposed 4 KB. M4
   measures whether smaller chunks improve variance vs SHA update
   overhead.
7. **MIXED dispatcher residual switch — silent-drop OTA cases** (raised by
   M1 PR-toolkit silent-failure review, 2026-05-23). `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`'s
   `handleMessage` residual switch currently hits the `default: ESP_LOGE`
   arm for any OTA packet that returns `UnsupportedType` from the PURE
   dispatcher. Benign on M1-only deployments (no OTA traffic on the wire
   yet), but during the M3→M4 window the master will start emitting
   `OTA_BEGIN` to padawans that may still be on M1+M2 firmware, producing
   `ESP_LOGE("Dispatcher returned UnsupportedType...")` noise on the
   padawan side. M3's plan should add explicit OTA cases to the padawan's
   residual switch (silent-drop with one-time `ESP_LOGW`) and route to
   the new `otaWriterQueue` in M4. The M3 author should confirm this is
   in their scope before writing code; otherwise a one-line patch can ship
   between M2 and M3 as scaffolding.

8. **M3 must emit `tick()` retransmits before pulling new chunks** (raised by M2
   PR-toolkit third-pass silent-failure review, 2026-05-23). `BulkSender::tick`
   appends timed-out seqs to `TickResult::retransmitSeqs[]` but does NOT update
   `nextSeqToSend_`. If M3's OtaForwarder loop calls `nextChunkToSend()` without
   first emitting bytes for each entry in `retransmitSeqs[]`, those retransmits
   silently never go on the wire. M3 must structure the loop as: tick → emit
   retransmits → drain via nextChunkToSend.

9. **M3 must consult `status()` to disambiguate `tick(count=0, abandon=false)`**
   (raised by M2 PR-toolkit third-pass silent-failure review). `tick()` returns
   the same shape for "STREAMING, no timeouts fired" and "non-STREAMING (DONE_OK
   / ABANDONED / IDLE)". M3 must read `status()` separately to detect terminal
   states; treating tick alone as the state oracle would leave a buggy forwarder
   ticking forever on a finished or abandoned transfer.

10. **NAK below the previously-ACKed watermark — protocol contract decision**
    (raised by M2 PR-toolkit third-pass silent-failure review). A NAK with
    `nextExpectedSeq=0` after `cumulativeSeq=0` was ACKed today rewinds and
    re-emits seq 0, which the receiver will then NAK as STALE — burning wire
    traffic until the timeout-driven abandonment kicks in. M3 should decide:
    (a) reject such NAKs at the wire layer as protocol violations, OR (b) add
    a `NakResult::Decision::REWIND_BELOW_WATERMARK` enumerator and force M3 to
    abort the transfer explicitly.

11. **`newlyConfirmedCount` semantics blend explicit + implicit confirmations**
    (raised by M2 PR-toolkit third-pass silent-failure review). After a NAK
    advances `highestConfirmedSeq_` via `impliedCumulative`, a later ACK's
    `newlyConfirmedCount` includes the implicitly-confirmed seqs in `prev`. M3
    should treat this field as "slots freed in the sender's in-flight table",
    not as "wire-level OTA_DATA_ACK frames received from the padawan."

12. **M3 abandonment path must check `TickResult::abandon` independent of
    `count`** (raised by M2 PR-toolkit third-pass silent-failure review). The
    abandon path sets `count=0` and exits before processing remaining in-flight
    slots — any seqs already pushed to `retransmitSeqs[]` in earlier loop
    iterations are intentionally discarded. M3's tick consumer must check
    `abandon` first, then iterate retransmitSeqs only if `!abandon`. Treating
    `count > 0 || abandon` as the "something happened" signal would correctly
    fire on both paths, but treating `abandon` as orthogonal to count would
    miss the M3 contract.

## Related documents

- `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md`
  — preceding milestone (serial receive). PR set 1 reads its output file.
- `AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md`
  — cross-repo decomposition; defines sub-projects (a) and (b).
- `.docs/qa/ota-master-serial-receive.md` — preceding milestone's QA plan.
  M5 adds the parallel `.docs/qa/ota-mesh-forward.md`.
- `.docs/protocol.md` — shared wire-format contract.
