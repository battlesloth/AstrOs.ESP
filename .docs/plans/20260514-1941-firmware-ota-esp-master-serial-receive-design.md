# Firmware OTA — ESP master serial-receive milestone (design)

## Context

The cross-repo Firmware OTA feature is decomposed in
`AstrOs.Server/.docs/plans/20260427-2202-firmware-ota-decomposition.md` into five
sub-projects. As of 2026-05-14 the Server-side sub-projects (**c0** JobLock,
**c** orchestrator, **d** Vue Firmware view) have shipped through `d-7-followups`.
The two ESP-side sub-projects — **(a)** bulk-transport hardening and **(b)** OTA
receiver — have not started.

This design covers a deliberately narrow first milestone on the ESP side:

> Server → master over serial only. Land the `.bin` on the master's SD card.
> Verify SHA-256. **No mesh, no `esp_ota_*` calls, no padawan side.**

It maps to all of sub-project **(a)** plus the master-receive half of sub-project
**(b)**. The remainder of **(b)** (master SD → ESP-NOW forwarder, padawan OTA
writer, commit/reboot) is explicitly out of scope and is the next plan.

The wire-format contract is already frozen in `.docs/protocol.md` (mirrored in
both repos). This design does not change protocol; it implements the master-side
half of it.

### What's already shipped that this builds on

- PR #27 (commit `1aefed3`): `POLL_ACK` carries the controller's firmware
  version end-to-end. The server already uses this to record starting versions
  before a transfer and to recognize `VERSION_CONFIRMED` after one — this
  milestone doesn't need to touch the heartbeat path.
- Commit `f4d84d8` + `165ce6d`: `.docs/protocol.md` froze the cross-repo wire
  format including `FW_TRANSFER_BEGIN_ACK` payload shape.

## Decisions (confirmed with user)

- **Scope**: serial-only, server→master, land on SD, verify SHA-256. No mesh, no
  flash.
- **Transport**: build a new `lib_native/AstrOsBulkTransport` PURE lib for the
  chunk/CRC/ACK state machine. Per the cross-repo decomposition plan, the same
  lib will be plugged into ESP-NOW in a future milestone; this milestone only
  wires it into the serial path.
- **Receiver placement**: new `lib/OtaReceiver` MIXED lib owning the state
  machine, the staging file handle, and the streaming SHA context. New
  `otaQueue` carries decoded chunk bytes from `AstrOsSerialMsgHandler`. New
  FreeRTOS task drains the queue so SD writes never block the serial RX task.
- **SD layout**: `/sdcard/firmware/staging.bin` (overwritten per transfer) →
  atomic rename to `/sdcard/firmware/<sha256-hex[0..16)>.bin` on successful hash
  verify. Content-addressed final filename; no transfer-id leakage on disk.
- **`FW_DEPLOY_BEGIN` handling**: master replies with `FW_DEPLOY_DONE` listing
  every target as FAILED, `errorOrEmpty="not_implemented"`. Server marks the
  job failed, the JobLock releases cleanly, the UI surfaces a recognizable
  error state. ~10 lines for the stub.
- **State-machine variant**: phased complexity. Phase 2 implements sliding
  window + CRC + ACK/NAK. `FW_BACKPRESSURE`, the 5-min whole-transfer
  watchdog, and resume-after-retry are deferred to Phase 5 and only built if
  Phase 3/4 testing shows they're needed. Server-side already retries on
  `FW_CHUNK_NAK`, so the fast path is identical either way.
- **Base64 decode location**: in `AstrOsSerialMsgHandler` (handler task), before
  the queue handoff. The `otaQueue` carries decoded bytes, not encoded strings.
  A malformed base64 payload becomes an immediate `FW_CHUNK_NAK reason=SIZE`
  from the handler task.
- **Sliding window is sender-only**. The master commits chunks strictly in seq
  order. `FW_CHUNK_ACK` carries `highest-contiguous-seq` and `next-expected-seq`;
  the window is a server-side optimization for round-trip amortization, not a
  receiver-side reorder buffer.
- **Keep `staging.bin` on HASH_MISMATCH**: leave the staged bytes on disk after
  a hash-mismatch failure for forensic inspection. The next successful BEGIN
  opens with `"wb"` and truncates.
