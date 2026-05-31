# Firmware OTA Mesh-Forward M3 — Master OtaForwarder (MIXED) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the master-side `OtaForwarder` MIXED lib that consumes M1's wire format + M2's `BulkSender` to drive ESP-NOW traffic from master to padawans. After this milestone, `FW_DEPLOY_BEGIN` from the server triggers actual `OTA_BEGIN` / `OTA_DATA` frames going out the radio.

**Architecture:** New `lib/OtaForwarder/` MIXED lib runs on a dedicated FreeRTOS task (master only, core 1). Drains a new `otaForwarderQueue` carrying decoded ESP-NOW ACK/NAK arrivals plus the serial-side `FW_DEPLOY_BEGIN` trigger plus 50 ms tick events. Per-padawan transfer flow: lookup file → `BulkSender::begin` → emit `OTA_BEGIN` → wait/timeout → stream chunks with tick-driven retransmits → emit `OTA_END` → record result → advance to next padawan. AstrOsEspNow gains a binary-frame TX helper and dispatches OTA ACK/NAK arrivals into the new queue. `OtaReceiver` exposes a `getLastFirmwarePath()` accessor so the forwarder can find the staged `<sha-prefix>.bin` on SD.

**Tech Stack:** C++17, ESP-IDF (esp_now, esp_timer, freertos), PlatformIO. MIXED — no PURE constraints. Builds on M1's `lib_native/AstrOsMessaging` wire format + M2's `lib_native/AstrOsBulkTransport::BulkSender`.

---

## Context for the engineer

Read these first — load-bearing, will not be re-explained per task:

- **Design doc**: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md` — section "M3 — Master OtaForwarder (MIXED) + ESP-NOW binary TX path" is the canonical contract. Sections on "Master side — BulkSender (PURE) + OtaForwarder (MIXED)" and "Data flow (happy path, single padawan)" describe the runtime shape.
- **M2 BulkSender API**: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`. Re-read the `BulkSender` class (lines 614-665), the 7 result types (`BeginSenderResult`, `BeginAckResult`, `SendResult`, `AckResult`, `NakResult`, `TickResult`, `EndAckResult`), and the 5-state `Status` enum. **All result types are `[[nodiscard]]` at the type level** — every result must be observed.
- **M1 wire format**: `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp` — packed structs you'll memcpy into for builder calls. `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp` — `generateOtaPacket(type, payload, len)` is the wire builder. `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` — `parseOtaBeginAck` / `parseOtaBeginNak` / `parseOtaDataAck` / `parseOtaDataNak` / `parseOtaEndAck` produce the decoded records.
- **OtaReceiver as MIXED-lib precedent**: `lib/OtaReceiver/include/OtaReceiver.hpp` + `lib/OtaReceiver/src/OtaReceiver.cpp`. Mirror its discipline: `std::atomic<bool> active_` for polling-pause, `Init(QueueHandle_t)` two-step construction, single-task state invariant, `esp_timer` callbacks that just `xQueueSend` (state mutation runs on the consumer task).
- **Forward concerns #7-14** in the design doc's "Open questions" section — M3 plan resolves all of them; see the dispositions table at the bottom of this Context section.
- **Existing FW_DEPLOY_BEGIN routing**: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp:548-600` currently routes through `otaQueue` to `OtaReceiver::handleDeployBegin` (the Phase 3 all-FAILED stub at `lib/OtaReceiver/src/OtaReceiver.cpp:591-625`). M3 reroutes to `otaForwarderQueue`; the OtaReceiver stub is deleted.
- **Polling-pause precedent**: `src/main.cpp:418-470` — `pollingTimerCallback` gates the master polling branch on `AstrOs_OtaReceiver.isActive()`. M3 extends this to `OtaReceiver::isActive() || OtaForwarder::isActive()`.
- **ESP-NOW peer model**: `lib_native/AstrOsEspNowPeers/include/espnow_peer.h` defines `espnow_peer_t { int id; char name[16]; uint8_t mac_addr[6]; ... }`. Controller IDs in `FW_DEPLOY_BEGIN`'s order list match `peer.name`. `AstrOs_EspNow.getPeers()` (in `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:292`) returns a mutex-protected vector copy that the forwarder linear-scans (`ESPNOW_PEER_LIMIT = 10`, trivially fast).

### Forward-concern dispositions

| # | Concern | M3 disposition |
|---|---|---|
| 7 | MIXED dispatcher residual switch silent-drop OTA cases | Add OTA cases to `AstrOsEspNowService::handleMessage` residual switch. Master: route ACK/NAK to otaForwarderQueue. Padawan: ESP_LOGW drop. **Task 5.** |
| 8 | tick retransmits before nextChunkToSend | Forwarder loop: tick → emit retransmits → drain. **Task 7b.** |
| 9 | status() to disambiguate tick(count=0, abandon=false) | Forwarder skips tick body when `bulk_.status() != STREAMING`. **Task 7b.** |
| 10 | NAK below previously-ACKed watermark | **DEFERRED.** v1 accepts as-is; tick-timeout abandons naturally. Worth a follow-up if production logs show frequent firing. No code change. |
| 11 | newlyConfirmedCount blends explicit + implicit confirmations | Comment in OtaForwarder code; M3 uses `bulk_.status()` transitions, not newlyConfirmedCount accounting. **Task 7b.** |
| 12 | abandon flag check independent of count | Loop guard: `if (tr.abandon) { failPadawan; break; }`. **Task 7b.** |
| 13 | OUT_OF_RANGE distinct logging | OtaForwarder's ACK/NAK response handlers ESP_LOGW the specific Decision. **Task 7b.** |
| 14 | progress counter retransmits vs fresh sends | Comment for M5's progress counter implementer; M3 doesn't emit FW_PROGRESS. **Task 7b.** |

## File structure

**Created:**
- `lib/OtaForwarder/library.json`
- `lib/OtaForwarder/README`
- `lib/OtaForwarder/include/OtaForwarder.hpp` — class declaration, state machine fields
- `lib/OtaForwarder/include/OtaForwarderQueueMessage.h` — `queue_ota_forwarder_msg_t` discriminated union + `freeOtaForwarderMsg()`
- `lib/OtaForwarder/src/OtaForwarder.cpp` — transfer flow implementation

**Modified:**
- `lib/OtaReceiver/include/OtaReceiver.hpp` — add `getLastFirmwarePath()` accessor declaration + private `lastFirmwarePath_` + mutex
- `lib/OtaReceiver/src/OtaReceiver.cpp` — set `lastFirmwarePath_` inside `handleEnd`'s success-rename branch; remove obsolete `handleDeployBegin` (M3 reroutes; OtaReceiver no longer sees `OTA_MSG_DEPLOY_BEGIN`)
- `lib/OtaReceiver/include/OtaQueueMessage.h` — remove `OTA_MSG_DEPLOY_BEGIN` enum value + matching union arm + `freeOtaMsg` case (no consumer remains)
- `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp` — declare `sendOtaFrame(mac, type, payload, len)`; declare `setOtaForwarderQueue(QueueHandle_t)` setter; private member
- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — implement `sendOtaFrame`; OTA cases in residual switch (parse + xQueueSend to otaForwarderQueue on master, ESP_LOGW drop on padawan); setter implementation
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.hpp` — add `otaForwarderQueue_` member + extend `Init` signature
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` — `handleFwDeployBeginInbound` posts to `otaForwarderQueue_` instead of `otaQueue_`
- `src/main.cpp` — create `otaForwarderQueue` (size 16), spawn `otaForwarderTask` (master only, core 1), extend polling-pause gate, plumb queue handles into `AstrOs_SerialMsgHandler.Init` + `AstrOs_EspNow.setOtaForwarderQueue` + `AstrOs_OtaForwarder.Init`
- `test/test_native/astros_ota_forwarder_tests.cpp` (NEW) — native tests for the `getLastFirmwarePath` accessor and (if order-list parsing lives in PURE-testable code) the parser

**Singleton instance**: `extern OtaForwarder AstrOs_OtaForwarder;` declared in the header, defined in the .cpp. Mirrors `AstrOs_OtaReceiver` pattern.

## What does NOT ship in M3

- Padawan-side `OtaWriter` (M4)
- `FW_PROGRESS` emission during streaming (M5)
- Master self-flash from `/sdcard/firmware/<sha-prefix>.bin` (PR set 2)
- `esp_ota_set_boot_partition`, reboot, version-confirmation (PR set 2)
- Multi-firmware tracking — M3 uses single-firmware-at-a-time `OtaReceiver::getLastFirmwarePath()`

---

## Task 1: `OtaReceiver::getLastFirmwarePath()` accessor

Adds a tracked field on `OtaReceiver` that records the most recently successfully-renamed firmware path. `OtaForwarder` queries this on `FW_DEPLOY_BEGIN` to find the staged `<sha-prefix>.bin`. Single-firmware-at-a-time assumption — typical case is server uploads, then immediately deploys.

**Files:**
- Modify: `lib/OtaReceiver/include/OtaReceiver.hpp`
- Modify: `lib/OtaReceiver/src/OtaReceiver.cpp`
- Test: `test/test_native/astros_ota_forwarder_tests.cpp` (new file — will accumulate M3-related native tests)

- [ ] **Step 1: Write the failing test**

Create `test/test_native/astros_ota_forwarder_tests.cpp`:

```cpp
#include <gmock/gmock.h>
#include <gtest/gtest.h>

// The OtaReceiver accessor is a small, native-testable surface despite the
// rest of OtaReceiver being MIXED. The native tests cover the accessor's
// thread-safe getter/setter shape via a minimal stand-in class that mirrors
// the production discipline (mutex-protected std::string).
//
// Why a stand-in instead of testing OtaReceiver directly: OtaReceiver pulls
// in ESP-IDF / FreeRTOS / mbedtls headers and cannot link in [env:test].
// The accessor's logic is small enough that mirroring it under a native
// stand-in catches the same kinds of regressions (mutex discipline,
// copy-vs-move semantics, empty-by-default).

#include <mutex>
#include <optional>
#include <string>

namespace
{
    // Mirror of OtaReceiver's accessor surface. Production-side test is
    // bench-only (see Task 8). This test pins the accessor's contract.
    class LastFirmwarePathHolder
    {
    public:
        std::optional<std::string> get() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (path_.empty())
            {
                return std::nullopt;
            }
            return path_;
        }

        void set(const std::string &path)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_ = path;
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_.clear();
        }

    private:
        mutable std::mutex mutex_;
        std::string path_;
    };
} // namespace

TEST(LastFirmwarePathHolder, EmptyByDefault)
{
    LastFirmwarePathHolder holder;
    EXPECT_EQ(std::nullopt, holder.get());
}

TEST(LastFirmwarePathHolder, ReturnsSetValue)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/abcd1234.bin");

    auto got = holder.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("/sdcard/firmware/abcd1234.bin", *got);
}

TEST(LastFirmwarePathHolder, OverwriteReturnsLatest)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/first.bin");
    holder.set("/sdcard/firmware/second.bin");

    auto got = holder.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("/sdcard/firmware/second.bin", *got);
}

