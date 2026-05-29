# Firmware OTA Phase 3 — Wire It Up, Sink to /dev/null Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the firmware-side OTA receive pipeline end-to-end so the AstrOs.Server's Firmware view can drive a complete 1.2 MB transfer through the master over serial. Phase 3 plumbs every wire-protocol interaction (BEGIN / CHUNK / END / DEPLOY_BEGIN), exercises base64 decode, and produces the right ACK/NAK on every path — but **discards the payload bytes**. SD persistence and SHA-256 verification land in Phase 4.

**Architecture:** A new MIXED `lib/OtaReceiver` owns one `BulkReceiver` (Phase 2 PURE state machine) plus a FreeRTOS task that drains a new `otaQueue`. `AstrOsSerialMsgHandler::handleMessage` gets a short-circuit branch for the four inbound FW_* types: it parses the validated payload, base64-decodes FW_CHUNK frames, and posts a `queue_ota_msg_t` to `otaQueue` — bypassing Phase 1's placeholder `decodeFwInbound` (which stays in the codebase as harmless dead code for the serial-master path). `OtaReceiver` calls `BulkReceiver`, then builds and queues each FW_*_ACK / FW_CHUNK_NAK / FW_DEPLOY_DONE reply via new `sendFw*` helpers on `AstrOsSerialMsgHandler`, which serialize the response onto `serialCh1Queue` exactly like the existing `sendPollAckNak` / `sendBasicAckNakResponse` flows.

**Tech Stack:** C++17 firmware (PlatformIO, ESP-IDF), `mbedtls_base64_decode` (already linked transitively via ESP-IDF), FreeRTOS queues + pinned tasks, googletest for the small bits we can host-test. Phase 4 will add `mbedtls_sha256_*` and FAT/SD I/O on top of the receiver this phase scaffolds.

**Design doc:** `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md` (Phase 3 section)
**Phase 1 (merged):** `lib_native/AstrOsMessaging` FW_* builders + parsers + Phase 1 routing through `AstrOsSerialProtocol`.
**Phase 2 (merged):** `lib_native/AstrOsBulkTransport` `BulkReceiver` state machine + CRC-16/CCITT-FALSE helper.
**Phase context:** This is Phase 3 of 5. Phase 4 extends `OtaReceiver` with the SD writer + streaming SHA-256 context; Phase 5 hardens failure modes and adds a watchdog if measurements show it's needed.

---

## File Structure

| Path | Purpose | Action |
|---|---|---|
| `lib/OtaReceiver/include/OtaReceiver.hpp` | `class OtaReceiver` public API + global `AstrOs_OtaReceiver` extern | Create |
| `lib/OtaReceiver/include/OtaQueueMessage.h` | C-linkage `queue_ota_msg_t` POD + `ota_msg_kind_t` enum used by both handler and receiver task | Create |
| `lib/OtaReceiver/src/OtaReceiver.cpp` | `process()` dispatch + per-kind handlers + buffer-free hygiene | Create |
| `lib/OtaReceiver/README` | MIXED classification, queue ownership invariants, Phase 4 hooks | Create |
| `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp` | Add `otaQueue` field, extend `Init` signature, declare 5 new `sendFw*` methods | Modify |
| `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` | FW_* dispatch branch in `handleMessage` + base64 decode + 5 new `sendFw*` implementations with empty-string fallback | Modify |
| `src/main.cpp` | Create `otaQueue` (depth 16, item `queue_ota_msg_t`); init `AstrOs_OtaReceiver`; spawn `otaReceiverTask` on core 1; update `AstrOs_SerialMsgHandler.Init(...)` call site | Modify |
| `test/test_native/ota_queue_message_tests.cpp` | Minimal native test that proves the union layout is sized correctly and `kind` discriminates safely | Create |

No changes to `lib_native/AstrOsMessaging`, `lib_native/AstrOsBulkTransport`, or `lib_native/AstrOsSerialProtocol` — Phase 1's `decodeFwInbound` stays in place (vestigial for the master serial path; potentially relevant if Phase 5 reuses the dispatch table for ESP-NOW). Cleanup of that vestige is **explicitly deferred** to Phase 4 or later.

## Background notes for the implementer

- **Where `pio` lives.** Same as Phase 2 — assume it's on PATH, otherwise invoke via `~/.platformio/penv/bin/pio` or the VS Code-bundled binary. Build commands: `pio run -e metro_s3`, `pio run -e lolin_d32_pro`, `pio test -e test`. The pre-commit hook runs clang-format on staged C/C++ files.
- **Branch.** Implementation work happens on `feature/ota-phase3-wire-up`, branched from `develop`. The plan file you are reading is committed here as the first commit. Per `CLAUDE.md` the plan must be committed before any implementation code.
- **The `AstrOs_SerialMsgHandler` global lives near the top of `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` (line moves with edits; current is line 16 post-clang-format).** It is the only instance; both `handleMessage` (inbound parse) and the various `sendXxx` methods (outbound to server) run through it. Adding `sendFw*` methods follows the existing per-message-type pattern.
- **Inbound flow for the master:** `astrosRxTask` (in `src/main.cpp` around line 984) reads bytes from the AstrOs UART, accumulates a line, and on `\n` calls `AstrOs_SerialMsgHandler.handleMessage(line)` at roughly line 1011. That call runs on core 0. Phase 3's FW_* dispatch executes in this same task — base64 decode happens there, then the decoded payload is handed off via `otaQueue` to the new `otaReceiverTask` running on core 1.
- **`AstrOsSerialMsgHandler::handleMessage` currently always calls `AstrOsSerialProtocol::decodeSerialMessage` and pumps the resulting commands into `interfaceResponseQueue`** (the decode call sits at line 92 of the .cpp post-clang-format; in develop pre-Phase-3 it was line 47). Phase 3 adds an **early return** for the four FW_* types BEFORE that decode call. The Phase 1 `decodeFwInbound` path therefore becomes unreachable for these messages on the master serial path; leave it in place but do not modify it.
- **Base64 decoder choice: `mbedtls_base64_decode`.** Header `mbedtls/base64.h` is already in the ESP-IDF dependency tree (the SHA-256 path Phase 4 needs lives in the same component). The function signature is:
  ```c
  int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                            const unsigned char *src, size_t slen);
  ```
  It returns `0` on success and writes the decoded length into `*olen`. On invalid base64 input it returns `MBEDTLS_ERR_BASE64_INVALID_CHARACTER` (≠ 0); on buffer-too-small it returns `MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL`. The decoded length for a chunk is bounded by `payloadLen` declared on the wire, plus a small slack; allocate `payloadLen` exactly and treat any decoder error as `FW_CHUNK_NAK reason=SIZE`.