- **`otaQueue` full → fake CRC-NAK**: if `xQueueSend(otaQueue, …)` returns
  `errQUEUE_FULL`, the handler frees its decoded-chunk buffer and emits
  `FW_CHUNK_NAK reason=CRC` forcing the server to retransmit that seq. Small
  abuse of the reason code but stays inside the contract; cleaner backpressure
  via `FW_BACKPRESSURE` is a Phase 5 add.

## Architecture

### Lib classification

| New lib | Class | Summary |
|---|---|---|
| `lib_native/AstrOsBulkTransport` | PURE | Algorithmic state machine. Takes parsed `FwChunkRecord` in, emits ACK/NAK decisions and validated payload buffers out. No I/O, no FreeRTOS, no ESP-IDF. |
| `lib/OtaReceiver` | MIXED | Owns one `BulkReceiver` instance, one staging `FILE*`, one `mbedtls_sha256_context`, one FreeRTOS task draining `otaQueue`. Calls into the existing `interfaceResponseQueue` for FW_*_ACK replies. |

Wire-format builders and parsers live in `lib_native/AstrOsMessaging` and
`lib_native/AstrOsSerialProtocol` alongside the existing
`getPollAck` / `getBasicAckNak` / `validateSerialMsg` family.

### New queue

`otaQueue` carries `queue_ota_msg_t` records:

```c
typedef enum { OTA_BEGIN, OTA_CHUNK, OTA_END, OTA_DEPLOY_BEGIN } ota_msg_kind_t;

typedef struct {
    ota_msg_kind_t kind;
    uint8_t transferId;
    union {
        struct { uint32_t totalSize; uint32_t totalChunks; uint16_t chunkSize;
                 char sha256Hex[65]; /* null-terminated */
                 char *targetList; /* malloc'd, RS-separated; consumer frees */ } begin;
        struct { uint32_t seq; uint16_t payloadLen; uint16_t crc16;
                 uint8_t *payload; /* malloc'd, consumer frees */ } chunk;
        struct { uint32_t totalChunks; char finalSha256Hex[65]; } end;
        struct { char *orderList; /* malloc'd, RS-separated; consumer frees */ } deploy;
    };
} queue_ota_msg_t;
```

`payload`, `targetList`, and `orderList` follow the project's standard
producer-allocates / consumer-frees convention. Producer (handler) frees on
`xQueueSend` failure. Consumer (OtaReceiver task) frees after processing.

### Task placement

New FreeRTOS task `otaReceiverTask` pinned to core 1 alongside the other
queue-drainer tasks. Stack size starts at 4096 bytes with the standard
high-water-mark check that warns at 500 bytes remaining (existing pattern).

## Per-phase deliverables

Each phase ships independently. Each PR is sized for tight review. Each
phase has its own implementation plan written via `superpowers:writing-plans`
when work starts on it.

### Phase 1 — Wire format (PURE, native-testable)

Files:
- `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.{hpp,cpp}` —
  extend `AstrOsSerialMessageType` enum (values 30-40 per `.docs/protocol.md`),
  extend `AstrOsSC::` string table, add builders:
  - `getFwTransferBeginAck(msgId, transferId, status)` — payload `transfer-id<US>status` per `.docs/protocol.md` (no hash field on BEGIN_ACK).
  - `getFwChunkAck(transferId, highestContiguousSeq, nextExpectedSeq, windowRemaining)`
  - `getFwChunkNak(transferId, lastGoodSeq, nextExpectedSeq, reasonCode)` — payload `transfer-id<US>last-good-seq<US>next-expected-seq<US>reason-code`. `nextExpectedSeq` disambiguates the first-chunk NAK case where `lastGoodSeq=0` alone cannot distinguish "seq 0 committed, want seq 1" from "nothing committed, want seq 0". Phase 3 callers should pass `bulkResult.highestContiguousSeq` as `lastGoodSeq` and `bulkResult.nextExpectedSeq` as `nextExpectedSeq` (`BulkReceiver` already produces both with the correct first-chunk-NAK semantics: both are 0 when nothing committed yet).
  - `getFwTransferEndAck(msgId, transferId, status, computedSha256Hex)` — payload `transfer-id<US>OK|HASH_MISMATCH|IO_ERROR<US>computed-sha256-hex`. `msgId` echoes the originating END's id.
  - `getFwDeployDone(msgId, transferId, perControllerResults)` — each result `controllerId<US>OK|FAILED<US>finalVersion<US>errorOrEmpty`, RS-separated between results.