TEST(LastFirmwarePathHolder, ClearReturnsEmpty)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/abcd1234.bin");
    holder.clear();
    EXPECT_EQ(std::nullopt, holder.get());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -10`
Expected: 4 new `LastFirmwarePathHolder.*` tests fail to find the type (compile error on the missing `LastFirmwarePathHolder`).

Actually wait — the stand-in is defined inline in the test file. The tests should COMPILE and PASS immediately (the test file defines the type itself). The point of T1's native tests is to pin the contract that the production OtaReceiver accessor must mirror.

So this is "the test passes because the stand-in implements the contract; if a future refactor changes the contract, both stand-in and production fall out of sync — bench QA catches the production side."

Skip the red→green TDD cycle here; this is contract-pinning. Move directly to running the full suite to confirm the new test file compiles cleanly.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 431/431 pass (427 baseline + 4 new LastFirmwarePathHolder tests).

- [ ] **Step 3: Add the production-side accessor declaration to OtaReceiver.hpp**

In `lib/OtaReceiver/include/OtaReceiver.hpp`, add includes (after the existing includes):

```cpp
#include <mutex>
#include <optional>
```

Add the public accessor declaration to the `OtaReceiver` class (after `isActive()` at around line 81, before the `private:` keyword):

```cpp
    // Returns the path of the most recently successfully-renamed firmware,
    // or std::nullopt if no firmware has been received yet. Set by
    // handleEnd's success-rename branch. Thread-safe; OtaForwarder
    // (different task on same core) reads this on FW_DEPLOY_BEGIN to
    // locate the staged .bin file.
    //
    // Single-firmware-at-a-time assumption: the typical server flow is
    // "upload, then immediately deploy". Multi-firmware tracking (by
    // transferId or sha) is a future-milestone concern.
    std::optional<std::string> getLastFirmwarePath() const;
```

Add the private state alongside the other private members (around line 56, after the `shaActive_` field):

```cpp
    // Set inside handleEnd's success-rename branch. Read by
    // getLastFirmwarePath() under lastFirmwareMutex_.
    mutable std::mutex lastFirmwareMutex_;
    std::string lastFirmwarePath_;
```

- [ ] **Step 4: Implement the accessor in OtaReceiver.cpp**

Add at the end of `lib/OtaReceiver/src/OtaReceiver.cpp` (before the closing scope, after the existing destructor or last method):

```cpp
std::optional<std::string> OtaReceiver::getLastFirmwarePath() const
{
    std::lock_guard<std::mutex> lock(lastFirmwareMutex_);
    if (lastFirmwarePath_.empty())
    {
        return std::nullopt;
    }
    return lastFirmwarePath_;
}
```

Locate the success-rename branch inside `handleEnd` (search for the `rename(` or `unlink(... finalPath)` block — the existing success path that builds `finalPath` and renames `staging.bin` to it). Inside that branch, immediately after the successful `rename` returns 0 and before the `sendFwTransferEndAck(..., "OK", ...)` call, add:

```cpp
            {
                std::lock_guard<std::mutex> lock(lastFirmwareMutex_);
                lastFirmwarePath_ = finalPath;
            }
```

(The exact variable name may be slightly different — match whatever the surrounding code uses to hold the renamed path.)

- [ ] **Step 5: Verify the change builds on both boards**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 431/431 pass.

- [ ] **Step 6: Commit**

```bash
git add lib/OtaReceiver/include/OtaReceiver.hpp \
        lib/OtaReceiver/src/OtaReceiver.cpp \
        test/test_native/astros_ota_forwarder_tests.cpp
git commit -m "feat(ota-receiver): add getLastFirmwarePath() accessor for M3 forwarder"
```

---

## Task 2: `queue_ota_forwarder_msg_t` discriminated union + `freeOtaForwarderMsg`

Defines the message types `otaForwarderQueue` carries: serial-side `FW_DEPLOY_BEGIN`, 5 ESP-NOW ACK/NAK arrivals, and the 50 ms tick. Mirrors the existing `queue_ota_msg_t` discriminated-union pattern in `lib/OtaReceiver/include/OtaQueueMessage.h`.

**Files:**
- Create: `lib/OtaForwarder/include/OtaForwarderQueueMessage.h`
- Modify: `test/test_native/astros_ota_forwarder_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/test_native/astros_ota_forwarder_tests.cpp`:

```cpp
#include "../../lib/OtaForwarder/include/OtaForwarderQueueMessage.h"

#include <cstring>

// Free-helper contract: producer mallocs; consumer (or producer on send
// failure) calls freeOtaForwarderMsg. Pointers nulled after free so an
// accidental double-free is a no-op.

TEST(OtaForwarderMsg, FreeDeployBeginReleasesAllOwnedPointers)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = strdup("xfer-7");
    m.deploy.msgId = strdup("msg-3");
    m.deploy.orderList = strdup("body\x1Ecore\x1Edome");

    ASSERT_NE(nullptr, m.transferId);
    ASSERT_NE(nullptr, m.deploy.msgId);
    ASSERT_NE(nullptr, m.deploy.orderList);

    freeOtaForwarderMsg(&m);

    EXPECT_EQ(nullptr, m.transferId);
    EXPECT_EQ(nullptr, m.deploy.msgId);
    EXPECT_EQ(nullptr, m.deploy.orderList);
}

TEST(OtaForwarderMsg, FreeAckNakKindsAreNoOpForInlineFields)
{
    // The ACK/NAK kinds carry only inline fields (xferId, seqs, etc.).
    // freeOtaForwarderMsg should be safe on them (transferId is unused
    // for these kinds and stays nullptr).
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DATA_ACK;
    m.data_ack.xferId = 7;
    m.data_ack.highestContiguousSeq = 100;
    m.data_ack.nextExpectedSeq = 101;
    m.data_ack.windowRemaining = 7;

    freeOtaForwarderMsg(&m); // must not crash, must not free random memory
    // Fields stay set — only the malloc'd pointer arms get zeroed.
    EXPECT_EQ(7, m.data_ack.xferId);
}

TEST(OtaForwarderMsg, FreeTickIsNoOp)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_TICK;
    freeOtaForwarderMsg(&m); // must not crash; no allocated members
}

TEST(OtaForwarderMsg, FreeNullPtrIsNoOp)
{
    freeOtaForwarderMsg(nullptr); // must not crash
}

TEST(OtaForwarderMsg, FreeIsIdempotent)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = strdup("x");
    m.deploy.msgId = strdup("m");
    m.deploy.orderList = strdup("o");

    freeOtaForwarderMsg(&m); // first free
    freeOtaForwarderMsg(&m); // second free — no-op because pointers nulled
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: compile error — `OtaForwarderQueueMessage.h` not found / `queue_ota_forwarder_msg_t` undefined.

- [ ] **Step 3: Create the queue message header**

Create `lib/OtaForwarder/include/OtaForwarderQueueMessage.h`:

```c
#ifndef OTAFORWARDERQUEUEMESSAGE_H
#define OTAFORWARDERQUEUEMESSAGE_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Discriminated union for otaForwarderQueue.
    //
    // Memory ownership: producer mallocs every pointer for the kind it sends
    // and on xQueueSend failure calls freeOtaForwarderMsg() to release its
    // own allocations. Consumer (otaForwarderTask) calls freeOtaForwarderMsg()
    // after dispatching to the matching handler. Mixing kinds across the
    // free path will leak or double-free.
    //
    // ACK/NAK kinds carry their decoded record fields inline — no pointers,
    // no malloc on the hot path. DEPLOY_BEGIN carries three malloc'd strings
    // (transferId, msgId, orderList). TICK carries nothing.
    //
    // sha256Computed (END_ACK only) is a 32-byte inline buffer; the binary
    // ESP-NOW frame already carries it byte-for-byte and the consumer
    // typically just logs it — no malloc needed.
    //
    // srcMac (ACK/NAK kinds) is a 6-byte inline buffer. Used for forensic
    // logging and to cross-check against the current padawan's MAC.

    typedef enum
    {
        OTA_FWD_DEPLOY_BEGIN = 0, // from AstrOsSerialMsgHandler (serial side)
        OTA_FWD_BEGIN_ACK = 1,    // from AstrOsEspNow (master role)
        OTA_FWD_BEGIN_NAK = 2,
        OTA_FWD_DATA_ACK = 3,
        OTA_FWD_DATA_NAK = 4,
        OTA_FWD_END_ACK = 5,
        OTA_FWD_TICK = 6          // 50 ms tick from esp_timer
    } ota_forwarder_msg_kind_t;

    typedef struct
    {
        ota_forwarder_msg_kind_t kind;

        // Only populated for OTA_FWD_DEPLOY_BEGIN (malloc'd). Other kinds
        // leave it nullptr. Wire-level opaque string from the originating
        // FW_TRANSFER_BEGIN.
        char *transferId;

        union
        {
            struct
            {
                char *msgId;     // DEPLOY_BEGIN's msgId for FW_DEPLOY_DONE echo
                char *orderList; // RS-separated controller-id list
            } deploy;

            // ACK/NAK arrivals carry the decoded record fields inline.
            // Producer (AstrOsEspNow RX callback) parses the frame via M1's
            // parseOta*Ack/Nak free functions, copies the fields here,
            // posts to the queue.
            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
            } begin_ack;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint8_t reason; // OtaBeginNakReason value
            } begin_nak;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t highestContiguousSeq;
                uint32_t nextExpectedSeq;
                uint8_t windowRemaining;
            } data_ack;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t highestContiguousSeq;
                uint32_t nextExpectedSeq;
                uint8_t windowRemaining;
                uint8_t reason; // OtaDataNakReason value
            } data_nak;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint8_t status; // OtaEndStatus value
                uint8_t sha256Computed[32];
            } end_ack;
            // OTA_FWD_TICK has no union arm.
        };
    } queue_ota_forwarder_msg_t;

    // Sole implementation of the per-kind free contract above. Producer and
    // consumer both call this. Freed pointers are nulled so accidental
    // double-frees become no-ops.
    static inline void freeOtaForwarderMsg(queue_ota_forwarder_msg_t *m)
    {
        if (m == NULL)
        {
            return;
        }
        if (m->kind == OTA_FWD_DEPLOY_BEGIN)
        {
            free(m->deploy.msgId);
            free(m->deploy.orderList);
            m->deploy.msgId = NULL;
            m->deploy.orderList = NULL;
        }
        // ACK/NAK kinds and TICK have no malloc'd union arm members —
        // nothing to free beyond transferId below.
        free(m->transferId);
        m->transferId = NULL;
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -10`
Expected: 5 new `OtaForwarderMsg.*` tests pass.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 436/436 (431 prior + 5 new).

- [ ] **Step 5: Commit**

```bash
git add lib/OtaForwarder/include/OtaForwarderQueueMessage.h \
        test/test_native/astros_ota_forwarder_tests.cpp
git commit -m "feat(ota-forwarder): add queue_ota_forwarder_msg_t + freeOtaForwarderMsg"
```

---