- **`mbedtls_base64_decode` is host-portable too** — its header lives at `<mbedtls/base64.h>` in ESP-IDF. The `[env:test]` native build does NOT have mbedtls available, so any test that exercises base64 decoding must run as a board-side QA case, not a unit test.
- **`BulkReceiver::begin()` returns `BeginResult { valid, reason }`** with `reason ∈ { OK, ZERO_CHUNK_SIZE, ZERO_TOTAL_CHUNKS, ZERO_WINDOW_SIZE, SIZE_INCONSISTENT }`. Phase 3 maps every non-OK reason to wire-level BEGIN_ACK `status="io_error"` (the protocol spec defines `OK | sd_full | busy | unsupported_version | io_error`; the four BulkReceiver validation failures all collapse to `io_error` because they're caused by the server sending a malformed BEGIN). Phase 3 still has its own `busy` check (transfer already active) that runs **before** `BulkReceiver::begin()`. `sd_full` and `unsupported_version` are dormant in Phase 3 — no SD, no version gate yet — but the plumbing for them lives in Phase 4.
- **`BulkReceiver::onChunk` returns `ChunkResult` with public `const` fields.** Construct results via the static factories (`ChunkResult::ack`, `nakActive`, `nakInactive`). Phase 3 reads `.decision`, `.highestContiguousSeq`, `.nextExpectedSeq`, `.windowRemaining`, `.reason`, `.payload`, `.payloadLen` directly. Note: `.windowRemaining == 0` is the inactive-receiver sentinel; on the wire we still send the configured window value back to the server when the receiver IS active, otherwise 0.
- **`BulkReceiver::onEnd` returns `EndResult { Status, Reason }`.** `Status::OK` on success; `Status::IO_ERROR` (with one of `NOT_ACTIVE`, `WRONG_XFER_ID`, `SENDER_TOTAL_MISMATCH`, `RECEIVER_SHORT_COUNT`) on failure. `Status::HASH_MISMATCH` is reserved for Phase 4. Map every `Status::IO_ERROR` reason to wire `status="IO_ERROR"`; log the specific `Reason` via `ESP_LOGW` so the bench can diagnose.
- **Receiver lifecycle:** `BulkReceiver` does not auto-reset after `onEnd`. After every `onEnd` (OK or IO_ERROR) the receiver task **must** call `reset()` before the next BEGIN can succeed. Track this with an `active_` flag mirrored on the `OtaReceiver` side so the busy check at BEGIN time has an unambiguous answer.
- **`transferId` type mismatch.** Phase 1's parsers return `transferId` as `std::string` (it's wire-level opaque). `BulkReceiver` takes `uint8_t xferId`. Phase 3 converts at the boundary using `std::strtoul` with `errno` check: on conversion failure (non-numeric or >255) emit BEGIN_ACK `status="io_error"` and never call `BulkReceiver::begin()`. The string form is still echoed back in every FW_*_ACK / NAK payload — the server treats `transferId` as opaque on the wire.
- **Queue message memory ownership.** Standard project convention applies: producer (`AstrOsSerialMsgHandler`) malloc's, on `xQueueSend` failure producer frees; consumer (`otaReceiverTask`) frees on the success path. The `payload` buffer (FW_CHUNK), `targetList` (FW_TRANSFER_BEGIN), `orderList` (FW_DEPLOY_BEGIN), and the `msgId` / `transferId` heads-up strings are all owned by the producer until the queue accepts the message.
- **The empty-string builder fallback** is load-bearing. `getFwChunkNak` returns `""` if `reasonCode` is not one of `{CRC, SIZE, OUT_OF_ORDER, FLASH_FULL}`. `getFwDeployDone` returns `""` if `results` is empty or any `status` is not `"OK"` / `"FAILED"`. Every Phase 3 call site must `if (response.empty()) { ESP_LOGE(TAG, "..."); /* hand-build a fallback */ }`. The fallback for a NAK is a hand-crafted `FW_CHUNK_NAK` payload built inline; for a malformed `FW_DEPLOY_DONE` the recovery is to `logError` and drop the reply (the server will time out and the job will be marked failed by JobLock — acceptable for an internal contract violation).
- **OtaReceiver dispatch is per-message-kind, not per-state.** The state lives entirely in `BulkReceiver`. `OtaReceiver` only tracks `active_` (mirrors `BulkReceiver`'s active state for the busy-at-BEGIN check) and the echoed-string fields for ACK construction (`transferId_`, the previous BEGIN's `msgId_`, etc.). Phase 4 adds `FILE *stagingFp_`, `mbedtls_sha256_context shaCtx_`.
- **Queue depth choice (16).** Matches the server's nominal sender window. The 4 KB chunk payload buffers dominate memory; 16 × 4 KB = 64 KB of in-flight chunk buffers worst-case. Heap budget on metro_s3 is comfortable here. If Phase 5 measurements show waste, we can shrink — not a Phase 3 concern.
- **Task stack 4096 bytes.** Phase 4 may need to bump this when SHA-256 + fwrite enter; Phase 3 doesn't allocate large stack locals. The standard `uxTaskGetStackHighWaterMark` check at the top of the task body warns at <500 bytes remaining.

---

### Task 1: Scaffold lib/OtaReceiver + commit plan

**Files:**
- Create: `lib/OtaReceiver/include/OtaReceiver.hpp`
- Create: `lib/OtaReceiver/include/OtaQueueMessage.h`
- Create: `lib/OtaReceiver/src/OtaReceiver.cpp`
- Create: `lib/OtaReceiver/README`

This task lays down the directory, an empty-but-valid class + queue message header, and the README. No functional code yet — the goal is "both boards compile, no tests fail". This also commits the plan file you are reading so the rest of the work has a permanent record on disk.

- [ ] **Step 1: Verify branch + commit the plan**

Confirm you are on `feature/ota-phase3-wire-up`:

```bash
git branch --show-current
```

Expected: `feature/ota-phase3-wire-up`.

Stage this plan file and commit:

```bash
git add .docs/plans/20260515-1428-firmware-ota-phase3-wire-up.md
git commit -m "docs(plan): Phase 3 implementation plan — wire-up + base64 + /dev/null"
```

Per `CLAUDE.md` the plan must be committed before any implementation code lands.

- [ ] **Step 2: Create the README**

`lib/OtaReceiver/README`:

```
OtaReceiver
===========

MIXED lib. Owns the master-side state machine for an in-flight firmware
OTA transfer over serial: drains the otaQueue, calls into the PURE
AstrOsBulkTransport state machine, and produces FW_*_ACK / NAK / DONE
replies via AstrOs_SerialMsgHandler. Wraps Phase 2's PURE BulkReceiver
with the FreeRTOS task, queue handle, and (Phase 4+) staging FILE* +
streaming SHA-256 context that BulkReceiver intentionally does not own.

Classification
--------------

MIXED — includes <freertos/...>, <esp_log.h>, and (Phase 4+)
<mbedtls/sha256.h>. Does NOT compile under [env:test]. Native tests
live alongside the PURE pieces in AstrOsBulkTransport / AstrOsMessaging.

Queue ownership invariants
--------------------------

queue_ota_msg_t records carry malloc'd pointers (payload, targetList,
orderList, msgId, transferId). The producer (AstrOsSerialMsgHandler)
mallocs and on xQueueSend failure frees its own allocations. The
consumer (otaReceiverTask) frees every malloc'd pointer in the
discriminated union after processing the kind it actually receives.
See OtaQueueMessage.h for the per-kind ownership notes.

Phase 4 hooks
-------------

handleBegin will gain: SD free-space pre-check, staging.bin open, sha256
context init. handleChunk will gain: fwrite(payload) + sha256_update.
handleEnd will gain: sha256_finish, computed-hash echo on END_ACK,
content-addressed rename of staging.bin. handleDeployBegin will
eventually drive ESP-NOW mesh forwarding; the all-FAILED
"not_implemented" stub Phase 3 returns is replaced wholesale.
```

- [ ] **Step 3: Create OtaQueueMessage.h**

`lib/OtaReceiver/include/OtaQueueMessage.h`:

```c
#ifndef OTAQUEUEMESSAGE_H
#define OTAQUEUEMESSAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        OTA_MSG_BEGIN = 0,
        OTA_MSG_CHUNK = 1,
        OTA_MSG_END = 2,
        OTA_MSG_DEPLOY_BEGIN = 3
    } ota_msg_kind_t;

    // Discriminated union carrying one decoded inbound FW_* message from
    // AstrOsSerialMsgHandler (producer) to otaReceiverTask (consumer).
    //
    // Memory ownership:
    //   - Producer mallocs every pointer field listed below for the kind
    //     it sends. On xQueueSend failure, producer frees its own
    //     allocations and emits a fallback NAK so the server can retry.
    //   - Consumer reads `kind` first, then frees only the pointers
    //     belonging to that kind's union arm. Mixing kinds across the
    //     free path will leak or double-free.
    //
    // Per-kind owned pointers:
    //   OTA_MSG_BEGIN         transferId, msgId, targetList
    //   OTA_MSG_CHUNK         transferId, payload
    //   OTA_MSG_END           transferId, msgId
    //   OTA_MSG_DEPLOY_BEGIN  transferId, msgId, orderList
    //
    // sha256Hex / finalSha256Hex are fixed-size 65-byte buffers (64 hex
    // chars + NUL); they live inline in the struct and never need freeing.
    typedef struct
    {
        ota_msg_kind_t kind;
        char *transferId; // wire-level opaque string, malloc'd, NUL-terminated

        union {
            struct
            {
                char *msgId;     // BEGIN's msgId for ACK echo
                uint32_t totalSize;
                uint32_t totalChunks;
                uint16_t chunkSize;
                char sha256Hex[65];
                char *targetList; // RS-separated controller-id list
            } begin;

            struct
            {
                uint32_t seq;
                uint16_t payloadLen; // base64-DECODED length, bytes
                uint16_t crc16;
                uint8_t *payload; // decoded bytes, length == payloadLen
            } chunk;

            struct
            {
                char *msgId; // END's msgId for ACK echo
                uint32_t totalChunks;
                char finalSha256Hex[65];
            } end;

            struct
            {
                char *msgId;     // DEPLOY_BEGIN's msgId for DONE echo
                char *orderList; // RS-separated controller-id list
            } deploy;
        };
    } queue_ota_msg_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif
```