- Parsers returning POD result structs:
  - `parseFwTransferBegin(payload) -> FwTransferBeginRecord { transferId, totalSize, sha256Hex, chunkSize, targetIds[] }`
  - `parseFwChunk(payload) -> FwChunkRecord { transferId, seq, payloadLen, base64Payload, uint16_t crc16 }` — CRC field is parsed from its 4-char hex form into a `uint16_t`; the MIXED handler doesn't re-parse it.
  - `parseFwTransferEnd(payload) -> FwTransferEndRecord { transferId, totalChunks, finalSha256Hex }`
  - `parseFwDeployBegin(payload) -> FwDeployBeginRecord { transferId, orderIds[] }`
- `lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp` — route the new
  types into `AstrOsInterfaceResponseType`. New response-type values + the
  type-to-response map.
- `test/test_native/astros_serial_messages_tests.cpp` — round-trip tests for
  every builder; reject paths for every parser (truncated payload, wrong field
  count, bad hex, oversized payload-len, mismatched US/RS counts). The
  `base64Payload` field on FwChunkRecord is **not** validated at this layer —
  base64 decoding and length-vs-content cross-checking is deferred to the
  Phase 3 MIXED handler.

Approx 5-7 files. No behavior change in firmware.

### Phase 2 — Bulk-transport state machine (PURE)

Files:
- `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` —
  ```cpp
  enum class Decision { ACK, NAK };
  enum class NakReason : uint8_t { NONE=0, CRC=1, SIZE=2, OUT_OF_ORDER=3, FLASH_FULL=4 };

  struct ChunkResult {
      Decision decision;
      uint32_t highestContiguousSeq;
      uint32_t nextExpectedSeq;
      uint8_t  windowRemaining;
      NakReason reason;
      // On ACK only: payload pointer + len pass through unchanged from input.
      const uint8_t *payload;
      uint16_t payloadLen;
  };

  struct EndResult {
      enum class Status { OK, HASH_MISMATCH, IO_ERROR };
      Status status;
      // EndResult does not carry the computed hash — that lives in OtaReceiver,
      // which holds the streaming SHA context. BulkReceiver just validates
      // total-chunk count.
  };

  class BulkReceiver {
   public:
      void begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks,
                 uint16_t chunkSize, uint8_t windowSize);
      ChunkResult onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen,
                          uint16_t crc16, const uint8_t *payload);
      EndResult   onEnd  (uint8_t xferId, uint32_t totalChunksSent);
      void reset();
   private:
      uint8_t  xferId_;
      uint32_t nextSeq_;
      uint32_t totalChunks_;
      uint16_t chunkSize_;
      uint8_t  windowSize_;
      bool     active_;
  };

  // Standalone CRC for testability; the firmware path can either call this
  // directly or call esp_crc16_le with the matching parameters and one bench
  // test that confirms they agree byte-for-byte.
  uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);
  ```
- `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp` — implementation.
- `lib_native/AstrOsBulkTransport/README` — purity rule + forbidden include
  prefixes (matches the existing PURE-lib pattern).
- `test/test_native/bulk_transport_tests.cpp` — in-order happy path, out-of-order,
  duplicate (treated as OUT_OF_ORDER for simplicity), CRC fail, payload-len
  mismatch (SIZE), END with mismatched total → IO_ERROR.
- `.github/workflows/pr-validation.yml` — append `lib_native/AstrOsBulkTransport`
  to the `PURE_LIBS` array in the `native-purity` job.

Approx 6-8 files. Still no firmware behavior change.

### Phase 3 — Wire it up, sink to `/dev/null`

Files:
- `lib/OtaReceiver/{include,src}` — new MIXED lib. Phase 3 version holds the
  `BulkReceiver`, no SD, no SHA. On `OTA_CHUNK` queue messages, calls
  `BulkReceiver.onChunk`, builds the corresponding `FW_CHUNK_ACK` or
  `FW_CHUNK_NAK`, frees the payload buffer, posts the reply to
  `interfaceResponseQueue`. `OTA_DEPLOY_BEGIN` immediately produces
  `FW_DEPLOY_DONE` with all targets FAILED `not_implemented`.
- `lib/AstrOsSerialMsgHandler/src/...` — dispatch the new `FW_*` message types.
  For `FW_CHUNK`: base64-decode the payload into a `malloc`'d buffer, package
  into `queue_ota_msg_t`, `xQueueSend(otaQueue, …)`. On send failure: free, emit
  `FW_CHUNK_NAK reason=CRC`. On base64 decode failure: emit
  `FW_CHUNK_NAK reason=SIZE`.