## Task 3: `OtaForwarder` skeleton + `main.cpp` wiring + polling-pause gate

Lay down the lib structure and process-lifetime infrastructure: class declaration with the state machine fields, `Init(QueueHandle_t)`, `process(msg)` dispatcher (logs unimplemented for each kind for now), `active_` atomic + `isActive()`, singleton instance. Plus `main.cpp` creates the queue, spawns the task on master only, extends the polling-pause gate. After this task the firmware compiles and runs but `FW_DEPLOY_BEGIN` still routes to the existing OtaReceiver stub (Task 6 reroutes it).

**Files:**
- Create: `lib/OtaForwarder/library.json`
- Create: `lib/OtaForwarder/README`
- Create: `lib/OtaForwarder/include/OtaForwarder.hpp`
- Create: `lib/OtaForwarder/src/OtaForwarder.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Create the lib metadata files**

Create `lib/OtaForwarder/library.json` (minimal — PlatformIO uses this to identify the lib):

```json
{
    "name": "OtaForwarder",
    "version": "0.0.1",
    "description": "Master-side OTA forwarder: drives ESP-NOW chunk transfer to padawans"
}
```

Create `lib/OtaForwarder/README`:

```
# OtaForwarder (MIXED)

Master-only OTA forwarder for the mesh-forward feature (PR set 1 of the
firmware OTA decomposition). Consumes:

- `lib_native/AstrOsBulkTransport::BulkSender` (PURE state machine, M2)
- `lib_native/AstrOsMessaging` wire-format builders/parsers (M1)
- `lib/OtaReceiver::getLastFirmwarePath()` to locate the staged firmware
- `lib/AstrOsEspNow::sendOtaFrame()` for binary-frame TX

Runs on a dedicated FreeRTOS task pinned to core 1. Master only — gated
on `isMasterNode` at task spawn in `src/main.cpp`.

See `.docs/plans/20260524-0912-firmware-ota-mesh-forward-m3-ota-forwarder.md`
for the M3 plan; `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md`
for the cross-milestone design.
```

- [ ] **Step 2: Create the class header**

Create `lib/OtaForwarder/include/OtaForwarder.hpp`:

```cpp
#ifndef OTAFORWARDER_HPP
#define OTAFORWARDER_HPP

#include <AstrOsBulkTransport.hpp>
#include <OtaForwarderQueueMessage.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_timer.h>

// Threading: all members are accessed only from otaForwarderTask via
// `process(msg)`. The one exception is `active_` (atomic; read from the
// pollingTimer's esp_timer dispatch task for polling-pause gating).
//
// The tick timer fires from esp_timer's dispatch task, but its callback
// only does xQueueSend(OTA_FWD_TICK) — state mutation runs on
// otaForwarderTask, preserving the single-task-state invariant.

// One row in the per-padawan results vector. Mirrors astros_fw_deploy_result_t
// in the serial messaging service.
struct OtaForwarderPadawanResult
{
    std::string controllerId;
    std::string status;       // "OK" or "FAILED"
    std::string finalVersion; // empty until version-confirmation lands (PR set 2)
    std::string errorOrEmpty; // failure reason ("begin_ack_timeout", etc.)
};

class OtaForwarder
{
public:
    OtaForwarder();
    ~OtaForwarder();

    OtaForwarder(const OtaForwarder &) = delete;
    OtaForwarder &operator=(const OtaForwarder &) = delete;
    OtaForwarder(OtaForwarder &&) = delete;
    OtaForwarder &operator=(OtaForwarder &&) = delete;

    // Two-step construction (mirror OtaReceiver): the singleton can be
    // built at static-init time, FreeRTOS queues attach later.
    void Init(QueueHandle_t otaForwarderQueue);

    // Single entry point from otaForwarderTask.
    void process(queue_ota_forwarder_msg_t &msg);

    // Safe to call from any task. Gates polling work during a forward.
    bool isActive() const noexcept
    {
        return active_;
    }

private:
    // State machine (single in-flight transfer; sequential per-padawan).
    enum class Phase : uint8_t
    {
        IDLE = 0,
        AWAITING_BEGIN_ACK = 1, // emitted OTA_BEGIN, waiting on padawan
        STREAMING = 2,
        AWAITING_END_ACK = 3,
        BETWEEN_PADAWANS = 4, // result recorded; pulling next from order list
        DONE = 5              // order list exhausted; FW_DEPLOY_DONE emitted
    };

    // Per-handler entry points. All run on otaForwarderTask.
    void handleDeployBegin(queue_ota_forwarder_msg_t &msg);
    void handleBeginAck(queue_ota_forwarder_msg_t &msg);
    void handleBeginNak(queue_ota_forwarder_msg_t &msg);
    void handleDataAck(queue_ota_forwarder_msg_t &msg);
    void handleDataNak(queue_ota_forwarder_msg_t &msg);
    void handleEndAck(queue_ota_forwarder_msg_t &msg);
    void handleTick();

    // Per-padawan lifecycle helpers.
    void startNextPadawan();                          // open file, BulkSender.begin, emit OTA_BEGIN
    void abortCurrentPadawan(const std::string &reason); // record FAILED, advance
    void completeCurrentPadawan();                    // record OK, close file, advance
    void emitDeployDoneAndReset();                    // FW_DEPLOY_DONE, return to IDLE

    // Wire-emission helpers.
    void emitOtaBeginFrame();
    void emitOtaEndFrame();
    void streamDrain(uint64_t nowMs); // for each SEND from BulkSender::nextChunkToSend, read+emit OTA_DATA

    // Tick timer (50 ms cadence — fast enough to keep latency under the
    // 400 ms ack timeout while keeping CPU overhead negligible). Started
    // when a transfer begins, stopped when DONE/abandoned.
    void tickTimerStart();
    void tickTimerStop();
    static void tickTimerCb(void *arg);

    // BEGIN_ACK / END_ACK deadline timers — separate from tickTimer to
    // avoid polluting tick cadence with longer-lived deadlines.
    void beginAckTimerStart();
    void beginAckTimerStop();
    void endAckTimerStart();
    void endAckTimerStop();
    static void beginAckTimerCb(void *arg);
    static void endAckTimerCb(void *arg);

    // Resolves a controller-id (from FW_DEPLOY_BEGIN's order list) to a
    // MAC. Linear-scans AstrOs_EspNow.getPeers() — small list, cheap.
    // Returns true if found; fills outMac. Returns false if no peer
    // with that name is registered.
    bool resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const;

    // Active gate (read by pollingTimer's task).
    std::atomic<bool> active_{false};

    // Queue handle held so timer callbacks can post tick + deadline events
    // into the same queue otaForwarderTask drains.
    QueueHandle_t otaForwarderQueue_ = nullptr;

    // Tick timer (50 ms periodic). esp_timer_handle_t is opaque; held as a
    // bare pointer per ESP-IDF conventions.
    esp_timer_handle_t tickTimer_ = nullptr;
    esp_timer_handle_t beginAckTimer_ = nullptr;
    esp_timer_handle_t endAckTimer_ = nullptr;

    // Cadence: 50 ms tick, 2 s BEGIN_ACK timeout, 5 s END_ACK timeout
    // (from the frozen contract; see design doc §Timeouts).
    static constexpr uint64_t kTickPeriodUs = 50ULL * 1000ULL;
    static constexpr uint64_t kBeginAckTimeoutUs = 2ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kEndAckTimeoutUs = 5ULL * 1000ULL * 1000ULL;

    // BulkSender params (from the frozen contract).
    static constexpr uint16_t kChunkSize = 128;
    static constexpr uint8_t kWindowSize = 8;
    static constexpr uint32_t kAckTimeoutMs = 400;
    static constexpr uint8_t kMaxRetries = 3;

    // Per-deploy state.
    AstrOsBulkTransport::BulkSender bulk_;
    Phase phase_ = Phase::IDLE;

    // Deploy-scope state (lives across all padawans of one
    // FW_DEPLOY_BEGIN).
    std::string deployMsgId_;
    std::string deployTransferId_;
    std::vector<std::string> orderList_;
    size_t nextOrderIdx_ = 0;
    std::vector<OtaForwarderPadawanResult> results_;

    // Per-padawan state (lives across one padawan's transfer; reset on
    // advance).
    std::string currentControllerId_;
    uint8_t currentPadawanMac_[6] = {0};
    uint8_t currentXferId_ = 0;
    FILE *firmwareFile_ = nullptr;
    uint32_t firmwareTotalSize_ = 0;
    uint32_t firmwareTotalChunks_ = 0;
    uint8_t firmwareSha256_[32] = {0};
};

extern OtaForwarder AstrOs_OtaForwarder;

#endif
```

- [ ] **Step 3: Create the skeleton implementation**

Create `lib/OtaForwarder/src/OtaForwarder.cpp` with the skeleton (Task 7a/7b will fill in the transfer flow):

```cpp
#include "OtaForwarder.hpp"

#include <AstrOsSerialMsgHandler.hpp>
#include <esp_log.h>

namespace
{
constexpr const char *TAG = "OtaForwarder";
} // namespace

OtaForwarder AstrOs_OtaForwarder;

OtaForwarder::OtaForwarder() = default;
OtaForwarder::~OtaForwarder() = default;