- [ ] **Step 4: Create OtaReceiver.hpp (skeleton)**

`lib/OtaReceiver/include/OtaReceiver.hpp`:

```cpp
#ifndef OTARECEIVER_HPP
#define OTARECEIVER_HPP

#include <AstrOsBulkTransport.hpp>
#include <OtaQueueMessage.h>

#include <cstdint>
#include <string>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class OtaReceiver
{
private:
    AstrOsBulkTransport::BulkReceiver bulk_;
    bool active_ = false;
    // Echoed back in every FW_*_ACK / NAK on this transfer.
    std::string transferIdStr_;
    // Captured at BEGIN time, echoed in END_ACK (END's own msgId echoes via
    // the END record itself; this field is for BEGIN_ACK only).
    std::string beginMsgId_;

public:
    OtaReceiver();
    ~OtaReceiver();

    // Called once at boot from main.cpp; intentionally separate from the
    // constructor so the global instance can be constructed at static-init
    // time, before FreeRTOS queues exist.
    void Init();

    // Drains one queue_ota_msg_t. Dispatches by `msg.kind` and frees every
    // malloc'd pointer in the corresponding union arm before returning.
    void process(queue_ota_msg_t &msg);

private:
    void handleBegin(queue_ota_msg_t &msg);
    void handleChunk(queue_ota_msg_t &msg);
    void handleEnd(queue_ota_msg_t &msg);
    void handleDeployBegin(queue_ota_msg_t &msg);
};

extern OtaReceiver AstrOs_OtaReceiver;

#endif
```

- [ ] **Step 5: Create OtaReceiver.cpp (skeleton)**

`lib/OtaReceiver/src/OtaReceiver.cpp`:

```cpp
#include <OtaReceiver.hpp>

#include <esp_log.h>

static const char *TAG = "OtaReceiver";

OtaReceiver AstrOs_OtaReceiver;

OtaReceiver::OtaReceiver() {}

OtaReceiver::~OtaReceiver() {}

void OtaReceiver::Init()
{
    // Phase 3: nothing to do here yet. Phase 4 will allocate the SHA
    // context and ensure the firmware staging dir exists on SD.
    ESP_LOGI(TAG, "OtaReceiver initialized");
}

void OtaReceiver::process(queue_ota_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_MSG_BEGIN:
        handleBegin(msg);
        break;
    case OTA_MSG_CHUNK:
        handleChunk(msg);
        break;
    case OTA_MSG_END:
        handleEnd(msg);
        break;
    case OTA_MSG_DEPLOY_BEGIN:
        handleDeployBegin(msg);
        break;
    default:
        ESP_LOGE(TAG, "Unknown ota_msg_kind_t: %d", static_cast<int>(msg.kind));
        free(msg.transferId);
        break;
    }
}

void OtaReceiver::handleBegin(queue_ota_msg_t &msg)
{
    // Task 3 implements this.
    free(msg.begin.msgId);
    free(msg.begin.targetList);
    free(msg.transferId);
}

void OtaReceiver::handleChunk(queue_ota_msg_t &msg)
{
    // Task 4 implements this.
    free(msg.chunk.payload);
    free(msg.transferId);
}

void OtaReceiver::handleEnd(queue_ota_msg_t &msg)
{
    // Task 5 implements this.
    free(msg.end.msgId);
    free(msg.transferId);
}

void OtaReceiver::handleDeployBegin(queue_ota_msg_t &msg)
{
    // Task 6 implements this.
    free(msg.deploy.msgId);
    free(msg.deploy.orderList);
    free(msg.transferId);
}
```

- [ ] **Step 6: Verify both boards compile**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean. The new lib is auto-discovered because PlatformIO scans `lib/` (existing behavior — no `library.json` needed).

If linker errors complain about `AstrOs_OtaReceiver` being defined but unused: that's fine for now; nothing in `main.cpp` references it yet.

- [ ] **Step 7: Commit**

```bash
git add lib/OtaReceiver
git commit -m "feat(ota-receiver): scaffold lib/OtaReceiver + queue_ota_msg_t header"
```

---

### Task 2: Native test for queue_ota_msg_t layout

**Files:**
- Create: `test/test_native/ota_queue_message_tests.cpp`

The queue message header is C-linkage and uses an anonymous union. Get one smoke test in place that proves the layout compiles under the native toolchain and the union arms are sized as we expect — catches future drift between handler producer and receiver consumer.

- [ ] **Step 1: Write the test**

`test/test_native/ota_queue_message_tests.cpp`:

```cpp
#include <gtest/gtest.h>

#include <OtaQueueMessage.h>

#include <cstdint>
#include <cstring>

TEST(OtaQueueMessage, BeginArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_BEGIN;
    m.transferId = nullptr;
    m.begin.msgId = nullptr;
    m.begin.totalSize = 1234567;
    m.begin.totalChunks = 300;
    m.begin.chunkSize = 4096;
    std::strncpy(m.begin.sha256Hex, "deadbeefcafebabe1111222233334444aaaabbbbccccddddeeeeffff00001111", 64);
    m.begin.sha256Hex[64] = '\0';
    m.begin.targetList = nullptr;

    EXPECT_EQ(OTA_MSG_BEGIN, m.kind);
    EXPECT_EQ(1234567u, m.begin.totalSize);
    EXPECT_EQ(300u, m.begin.totalChunks);
    EXPECT_EQ(4096u, m.begin.chunkSize);
    EXPECT_STREQ("deadbeefcafebabe1111222233334444aaaabbbbccccddddeeeeffff00001111", m.begin.sha256Hex);
}

TEST(OtaQueueMessage, ChunkArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_CHUNK;
    m.transferId = nullptr;
    m.chunk.seq = 17;
    m.chunk.payloadLen = 4096;
    m.chunk.crc16 = 0xABCD;
    m.chunk.payload = nullptr;

    EXPECT_EQ(OTA_MSG_CHUNK, m.kind);
    EXPECT_EQ(17u, m.chunk.seq);
    EXPECT_EQ(4096u, m.chunk.payloadLen);
    EXPECT_EQ(0xABCDu, m.chunk.crc16);
}

TEST(OtaQueueMessage, EndArmRoundTrip)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_END;
    m.end.msgId = nullptr;
    m.end.totalChunks = 300;
    std::strncpy(m.end.finalSha256Hex, "00000000000000000000000000000000ffffffffffffffffffffffffffffffff", 64);
    m.end.finalSha256Hex[64] = '\0';

    EXPECT_EQ(OTA_MSG_END, m.kind);
    EXPECT_EQ(300u, m.end.totalChunks);
}

TEST(OtaQueueMessage, KindDiscriminatesUnion)
{
    queue_ota_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_DEPLOY_BEGIN;
    m.deploy.msgId = nullptr;
    m.deploy.orderList = nullptr;

    EXPECT_EQ(OTA_MSG_DEPLOY_BEGIN, m.kind);
}
```

- [ ] **Step 2: Make the header reachable from `lib_native/` test path**

The native `[env:test]` build does not by default include `lib/` (those are firmware-only / MIXED). We need to make `OtaQueueMessage.h` visible to the native test target. Two options:

**Option A (recommended): add `lib/OtaReceiver/include` to the test build's include path.**

In `platformio.ini` under the `[env:test]` section, locate `build_flags`. Append:

```ini
    -I lib/OtaReceiver/include
```

This exposes ONLY the C-linkage header, not the C++ class — the header has no FreeRTOS or ESP-IDF includes, so it is safe to expose under native.

**Option B: duplicate the header into `lib_native/AstrOsBulkTransport/include/`.** Rejected — duplication.

Confirm `OtaQueueMessage.h` has zero FreeRTOS / ESP-IDF includes (it does — just `<stdint.h>`).

- [ ] **Step 3: Run the test, watch it pass**

```bash
pio test -e test --filter "*OtaQueueMessage*"
```

Expected: 4 tests pass.

- [ ] **Step 4: Run the full native test suite to confirm no regressions**

```bash
pio test -e test
```

Expected: all 294 (pre-existing) + 4 (new) = 298 tests pass.

- [ ] **Step 5: Commit**

```bash
git add lib/OtaReceiver/include/OtaQueueMessage.h test/test_native/ota_queue_message_tests.cpp platformio.ini
git commit -m "test(ota-receiver): smoke-test queue_ota_msg_t union layout"
```