- `src/main.cpp` — create `otaQueue` (size 16, item-size sizeof(queue_ota_msg_t)),
  spawn `otaReceiverTask` pinned to core 1.

Approx 4-6 files.

**Phase 3 review checkpoint**: at the end of this PR, you can drive a full
1.2 MB transfer through the AstrOs.Server UI and watch BEGIN_ACK / CHUNK_ACK /
END_ACK flow end-to-end. Chunks are discarded but every byte of the wire path,
queue plumbing, response routing, and DEPLOY_BEGIN stub is exercised. SD and
crypto failure modes are excluded.

### Phase 4 — SD writer + streaming SHA-256

Files:
- `lib/OtaReceiver/...` — extend:
  - On `OTA_BEGIN`: free-space pre-check; if `freeBytes < totalSize` reply
    `FW_TRANSFER_BEGIN_ACK status=sd_full`. Otherwise open
    `/sdcard/firmware/staging.bin` with `"wb"`, init `mbedtls_sha256_context`.
    Reply `status=OK` (or `status=io_error` on file-open failure).
    Also reject with `status=busy` if a transfer is already active.
  - On `OTA_CHUNK` (after BulkReceiver returns ACK): `fwrite(payload, len, 1, fp)`;
    `mbedtls_sha256_update(ctx, payload, len)`. On `fwrite` failure: reply
    `FW_CHUNK_NAK reason=FLASH_FULL`; transfer effectively dead, next NAK or
    server timeout aborts it.
  - On `OTA_END`: close staging.bin; `mbedtls_sha256_finish` → computed hash;
    compare to `finalSha256Hex` from END (and assert against BEGIN's `sha256Hex`
    — they should match). On match: rename `staging.bin` →
    `<sha256Hex[0..16)>.bin`; reply `FW_TRANSFER_END_ACK status=OK computedHex=…`.
    On mismatch: leave staging.bin in place; reply
    `FW_TRANSFER_END_ACK status=HASH_MISMATCH computedHex=…`. On total-chunks
    mismatch: reply `status=IO_ERROR computedHex=…`.

Approx 3-5 files (single lib touched).

**Phase 4 review checkpoint**: same QA flow as Phase 3 plus a hash-verify
assertion. After the UI reports success, pop the SD card, verify
`<sha-prefix>.bin` exists and its disk SHA-256 matches what the server uploaded.

### Phase 5 — Hardening + QA plan

Files:
- `lib/OtaReceiver/...` — failure paths confirmed against the protocol contract:
  hash-mismatch keeps staging.bin, SD-full and busy paths return distinct
  status codes, etc. (Most of this is already in place from Phase 4; Phase 5
  adds the negative-path testing that proves it.)
- Optional `FW_BACKPRESSURE PAUSE/RESUME` from master to server, if Phase 3/4
  measurements show the master falling behind. With SD writes on a dedicated
  task and 5.5 KB/frame at 115200 baud (≈11.5 KB/s sustained), we expect not
  to need this.
- Optional 5-min whole-transfer watchdog (an `esp_timer` that aborts and frees
  resources if no chunk arrives within 5 min). A reboot during transfer
  recovers naturally because `OTA_BEGIN` opens staging.bin with `"wb"` and
  truncates.
- New `.docs/qa/ota-master-serial-receive.md` — full QA plan covering all five
  phases with explicit preconditions, steps, and expected results.

Approx 4-6 files.

## Data flow (happy path, one transfer)