void OtaForwarder::Init(QueueHandle_t otaForwarderQueue)
{
    otaForwarderQueue_ = otaForwarderQueue;

    // Create timers eagerly; start/stop them per transfer.
    esp_timer_create_args_t tickArgs = {
        .callback = &OtaForwarder::tickTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_tick",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tickArgs, &tickTimer_));

    esp_timer_create_args_t beginArgs = {
        .callback = &OtaForwarder::beginAckTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_begin_ack",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&beginArgs, &beginAckTimer_));

    esp_timer_create_args_t endArgs = {
        .callback = &OtaForwarder::endAckTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_end_ack",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&endArgs, &endAckTimer_));
}

void OtaForwarder::process(queue_ota_forwarder_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_FWD_DEPLOY_BEGIN:
        handleDeployBegin(msg);
        break;
    case OTA_FWD_BEGIN_ACK:
        handleBeginAck(msg);
        break;
    case OTA_FWD_BEGIN_NAK:
        handleBeginNak(msg);
        break;
    case OTA_FWD_DATA_ACK:
        handleDataAck(msg);
        break;
    case OTA_FWD_DATA_NAK:
        handleDataNak(msg);
        break;
    case OTA_FWD_END_ACK:
        handleEndAck(msg);
        break;
    case OTA_FWD_TICK:
        handleTick();
        break;
    default:
        ESP_LOGE(TAG, "process: unknown kind %d", (int)msg.kind);
        break;
    }
    freeOtaForwarderMsg(&msg);
}

// Task 7a/7b fill in the bodies. Stubs for now so the firmware builds.
void OtaForwarder::handleDeployBegin(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleDeployBegin: stubbed (Task 7a)");
    (void)msg;
}
void OtaForwarder::handleBeginAck(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBeginAck: stubbed (Task 7a)");
    (void)msg;
}
void OtaForwarder::handleBeginNak(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBeginNak: stubbed (Task 7a)");
    (void)msg;
}
void OtaForwarder::handleDataAck(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleDataAck: stubbed (Task 7b)");
    (void)msg;
}
void OtaForwarder::handleDataNak(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleDataNak: stubbed (Task 7b)");
    (void)msg;
}
void OtaForwarder::handleEndAck(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleEndAck: stubbed (Task 7b)");
    (void)msg;
}
void OtaForwarder::handleTick()
{
    // Stub for Task 7b; deliberately silent on the hot path.
}

void OtaForwarder::startNextPadawan()
{
    ESP_LOGW(TAG, "startNextPadawan: stubbed (Task 7a)");
}
void OtaForwarder::abortCurrentPadawan(const std::string &reason)
{
    ESP_LOGW(TAG, "abortCurrentPadawan(%s): stubbed (Task 7a)", reason.c_str());
}
void OtaForwarder::completeCurrentPadawan()
{
    ESP_LOGW(TAG, "completeCurrentPadawan: stubbed (Task 7b)");
}
void OtaForwarder::emitDeployDoneAndReset()
{
    ESP_LOGW(TAG, "emitDeployDoneAndReset: stubbed (Task 7a)");
}

void OtaForwarder::emitOtaBeginFrame()
{
    ESP_LOGW(TAG, "emitOtaBeginFrame: stubbed (Task 7a)");
}
void OtaForwarder::emitOtaEndFrame()
{
    ESP_LOGW(TAG, "emitOtaEndFrame: stubbed (Task 7b)");
}
void OtaForwarder::streamDrain(uint64_t nowMs)
{
    (void)nowMs;
}

void OtaForwarder::tickTimerStart()
{
    if (tickTimer_)
    {
        esp_timer_start_periodic(tickTimer_, kTickPeriodUs);
    }
}
void OtaForwarder::tickTimerStop()
{
    if (tickTimer_)
    {
        esp_timer_stop(tickTimer_);
    }
}
void OtaForwarder::beginAckTimerStart()
{
    if (beginAckTimer_)
    {
        esp_timer_start_once(beginAckTimer_, kBeginAckTimeoutUs);
    }
}
void OtaForwarder::beginAckTimerStop()
{
    if (beginAckTimer_)
    {
        esp_timer_stop(beginAckTimer_);
    }
}
void OtaForwarder::endAckTimerStart()
{
    if (endAckTimer_)
    {
        esp_timer_start_once(endAckTimer_, kEndAckTimeoutUs);
    }
}
void OtaForwarder::endAckTimerStop()
{
    if (endAckTimer_)
    {
        esp_timer_stop(endAckTimer_);
    }
}

void OtaForwarder::tickTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_TICK;
    // Best-effort post; if the queue is full the next tick will catch up.
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}

void OtaForwarder::beginAckTimerCb(void *arg)
{
    (void)arg;
    // Stubbed in this skeleton; Task 7a posts a synthetic timeout event
    // by triggering a NAK-equivalent path. For now the timer just fires
    // and the stubbed handlers ignore it.
}

void OtaForwarder::endAckTimerCb(void *arg)
{
    (void)arg;
    // See beginAckTimerCb note. Task 7b implements.
}

bool OtaForwarder::resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const
{
    ESP_LOGW(TAG, "resolveControllerMac(%s): stubbed (Task 7a)", controllerId.c_str());
    (void)outMac;
    return false;
}
```

- [ ] **Step 4: Wire main.cpp**

In `src/main.cpp`, add the include near the other lib includes (find the existing `#include <OtaReceiver.hpp>` line):

```cpp
#include <OtaForwarder.hpp>
```

Add the queue handle alongside `otaQueue` (around line 80):

```cpp
static QueueHandle_t otaForwarderQueue;
```

Add the task forward-declaration alongside `otaReceiverTask` (around line 184):

```cpp
void otaForwarderTask(void *arg);
```

Create the queue at the same place as `otaQueue` is created (search for `otaQueue = xQueueCreate`, around line 265):

```cpp
    otaForwarderQueue = xQueueCreate(16, sizeof(queue_ota_forwarder_msg_t));
    if (otaForwarderQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create otaForwarderQueue — aborting init");
        return;
    }
```

Init the forwarder singleton near where `AstrOs_OtaReceiver` is initialized (search for `AstrOs_OtaReceiver.Init`):

```cpp
    AstrOs_OtaForwarder.Init(otaForwarderQueue);
```

Spawn the task — master only — at the bottom of the task-spawn block (after the existing `xTaskCreatePinnedToCore(&otaReceiverTask, ...)` around line 222):

```cpp
    if (isMasterNode.load())
    {
        if (xTaskCreatePinnedToCore(&otaForwarderTask, "ota_forwarder_task", 4096,
                                     (void *)otaForwarderQueue, 6, NULL, 1) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create otaForwarderTask — aborting init");
            return;
        }
    }
```

Add the task body somewhere near `otaReceiverTask` (search for `void otaReceiverTask(void *arg)`):

```cpp
void otaForwarderTask(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    queue_ota_forwarder_msg_t msg;

    while (true)
    {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            AstrOs_OtaForwarder.process(msg);
        }

        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        if (hwm < 500)
        {
            ESP_LOGW(TAG, "OTA Forwarder Stack HWM: %u", (unsigned int)hwm);
        }
    }
}
```

Extend the polling-pause gate. Find `pollingTimerCallback` around line 418, locate the line:

```cpp
    const bool otaActive = AstrOs_OtaReceiver.isActive();
```

Change to:

```cpp
    const bool otaActive = AstrOs_OtaReceiver.isActive() || AstrOs_OtaForwarder.isActive();
```

- [ ] **Step 5: Verify builds clean and existing tests pass**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 436/436 (no change from Task 2 — no new native tests in T3).

- [ ] **Step 6: Commit**

```bash
git add lib/OtaForwarder/library.json \
        lib/OtaForwarder/README \
        lib/OtaForwarder/include/OtaForwarder.hpp \
        lib/OtaForwarder/src/OtaForwarder.cpp \
        src/main.cpp
git commit -m "feat(ota-forwarder): skeleton class + main.cpp task wiring + polling-pause"
```

---

## Task 4: `AstrOsEspNow::sendOtaFrame()` binary TX path

The OtaForwarder needs to emit ESP-NOW frames whose payload bytes are the M1 OTA wire format. AstrOsEspNow currently has string-based builders (REGISTRATION etc.) but no binary-frame send path. Add one method that wraps `messageService.generateOtaPacket(type, payload, len)` + `esp_now_send(mac, ...)`.

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

- [ ] **Step 1: Declare the method in the header**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`, add a public method declaration to the `AstrOsEspNow` class (after the existing public methods like `getPeers()`):

```cpp
    // Binary-frame TX for OTA. Builds the wire frame via
    // messageService.generateOtaPacket(type, payload, len), unicasts to
    // `mac` via esp_now_send. Returns ESP_OK on successful enqueue;
    // ESP_ERR_INVALID_ARG if the builder rejected (wrong type / oversize);
    // whatever esp_now_send returned otherwise.
    //
    // Memory ownership: the frame buffer is freed in the existing send
    // callback path after the radio confirms delivery (same as existing
    // string-payload sends).
    esp_err_t sendOtaFrame(const uint8_t mac[6], AstrOsPacketType type, const uint8_t *payload, size_t len);
```

- [ ] **Step 2: Implement it**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, add the method definition near the other `send*` implementations (e.g., after `sendRegistrationRequest` or `sendPoll`):

```cpp
esp_err_t AstrOsEspNow::sendOtaFrame(const uint8_t mac[6], AstrOsPacketType type, const uint8_t *payload, size_t len)
{
    auto frames = this->messageService.generateOtaPacket(type, payload, len);
    if (frames.empty())
    {
        ESP_LOGE(TAG, "sendOtaFrame: generateOtaPacket rejected type=%d len=%zu", (int)type, len);
        return ESP_ERR_INVALID_ARG;
    }
    if (frames.size() != 1)
    {
        // generateOtaPacket is documented to return 0 or 1 entries (OTA
        // frames always fit one ESP-NOW transmission). Defensive only.
        ESP_LOGE(TAG, "sendOtaFrame: unexpected frame count %zu", frames.size());
        for (auto &f : frames)
        {
            free(f.data);
        }
        return ESP_ERR_INVALID_STATE;
    }

    astros_espnow_data_t frame = frames[0];
    esp_err_t err = esp_now_send(mac, frame.data, frame.size);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "sendOtaFrame: esp_now_send returned %s for type=%d", esp_err_to_name(err), (int)type);
        free(frame.data);
    }
    // On ESP_OK the existing send-completion callback (espnowSendCallback)
    // path takes ownership of frame.data and frees it. Mirrors the
    // existing send pattern for string-payload sends.
    return err;
}
```

Wait — the existing send pattern needs verification. Let me re-check before committing.

- [ ] **Step 3: Verify the existing send-callback ownership pattern**

Read the existing `esp_now_send` call sites (search for `esp_now_send` in `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`). Find one of the string-payload sends (e.g., `sendRegistrationRequest` at the `astros_espnow_data_t data = ... esp_now_send(destMac, data.data, data.size); free(data.data);` pattern around line 413).

If the existing pattern frees `data.data` IMMEDIATELY after `esp_now_send` returns (regardless of completion), the new `sendOtaFrame` should do the same. The send-completion callback is for status notification, not buffer lifetime.

In that case, modify the new method body's success path:

```cpp
    esp_err_t err = esp_now_send(mac, frame.data, frame.size);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "sendOtaFrame: esp_now_send returned %s for type=%d", esp_err_to_name(err), (int)type);
    }
    free(frame.data); // free regardless of err — esp_now copies into its own buffer
    return err;
```

Confirm by reading lines 410-420 of `AstrOsEspNowService.cpp` and matching whatever pattern is already there. If immediate-free is the pattern, use the simplified body above.

- [ ] **Step 4: Build both boards**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add lib/AstrOsEspNow/src/AstrOsEspNowService.hpp \
        lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
git commit -m "feat(espnow): add sendOtaFrame binary-frame TX helper"
```

---

## Task 5: `AstrOsEspNow` residual-switch OTA dispatch + queue handle plumbing

When an ESP-NOW OTA ACK/NAK arrives, the dispatcher currently returns `HandlerStatus::UnsupportedType` and falls through to the residual switch, which emits an `ESP_LOGE` "no residual handler exists". This task: add explicit OTA cases to the residual switch. Master role: parse via M1's `parseOta*` free functions, fill a `queue_ota_forwarder_msg_t`, post to `otaForwarderQueue`. Padawan role: `ESP_LOGW` + drop (M4 wires the writer queue).

Closes forward-concern #7. Also closes #13: each rejection path's ESP_LOGW names the parsed-record `valid` state, so a malformed peer ACK is distinguishable from a non-OTA cause in logs.

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Declare the queue setter**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`, add a public method and a private member:

```cpp
public:
    // ... existing methods ...

    // OTA ACK/NAK arrivals on the master are routed into this queue.
    // Called from main.cpp during init; before this is set, OTA arrivals
    // on master fall through to ESP_LOGW + drop (same path as padawan).
    void setOtaForwarderQueue(QueueHandle_t q);