---

### Task 3: AstrOsSerialMsgHandler — extend Init + add sendFw* helpers (no dispatch yet)

**Files:**
- Modify: `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`
- Modify: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`

Add the `otaQueue` field, extend `Init`, and implement five new outbound helpers (`sendFwTransferBeginAck`, `sendFwChunkAck`, `sendFwChunkNak`, `sendFwTransferEndAck`, `sendFwDeployDone`). Each follows the existing `sendBasicAckNakResponse` pattern: build via `msgService`, malloc on `serialQueue`, free on send failure. The five new methods each carry the empty-string fallback per the design doc's load-bearing caller contract.

- [ ] **Step 1: Extend the header**

In `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`:

After the existing `serialQueue` field (around line 17), add:

```cpp
    QueueHandle_t otaQueue;
```

Change the `Init` declaration from:

```cpp
    void Init(QueueHandle_t serverResponseQueue, QueueHandle_t serialQueue);
```

to:

```cpp
    void Init(QueueHandle_t serverResponseQueue, QueueHandle_t serialQueue, QueueHandle_t otaQueue);
```

After the existing `sendBasicAckNakResponse` declaration (around line 32), add:

```cpp
    void sendFwTransferBeginAck(std::string msgId, std::string transferId, std::string status);
    void sendFwChunkAck(std::string transferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                        uint8_t windowRemaining);
    void sendFwChunkNak(std::string transferId, uint32_t lastGoodSeq, uint32_t nextExpectedSeq,
                        std::string reasonCode);
    void sendFwTransferEndAck(std::string msgId, std::string transferId, std::string status,
                              std::string computedSha256Hex);
    void sendFwDeployDone(std::string msgId, std::string transferId,
                          std::vector<astros_fw_deploy_result_t> results);
```

- [ ] **Step 2: Extend Init in the cpp**

In `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`:

Change `Init` from:

```cpp
void AstrOsSerialMsgHandler::Init(QueueHandle_t handlerQueue, QueueHandle_t serialQueue)
{
    this->handlerQueue = handlerQueue;
    this->serialQueue = serialQueue;

    this->msgService = AstrOsSerialMessageService();
}
```

to:

```cpp
void AstrOsSerialMsgHandler::Init(QueueHandle_t handlerQueue, QueueHandle_t serialQueue, QueueHandle_t otaQueue)
{
    this->handlerQueue = handlerQueue;
    this->serialQueue = serialQueue;
    this->otaQueue = otaQueue;

    this->msgService = AstrOsSerialMessageService();
}
```

- [ ] **Step 3: Implement `sendFwTransferBeginAck`**

Append at the bottom of the .cpp file:

```cpp
void AstrOsSerialMsgHandler::sendFwTransferBeginAck(std::string msgId, std::string transferId, std::string status)
{
    auto response = this->msgService.getFwTransferBeginAck(msgId, transferId, status);

    if (response.empty())
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN_ACK build returned empty — caller-contract violation. "
                      "msgId=%s transferId=%s status=%s",
                 msgId.c_str(), transferId.c_str(), status.c_str());
        return;
    }

    queue_serial_msg_t serialMsg;
    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail (FW_TRANSFER_BEGIN_ACK)");
        free(serialMsg.data);
    }
}
```

Note: `getFwTransferBeginAck` does not currently validate its `status` argument, so the empty-string branch is unreachable from a correct caller — but we keep the guard to flag any future builder-side validation change.

- [ ] **Step 4: Implement `sendFwChunkAck`**

```cpp
void AstrOsSerialMsgHandler::sendFwChunkAck(std::string transferId, uint32_t highestContiguousSeq,
                                            uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    auto response = this->msgService.getFwChunkAck(transferId, highestContiguousSeq, nextExpectedSeq, windowRemaining);

    if (response.empty())
    {
        ESP_LOGE(TAG, "FW_CHUNK_ACK build returned empty — transferId=%s seq=%u", transferId.c_str(),
                 (unsigned)highestContiguousSeq);
        return;
    }

    queue_serial_msg_t serialMsg;
    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail (FW_CHUNK_ACK)");
        free(serialMsg.data);
    }
}
```

- [ ] **Step 5: Implement `sendFwChunkNak`**

```cpp
void AstrOsSerialMsgHandler::sendFwChunkNak(std::string transferId, uint32_t lastGoodSeq, uint32_t nextExpectedSeq,
                                            std::string reasonCode)
{
    auto response = this->msgService.getFwChunkNak(transferId, lastGoodSeq, nextExpectedSeq, reasonCode);

    if (response.empty())
    {
        // getFwChunkNak rejects reasonCode ∉ {CRC, SIZE, OUT_OF_ORDER, FLASH_FULL}. This is a fatal
        // local bug — we can't legitimately recover, but we MUST emit *something* so the server
        // doesn't hang waiting on this seq. Fall back to a hand-built NAK with reason="CRC" which
        // is the safe "ask for retransmit" default per the design doc's load-bearing contract.
        ESP_LOGE(TAG, "FW_CHUNK_NAK build returned empty for reasonCode='%s'. Falling back to CRC.",
                 reasonCode.c_str());
        response = this->msgService.getFwChunkNak(transferId, lastGoodSeq, nextExpectedSeq, "CRC");
        if (response.empty())
        {
            ESP_LOGE(TAG, "Fallback FW_CHUNK_NAK also empty — server will time out. Dropping reply.");
            return;
        }
    }

    queue_serial_msg_t serialMsg;
    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail (FW_CHUNK_NAK)");
        free(serialMsg.data);
    }
}
```

- [ ] **Step 6: Implement `sendFwTransferEndAck`**

```cpp
void AstrOsSerialMsgHandler::sendFwTransferEndAck(std::string msgId, std::string transferId, std::string status,
                                                  std::string computedSha256Hex)
{
    auto response = this->msgService.getFwTransferEndAck(msgId, transferId, status, computedSha256Hex);

    if (response.empty())
    {
        ESP_LOGE(TAG, "FW_TRANSFER_END_ACK build returned empty — transferId=%s status=%s", transferId.c_str(),
                 status.c_str());
        return;
    }

    queue_serial_msg_t serialMsg;
    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail (FW_TRANSFER_END_ACK)");
        free(serialMsg.data);
    }
}
```

- [ ] **Step 7: Implement `sendFwDeployDone`**

```cpp
void AstrOsSerialMsgHandler::sendFwDeployDone(std::string msgId, std::string transferId,
                                              std::vector<astros_fw_deploy_result_t> results)
{
    auto response = this->msgService.getFwDeployDone(msgId, transferId, results);

    if (response.empty())
    {
        // getFwDeployDone returns "" if results is empty or any status is not "OK"/"FAILED".
        // Both are caller programming errors. We can't legitimately recover — the server has no
        // sentinel to retry against, only timeout. Log and drop.
        ESP_LOGE(TAG, "FW_DEPLOY_DONE build returned empty — transferId=%s resultCount=%zu", transferId.c_str(),
                 results.size());
        return;
    }

    queue_serial_msg_t serialMsg;
    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail (FW_DEPLOY_DONE)");
        free(serialMsg.data);
    }
}
```

- [ ] **Step 8: Build (fails because `Init` callsites in main.cpp don't pass otaQueue yet)**

```bash
pio run -e metro_s3
```

Expected: build fails at the call site `AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue);` in `src/main.cpp:270` — "too few arguments to function call". This is the correct failure; Task 7 fixes it.

For Task 3 only, temporarily extend the call site to pass a placeholder so we can verify the new methods compile:

In `src/main.cpp` around line 270, change to:

```cpp
    AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue, /*otaQueue=*/nullptr);
```

This is a deliberate placeholder; Task 7 replaces `nullptr` with the real queue handle.

- [ ] **Step 9: Build again**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 10: Commit**

```bash
git add lib/AstrOsSerialMsgHandler src/main.cpp
git commit -m "feat(serial-handler): add otaQueue field + 5 sendFw* helpers with empty-string fallback"
```

---

### Task 4: AstrOsSerialMsgHandler — FW_* dispatch in handleMessage

**Files:**
- Modify: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`

Add the early-return branch for FW_* types BEFORE the existing `decodeSerialMessage` call. Each branch:
1. Parses the validated payload via the relevant `parseFw*`.
2. For FW_CHUNK only: base64-decodes the payload into a freshly-malloc'd buffer.
3. Builds a `queue_ota_msg_t`, allocates char buffers for each malloc'd field.
4. `xQueueSend(otaQueue, …)`; on failure, frees and emits an appropriate NAK so the server can retry.