```
Server                                                    Master
  |---FW_TRANSFER_BEGIN--------------------------------->| AstrOsSerialMsgHandler
  |   xfer-id, total-size, sha256, chunk-size, target   |   validateSerialMsg → ok
  |                                                      |   parseFwTransferBegin → record
  |                                                      |   xQueueSend(otaQueue, OTA_BEGIN, record)
  |                                                      |
  |                                                      | OtaReceiver task
  |                                                      |   busy/sd-full/io-error checks
  |                                                      |   open staging.bin; sha256_starts
  |                                                      |   BulkReceiver.begin(...)
  |                                                      |   interfaceResponseQueue ← BEGIN_ACK
  |<--FW_TRANSFER_BEGIN_ACK OK---------------------------|
  |
  |---FW_CHUNK seq=0..N (sliding window 16 from sender)->| AstrOsSerialMsgHandler
  |                                                      |   base64-decode → malloc 4 KB buf
  |                                                      |   xQueueSend(otaQueue, OTA_CHUNK, …)
  |                                                      | OtaReceiver task
  |                                                      |   BulkReceiver.onChunk → ACK/NAK decision
  |                                                      |   on ACK: fwrite + sha256_update
  |                                                      |   free(buf)
  |                                                      |   interfaceResponseQueue ← CHUNK_ACK/NAK
  |<--FW_CHUNK_ACK next-seq, window-rem------------------|
  |   (or FW_CHUNK_NAK on CRC/SIZE/OUT_OF_ORDER)         |
  |
  |---FW_TRANSFER_END total-chunks, final-sha---------->| OtaReceiver task
  |                                                      |   close staging.bin
  |                                                      |   sha256_finish → computed
  |                                                      |   compare to finalSha
  |                                                      |   on match: rename → <sha-prefix>.bin
  |                                                      |   interfaceResponseQueue ← END_ACK
  |<--FW_TRANSFER_END_ACK OK | HASH_MISMATCH | IO_ERROR-|
  |
  |---FW_DEPLOY_BEGIN order-list------------------------>| OtaReceiver task (Phase 3+)
  |                                                      |   parse order-list
  |                                                      |   build per-target FAILED records
  |                                                      |   interfaceResponseQueue ← DEPLOY_DONE
  |<--FW_DEPLOY_DONE all FAILED err=not_implemented------| Server marks job failed
                                                          Server JobLock releases
```

### Buffer ownership

| Step | Allocates | Frees |
|---|---|---|
| Handler base64-decodes 4 KB into malloc'd buf | producer (handler) | — |
| `xQueueSend(OTA_CHUNK, …)` success | — | OtaReceiver task |
| `xQueueSend` failure | — | producer (handler), then `FW_CHUNK_NAK reason=CRC` |
| OtaReceiver processes chunk (ACK or NAK) | — | OtaReceiver task (free *after* CRC + write) |
| BEGIN's `targetList` / DEPLOY's `orderList` | producer (handler) | OtaReceiver task |

## Error handling matrix

| Trigger | Wire response | State action |
|---|---|---|
| `validateSerialMsg` malformed | (silent — existing behavior) | no change |
| BEGIN, transfer already in flight | `FW_TRANSFER_BEGIN_ACK status=busy` | existing transfer continues |
| BEGIN, SD unavailable / open fail | `FW_TRANSFER_BEGIN_ACK status=io_error` | no state change |
| BEGIN, free space < total-size | `FW_TRANSFER_BEGIN_ACK status=sd_full` | no state change |
| BEGIN OK | `FW_TRANSFER_BEGIN_ACK status=OK` | staging.bin opened, sha256 started |
| CHUNK bad CRC-16 | `FW_CHUNK_NAK last-good-seq next-expected-seq reason=CRC` | nothing committed |
| CHUNK out of order | `FW_CHUNK_NAK last-good-seq next-expected-seq reason=OUT_OF_ORDER` | nothing committed |
| CHUNK payload-len mismatch | `FW_CHUNK_NAK last-good-seq next-expected-seq reason=SIZE` | nothing committed |
| CHUNK base64 decode fail (handler) | `FW_CHUNK_NAK last-good-seq next-expected-seq reason=SIZE` | handler frees own buf |
| CHUNK fwrite fails (Phase 4+) | `FW_CHUNK_NAK last-good-seq next-expected-seq reason=FLASH_FULL` | transfer effectively dead |
| END hash match | `FW_TRANSFER_END_ACK OK <hex>` | rename staging → `<sha-prefix>.bin`, reset state |
| END hash mismatch | `FW_TRANSFER_END_ACK HASH_MISMATCH <hex>` | **keep staging.bin** for forensics, reset state |
| END total-chunks mismatch | `FW_TRANSFER_END_ACK IO_ERROR <hex>` | keep staging.bin, reset state |
| DEPLOY_BEGIN (Phase 3+) | `FW_DEPLOY_DONE` all-FAILED `not_implemented` | no state change |
| `xQueueSend(otaQueue, …)` full | handler frees, emits `FW_CHUNK_NAK last-good-seq next-expected-seq reason=CRC` | no state change |

## Testing

### Native tests (`pio test -e test`)