```

Add the private member (alongside other private state):

```cpp
private:
    // ... existing private state ...

    QueueHandle_t otaForwarderQueue_ = nullptr;
```

- [ ] **Step 2: Implement the setter**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, add:

```cpp
void AstrOsEspNow::setOtaForwarderQueue(QueueHandle_t q)
{
    this->otaForwarderQueue_ = q;
}
```

- [ ] **Step 3: Extend the residual switch with OTA cases**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, find `handleMessage`'s residual switch (around line 381 — the second switch in the function, after the `UnsupportedType` fall-through). The current default case ends with:

```cpp
    default:
        ESP_LOGE(TAG, "Dispatcher returned UnsupportedType for packet type %d but no residual handler exists",
                 (int)packet.packetType);
        return false;
    }
```

Add the includes at the top of the file if not already present:

```cpp
#include <AstrOsEspNowProtocol.hpp> // for parseOta* free functions
#include <OtaForwarderQueueMessage.h>
#include <cstring>
```

Add OTA cases BEFORE the `default:` arm:

```cpp
    case AstrOsPacketType::OTA_BEGIN_ACK:
    case AstrOsPacketType::OTA_BEGIN_NAK:
    case AstrOsPacketType::OTA_DATA_ACK:
    case AstrOsPacketType::OTA_DATA_NAK:
    case AstrOsPacketType::OTA_END_ACK:
        return this->routeOtaAckNakToForwarder(src, packet);

    case AstrOsPacketType::OTA_BEGIN:
    case AstrOsPacketType::OTA_DATA:
    case AstrOsPacketType::OTA_END:
        // Master receives these only by mistake (wrong role per the
        // protocol contract); padawan receives them legitimately but the
        // M4 OtaWriter queue isn't wired yet. Drop with a warning so
        // misrouted-master cases are distinguishable from missing-M4.
        ESP_LOGW(TAG, "OTA master→padawan packet type=%d received on %s; dropping (M4 not wired)",
                 (int)packet.packetType, this->isMasterNode ? "master" : "padawan");
        return true;
```

Add the `routeOtaAckNakToForwarder` method declaration to the header (private):

```cpp
private:
    // Routes an OTA ACK/NAK packet (master-side receive) into
    // otaForwarderQueue_. Parses via M1's parseOta* free functions to
    // distinguish wire-malformed (Decision::OK == false on the record)
    // from wire-valid-but-queue-full failures. Returns true on success;
    // false logs and drops.
    bool routeOtaAckNakToForwarder(const uint8_t *src, const astros_packet_t &packet);