- [ ] **Step 1: Add the includes**

At the top of `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`, add:

```cpp
#include <OtaQueueMessage.h>
#include <mbedtls/base64.h>
#include <errno.h>
#include <stdlib.h>
```

- [ ] **Step 2: Add a private helper to allocate + copy a C string**

The handler already does this pattern inline in several places. Centralize it as a static helper near the top of the .cpp:

```cpp
namespace
{
    // Returns a malloc'd, NUL-terminated copy of `s`. Caller frees. Returns
    // nullptr on malloc failure (caller must handle).
    char *dupString(const std::string &s)
    {
        char *p = (char *)malloc(s.size() + 1);
        if (p == nullptr)
        {
            return nullptr;
        }
        memcpy(p, s.c_str(), s.size());
        p[s.size()] = '\0';
        return p;
    }
} // namespace
```

- [ ] **Step 3: Add the FW_* dispatch branch in `handleMessage`**

Insert this block immediately after the existing `if (validation.type == AstrOsSerialMessageType::UNKNOWN)` check and **before** the `auto decoded = AstrOsSerialProtocol::decodeSerialMessage(...)` call:

```cpp
    // FW_* OTA messages take a different path from the standard
    // decodeSerialMessage → interfaceResponseQueue pipeline: the OtaReceiver
    // task owns its own queue and handles all four kinds directly. Phase 1's
    // decodeFwInbound shim still exists in AstrOsSerialProtocol for the
    // (eventual) ESP-NOW path; on the master serial path it is unreachable.
    switch (validation.type)
    {
    case AstrOsSerialMessageType::FW_TRANSFER_BEGIN:
        this->handleFwTransferBeginInbound(validation.msgId, validation.payload);
        return;
    case AstrOsSerialMessageType::FW_CHUNK:
        this->handleFwChunkInbound(validation.payload);
        return;
    case AstrOsSerialMessageType::FW_TRANSFER_END:
        this->handleFwTransferEndInbound(validation.msgId, validation.payload);
        return;
    case AstrOsSerialMessageType::FW_DEPLOY_BEGIN:
        this->handleFwDeployBeginInbound(validation.msgId, validation.payload);
        return;
    default:
        break;
    }
```

- [ ] **Step 4: Add declarations for the four inbound helpers**

In `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`, in the `private:` section after `sendToInterfaceQueue`, add:

```cpp
    void handleFwTransferBeginInbound(const std::string &msgId, const std::string &payload);
    void handleFwChunkInbound(const std::string &payload);
    void handleFwTransferEndInbound(const std::string &msgId, const std::string &payload);
    void handleFwDeployBeginInbound(const std::string &msgId, const std::string &payload);
```

- [ ] **Step 5: Implement `handleFwTransferBeginInbound`**

Append in the .cpp:

```cpp
void AstrOsSerialMsgHandler::handleFwTransferBeginInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwTransferBegin(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN parse rejected payload");
        this->sendFwTransferBeginAck(msgId, /*transferId=*/"", "io_error");
        return;
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_BEGIN;
    m.transferId = dupString(rec.transferId);
    m.begin.msgId = dupString(msgId);
    m.begin.totalSize = rec.totalSize;
    m.begin.totalChunks = (rec.totalSize + rec.chunkSize - 1) / rec.chunkSize;
    m.begin.chunkSize = rec.chunkSize;
    strncpy(m.begin.sha256Hex, rec.sha256Hex.c_str(), 64);
    m.begin.sha256Hex[64] = '\0';

    // Join targetIds back into a single RS-separated string. Phase 3 doesn't
    // actually fan out to targets (no SD, no mesh), so OtaReceiver just frees
    // this field after parsing it back if needed. Phase 4+ may keep it for
    // the deploy stage.
    std::string joined;
    for (size_t i = 0; i < rec.targetIds.size(); i++)
    {
        if (i > 0)
            joined += '\x1E'; // RECORD_SEPARATOR
        joined += rec.targetIds[i];
    }
    m.begin.targetList = dupString(joined);

    if (m.transferId == nullptr || m.begin.msgId == nullptr || m.begin.targetList == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_TRANSFER_BEGIN dispatch");
        free(m.transferId);
        free(m.begin.msgId);
        free(m.begin.targetList);
        this->sendFwTransferBeginAck(msgId, rec.transferId, "io_error");
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        ESP_LOGW(TAG, "otaQueue full at FW_TRANSFER_BEGIN; rejecting with busy");
        free(m.transferId);
        free(m.begin.msgId);
        free(m.begin.targetList);
        this->sendFwTransferBeginAck(msgId, rec.transferId, "busy");
    }
}
```

- [ ] **Step 6: Implement `handleFwChunkInbound`** (the base64-decode path)

```cpp
void AstrOsSerialMsgHandler::handleFwChunkInbound(const std::string &payload)
{
    auto rec = parseFwChunk(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_CHUNK parse rejected payload");
        // No way to recover a transferId from a malformed payload — log and drop.
        return;
    }

    // mbedtls_base64_decode requires dst sized for the decoded output. The
    // declared wire-level payloadLen is the decoded byte count; allocate it
    // exactly and treat any size mismatch from the decoder as SIZE failure.
    uint8_t *decoded = (uint8_t *)malloc(rec.payloadLen);
    if (decoded == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_CHUNK dispatch");
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "SIZE");
        return;
    }

    size_t outLen = 0;
    int rc = mbedtls_base64_decode(decoded, rec.payloadLen, &outLen,
                                   reinterpret_cast<const unsigned char *>(rec.base64Payload.data()),
                                   rec.base64Payload.size());
    if (rc != 0 || outLen != rec.payloadLen)
    {
        ESP_LOGW(TAG, "FW_CHUNK base64 decode failed rc=%d out=%zu expected=%u", rc, outLen,
                 (unsigned)rec.payloadLen);
        free(decoded);
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "SIZE");
        return;
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_CHUNK;
    m.transferId = dupString(rec.transferId);
    m.chunk.seq = rec.seq;
    m.chunk.payloadLen = rec.payloadLen;
    m.chunk.crc16 = rec.crc16;
    m.chunk.payload = decoded;

    if (m.transferId == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_CHUNK dispatch (transferId)");
        free(decoded);
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "SIZE");
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        // Queue full = transient backpressure. Emit a CRC NAK so the server retransmits this
        // seq once the receiver drains. Stays inside the contract; cleaner backpressure via
        // FW_BACKPRESSURE is Phase 5. The decoded payload is freed here; the receiver never
        // saw this chunk.
        ESP_LOGW(TAG, "otaQueue full at FW_CHUNK seq=%u; emitting CRC NAK to force retransmit",
                 (unsigned)rec.seq);
        free(m.transferId);
        free(decoded);
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "CRC");
    }
}
```

Note: `lastGoodSeq` is set to `0` in all NAK paths from this layer because at decode time the handler has no insight into the receiver's `highestContiguousSeq`. The server is fine with this — its retransmit logic uses `nextExpectedSeq` (which we set to the rejected chunk's `seq`) as the authoritative resume point. Once the chunk reaches `BulkReceiver`, future NAKs originate from `OtaReceiver` and carry the receiver's real `highestContiguousSeq` (Task 5).

- [ ] **Step 7: Implement `handleFwTransferEndInbound`**

```cpp
void AstrOsSerialMsgHandler::handleFwTransferEndInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwTransferEnd(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_END parse rejected payload");
        this->sendFwTransferEndAck(msgId, /*transferId=*/"", "IO_ERROR",
                                   "0000000000000000000000000000000000000000000000000000000000000000");
        return;
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_END;
    m.transferId = dupString(rec.transferId);
    m.end.msgId = dupString(msgId);
    m.end.totalChunks = rec.totalChunks;
    strncpy(m.end.finalSha256Hex, rec.finalSha256Hex.c_str(), 64);
    m.end.finalSha256Hex[64] = '\0';

    if (m.transferId == nullptr || m.end.msgId == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_TRANSFER_END dispatch");
        free(m.transferId);
        free(m.end.msgId);
        this->sendFwTransferEndAck(msgId, rec.transferId, "IO_ERROR", rec.finalSha256Hex);
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "otaQueue full at FW_TRANSFER_END");
        free(m.transferId);
        free(m.end.msgId);
        this->sendFwTransferEndAck(msgId, rec.transferId, "IO_ERROR", rec.finalSha256Hex);
    }
}
```