Phase 1 — `test/test_native/astros_serial_messages_tests.cpp`:
- Round-trip per builder: build → `validateSerialMsg` → parse → assert fields.
- Reject paths per parser: truncated, wrong field count, bad hex (sha256 +
  crc16), oversized payload-len, mismatched separators. **Base64 payload
  content is not validated here** — `parseFwChunk` stores the encoded string
  as-is; the Phase 3 MIXED handler is responsible for decoding and cross-
  checking length against `payloadLen`.
- Existing `getPollAck` 4-field test continues to pass unchanged.

Phase 2 — `test/test_native/bulk_transport_tests.cpp`:
- Happy: BEGIN → 10 in-order chunks → END → expected ACK sequence.
- Out-of-order: chunks {0, 2} → NAK on 2, reason=OUT_OF_ORDER, last-good=0.
- CRC fail: corrupt one payload byte, crc16 still says original → NAK reason=CRC.
- Duplicate: chunks {0, 0} → second NAK'd as OUT_OF_ORDER (documented).
- SIZE: declared payload-len ≠ provided buffer → NAK reason=SIZE.
- END before all chunks: END after seq=3 of 10 → IO_ERROR.
- END after all chunks with mismatched final hash → (returned to caller; the
  hash check itself lives in OtaReceiver, but BulkReceiver returns OK once
  total-chunks count is right).

### Manual QA (`.docs/qa/ota-master-serial-receive.md`)

Phase 3 milestone:
- **Pre**: master flashed, server pointed at master serial. SD present, OtaReceiver
  sinking to /dev/null.
- **Steps**: in AstrOs.Server Firmware view, upload 1.2 MB `.bin`, click
  Send-to-master. Watch logs.
- **Expected**: UI reports transfer complete; END_ACK OK in logs; immediately
  followed by FW_DEPLOY_DONE all-FAILED `not_implemented`; JobLock releases
  (next write action is not blocked).

Phase 4 milestone:
- **Pre**: as Phase 3, plus a known-good `.bin` with recorded SHA-256.
- **Steps**: repeat the upload. After END_ACK OK, pop SD card.
- **Expected**: `/sdcard/firmware/<sha256-hex[0..16)>.bin` exists; its disk
  SHA-256 matches recorded value exactly. `staging.bin` is absent.

Phase 5 milestone — negative paths:
- Server-side debug toggle to send wrong sha256 in END → expect HASH_MISMATCH;
  staging.bin remains; JobLock releases.
- Pre-fill SD so free-space < 1.2 MB → expect `BEGIN_ACK status=sd_full`;
  fast-fail; JobLock releases.
- Mid-transfer CRC injection if feasible → expect NAK reason=CRC; server
  retransmits; transfer continues; END_ACK OK.

### Verification-before-completion (per `superpowers:verification-before-completion`)

Each PR's "done" definition:
1. `pio test -e test` 100% pass.
2. `pio run -e metro_s3` and `pio run -e lolin_d32_pro` both build clean.
3. `clang-format` clean (CI gates).
4. Phase 3+: the milestone-appropriate QA case has been exercised end-to-end
   against a bench rig + the live AstrOs.Server.

## Out of scope

- ESP-NOW `OTA_*` packet types (sub-project **(b)** phase 2).
- `esp_ota_begin/write/end/set_boot_partition` calls on either master or
  padawan (sub-project **(b)** phase 2).
- `FW_PROGRESS` per-controller updates (only meaningful once mesh deploy
  exists).
- Cleanup of staged `<sha-prefix>.bin` files after consumption (owned by the
  next milestone — that's when "consumption" first becomes a thing).
- Server-side or Vue changes (those sub-projects are already complete).

## Cross-repo coordination

- The wire-format contract in `.docs/protocol.md` is shared with AstrOs.Server.
  This milestone does not amend it.
- No Server-side or Vue changes required. The existing Firmware view is the
  test driver for Phase 3 onwards.
- Phase 3+ QA depends on the AstrOs.Server `develop` branch being current.

## Open questions

None at design time. Implementation-time decisions (exact stack size for
`otaReceiverTask`, base64 decoder choice, fwrite chunking granularity) are
deferred to the per-phase implementation plans.

## Related documents

- `.docs/protocol.md` — frozen wire-format contract.
- `AstrOs.Server/.docs/plans/20260427-2202-firmware-ota-decomposition.md` —
  cross-repo decomposition; defines sub-projects **(a)** and **(b)**.
- `.docs/completed-plans/20260427-1730-firmware-version-phase1-firmware.md` —
  POLL_ACK version reporting (foundation for VERSION_CONFIRMED).