```

Implement in the .cpp:

```cpp
bool AstrOsEspNow::routeOtaAckNakToForwarder(const uint8_t *src, const astros_packet_t &packet)
{
    if (!this->isMasterNode)
    {
        ESP_LOGW(TAG, "OTA padawan→master packet type=%d received on padawan; dropping",
                 (int)packet.packetType);
        return true;
    }
    if (this->otaForwarderQueue_ == nullptr)
    {
        ESP_LOGW(TAG, "OTA ACK/NAK type=%d received before otaForwarderQueue_ set; dropping",
                 (int)packet.packetType);
        return true;
    }

    queue_ota_forwarder_msg_t m{};

    switch (packet.packetType)
    {
    case AstrOsPacketType::OTA_BEGIN_ACK:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaBeginAck(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_BEGIN_ACK parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_FWD_BEGIN_ACK;
        std::memcpy(m.begin_ack.srcMac, src, 6);
        m.begin_ack.xferId = rec.xferId;
        break;
    }
    case AstrOsPacketType::OTA_BEGIN_NAK:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaBeginNak(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_BEGIN_NAK parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_FWD_BEGIN_NAK;
        std::memcpy(m.begin_nak.srcMac, src, 6);
        m.begin_nak.xferId = rec.xferId;
        m.begin_nak.reason = static_cast<uint8_t>(rec.reason);
        break;
    }
    case AstrOsPacketType::OTA_DATA_ACK:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaDataAck(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_DATA_ACK parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_FWD_DATA_ACK;
        std::memcpy(m.data_ack.srcMac, src, 6);
        m.data_ack.xferId = rec.xferId;
        m.data_ack.highestContiguousSeq = rec.highestContiguousSeq;
        m.data_ack.nextExpectedSeq = rec.nextExpectedSeq;
        m.data_ack.windowRemaining = rec.windowRemaining;
        break;
    }
    case AstrOsPacketType::OTA_DATA_NAK:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaDataNak(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_DATA_NAK parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_FWD_DATA_NAK;
        std::memcpy(m.data_nak.srcMac, src, 6);
        m.data_nak.xferId = rec.xferId;
        m.data_nak.highestContiguousSeq = rec.highestContiguousSeq;
        m.data_nak.nextExpectedSeq = rec.nextExpectedSeq;
        m.data_nak.windowRemaining = rec.windowRemaining;
        m.data_nak.reason = static_cast<uint8_t>(rec.reason);
        break;
    }
    case AstrOsPacketType::OTA_END_ACK:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaEndAck(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_END_ACK parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_FWD_END_ACK;
        std::memcpy(m.end_ack.srcMac, src, 6);
        m.end_ack.xferId = rec.xferId;
        m.end_ack.status = static_cast<uint8_t>(rec.status);
        std::memcpy(m.end_ack.sha256Computed, rec.sha256Computed, 32);
        break;
    }
    default:
        ESP_LOGE(TAG, "routeOtaAckNakToForwarder: unexpected packet type %d", (int)packet.packetType);
        return false;
    }

    if (xQueueSend(this->otaForwarderQueue_, &m, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGE(TAG, "otaForwarderQueue full; dropping OTA ACK/NAK type=%d", (int)packet.packetType);
        freeOtaForwarderMsg(&m);
        return false;
    }
    return true;
}
```

- [ ] **Step 4: Plumb the queue handle in main.cpp**

In `src/main.cpp`, after `AstrOs_OtaForwarder.Init(otaForwarderQueue);`, add:

```cpp
    AstrOs_EspNow.setOtaForwarderQueue(otaForwarderQueue);
```

- [ ] **Step 5: Build both boards**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 436/436 (no native test changes).

- [ ] **Step 6: Commit**

```bash
git add lib/AstrOsEspNow/src/AstrOsEspNowService.hpp \
        lib/AstrOsEspNow/src/AstrOsEspNowService.cpp \
        src/main.cpp
git commit -m "feat(espnow): residual-switch OTA dispatch routes ACK/NAK to forwarder queue"
```

---

## Task 6: Re-route `FW_DEPLOY_BEGIN` from `otaQueue` to `otaForwarderQueue`

Currently `AstrOsSerialMsgHandler::handleFwDeployBeginInbound` posts an `OTA_MSG_DEPLOY_BEGIN` to `otaQueue` (consumed by `OtaReceiver::handleDeployBegin`, the Phase 3 all-FAILED stub). After this task, the same handler posts a `queue_ota_forwarder_msg_t { kind = OTA_FWD_DEPLOY_BEGIN }` to `otaForwarderQueue` instead. `OtaReceiver`'s deploy handler + the obsolete enum value go away.

**Files:**
- Modify: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.hpp`
- Modify: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`
- Modify: `lib/OtaReceiver/include/OtaQueueMessage.h`
- Modify: `lib/OtaReceiver/include/OtaReceiver.hpp`
- Modify: `lib/OtaReceiver/src/OtaReceiver.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add `otaForwarderQueue_` to AstrOsSerialMsgHandler**

In `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.hpp`, find the existing `Init` signature and the existing private queue members. Extend `Init`:

Existing signature (find it):
```cpp
void Init(QueueHandle_t interfaceResponseQueue, QueueHandle_t serialCh1Queue, QueueHandle_t otaQueue);
```

Replace with:
```cpp
void Init(QueueHandle_t interfaceResponseQueue, QueueHandle_t serialCh1Queue, QueueHandle_t otaQueue,
          QueueHandle_t otaForwarderQueue);
```

Add a private member alongside the existing queue handles:
```cpp
QueueHandle_t otaForwarderQueue_ = nullptr;
```

- [ ] **Step 2: Implement the new Init signature**

In `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`, find the existing `Init` body. Add the new parameter at the end and store it:

```cpp
void AstrOsSerialMsgHandler::Init(QueueHandle_t interfaceResponseQueue, QueueHandle_t serialCh1Queue,
                                   QueueHandle_t otaQueue, QueueHandle_t otaForwarderQueue)
{
    this->interfaceResponseQueue = interfaceResponseQueue;
    this->serialCh1Queue = serialCh1Queue;
    this->otaQueue = otaQueue;
    this->otaForwarderQueue_ = otaForwarderQueue;
}
```

(Match whatever the existing field names actually are — `interfaceResponseQueue` may or may not have a trailing underscore; align with the existing style.)

- [ ] **Step 3: Re-route `handleFwDeployBeginInbound`**

In `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`, find `handleFwDeployBeginInbound` (around line 548). Replace its body. The existing code:

```cpp
void AstrOsSerialMsgHandler::handleFwDeployBeginInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwDeployBegin(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN parse rejected payload");
        std::vector<astros_fw_deploy_result_t> empty;
        this->sendFwDeployDone(msgId, /*transferId=*/"", empty);
        return;
    }

    std::string joined;
    for (size_t i = 0; i < rec.orderIds.size(); i++)
    {
        if (i > 0)
        {
            joined += '\x1E';
        }
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
        freeOtaMsg(&m);
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
        freeOtaMsg(&m);
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
        return;
    }
}
```

Replace with the rerouted version (uses `queue_ota_forwarder_msg_t` + `otaForwarderQueue_`):

```cpp
void AstrOsSerialMsgHandler::handleFwDeployBeginInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwDeployBegin(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN parse rejected payload");
        std::vector<astros_fw_deploy_result_t> empty;
        this->sendFwDeployDone(msgId, /*transferId=*/"", empty);
        return;
    }

    std::string joined;
    for (size_t i = 0; i < rec.orderIds.size(); i++)
    {
        if (i > 0)
        {
            joined += '\x1E';
        }
        joined += rec.orderIds[i];
    }

    queue_ota_forwarder_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = dupString(rec.transferId);
    m.deploy.msgId = dupString(msgId);
    m.deploy.orderList = dupString(joined);

    if (m.transferId == nullptr || m.deploy.msgId == nullptr || m.deploy.orderList == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_DEPLOY_BEGIN dispatch");
        freeOtaForwarderMsg(&m);
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
        return;
    }

    if (xQueueSend(this->otaForwarderQueue_, &m, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "otaForwarderQueue full at FW_DEPLOY_BEGIN");
        freeOtaForwarderMsg(&m);
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
        return;
    }
}
```

Add the include at the top of the file (if not already present):

```cpp
#include <OtaForwarderQueueMessage.h>
```

- [ ] **Step 4: Remove obsolete OTA_MSG_DEPLOY_BEGIN handling from OtaReceiver**

In `lib/OtaReceiver/include/OtaQueueMessage.h`, remove `OTA_MSG_DEPLOY_BEGIN = 3` from the enum and the matching union arm. The enum becomes:

```c
typedef enum
{
    OTA_MSG_BEGIN = 0,
    OTA_MSG_CHUNK = 1,
    OTA_MSG_END = 2,
    OTA_MSG_WATCHDOG_FIRE = 3 // renumbered down from 4 since DEPLOY_BEGIN removed
} ota_msg_kind_t;
```

Remove the `deploy` union arm. Remove its `freeOtaMsg` case.

In `lib/OtaReceiver/include/OtaReceiver.hpp`, remove the `handleDeployBegin` method declaration.

In `lib/OtaReceiver/src/OtaReceiver.cpp`, remove the `handleDeployBegin` method definition (lines 591-625 or thereabouts). Find the `process(msg)` dispatcher and remove the `case OTA_MSG_DEPLOY_BEGIN:` arm.

- [ ] **Step 5: Update main.cpp Init call**

In `src/main.cpp`, find the existing call:

```cpp
AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue, otaQueue);
```

Change to:

```cpp
AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue, otaQueue, otaForwarderQueue);
```

Ensure this line is AFTER both `otaQueue` and `otaForwarderQueue` have been created (they're created in the same init block).

- [ ] **Step 6: Build and test**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS. Watch for compile errors on stale references to OTA_MSG_DEPLOY_BEGIN — there should be none after the removal in T6.4.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 436/436. (No native test references the removed enum value; if any do, fix them in this commit.)

- [ ] **Step 7: Commit**

```bash
git add lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.hpp \
        lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp \
        lib/OtaReceiver/include/OtaQueueMessage.h \
        lib/OtaReceiver/include/OtaReceiver.hpp \
        lib/OtaReceiver/src/OtaReceiver.cpp \
        src/main.cpp
git commit -m "refactor(ota): reroute FW_DEPLOY_BEGIN to otaForwarderQueue (drop receiver stub)"
```

---

## Task 7a: OtaForwarder begin-side flow (bench-exercised path)

Implements the part of the forwarder that the M3 bench checkpoint actually runs: receive `FW_DEPLOY_BEGIN`, walk the order list, for each padawan resolve the MAC, open the firmware file, `BulkSender::begin`, emit `OTA_BEGIN`, start the 2 s timeout. On timeout (no padawan responding because M4 doesn't exist yet), record the padawan as FAILED("begin_ack_timeout"), advance, eventually emit FW_DEPLOY_DONE.

This is the load-bearing task for M3's merge bar.

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

- [ ] **Step 1: Add the needed includes**

At the top of `lib/OtaForwarder/src/OtaForwarder.cpp`, add (or confirm):

```cpp
#include <AstrOsConstants.h>
#include <AstrOsEspNow.h> // AstrOs_EspNow singleton + sendOtaFrame + getPeers
#include <AstrOsMessaging.hpp> // generateOtaPacket / OtaWirePayloads / AstrOsPacketType
#include <AstrOsSha256.h>
#include <AstrOsStorageManager.hpp> // if needed for SD path constants
#include <OtaReceiver.hpp>          // AstrOs_OtaReceiver.getLastFirmwarePath()

#include <algorithm>
#include <cstring>
#include <sys/stat.h>
```

- [ ] **Step 2: Implement `resolveControllerMac`**

Replace the skeleton stub:

```cpp
bool OtaForwarder::resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const
{
    // Linear scan over AstrOs_EspNow.getPeers() — list is bounded by
    // ESPNOW_PEER_LIMIT (10), so the cost is negligible. getPeers
    // internally acquires the peer mutex and returns a vector copy, so
    // the read is thread-safe.
    auto peers = AstrOs_EspNow.getPeers();
    for (const auto &p : peers)
    {
        if (controllerId == p.name)
        {
            std::memcpy(outMac, p.mac_addr, 6);
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 3: Implement `handleDeployBegin`**

Replace the skeleton stub:

```cpp
void OtaForwarder::handleDeployBegin(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::IDLE)
    {
        ESP_LOGW(TAG, "handleDeployBegin while not IDLE (phase=%d); rejecting", (int)phase_);
        // Reply all-FAILED so the JobLock releases on the server side.
        std::vector<astros_fw_deploy_result_t> failures;
        // Don't have an order list parsed yet; best we can do is one
        // synthetic FAILED so the server sees something terminal.
        failures.push_back({"unknown", "FAILED", "", "forwarder_busy"});
        std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
        std::string xferId = msg.transferId ? msg.transferId : "";
        AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, xferId, failures);
        return;
    }

    deployMsgId_ = msg.deploy.msgId ? msg.deploy.msgId : "";
    deployTransferId_ = msg.transferId ? msg.transferId : "";

    // Parse the RS-separated order list into a vector.
    orderList_.clear();
    if (msg.deploy.orderList != nullptr)
    {
        std::string raw = msg.deploy.orderList;
        size_t start = 0;
        while (start < raw.size())
        {
            size_t end = raw.find('\x1E', start);
            std::string id =
                (end == std::string::npos) ? raw.substr(start) : raw.substr(start, end - start);
            if (!id.empty())
            {
                orderList_.push_back(id);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    if (orderList_.empty())
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN orderList empty — dropping");
        std::vector<astros_fw_deploy_result_t> empty;
        AstrOs_SerialMsgHandler.sendFwDeployDone(deployMsgId_, deployTransferId_, empty);
        return;
    }

    nextOrderIdx_ = 0;
    results_.clear();
    results_.reserve(orderList_.size());
    active_.store(true);

    ESP_LOGI(TAG, "FW_DEPLOY_BEGIN: transferId=%s targets=%zu", deployTransferId_.c_str(), orderList_.size());

    startNextPadawan();
}
```

- [ ] **Step 4: Implement `startNextPadawan`**

```cpp
void OtaForwarder::startNextPadawan()
{
    // Skip the "master" entry (PR set 2 will self-flash); record as FAILED.
    while (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "master")
    {
        results_.push_back(
            {orderList_[nextOrderIdx_], "FAILED", "", "not_implemented_in_pr_set_1"});
        nextOrderIdx_++;
    }

    if (nextOrderIdx_ >= orderList_.size())
    {
        emitDeployDoneAndReset();
        return;
    }

    currentControllerId_ = orderList_[nextOrderIdx_];

    if (!resolveControllerMac(currentControllerId_, currentPadawanMac_))
    {
        ESP_LOGW(TAG, "Controller %s not registered as a peer; recording FAILED",
                 currentControllerId_.c_str());
        results_.push_back({currentControllerId_, "FAILED", "", "unknown_peer"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    auto firmwarePathOpt = AstrOs_OtaReceiver.getLastFirmwarePath();
    if (!firmwarePathOpt.has_value())
    {
        ESP_LOGW(TAG, "No staged firmware (getLastFirmwarePath empty) — all-FAILED");
        // Apply no_firmware to the remaining targets.
        while (nextOrderIdx_ < orderList_.size())
        {
            results_.push_back({orderList_[nextOrderIdx_], "FAILED", "", "no_firmware"});
            nextOrderIdx_++;
        }
        emitDeployDoneAndReset();
        return;
    }

    const std::string &firmwarePath = *firmwarePathOpt;
    firmwareFile_ = std::fopen(firmwarePath.c_str(), "rb");
    if (firmwareFile_ == nullptr)
    {
        ESP_LOGE(TAG, "fopen(%s) failed", firmwarePath.c_str());
        results_.push_back({currentControllerId_, "FAILED", "", "firmware_open_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    struct stat st;
    if (stat(firmwarePath.c_str(), &st) != 0)
    {
        ESP_LOGE(TAG, "stat(%s) failed", firmwarePath.c_str());
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, "FAILED", "", "firmware_stat_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }
    firmwareTotalSize_ = static_cast<uint32_t>(st.st_size);
    firmwareTotalChunks_ = (firmwareTotalSize_ + kChunkSize - 1) / kChunkSize;

    // Compute SHA-256 of the file (forensic-grade defensive check; the
    // padawan also verifies). This is one-shot at file open; the result
    // ships in the OTA_BEGIN frame's sha256Expected field. For M3 we
    // compute over the whole file at start; M4/M5 may move to streaming
    // hash alongside the chunk read if file-size growth justifies it.
    AstrOsSha256Ctx shaCtx;
    AstrOsSha256_init(&shaCtx);
    uint8_t buf[1024];
    while (size_t r = std::fread(buf, 1, sizeof(buf), firmwareFile_))
    {
        AstrOsSha256_update(&shaCtx, buf, r);
    }
    AstrOsSha256_final(&shaCtx, firmwareSha256_);
    std::rewind(firmwareFile_);

    currentXferId_ = static_cast<uint8_t>(nextOrderIdx_ + 1); // simple monotonic per-padawan xferId

    auto br = bulk_.begin(currentXferId_, firmwareTotalChunks_, kChunkSize, kWindowSize, kAckTimeoutMs,
                          kMaxRetries);
    if (!br.valid)
    {
        ESP_LOGE(TAG, "BulkSender::begin rejected reason=%d", (int)br.reason);
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, "FAILED", "", "begin_rejected"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    ESP_LOGI(TAG, "Starting transfer to %s (xferId=%u, chunks=%u, size=%u)",
             currentControllerId_.c_str(), currentXferId_, firmwareTotalChunks_, firmwareTotalSize_);

    phase_ = Phase::AWAITING_BEGIN_ACK;
    emitOtaBeginFrame();
    beginAckTimerStart();
    tickTimerStart();
}
```

- [ ] **Step 5: Implement `emitOtaBeginFrame`**

```cpp
void OtaForwarder::emitOtaBeginFrame()
{
    OtaBeginPayload payload{};
    payload.xferId = currentXferId_;
    payload.totalSize = firmwareTotalSize_;
    payload.chunkSize = kChunkSize;
    payload.totalChunks = firmwareTotalChunks_;
    std::memcpy(payload.sha256Expected, firmwareSha256_, 32);
    payload.flags = 0;

    esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_BEGIN,
                                                reinterpret_cast<const uint8_t *>(&payload),
                                                sizeof(payload));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA_BEGIN sendOtaFrame returned %s; the BEGIN_ACK timeout will catch this",
                 esp_err_to_name(err));
    }
}
```

- [ ] **Step 6: Implement `handleBeginAck` and `handleBeginNak`**

```cpp
void OtaForwarder::handleBeginAck(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_BEGIN_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_BEGIN_ACK while phase=%d (xferId=%u); dropping", (int)phase_,
                 msg.begin_ack.xferId);
        return;
    }
    auto r = bulk_.onBeginAck(msg.begin_ack.xferId);
    if (r.decision != AstrOsBulkTransport::BeginAckResult::Decision::OK)
    {
        ESP_LOGW(TAG, "BulkSender::onBeginAck rejected decision=%d (xferId=%u); waiting on timeout",
                 (int)r.decision, msg.begin_ack.xferId);
        return;
    }
    beginAckTimerStop();
    phase_ = Phase::STREAMING;
    // Streaming drains will happen on the next tick (Task 7b's handleTick
    // body covers this). For now, kick once to start the flow.
    handleTick();
}

void OtaForwarder::handleBeginNak(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_BEGIN_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_BEGIN_NAK while phase=%d (xferId=%u reason=%u); dropping",
                 (int)phase_, msg.begin_nak.xferId, msg.begin_nak.reason);
        return;
    }
    ESP_LOGW(TAG, "OTA_BEGIN_NAK from %s reason=%u; abandoning padawan",
             currentControllerId_.c_str(), msg.begin_nak.reason);
    std::string reasonStr = "begin_nak_" + std::to_string(msg.begin_nak.reason);
    abortCurrentPadawan(reasonStr);
}
```

- [ ] **Step 7: Implement `beginAckTimerCb` and timeout handling**

Replace the skeleton stub:

```cpp
void OtaForwarder::beginAckTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    // Post a synthetic NAK-equivalent so all transitions happen on
    // otaForwarderTask. Use OTA_FWD_BEGIN_NAK with a synthetic reason
    // value the forwarder's handler maps to "begin_ack_timeout".
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_BEGIN_NAK;
    // Sentinel reason — out-of-band of the wire enum, but
    // handleBeginNak doesn't act on the wire-valid range. The forwarder
    // detects this sentinel by phase_ == AWAITING_BEGIN_ACK at handle
    // time (timeout, no real NAK arrived) and uses the dedicated
    // "begin_ack_timeout" reason string.
    m.begin_nak.xferId = 0xFF; // wire-invalid sentinel
    m.begin_nak.reason = 0xFF;
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}
```

Update `handleBeginNak` to detect the sentinel:

```cpp
void OtaForwarder::handleBeginNak(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_BEGIN_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_BEGIN_NAK while phase=%d (xferId=%u reason=%u); dropping",
                 (int)phase_, msg.begin_nak.xferId, msg.begin_nak.reason);
        return;
    }

    // Sentinel xferId=0xFF + reason=0xFF means "begin ack timeout fired".
    if (msg.begin_nak.xferId == 0xFF && msg.begin_nak.reason == 0xFF)
    {
        ESP_LOGW(TAG, "OTA_BEGIN_ACK timeout for %s after 2s; abandoning",
                 currentControllerId_.c_str());
        abortCurrentPadawan("begin_ack_timeout");
        return;
    }

    ESP_LOGW(TAG, "OTA_BEGIN_NAK from %s reason=%u; abandoning padawan",
             currentControllerId_.c_str(), msg.begin_nak.reason);
    std::string reasonStr = "begin_nak_" + std::to_string(msg.begin_nak.reason);
    abortCurrentPadawan(reasonStr);
}
```

- [ ] **Step 8: Implement `abortCurrentPadawan` and `emitDeployDoneAndReset`**

```cpp
void OtaForwarder::abortCurrentPadawan(const std::string &reason)
{
    beginAckTimerStop();
    endAckTimerStop();
    tickTimerStop();
    bulk_.reset();
    if (firmwareFile_)
    {
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
    }
    results_.push_back({currentControllerId_, "FAILED", "", reason});
    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}

void OtaForwarder::emitDeployDoneAndReset()
{
    ESP_LOGI(TAG, "FW_DEPLOY_DONE: %zu targets, transferId=%s", results_.size(),
             deployTransferId_.c_str());
    AstrOs_SerialMsgHandler.sendFwDeployDone(deployMsgId_, deployTransferId_, results_);

    deployMsgId_.clear();
    deployTransferId_.clear();
    orderList_.clear();
    nextOrderIdx_ = 0;
    results_.clear();
    phase_ = Phase::IDLE;
    active_.store(false);
}
```

- [ ] **Step 9: Build both boards**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 436/436.

- [ ] **Step 10: Commit**

```bash
git add lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "feat(ota-forwarder): begin-side flow (DEPLOY_BEGIN through BEGIN_ACK timeout)"
```

---

## Task 7b: OtaForwarder data + end-side flow (structurally complete; exercised by M4)

Implements the streaming loop: `handleTick` drives `BulkSender::tick`, emits retransmits, drains `nextChunkToSend`, reads file bytes, emits `OTA_DATA` frames. `handleDataAck`/`handleDataNak` advance state. When all chunks confirmed, emit `OTA_END` and start the END_ACK timeout. `handleEndAck` records the terminal result and advances.

This code is exercised by M4's bench checkpoint (first round-trip), not M3's. M3 ships it structurally complete so M4 doesn't have to retrofit.

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

- [ ] **Step 1: Implement `streamDrain`**

Replace the skeleton stub:

```cpp
void OtaForwarder::streamDrain(uint64_t nowMs)
{
    for (;;)
    {
        auto sr = bulk_.nextChunkToSend(nowMs);
        if (sr.decision == AstrOsBulkTransport::SendResult::Decision::SEND)
        {
            // Read the chunk from disk. Chunks are chunkSize except
            // possibly the last one.
            const uint32_t seq = sr.seq;
            const uint32_t offset = seq * kChunkSize;
            uint32_t expectedLen = kChunkSize;
            if (offset + kChunkSize > firmwareTotalSize_)
            {
                expectedLen = firmwareTotalSize_ - offset;
            }

            uint8_t payloadBuf[sizeof(OtaDataHeader) + kChunkSize];
            OtaDataHeader hdr{};
            hdr.xferId = currentXferId_;
            hdr.seq = seq;
            hdr.payloadLen = static_cast<uint16_t>(expectedLen);
            // CRC computed after the bytes are loaded below.

            std::memcpy(payloadBuf, &hdr, sizeof(hdr));
            if (std::fseek(firmwareFile_, offset, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "fseek(%u) failed mid-transfer; abandoning", offset);
                abortCurrentPadawan("file_seek_failed");
                return;
            }
            size_t read = std::fread(payloadBuf + sizeof(hdr), 1, expectedLen, firmwareFile_);
            if (read != expectedLen)
            {
                ESP_LOGE(TAG, "fread(%u) returned %zu mid-transfer; abandoning", expectedLen, read);
                abortCurrentPadawan("file_read_short");
                return;
            }

            // CRC-16/CCITT-FALSE over the header (post-xferId) + payload —
            // matching BulkReceiver's verification. Compute over the same
            // span the receiver hashes.
            uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payloadBuf,
                                                                   sizeof(hdr) + expectedLen);
            // Patch the CRC into the header bytes that are now in payloadBuf.
            std::memcpy(payloadBuf + offsetof(OtaDataHeader, crc16), &crc, sizeof(crc));

            esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_DATA,
                                                       payloadBuf, sizeof(hdr) + expectedLen);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "OTA_DATA seq=%u sendOtaFrame returned %s; tick will retry", seq,
                         esp_err_to_name(err));
                // Don't abort here — tick-based retransmit will catch it.
                return;
            }
            continue;
        }

        // WINDOW_FULL, ALL_SENT, NOT_STREAMING — stop draining for now.
        if (sr.decision == AstrOsBulkTransport::SendResult::Decision::ALL_SENT)
        {
            // If the in-flight table is empty (everything ACKed), it's
            // time to send OTA_END.
            if (bulk_.status() == AstrOsBulkTransport::BulkSender::Status::STREAMING &&
                phase_ == Phase::STREAMING)
            {
                // Check via a benign onDataAck side-effect? No — we don't
                // know the high-water-confirmed seq from outside. Track
                // it locally instead: when handleDataAck advances the
                // confirmed watermark to totalChunks-1, fire OTA_END.
                // (Done in handleDataAck below; streamDrain doesn't fire
                // OTA_END here.)
            }
            return;
        }
        return; // WINDOW_FULL or NOT_STREAMING
    }
}
```

- [ ] **Step 2: Implement `handleDataAck`**

```cpp
void OtaForwarder::handleDataAck(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::STREAMING && phase_ != Phase::AWAITING_END_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_DATA_ACK while phase=%d (xferId=%u); dropping",
                 (int)phase_, msg.data_ack.xferId);
        return;
    }
    auto r = bulk_.onDataAck(msg.data_ack.xferId, msg.data_ack.highestContiguousSeq);
    switch (r.decision)
    {
    case AstrOsBulkTransport::AckResult::Decision::OK:
        break;
    case AstrOsBulkTransport::AckResult::Decision::STALE:
        ESP_LOGD(TAG, "Stale ACK cumulativeSeq=%u — ignoring", msg.data_ack.highestContiguousSeq);
        return;
    case AstrOsBulkTransport::AckResult::Decision::OUT_OF_RANGE:
        // forward-concern #13: log distinctly so a peer-ahead-of-sender
        // mistake is recognizable in production logs.
        ESP_LOGW(TAG,
                 "OTA_DATA_ACK OUT_OF_RANGE from %s xferId=%u cumulativeSeq=%u (peer ahead of "
                 "sender) — ignoring",
                 currentControllerId_.c_str(), msg.data_ack.xferId,
                 msg.data_ack.highestContiguousSeq);
        return;
    default:
        ESP_LOGW(TAG, "OTA_DATA_ACK rejected decision=%d cumulativeSeq=%u — ignoring",
                 (int)r.decision, msg.data_ack.highestContiguousSeq);
        return;
    }

    // forward-concern #11: newlyConfirmedCount is a watermark delta, not
    // a slot count. Use highestContiguousSeq for "have we received
    // everything?" rather than counting slot evictions.
    if (msg.data_ack.highestContiguousSeq + 1 >= firmwareTotalChunks_ &&
        phase_ == Phase::STREAMING)
    {
        // All chunks confirmed — time to send OTA_END.
        emitOtaEndFrame();
        phase_ = Phase::AWAITING_END_ACK;
        endAckTimerStart();
        return;
    }

    // Otherwise, the freed in-flight slots let us send more.
    streamDrain(static_cast<uint64_t>(esp_timer_get_time() / 1000));
}
```

- [ ] **Step 3: Implement `handleDataNak`**

```cpp
void OtaForwarder::handleDataNak(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::STREAMING)
    {
        ESP_LOGW(TAG, "Spurious OTA_DATA_NAK while phase=%d (xferId=%u); dropping",
                 (int)phase_, msg.data_nak.xferId);
        return;
    }
    auto r = bulk_.onDataNak(
        msg.data_nak.xferId, msg.data_nak.nextExpectedSeq,
        static_cast<AstrOsBulkTransport::NakReason>(msg.data_nak.reason));
    switch (r.decision)
    {
    case AstrOsBulkTransport::NakResult::Decision::OK:
        break;
    case AstrOsBulkTransport::NakResult::Decision::OUT_OF_RANGE:
        ESP_LOGW(TAG,
                 "OTA_DATA_NAK OUT_OF_RANGE from %s xferId=%u nextExpectedSeq=%u (peer NAK ahead "
                 "of sender) — ignoring",
                 currentControllerId_.c_str(), msg.data_nak.xferId, msg.data_nak.nextExpectedSeq);
        return;
    default:
        ESP_LOGW(TAG, "OTA_DATA_NAK rejected decision=%d — ignoring", (int)r.decision);
        return;
    }
    // The NAK rewinds nextSeqToSend_; the next tick or streamDrain will
    // re-emit from there.
    streamDrain(static_cast<uint64_t>(esp_timer_get_time() / 1000));
}
```

- [ ] **Step 4: Implement `handleTick`**

```cpp
void OtaForwarder::handleTick()
{
    // forward-concern #9: tick(count=0, abandon=false) is indistinguishable
    // from "not streaming" — read status() separately.
    auto status = bulk_.status();
    if (status != AstrOsBulkTransport::BulkSender::Status::STREAMING &&
        status != AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK)
    {
        // BulkSender isn't in a state where tick has work; this commonly
        // means we're between transfers or in a terminal state. Skip.
        return;
    }
    if (phase_ != Phase::STREAMING)
    {
        // We're in AWAITING_BEGIN_ACK or AWAITING_END_ACK; tick has no
        // role here (begin/end timeouts are separate timers).
        return;
    }

    uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    auto tr = bulk_.tick(nowMs);

    // forward-concern #12: check abandon BEFORE iterating retransmitSeqs.
    if (tr.abandon)
    {
        ESP_LOGW(TAG, "BulkSender abandoned (retry count exceeded) for %s; recording FAILED",
                 currentControllerId_.c_str());
        abortCurrentPadawan("data_retry_exceeded");
        return;
    }

    // forward-concern #8: emit retransmits BEFORE pulling new chunks. The
    // retransmit list and the streamDrain loop both call sendOtaFrame —
    // ordering matters because a future "send before tick" loop would
    // silently skip the retransmits (BulkSender doesn't rewind
    // nextSeqToSend_ when a tick triggers retransmit).
    for (uint8_t i = 0; i < tr.count; i++)
    {
        const uint32_t seq = tr.retransmitSeqs[i];
        const uint32_t offset = seq * kChunkSize;
        uint32_t expectedLen = kChunkSize;
        if (offset + kChunkSize > firmwareTotalSize_)
        {
            expectedLen = firmwareTotalSize_ - offset;
        }

        uint8_t payloadBuf[sizeof(OtaDataHeader) + kChunkSize];
        OtaDataHeader hdr{};
        hdr.xferId = currentXferId_;
        hdr.seq = seq;
        hdr.payloadLen = static_cast<uint16_t>(expectedLen);

        std::memcpy(payloadBuf, &hdr, sizeof(hdr));
        if (std::fseek(firmwareFile_, offset, SEEK_SET) != 0 ||
            std::fread(payloadBuf + sizeof(hdr), 1, expectedLen, firmwareFile_) != expectedLen)
        {
            ESP_LOGE(TAG, "Failed to re-read seq=%u for retransmit", seq);
            abortCurrentPadawan("file_read_short");
            return;
        }

        uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payloadBuf, sizeof(hdr) + expectedLen);
        std::memcpy(payloadBuf + offsetof(OtaDataHeader, crc16), &crc, sizeof(crc));

        AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_DATA, payloadBuf,
                                    sizeof(hdr) + expectedLen);
    }

    // After retransmits, drain new chunks until WINDOW_FULL / ALL_SENT.
    streamDrain(nowMs);
}
```

- [ ] **Step 5: Implement `emitOtaEndFrame`, `endAckTimerCb`, and `handleEndAck`**

```cpp
void OtaForwarder::emitOtaEndFrame()
{
    OtaEndPayload payload{};
    payload.xferId = currentXferId_;
    payload.totalChunksSent = firmwareTotalChunks_;
    std::memcpy(payload.sha256Final, firmwareSha256_, 32);

    esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_END,
                                                reinterpret_cast<const uint8_t *>(&payload),
                                                sizeof(payload));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA_END sendOtaFrame returned %s; endAckTimer will catch the timeout",
                 esp_err_to_name(err));
    }
}