- [ ] **Step 8: Implement `handleFwDeployBeginInbound`**

```cpp
void AstrOsSerialMsgHandler::handleFwDeployBeginInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwDeployBegin(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN parse rejected payload");
        std::vector<astros_fw_deploy_result_t> empty; // sendFwDeployDone drops on empty, intentional
        this->sendFwDeployDone(msgId, /*transferId=*/"", empty);
        return;
    }

    std::string joined;
    for (size_t i = 0; i < rec.orderIds.size(); i++)
    {
        if (i > 0)
            joined += '\x1E';
        joined += rec.orderIds[i];
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_DEPLOY_BEGIN;
    m.transferId = dupString(rec.transferId);
    m.deploy.msgId = dupString(msgId);
    m.deploy.orderList = dupString(joined);

    if (m.transferId == nullptr || m.deploy.msgId == nullptr || m.deploy.orderList == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_DEPLOY_BEGIN dispatch");
        free(m.transferId);
        free(m.deploy.msgId);
        free(m.deploy.orderList);
        // Synthesize a FAILED result per target so JobLock can release.
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "otaQueue full at FW_DEPLOY_BEGIN");
        free(m.transferId);
        free(m.deploy.msgId);
        free(m.deploy.orderList);
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
    }
}
```

- [ ] **Step 9: Build both boards**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 10: Commit**

```bash
git add lib/AstrOsSerialMsgHandler
git commit -m "feat(serial-handler): FW_* dispatch + base64 decode + queue handoff to otaQueue"
```

---

### Task 5: OtaReceiver::handleBegin

**Files:**
- Modify: `lib/OtaReceiver/src/OtaReceiver.cpp`

Replace the Task 1 stub. Implementation:
1. If `active_` is already true: reply `FW_TRANSFER_BEGIN_ACK status="busy"`. Don't touch `bulk_`.
2. Convert `transferId` string to `uint8_t` via `strtoul`. On failure: reply `status="io_error"`.
3. Call `bulk_.begin(...)`. If `result.valid == false`: reply `status="io_error"` (log the specific reason for the bench).
4. On success: set `active_ = true`, capture `transferIdStr_` and `beginMsgId_`, reply `status="OK"`.

- [ ] **Step 1: Include the handler header so we can call sendFw* helpers**

At the top of `lib/OtaReceiver/src/OtaReceiver.cpp` add:

```cpp
#include <AstrOsSerialMsgHandler.hpp>
#include <errno.h>
#include <stdlib.h>
```

- [ ] **Step 2: Replace `handleBegin`**

```cpp
void OtaReceiver::handleBegin(queue_ota_msg_t &msg)
{
    std::string msgId = msg.begin.msgId ? msg.begin.msgId : "";
    std::string transferIdIn = msg.transferId ? msg.transferId : "";

    if (active_)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN while transfer %s active; replying busy", transferIdStr_.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "busy");
        free(msg.begin.msgId);
        free(msg.begin.targetList);
        free(msg.transferId);
        return;
    }

    // Convert the wire-level opaque string transferId to BulkReceiver's uint8_t form.
    errno = 0;
    char *endp = nullptr;
    unsigned long xidUL = strtoul(transferIdIn.c_str(), &endp, 10);
    if (errno != 0 || endp == transferIdIn.c_str() || *endp != '\0' || xidUL > 255)
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN transferId='%s' is not 0..255 numeric", transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        free(msg.begin.msgId);
        free(msg.begin.targetList);
        free(msg.transferId);
        return;
    }
    uint8_t xferId = static_cast<uint8_t>(xidUL);

    // Phase 3 sliding window: hard-coded to 16 (matches the server's nominal sender window).
    // Phase 5 may make this configurable.
    constexpr uint8_t kPhase3WindowSize = 16;

    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kPhase3WindowSize);
    if (!br.valid)
    {
        ESP_LOGW(TAG, "BulkReceiver::begin rejected: reason=%d (totalSize=%u chunks=%u chunkSize=%u)",
                 static_cast<int>(br.reason), (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks,
                 (unsigned)msg.begin.chunkSize);
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        free(msg.begin.msgId);
        free(msg.begin.targetList);
        free(msg.transferId);
        return;
    }

    active_ = true;
    transferIdStr_ = transferIdIn;
    beginMsgId_ = msgId;

    ESP_LOGI(TAG, "FW_TRANSFER_BEGIN accepted: transferId=%s totalSize=%u chunks=%u sha=%s", transferIdIn.c_str(),
             (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, msg.begin.sha256Hex);

    AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "OK");

    free(msg.begin.msgId);
    free(msg.begin.targetList);
    free(msg.transferId);
}
```

- [ ] **Step 3: Build both boards**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 4: Commit**

```bash
git add lib/OtaReceiver
git commit -m "feat(ota-receiver): handleBegin — BulkReceiver.begin + busy/io_error/OK reply"
```

---

### Task 6: OtaReceiver::handleChunk

**Files:**
- Modify: `lib/OtaReceiver/src/OtaReceiver.cpp`

Replace the Task 1 stub. Implementation:
1. If `!active_`: emit `FW_CHUNK_NAK reason=OUT_OF_ORDER lastGoodSeq=0 nextExpectedSeq=0` (the inactive sentinel). Free payload.
2. Call `bulk_.onChunk(...)`. Branch on `result.decision`:
   - `ACK`: emit `FW_CHUNK_ACK` with the receiver's window fields; free payload (Phase 3 sinks to /dev/null).
   - `NAK`: emit `FW_CHUNK_NAK` mapping `result.reason → wire code`; free payload.
3. Always free `payload` and `transferId`.

- [ ] **Step 1: Replace `handleChunk`**

```cpp
void OtaReceiver::handleChunk(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";

    if (!active_)
    {
        ESP_LOGW(TAG, "FW_CHUNK while no transfer active; emitting inactive NAK");
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/0, "OUT_OF_ORDER");
        free(msg.chunk.payload);
        free(msg.transferId);
        return;
    }

    // Re-derive the uint8_t form. transferIdStr_ is the authoritative running
    // value — if the incoming string mismatches, BulkReceiver::onChunk's
    // internal xferId check will return nakInactive() with reason=OUT_OF_ORDER.
    uint8_t xferId = 0;
    {
        errno = 0;
        char *endp = nullptr;
        unsigned long ul = strtoul(transferIdIn.c_str(), &endp, 10);
        if (errno != 0 || endp == transferIdIn.c_str() || *endp != '\0' || ul > 255)
        {
            // Wire form was numeric at BEGIN time; arriving as garbage now is a server bug.
            ESP_LOGW(TAG, "FW_CHUNK transferId='%s' not numeric; emitting OUT_OF_ORDER", transferIdIn.c_str());
            AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/0,
                                                   "OUT_OF_ORDER");
            free(msg.chunk.payload);
            free(msg.transferId);
            return;
        }
        xferId = static_cast<uint8_t>(ul);
    }

    auto cr = bulk_.onChunk(xferId, msg.chunk.seq, msg.chunk.payloadLen, msg.chunk.crc16, msg.chunk.payload);

    if (cr.decision == AstrOsBulkTransport::Decision::ACK)
    {
        // Phase 3 sinks payload to /dev/null. Phase 4 fwrites cr.payload here.
        AstrOs_SerialMsgHandler.sendFwChunkAck(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq,
                                               cr.windowRemaining);
    }
    else
    {
        const char *reasonStr = "OUT_OF_ORDER";
        switch (cr.reason)
        {
        case AstrOsBulkTransport::NakReason::CRC:
            reasonStr = "CRC";
            break;
        case AstrOsBulkTransport::NakReason::SIZE:
            reasonStr = "SIZE";
            break;
        case AstrOsBulkTransport::NakReason::OUT_OF_ORDER:
            reasonStr = "OUT_OF_ORDER";
            break;
        case AstrOsBulkTransport::NakReason::FLASH_FULL:
            reasonStr = "FLASH_FULL";
            break;
        case AstrOsBulkTransport::NakReason::NONE:
            // Unreachable on the NAK path — log and force CRC for safe retransmit.
            ESP_LOGE(TAG, "BulkReceiver returned NAK with reason=NONE; forcing CRC");
            reasonStr = "CRC";
            break;
        }
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq, reasonStr);
    }

    free(msg.chunk.payload);
    free(msg.transferId);
}
```