void OtaForwarder::endAckTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    // Synthetic OTA_FWD_END_ACK with sentinel status=0xFF.
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_END_ACK;
    m.end_ack.xferId = 0xFF;
    m.end_ack.status = 0xFF;
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}

void OtaForwarder::handleEndAck(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_END_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_END_ACK while phase=%d (xferId=%u status=%u); dropping",
                 (int)phase_, msg.end_ack.xferId, msg.end_ack.status);
        return;
    }
    endAckTimerStop();
    tickTimerStop();

    // Sentinel xferId=0xFF + status=0xFF means END_ACK timeout fired.
    if (msg.end_ack.xferId == 0xFF && msg.end_ack.status == 0xFF)
    {
        ESP_LOGW(TAG, "OTA_END_ACK timeout for %s after 5s; abandoning",
                 currentControllerId_.c_str());
        abortCurrentPadawan("end_ack_timeout");
        return;
    }

    auto endResult =
        bulk_.onEndAck(msg.end_ack.xferId, static_cast<OtaEndStatus>(msg.end_ack.status));
    switch (endResult.decision)
    {
    case AstrOsBulkTransport::EndAckResult::Decision::DONE_OK:
        ESP_LOGI(TAG, "Transfer to %s OK", currentControllerId_.c_str());
        completeCurrentPadawan();
        return;
    case AstrOsBulkTransport::EndAckResult::Decision::ABANDONED:
        ESP_LOGW(TAG, "Transfer to %s ABANDONED (status=%u)", currentControllerId_.c_str(),
                 msg.end_ack.status);
        {
            std::string reason =
                (msg.end_ack.status == static_cast<uint8_t>(OtaEndStatus::HASH_MISMATCH))
                    ? "hash_mismatch"
                    : "write_error";
            abortCurrentPadawan(reason);
        }
        return;
    case AstrOsBulkTransport::EndAckResult::Decision::PREMATURE:
        ESP_LOGW(TAG, "OTA_END_ACK PREMATURE (sender state machine internal); abandoning");
        abortCurrentPadawan("premature_end_ack");
        return;
    default:
        ESP_LOGW(TAG, "OTA_END_ACK rejected decision=%d — abandoning", (int)endResult.decision);
        abortCurrentPadawan("end_ack_rejected");
        return;
    }
}
```

- [ ] **Step 6: Implement `completeCurrentPadawan`**

```cpp
void OtaForwarder::completeCurrentPadawan()
{
    beginAckTimerStop();
    endAckTimerStop();
    tickTimerStop();
    bulk_.reset();
    if (firmwareFile_)
    {
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
    }
    results_.push_back({currentControllerId_, "OK", "", ""});
    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}
```

- [ ] **Step 7: Build and test**

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio test -e test 2>&1 | tail -5`
Expected: 436/436 (no native tests for the streaming flow — it's bench-tested in M4).

- [ ] **Step 8: Commit**

```bash
git add lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "feat(ota-forwarder): data + end-side flow (structural — exercised by M4)"
```

---

## Task 8: Bench verification + plan closeout

NO code changes — final verification only.

- [ ] **Step 1: Branch sanity check**

```bash
git branch --show-current   # feature/ota-mesh-forward-m3-ota-forwarder
git log --oneline develop..HEAD  # 7 commits expected (T1, T2, T3, T4, T5, T6, T7a, T7b)
```

- [ ] **Step 2: Final test run**

```bash
pio test -e test 2>&1 | tail -5
```

Expected: 436/436 (427 baseline + 4 LastFirmwarePathHolder + 5 OtaForwarderMsg).

- [ ] **Step 3: Both board builds**

```bash
pio run -e metro_s3 2>&1 | tail -5
pio run -e lolin_d32_pro 2>&1 | tail -5
```

Expected: both SUCCESS.

- [ ] **Step 4: clang-format check**

```bash
git diff --name-only develop...HEAD | grep -E '\.(hpp|cpp|h)$' \
  | xargs -I {} /home/jeff/.platformio/penv/bin/clang-format --dry-run --Werror {} 2>&1
```

Expected: empty (no drift on changed C/C++ files).

- [ ] **Step 5: Bench checkpoint**

Flash a master with this branch on `metro_s3`. Have the server upload a known-good `.bin` so `OtaReceiver` lands `/sdcard/firmware/<sha-prefix>.bin` and `getLastFirmwarePath()` is populated.

Then drive a deploy from the AstrOs.Server Firmware view to a single registered padawan (any controller-id that has a peer entry, even if the padawan hardware itself isn't powered on or isn't running M4 yet).

Expected master serial log flow:

```
I OtaForwarder: FW_DEPLOY_BEGIN: transferId=<id> targets=1
I OtaForwarder: Starting transfer to <controller-id> (xferId=1, chunks=<N>, size=<bytes>)
W OtaForwarder: OTA_BEGIN_ACK timeout for <controller-id> after 2s; abandoning
I OtaForwarder: FW_DEPLOY_DONE: 1 targets, transferId=<id>
```

Server-side: `FW_DEPLOY_DONE` arrives with one result: `<controller-id> = FAILED("begin_ack_timeout")`. JobLock releases.

Optional sniffer / temporary padawan-log verification: ESP-NOW frames carrying `OTA_BEGIN` (type byte = `AstrOsPacketType::OTA_BEGIN`, payload 44 bytes) are observed over the air during the 2-second window. This proves the binary-frame TX path works end-to-end and that the forwarder is reading the firmware file correctly to compute the SHA-256 + total-chunks in the OTA_BEGIN payload.

- [ ] **Step 6: Confirm bench checkpoint and report**

If the bench flow above produces the expected logs and FW_DEPLOY_DONE, M3 is feature-complete.

Report:
- Branch HEAD SHA
- Final test count (436/436)
- Both board builds: SUCCESS
- Bench checkpoint: PASS / FAIL with details
- 7 commits from `develop` (T1 through T7b)

---

## Out of scope for M3 (do NOT add)

- Padawan-side `OtaWriter` MIXED lib (M4)
- `FW_PROGRESS` emission during streaming (M5)
- Master self-flash from `/sdcard/firmware/<sha-prefix>.bin` (PR set 2)
- `esp_ota_set_boot_partition`, reboot, version-confirmation (PR set 2)
- Multi-firmware tracking — M3 assumes one staged firmware at a time
- Rejecting NAK-below-watermark at the wire layer (forward-concern #10 deferred)

## Verification gates per the design doc

1. `pio test -e test` 100% pass (436/436).
2. `pio run -e metro_s3` build clean.
3. `pio run -e lolin_d32_pro` build clean.
4. `clang-format` clean on changed files.
5. Bench checkpoint: server-driven deploy produces master `OTA_BEGIN` frames over the air; `FW_DEPLOY_DONE` arrives with all targets `FAILED("begin_ack_timeout")` plus `master = FAILED("not_implemented_in_pr_set_1")` if master is in the order list; JobLock releases cleanly.

## Cross-repo coordination

- Wire-format contract **frozen and unchanged**. M3 binds the contract via `sendOtaFrame()`; does not amend it.
- No `AstrOs.Server` changes required — the server's FW_DEPLOY_BEGIN producer and FW_DEPLOY_DONE consumer already exist from sub-project (c).
- Pre-existing M2 design-doc open questions #7-14 all dispositioned by this plan (see Context section table).