- [ ] **Step 2: Build both boards**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 3: Commit**

```bash
git add lib/OtaReceiver
git commit -m "feat(ota-receiver): handleChunk — BulkReceiver.onChunk + ACK/NAK reply + free payload"
```

---

### Task 7: OtaReceiver::handleEnd

**Files:**
- Modify: `lib/OtaReceiver/src/OtaReceiver.cpp`

Replace the Task 1 stub. Implementation:
1. Call `bulk_.onEnd(xferId, totalChunks)`.
2. On `Status::OK`: emit `FW_TRANSFER_END_ACK status="OK" computedSha256Hex=<echo of finalSha256Hex>` (Phase 3 has no real hash).
3. On `Status::IO_ERROR`: emit `FW_TRANSFER_END_ACK status="IO_ERROR" computedSha256Hex=<echo>`. Log the specific reason.
4. Always: clear `active_`, call `bulk_.reset()`, free pointers.

- [ ] **Step 1: Replace `handleEnd`**

```cpp
void OtaReceiver::handleEnd(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.end.msgId ? msg.end.msgId : "";
    std::string finalShaIn(msg.end.finalSha256Hex);

    uint8_t xferId = 0;
    {
        errno = 0;
        char *endp = nullptr;
        unsigned long ul = strtoul(transferIdIn.c_str(), &endp, 10);
        if (errno != 0 || endp == transferIdIn.c_str() || *endp != '\0' || ul > 255)
        {
            ESP_LOGW(TAG, "FW_TRANSFER_END transferId='%s' not numeric", transferIdIn.c_str());
            AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
            active_ = false;
            bulk_.reset();
            free(msg.end.msgId);
            free(msg.transferId);
            return;
        }
        xferId = static_cast<uint8_t>(ul);
    }

    auto er = bulk_.onEnd(xferId, msg.end.totalChunks);

    if (er.status == AstrOsBulkTransport::EndResult::Status::OK)
    {
        // Phase 3: no streaming SHA, so we echo the server's stated hash back
        // as the "computed" value. Phase 4 will replace this with the real
        // mbedtls_sha256_finish output.
        ESP_LOGI(TAG, "FW_TRANSFER_END OK: transferId=%s totalChunks=%u", transferIdIn.c_str(),
                 (unsigned)msg.end.totalChunks);
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "OK", finalShaIn);
    }
    else
    {
        ESP_LOGW(TAG, "FW_TRANSFER_END IO_ERROR reason=%d (transferId=%s)", static_cast<int>(er.reason),
                 transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
    }

    active_ = false;
    transferIdStr_.clear();
    beginMsgId_.clear();
    bulk_.reset();

    free(msg.end.msgId);
    free(msg.transferId);
}
```

- [ ] **Step 2: Build both boards**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 3: Commit**

```bash
git add lib/OtaReceiver
git commit -m "feat(ota-receiver): handleEnd — BulkReceiver.onEnd + END_ACK echo + reset"
```

---

### Task 8: OtaReceiver::handleDeployBegin

**Files:**
- Modify: `lib/OtaReceiver/src/OtaReceiver.cpp`

Replace the Task 1 stub. Implementation: split `orderList` on RS, build one `astros_fw_deploy_result_t { id, "FAILED", "", "not_implemented" }` per id, emit `FW_DEPLOY_DONE`. Free pointers. Does not touch `active_` or `bulk_` (deploy is its own phase, decoupled from transfer state).

- [ ] **Step 1: Replace `handleDeployBegin`**

```cpp
void OtaReceiver::handleDeployBegin(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
    std::string orderListStr = msg.deploy.orderList ? msg.deploy.orderList : "";

    std::vector<astros_fw_deploy_result_t> results;
    if (orderListStr.empty())
    {
        // Empty order list = server error. Don't send anything (sendFwDeployDone drops on empty).
        ESP_LOGE(TAG, "FW_DEPLOY_BEGIN orderList empty — dropping");
    }
    else
    {
        size_t start = 0;
        while (start < orderListStr.size())
        {
            size_t end = orderListStr.find('\x1E', start);
            std::string id = (end == std::string::npos) ? orderListStr.substr(start) : orderListStr.substr(start, end - start);
            if (!id.empty())
            {
                results.push_back({id, "FAILED", "", "not_implemented"});
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    ESP_LOGI(TAG, "FW_DEPLOY_BEGIN stub: transferId=%s target-count=%zu — all-FAILED not_implemented",
             transferIdIn.c_str(), results.size());

    AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, transferIdIn, results);

    free(msg.deploy.msgId);
    free(msg.deploy.orderList);
    free(msg.transferId);
}
```

- [ ] **Step 2: Build both boards**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 3: Commit**

```bash
git add lib/OtaReceiver
git commit -m "feat(ota-receiver): handleDeployBegin — all-FAILED 'not_implemented' DEPLOY_DONE stub"
```

---

### Task 9: main.cpp — create otaQueue + spawn otaReceiverTask

**Files:**
- Modify: `src/main.cpp`

Wire the new queue and task into boot. Three changes:
1. Declare `otaQueue` static handle alongside the other queues (~line 70).
2. Create it in the queue-init block (~line 247-255).
3. Replace the Task 3 placeholder `nullptr` in `AstrOs_SerialMsgHandler.Init(..., nullptr)` with the real `otaQueue` handle.
4. Spawn `otaReceiverTask` pinned to core 1 with 4 KB stack.

- [ ] **Step 1: Declare the queue handle**

In `src/main.cpp`, find the existing static queue declarations starting around line 70. Add after `serviceQueue`:

```cpp
static QueueHandle_t otaQueue;
```

- [ ] **Step 2: Add the include for OtaReceiver**

Near the top of `src/main.cpp`, with the other `<...>` includes (find e.g. `#include <AnimationController.hpp>`), add:

```cpp
#include <OtaReceiver.hpp>
#include <OtaQueueMessage.h>
```

- [ ] **Step 3: Declare the task entry**

Find the existing forward declarations (around line 171-172, e.g. `void serviceQueueTask(void *arg);`). Add:

```cpp
void otaReceiverTask(void *arg);
```

- [ ] **Step 4: Create the queue**

In the queue-init block (around line 247), after the `serviceQueue = xQueueCreate(...)` line, add:

```cpp
    otaQueue = xQueueCreate(16, sizeof(queue_ota_msg_t));
```

The standard `QUEUE_LENGTH` constant the file uses is 10; we hard-code 16 here to match the server's nominal sender window.

- [ ] **Step 5: Replace the Init nullptr placeholder**

Find the call (line 270 after Task 3):

```cpp
    AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue, /*otaQueue=*/nullptr);
```

Replace with:

```cpp
    AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue, otaQueue);
```

- [ ] **Step 6: Init the receiver and spawn the task**

In the same area where other tasks are spawned (after the `xTaskCreatePinnedToCore(&serviceQueueTask, ...)` block around line 208), add:

```cpp
    AstrOs_OtaReceiver.Init();
    xTaskCreatePinnedToCore(&otaReceiverTask, "ota_receiver_task", 4096, (void *)otaQueue, 6, NULL, 1);
```

Priority 6 matches `serviceQueueTask` — neither is on the hot path.

- [ ] **Step 7: Implement the task body**

At the bottom of `src/main.cpp` (after the other task implementations), add:

```cpp
void otaReceiverTask(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    queue_ota_msg_t msg;

    while (1)
    {
        if (xQueueReceive(queue, &msg, 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "OTA Receiver Stack HWM: %d", highWaterMark);
            }

            AstrOs_OtaReceiver.process(msg);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

Note: `process()` is responsible for freeing every `malloc`'d pointer in the union arm; the task body does not free anything directly.

- [ ] **Step 8: Build both boards**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both build clean.

- [ ] **Step 9: Run the full native test suite**

```bash
pio test -e test
```

Expected: 298 tests pass (294 baseline + 4 from Task 2).

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp
git commit -m "feat(main): create otaQueue + spawn otaReceiverTask on core 1"
```

---

### Task 10: Bench QA + final verification + open PR

**Files:**
- Create or extend: `.docs/qa/ota-master-serial-receive.md`

Phase 3 milestone check per the design doc's QA section: drive a complete 1.2 MB transfer through AstrOs.Server's Firmware view against a real master, watch the wire log, confirm the four expected exchanges. Phase 3 is **declared done** when:
1. All native tests pass (`pio test -e test`).
2. Both boards build clean (`pio run -e metro_s3`, `pio run -e lolin_d32_pro`).
3. `clang-format` clean on changed files (CI gates this; the pre-commit hook also auto-applies).
4. A live 1.2 MB transfer succeeds end-to-end: server reports `transfer complete`, `END_ACK OK` appears in master serial log, `FW_DEPLOY_DONE` all-FAILED `not_implemented` follows immediately, JobLock releases (the next non-OTA write operation succeeds without blocking).

- [ ] **Step 1: Confirm native tests pass**

```bash
pio test -e test
```

Expected: 298 / 298 pass.

- [ ] **Step 2: Confirm both boards build clean**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: no warnings or errors.

- [ ] **Step 3: Flash to the bench master and start serial monitor**

```bash
pio run -e metro_s3 -t upload
pio device monitor -e metro_s3
```

Adjust env to whichever board the bench master is.

- [ ] **Step 4: Drive the transfer**

In a separate AstrOs.Server session:
1. Open the Firmware view.
2. Upload a 1.2 MB `.bin` (any valid firmware file works for Phase 3 — content is discarded).
3. Click `Send to master`.

Watch the master serial monitor.

- [ ] **Step 5: Confirm the expected wire trace**

Expected log lines (paraphrased — timestamps and full payloads will differ):

```
I (...) OtaReceiver: FW_TRANSFER_BEGIN accepted: transferId=42 totalSize=1228800 chunks=300 sha=abc...
I (...) OtaReceiver: (no per-chunk log; 300 ACKs sent to server)
I (...) OtaReceiver: FW_TRANSFER_END OK: transferId=42 totalChunks=300
I (...) OtaReceiver: FW_DEPLOY_BEGIN stub: transferId=42 target-count=N — all-FAILED not_implemented
```

The server-side console should report `transfer complete`, then `deploy failed: not_implemented`. JobLock should release.

If any of the following appear, Phase 3 is **NOT done**:
- `FW_CHUNK_NAK` with reason `CRC` recurring (other than transient retransmits) — indicates a base64 decode or CRC verification bug.
- `OtaReceiver Stack HWM:` warning — task stack too small; bump to 5120 or 6144 and re-test.
- `Malloc failed in FW_*` — likely a per-chunk leak; review the free-on-success path.

- [ ] **Step 6: Capture the QA results**

Create `.docs/qa/ota-master-serial-receive.md` if it doesn't exist (the design doc's Phase 5 task creates the comprehensive version; we seed it here with just the Phase 3 entries):

```markdown
# OTA — Master Serial Receive QA

Manual QA plan for the ESP firmware OTA receive path. Phases referenced match
.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md.

## Phase 3 — Wire-up sink-to-/dev/null

### Preconditions

- Master flashed with `feature/ota-phase3-wire-up` (or merged develop).
- AstrOs.Server `develop` running, pointed at the master serial.
- Any 1.2 MB `.bin` available (content is discarded — Phase 4 will start hashing).

### Steps

1. Open Firmware view in AstrOs.Server.
2. Upload the 1.2 MB `.bin`.
3. Click `Send to master`.

### Expected results

- Master serial log shows: `FW_TRANSFER_BEGIN accepted`, no per-chunk WARN
  lines, `FW_TRANSFER_END OK`, `FW_DEPLOY_BEGIN stub … all-FAILED not_implemented`.
- Server UI shows: transfer complete, then deploy failed (`not_implemented`).
- JobLock releases — a subsequent non-OTA write (e.g. RUN_SCRIPT) succeeds.
- No `Stack HWM` warnings from `otaReceiverTask`.

### Negative paths (Phase 5 will formalize)

- Server sends BEGIN while a transfer is already active → `BEGIN_ACK status=busy`,
  in-flight transfer continues unaffected.
- Server sends a chunk while no transfer is active → `CHUNK_NAK reason=OUT_OF_ORDER
  lastGoodSeq=0 nextExpectedSeq=0`.
- Server sends a malformed base64 payload → `CHUNK_NAK reason=SIZE`.
```

- [ ] **Step 7: Commit the QA notes**

```bash
git add .docs/qa/ota-master-serial-receive.md
git commit -m "docs(qa): seed Phase 3 manual-QA notes for OTA master serial receive"
```

- [ ] **Step 8: Push branch + open PR**

The user runs these in their VS Code terminal (auth-dependent):

```bash
git push -u origin feature/ota-phase3-wire-up
gh pr create --base develop --title "feat(ota): Phase 3 — wire up master serial OTA receive" --body "$(cat <<'EOF'
## Summary
- Stand up `lib/OtaReceiver` (MIXED) wrapping the Phase 2 `BulkReceiver` state machine in a FreeRTOS task that drains a new `otaQueue`.
- Extend `AstrOsSerialMsgHandler` to dispatch the four inbound FW_* types: parse, base64-decode (FW_CHUNK), queue handoff.
- Add five `sendFw*` helpers with the empty-string load-bearing fallback per the design doc.
- `FW_DEPLOY_BEGIN` → `FW_DEPLOY_DONE` all-FAILED `not_implemented` stub.
- Phase 3 sinks payload to /dev/null; SD writer + streaming SHA-256 land in Phase 4.

## Test plan
- [x] `pio test -e test` (298 pass, 4 new for queue_ota_msg_t layout)
- [x] `pio run -e metro_s3` clean
- [x] `pio run -e lolin_d32_pro` clean
- [x] clang-format clean
- [x] AstrOs.Server Firmware view drives a 1.2 MB transfer end-to-end: BEGIN_ACK / N×CHUNK_ACK / END_ACK / DEPLOY_DONE all-FAILED, JobLock releases (`.docs/qa/ota-master-serial-receive.md`)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Expected: PR opened against `develop`.

---

## Summary

Ten tasks, each commit-sized. Eight produce firmware code (one of which adds the small native test for the queue header), one writes manual-QA notes, one opens the PR. Total scope is the file structure table at the top: four new files, three modifications. The whole change set is well under 1000 lines added.

## Test plan

| Layer | Coverage | Verification |
|---|---|---|
| Native (`pio test -e test`) | `queue_ota_msg_t` layout sanity (4 tests) | Task 2 |
| Firmware build (`pio run`) | Both boards compile clean | Tasks 1, 3-9 |
| Firmware behavior | End-to-end 1.2 MB transfer via AstrOs.Server | Task 10 |
| CI native-purity guard | No new PURE-lib regressions | `lib/OtaReceiver` is MIXED, not added to `PURE_LIBS` |
| clang-format | Pre-commit hook + PR validation | Automatic |

Base64 decode correctness is exercised end-to-end in the QA flow (Task 10) — the 1.2 MB `.bin` arriving with its real bytes intact at the receiver is the proof. Decoding bugs would manifest as a CRC mismatch on the very first chunk (the receiver's CRC-16 over the decoded bytes would fail).

## Self-review notes

- **Scope check.** 10 tasks is at the soft scope-guard ceiling in CLAUDE.md, but each task is small (<60 lines of new firmware on average). Splitting into two PRs would create awkward seams (e.g. the handler dispatch in Task 4 doesn't make sense to ship without the receiver implementations in Tasks 5-8). Keep as one PR.
- **Placeholder scan.** No "TBD" / "TODO" / "later" — every step has the actual code or command the engineer needs.
- **Type consistency.** `queue_ota_msg_t` is referenced identically in Tasks 1, 2, 4, 5-8, and 9. `BulkReceiver::begin/onChunk/onEnd` signatures match Phase 2's shipped `.hpp` (verified by reading `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` while drafting). `AstrOs_SerialMsgHandler` global name matches the existing extern (`lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp:12`).
- **Spec coverage.** Every item in the design doc's "Phase 3" deliverable section maps to a task — receiver scaffolding (Task 1), handler dispatch + base64 (Task 4), main.cpp wiring (Task 9), DEPLOY_BEGIN stub (Task 8), empty-string fallback (Task 3, Steps 3-7 and Task 4's NAK paths), QA milestone (Task 10).
- **Open question deliberately deferred.** Whether to remove the Phase 1 `decodeFwInbound` shim now that it's dead code on the master serial path. Recommendation: leave for Phase 4 — it costs nothing to keep, and Phase 5 might reuse it on the mesh path if `OTA_*` ESP-NOW packets reuse the same decoder.
- **Task 10's Step 8** uses `gh pr create` which requires authenticated terminal access. The user runs this in VS Code per the project's command-execution memory.
