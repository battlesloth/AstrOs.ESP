# Firmware OTA Mesh-Forward M4 — Padawan OtaWriter (MIXED) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the padawan-side `OtaWriter` MIXED lib that consumes M1's wire format + existing `BulkReceiver` to write incoming firmware bytes to the inactive OTA partition, then verify with read-back-rehash. After this milestone, master + 1 padawan on the bench complete a full `FW_DEPLOY_BEGIN → OTA_END_ACK OK` round trip, with the inactive partition holding hash-verified bytes (no boot-partition change yet — that's PR set 2).

**Architecture:** New `lib/OtaWriter/` MIXED lib runs on a dedicated FreeRTOS task (padawan only, core 1). Drains a new `otaWriterQueue` carrying decoded `OTA_BEGIN` / `OTA_DATA` / `OTA_END` frames from the ESP-NOW RX path plus an idle-watchdog timer signal. Per-transfer flow: busy-check → `esp_ota_get_next_update_partition` + `esp_ota_begin` → `BulkReceiver::begin` → `OTA_BEGIN_ACK` → for each chunk: `BulkReceiver::onChunk` + `esp_ota_write` + streaming `AstrOsSha256_update` + `OTA_DATA_ACK` → `BulkReceiver::onEnd` → final SHA compare → `esp_ota_end` → read-back-and-rehash via `esp_partition_read` → `OTA_END_ACK`. AstrOsEspNow's padawan-side residual-switch arms (currently the M3 "M4 not wired" stub) gain real parsing + queueing for `OTA_BEGIN` / `OTA_DATA` / `OTA_END`.

**Tech Stack:** C++17, ESP-IDF (esp_ota_ops, esp_partition, esp_timer, freertos), PlatformIO. MIXED — no PURE constraints. Uses project-internal `AstrOsSha256` (pure-software SHA-256 in `lib_native/AstrOsUtility`, **not** mbedtls — see the discipline comment on `OtaReceiver.hpp:53-56` about hardware-engine contention). Builds on M1's `lib_native/AstrOsMessaging` wire format + the existing `lib_native/AstrOsBulkTransport::BulkReceiver`.

---

## Context for the engineer

Read these first — load-bearing, will not be re-explained per task:

- **Design doc**: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md`. Section "M4 — Padawan OtaWriter (MIXED) + first end-to-end bench validation" is the canonical scope. Section "Padawan side — BulkReceiver (PURE, existing) + OtaWriter (MIXED)" (lines 215-325) describes the runtime shape including pseudocode for `handleBegin` / `handleData` / `handleEnd`. **Treat the pseudocode as a sketch, not a contract** — three deviations are required and called out below.
- **M3 plan as the mirror precedent**: `.docs/plans/20260524-0912-firmware-ota-mesh-forward-m3-ota-forwarder.md`. M4's `OtaWriter` is the structural mirror of M3's `OtaForwarder`: same Init two-step, same `process(msg)` dispatcher pattern, same `std::atomic<bool> active_` polling-pause gate, same singleton `extern` declaration, same discriminated-union queue message style. Read M3's Task 3 (skeleton) and Task 7a/7b (handler bodies) before starting; M4 lifts most of the wiring discipline wholesale.
- **`OtaReceiver` as same-role precedent**: `lib/OtaReceiver/include/OtaReceiver.hpp` + `lib/OtaReceiver/src/OtaReceiver.cpp`. `OtaReceiver` is the **serial-path** counterpart to M4's ESP-NOW-path `OtaWriter` — both run on a padawan, both wrap `BulkReceiver`, both own a 10 s idle watchdog with the `OTA_MSG_WATCHDOG_FIRE` posting pattern. Mirror its discipline:
  - `watchdogStart` / `watchdogRestart` / `watchdogStop` triplet (`OtaReceiver.cpp:88-119`)
  - `watchdogTimerCb` only does `xQueueSend` — state mutation runs on the consumer task (`OtaReceiver.cpp:70-86`)
  - `resetCryptoAndFile`-style idempotent cleanup (`OtaReceiver.cpp:121-147`) — M4's equivalent is `resetOtaHandleAndSha` that calls `esp_ota_abort` if `otaHandle_ != 0` and drops the SHA active flag
  - `AstrOsSha256_init` / `AstrOsSha256_update` / `AstrOsSha256_final` (used at `OtaReceiver.cpp:285,363,453`)
- **M1 OTA wire format**: `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp` — 8 packed structs with sizeof `static_assert`s. Reasons live in 3 wire-stable enums (`OtaBeginNakReason`, `OtaDataNakReason`, `OtaEndStatus`).
- **M1 OTA record types**: `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp:51-132`. `parseOtaBegin` / `parseOtaData` / `parseOtaEnd` produce `OtaBeginRecord` / `OtaDataRecord` / `OtaEndRecord` with `bool valid`. **`OtaDataRecord.payload` is a pointer into the parsed packet's buffer** (lines 75-79) — M4's dispatcher MUST `malloc` + `memcpy` the bytes before posting to the queue, because the packet buffer is freed/reused after the dispatcher returns.
- **M3 ACK/NAK dispatch precedent**: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:417-530`. M3's `routeOtaAckNakToForwarder` is the structural template M4 mirrors for the padawan side. Note the zero-init + queue-send + free-on-full pattern.
- **BulkReceiver API**: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` — re-read the `BulkReceiver` class declaration (lines 275-283) and the result types: `BeginResult` (lines 156-180), `ChunkResult` (lines 99-149 — note the `Decision::ACK` returns the payload pointer + length the caller passes through to `esp_ota_write`), `EndResult` (lines 183-227 — note `shouldTeardownOnEndResult` at line 227 that gates teardown). All result types are `[[nodiscard]]`.
- **OTA partition layout**: `partition_8mb.csv` and `partition_16mb.csv` both define `ota_0` + `ota_1` slots (8 MB board: 2 MB each; 16 MB board: 6.4 MB each) plus `otadata`. `esp_ota_get_next_update_partition(NULL)` returns the inactive slot. The size-check guard in `handleBegin` uses `inactivePartition_->size` so it just works on both boards.
- **Polling-pause precedent**: `src/main.cpp:460-477` — `pollingTimerCallback` already gates the master polling branch on `OtaReceiver::isActive() || OtaForwarder::isActive()`. The padawan branch (immediately above) currently has no gate. **M4 adds padawan-side gating** — see `src/main.cpp:470-490` for the current shape.
- **Existing padawan-side dispatcher stub**: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:400-409`. M3 left an `ESP_LOGW("OTA master→padawan packet type=%d received on %s; dropping (M4 not wired)")` stub for `OTA_BEGIN` / `OTA_DATA` / `OTA_END`. M4 replaces this with real per-packet parsing + queueing.

### Pseudocode-vs-reality deviations from the design doc

The design doc (sections 215-325) was written before the codebase landed `AstrOsSha256` and before the ACK/NAK builder layer was finalized. M4 follows the **codebase patterns**, not the doc verbatim:

| Doc says | M4 uses |
|---|---|
| `mbedtls_sha256_context shaCtx_` + `mbedtls_sha256_init/starts/update/finish/free` | `AstrOsSha256Ctx shaCtx_` + `AstrOsSha256_init/update/final` + `bool shaActive_` flag (mirrors `OtaReceiver.hpp:53-59`'s rationale: pure software, no hardware-engine contention, no malloc/free pair) |
| `sendOtaBeginAck(...)` / `sendOtaBeginNak(...)` / etc. helper methods | Direct calls to `AstrOs_EspNow.sendOtaFrame(mac, type, payload, len)` (the M3 binary TX helper at `AstrOsEspNowService.cpp:978`) with builder payload structs filled inline. Don't add a per-type wrapper layer in M4; that's premature abstraction for one caller. |
| `bulk_.reset()` in some error paths | Match `OtaReceiver.cpp:170-188`'s pattern — `bulk_.reset()` lives in `resetOtaHandleAndSha()`, called from every abort path. Centralizing prevents the "did I reset?" leak class. |

### Forward-concern dispositions (from design-doc Open Questions)

The design doc tags three open questions for M4:

| # | Concern | M4 disposition |
|---|---|---|
| 4 | Padawan idle watchdog timeout (proposed 10 s, mirrors serial-receive) | **Adopt 10 s.** `OtaReceiver` ships 10 s, bench data is happy, no reason to diverge. **Task 2.** |
| 5 | Whether `esp_ota_write`'s implicit erase fits inside one chunk's tick budget | **No explicit `esp_partition_erase_range` needed.** `esp_ota_write` calls `spi_flash_erase_sector` on first write per 4 KB sector — single-sector erase is ~40 ms on ESP32 + ~25 ms on ESP32-S3. Bench-validate via `M4` checkpoint (see Task 9). If watchdog or ACK timeouts fire from erase latency on the first few chunks, escalate to a follow-up plan that adds pre-`begin` `esp_partition_erase_range` over the full partition. **Tasks 5 + 9.** |
| 6 | Read-back chunk size for `esp_partition_read` (proposed 4 KB) | **Adopt 4 KB.** Matches a sector; fits comfortably on a 4 KB-aligned stack buffer; bench-validate readback throughput in the M4 checkpoint and consider 8 KB only if cumulative readback time approaches the END_ACK timeout (5 s) at full firmware size. **Task 7.** |

### One M4-specific concern not in the design doc

**Dispatcher ownership of `OTA_DATA` payload bytes.** `parseOtaData` returns `OtaDataRecord` with `const uint8_t *payload` pointing INTO the ESP-NOW RX packet buffer (`AstrOsEspNowProtocol.hpp:75-79`). That buffer is freed after the dispatcher returns. M4's dispatcher MUST `malloc(payloadLen)` + `memcpy` the bytes into the queue message; `freeOtaWriterMsg` then `free`s the malloc'd buffer after the consumer has called `esp_ota_write`. Tasks 1 + 4 enforce this discipline; failure to copy will manifest as random bytes appearing in the flashed partition + sha mismatches. **Tasks 1 + 4.**

## File structure

**Created:**
- `lib/OtaWriter/library.json` — PlatformIO lib metadata
- `lib/OtaWriter/README` — MIXED-lib classification + role-gating note (padawan only)
- `lib/OtaWriter/include/OtaWriter.hpp` — class declaration, state machine fields, singleton extern
- `lib/OtaWriter/include/OtaWriterQueueMessage.h` — `queue_ota_writer_msg_t` discriminated union + `freeOtaWriterMsg()`
- `lib/OtaWriter/src/OtaWriter.cpp` — handler implementations (Begin/Data/End/WatchdogFire)
- `test/test_native/astros_ota_writer_tests.cpp` — native tests for the queue-message free-contract (pointer ownership matters — explicit native-pinned tests catch a regression that bench testing wouldn't easily isolate)

**Modified:**
- `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp` — declare `setOtaWriterQueue(QueueHandle_t)` setter + private `otaWriterQueue_` member + private `routeOtaToWriter(src, packet)` helper declaration
- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — implement setter; replace the M3 "M4 not wired" `ESP_LOGW` stub at lines 400-409 with a call into `routeOtaToWriter` on padawan; implement `routeOtaToWriter` mirroring `routeOtaAckNakToForwarder`'s shape
- `src/main.cpp` — create `otaWriterQueue` (size 16, padawan only); spawn `otaWriterTask` pinned to core 1 (padawan only); extend padawan polling-pause gate with `OtaWriter::isActive()`; plumb the queue handle into `AstrOs_OtaWriter.Init` + `AstrOs_EspNow.setOtaWriterQueue`
- `.docs/qa/ota-mesh-forward.md` — extend the existing QA plan with the M4 single-padawan bench case (sub-section "M4 — single-padawan end-to-end")

**Singleton instance**: `extern OtaWriter AstrOs_OtaWriter;` declared in the header, defined in the .cpp. Mirrors `AstrOs_OtaReceiver` and `AstrOs_OtaForwarder`.

## What does NOT ship in M4

- `esp_ota_set_boot_partition(...)` (deferred to PR set 2 — M4 verifies the bytes in the inactive partition but does not change boot)
- Reboot / version-confirmation logic (PR set 2)
- `FW_PROGRESS` emission (M5)
- Master self-flash (PR set 2)
- Multi-padawan orchestration validation (M5 — M4's bench bar is one padawan)

---

## Task 1: `queue_ota_writer_msg_t` discriminated union + `freeOtaWriterMsg`

Defines the queue-message contract from the ESP-NOW RX path (producer) to `otaWriterTask` (consumer). Mirrors M3's `queue_ota_forwarder_msg_t` discipline. The critical M4-specific detail: `OTA_DATA` carries a `malloc`'d `uint8_t *payload` because the source bytes live in a packet buffer that the dispatcher returns from; the consumer frees after `esp_ota_write` runs.

**Files:**
- Create: `lib/OtaWriter/include/OtaWriterQueueMessage.h`
- Create: `test/test_native/astros_ota_writer_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Create `test/test_native/astros_ota_writer_tests.cpp`:

```cpp
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <OtaWriterQueueMessage.h>

#include <cstdlib>
#include <cstring>

// These tests pin the per-kind ownership contract of queue_ota_writer_msg_t.
// The actual OtaWriter class is MIXED and cannot link in [env:test]; the
// queue-message header is plain C and links cleanly. Pointer-ownership bugs
// here would manifest as random bytes appearing in the flashed partition
// (OTA_WR_DATA) or as leaks (OTA_WR_BEGIN's sha bytes — though Begin owns
// none, the test pins that contract too).

namespace
{
    // Build a synthetic OTA_WR_DATA message with a malloc'd payload, then
    // assert freeOtaWriterMsg releases the malloc + nulls the pointer.
    queue_ota_writer_msg_t makeDataMsg(uint8_t xferId, uint32_t seq, const uint8_t *bytes, uint16_t len)
    {
        queue_ota_writer_msg_t m{};
        m.kind = OTA_WR_DATA;
        memset(m.data.srcMac, 0, 6);
        m.data.xferId = xferId;
        m.data.seq = seq;
        m.data.payloadLen = len;
        m.data.crc16 = 0xBEEF;
        m.data.payload = static_cast<uint8_t *>(malloc(len));
        memcpy(m.data.payload, bytes, len);
        return m;
    }
} // namespace

TEST(OtaWriterQueueMsg, FreeNullIsNoop)
{
    freeOtaWriterMsg(nullptr); // must not crash
}

TEST(OtaWriterQueueMsg, FreeBeginReleasesNothing)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_BEGIN;
    memset(m.begin.srcMac, 0, 6);
    m.begin.xferId = 7;
    m.begin.totalSize = 1024;
    m.begin.totalChunks = 8;
    m.begin.chunkSize = 128;
    memset(m.begin.sha256Expected, 0xAB, 32);
    m.begin.flags = 0;

    // No pointers owned by BEGIN; freeOtaWriterMsg is a no-op for it.
    // The assertion is "doesn't crash" — a real leak would surface as a
    // valgrind/ASAN finding under host CI in the future.
    freeOtaWriterMsg(&m);
    EXPECT_EQ(7, m.begin.xferId); // POD fields untouched
}

TEST(OtaWriterQueueMsg, FreeDataReleasesPayload)
{
    uint8_t scratch[16];
    memset(scratch, 0xCD, sizeof(scratch));

    queue_ota_writer_msg_t m = makeDataMsg(/*xferId=*/9, /*seq=*/42, scratch, sizeof(scratch));
    ASSERT_NE(nullptr, m.data.payload);

    freeOtaWriterMsg(&m);
    EXPECT_EQ(nullptr, m.data.payload); // nulled after free
}

TEST(OtaWriterQueueMsg, FreeEndReleasesNothing)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_END;
    memset(m.end.srcMac, 0, 6);
    m.end.xferId = 3;
    m.end.totalChunksSent = 8;
    memset(m.end.sha256Final, 0x12, 32);

    freeOtaWriterMsg(&m);
    EXPECT_EQ(3, m.end.xferId);
}

TEST(OtaWriterQueueMsg, FreeWatchdogIsSafe)
{
    queue_ota_writer_msg_t m{};
    m.kind = OTA_WR_WATCHDOG_FIRE;
    // No payload; freeOtaWriterMsg must tolerate the zero-init.
    freeOtaWriterMsg(&m);
}

TEST(OtaWriterQueueMsg, DoubleFreeIsSafe)
{
    uint8_t scratch[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    queue_ota_writer_msg_t m = makeDataMsg(/*xferId=*/1, /*seq=*/0, scratch, sizeof(scratch));
    freeOtaWriterMsg(&m); // first free
    freeOtaWriterMsg(&m); // second free must be no-op (pointer nulled)
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test 2>&1 | tail -10`
Expected: compile error — `OtaWriterQueueMessage.h` does not exist yet.

- [ ] **Step 3: Create the queue message header**

Create `lib/OtaWriter/include/OtaWriterQueueMessage.h`:

```c
#ifndef OTAWRITERQUEUEMESSAGE_H
#define OTAWRITERQUEUEMESSAGE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Discriminated union for otaWriterQueue.
    //
    // Memory ownership: producer mallocs every pointer for the kind it sends
    // and on xQueueSend failure calls freeOtaWriterMsg() to release its own
    // allocations. Consumer (otaWriterTask) calls freeOtaWriterMsg() after
    // dispatching to the matching handler. Mixing kinds across the free path
    // will leak or double-free.
    //
    // Per-kind owned pointers:
    //   OTA_WR_BEGIN          none (all inline fixed-size fields)
    //   OTA_WR_DATA           payload (malloc'd by dispatcher because
    //                         parseOtaData returns a pointer INTO the
    //                         packet buffer, which is freed when the
    //                         dispatcher returns)
    //   OTA_WR_END            none (all inline fixed-size fields)
    //   OTA_WR_WATCHDOG_FIRE  none
    //
    // srcMac (BEGIN/DATA/END) is a 6-byte inline buffer used for forensic
    // logging only — the protocol assumes a single master peer, so DATA/END
    // already constrain themselves to the in-flight transfer's xferId.
    // Keeping srcMac inline avoids a malloc on the chunk hot path.
    //
    // Producers MUST zero-initialize the struct before populating it
    // (e.g., `queue_ota_writer_msg_t m = {};`) so freeOtaWriterMsg's
    // free(m.data.payload) on a non-DATA kind sees a NULL pointer.

    typedef enum
    {
        OTA_WR_BEGIN = 0,
        OTA_WR_DATA = 1,
        OTA_WR_END = 2,
        OTA_WR_WATCHDOG_FIRE = 3
    } ota_writer_msg_kind_t;

    typedef struct
    {
        ota_writer_msg_kind_t kind;

        union
        {
            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t totalSize;
                uint16_t chunkSize;
                uint32_t totalChunks;
                uint8_t sha256Expected[32];
                uint8_t flags;
            } begin;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t seq;
                uint16_t payloadLen;
                uint16_t crc16;
                uint8_t *payload; // malloc'd; freed by freeOtaWriterMsg
            } data;

            struct
            {
                uint8_t srcMac[6];
                uint8_t xferId;
                uint32_t totalChunksSent;
                uint8_t sha256Final[32];
            } end;
            // OTA_WR_WATCHDOG_FIRE has no union arm.
        };
    } queue_ota_writer_msg_t;

    // Sole implementation of the per-kind free contract above. Producer and
    // consumer both call this. Freed pointers are nulled so accidental
    // double-frees become no-ops.
    static inline void freeOtaWriterMsg(queue_ota_writer_msg_t *m)
    {
        if (m == NULL)
        {
            return;
        }
        if (m->kind == OTA_WR_DATA)
        {
            free(m->data.payload);
            m->data.payload = NULL;
        }
        // BEGIN / END / WATCHDOG_FIRE own no heap pointers.
    }

#ifdef __cplusplus
} // extern "C"
#endif

#endif
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e test 2>&1 | tail -5`
Expected: all `OtaWriterQueueMsg.*` tests pass; total count = baseline + 6.

Note: `pio test -e test` resolves the include path via PlatformIO's lib auto-discovery. The new `lib/OtaWriter/` directory needs to exist (even if only with the header) for resolution to work in `[env:test]` — the include path resolution treats `lib/` lib directories as available headers regardless of MIXED-vs-PURE classification. If the test build complains about the include, confirm the new header is the only file in `lib/OtaWriter/include/` at this point.

- [ ] **Step 5: Commit**

```bash
git add lib/OtaWriter/include/OtaWriterQueueMessage.h test/test_native/astros_ota_writer_tests.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T1 — queue_ota_writer_msg_t discriminated union

Pins the per-kind ownership contract for the new otaWriterQueue. OTA_WR_DATA
owns a malloc'd payload (parseOtaData returns a pointer into the packet
buffer; dispatcher must copy before queueing). BEGIN / END / WATCHDOG_FIRE
own no heap.

Native tests cover null-safe free, per-kind free behavior, and double-free
idempotency. M4 Task 1 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `OtaWriter` skeleton — class declaration, Init, watchdog, stubbed handlers

Lay down the lib structure: class declaration with state-machine fields, `Init(QueueHandle_t)`, `process(msg)` dispatcher (logs unimplemented for each kind for now), `active_` atomic + `isActive()` accessor, watchdog timer infrastructure (start/restart/stop + callback that posts `OTA_WR_WATCHDOG_FIRE`), singleton instance. After this task the firmware compiles and runs but `OTA_BEGIN` / `OTA_DATA` / `OTA_END` still hit the M3 "M4 not wired" dispatcher stub (Task 4 reroutes them); the queue + task + watchdog exist but no real handler bodies do.

**Files:**
- Create: `lib/OtaWriter/library.json`
- Create: `lib/OtaWriter/README`
- Create: `lib/OtaWriter/include/OtaWriter.hpp`
- Create: `lib/OtaWriter/src/OtaWriter.cpp`

- [ ] **Step 1: Create the lib metadata files**

Create `lib/OtaWriter/library.json`:

```json
{
    "name": "OtaWriter",
    "version": "0.1.0",
    "description": "Padawan-side ESP-NOW OTA writer: wraps BulkReceiver, drives esp_ota_*, verifies via streaming + read-back SHA-256",
    "frameworks": "espidf",
    "platforms": "espressif32",
    "build": {
        "flags": ["-std=gnu++17"]
    },
    "dependencies": {
        "AstrOsBulkTransport": "*",
        "AstrOsEspNow": "*",
        "AstrOsMessaging": "*",
        "AstrOsUtility": "*"
    }
}
```

Create `lib/OtaWriter/README`:

```
OtaWriter (MIXED) — padawan-side ESP-NOW OTA receiver
====================================================

Padawan-only. Consumes OTA_BEGIN / OTA_DATA / OTA_END frames from the
otaWriterQueue, writes incoming bytes to the inactive OTA partition,
verifies via streaming AstrOsSha256 + post-write read-back-rehash.
Does NOT change the boot partition (PR set 2).

Pairs with: M3 OtaForwarder (master-side counterpart).
Singleton: AstrOs_OtaWriter (defined in OtaWriter.cpp).

MIXED — uses esp_ota_ops, esp_partition, esp_timer, FreeRTOS. Cannot link
in [env:test]; native test coverage lives at the queue-message layer
(test/test_native/astros_ota_writer_tests.cpp) and at the wrapped
BulkReceiver layer (bulk_transport_tests.cpp).
```

- [ ] **Step 2: Create the class header**

Create `lib/OtaWriter/include/OtaWriter.hpp`:

```cpp
#ifndef OTAWRITER_HPP
#define OTAWRITER_HPP

#include <AstrOsBulkTransport.hpp>
#include <AstrOsSha256.h>
#include <OtaWriterQueueMessage.h>

#include <atomic>
#include <cstdint>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>

// Threading: all members are accessed only from otaWriterTask via
// `process(msg)`. The one exception is `active_` (atomic; read from the
// pollingTimer's esp_timer dispatch task for polling-pause gating).
//
// The watchdog timer fires from esp_timer's dispatch task, but its
// callback only does xQueueSend(OTA_WR_WATCHDOG_FIRE) — state mutation
// runs on otaWriterTask, preserving the single-task-state invariant.

class OtaWriter
{
public:
    OtaWriter();
    ~OtaWriter();

    OtaWriter(const OtaWriter &) = delete;
    OtaWriter &operator=(const OtaWriter &) = delete;
    OtaWriter(OtaWriter &&) = delete;
    OtaWriter &operator=(OtaWriter &&) = delete;

    // Two-step construction (mirror OtaReceiver / OtaForwarder): the
    // singleton can be built at static-init time, FreeRTOS queues attach
    // later. The queue handle is held so the watchdog callback can post
    // into the same queue otaWriterTask drains.
    void Init(QueueHandle_t otaWriterQueue);

    // Single entry point from otaWriterTask.
    void process(queue_ota_writer_msg_t &msg);

    // Safe to call from any task. Gates polling work during a transfer.
    bool isActive() const noexcept
    {
        return active_;
    }

private:
    // Per-handler entry points. All run on otaWriterTask.
    void handleBegin(queue_ota_writer_msg_t &msg);
    void handleData(queue_ota_writer_msg_t &msg);
    void handleEnd(queue_ota_writer_msg_t &msg);
    void handleWatchdogFire();

    // Idempotent cleanup. Calls esp_ota_abort if otaHandle_ != 0, drops
    // shaActive_, resets BulkReceiver, clears partition/size fields,
    // sets active_ = false. Invoked from every abort path (data-write
    // fail, end-side failure, watchdog fire, dtor). Mirrors
    // OtaReceiver::resetCryptoAndFile's invariant: every exit path routes
    // through this so we never leak an OTA handle across transfers.
    void resetOtaHandleAndSha();

    // esp_timer's one-shot has no native "restart" — these centralize the
    // stop-then-start pattern.
    void watchdogStart();
    void watchdogRestart();
    void watchdogStop();

    // esp_timer callback indirection — arg is `this`. Callback ONLY posts
    // OTA_WR_WATCHDOG_FIRE; state mutation happens on otaWriterTask.
    static void watchdogTimerCb(void *arg);

    // Emits an OTA_BEGIN_ACK / NAK frame via AstrOs_EspNow.sendOtaFrame.
    // Returns esp_err_t from the underlying send. Caller logs but does not
    // act on the result — a failed reply will be re-elicited by the
    // master's tick-driven retransmit.
    esp_err_t sendBeginAck(const uint8_t mac[6], uint8_t xferId);
    esp_err_t sendBeginNak(const uint8_t mac[6], uint8_t xferId, OtaBeginNakReason reason);
    esp_err_t sendDataAck(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                          uint8_t windowRemaining);
    esp_err_t sendDataNak(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                          uint8_t windowRemaining, OtaDataNakReason reason);
    esp_err_t sendEndAck(const uint8_t mac[6], uint8_t xferId, OtaEndStatus status, const uint8_t sha256Computed[32]);

    // Active gate (read by pollingTimer's task).
    std::atomic<bool> active_{false};

    // Queue handle held so timer callback can post WATCHDOG_FIRE into the
    // same queue otaWriterTask drains.
    QueueHandle_t otaWriterQueue_ = nullptr;

    // Idle watchdog (10 s — mirrors OtaReceiver; resolves design-doc
    // open question #4).
    esp_timer_handle_t watchdog_ = nullptr;
    static constexpr uint64_t kWatchdogIdleUs = 10ULL * 1000ULL * 1000ULL;

    // BulkReceiver (existing PURE lib) handles seq tracking + windowing.
    AstrOsBulkTransport::BulkReceiver bulk_;

    // Per-transfer state — live between handleBegin success and the
    // terminating handleEnd / abort. resetOtaHandleAndSha() restores
    // all-zero / nullptr.
    const esp_partition_t *inactivePartition_ = nullptr;
    esp_ota_handle_t otaHandle_ = 0; // 0 means "no handle open"
    AstrOsSha256Ctx shaCtx_;
    bool shaActive_ = false; // gates init-vs-update ordering without explicit free
    uint8_t currentXferId_ = 0;
    uint8_t currentMasterMac_[6] = {0};
    uint32_t currentTotalSize_ = 0;
    uint8_t expectedSha256_[32] = {0};
};

extern OtaWriter AstrOs_OtaWriter;

#endif
```

- [ ] **Step 3: Create the skeleton implementation**

Create `lib/OtaWriter/src/OtaWriter.cpp` with the skeleton (Tasks 5/6/7 fill in the handler bodies; Task 8 fills in handleWatchdogFire):

```cpp
#include <OtaWriter.hpp>

#include <AstrOsEspNowService.hpp>
#include <esp_log.h>

#include <cstring>

static const char *TAG = "OtaWriter";

OtaWriter AstrOs_OtaWriter;

OtaWriter::OtaWriter() {}

OtaWriter::~OtaWriter()
{
    // The singleton is process-lifetime in firmware; this dtor exists for
    // native-link symmetry (when/if a future host build links OtaWriter).
    // esp_timer_stop on an idle timer is harmless.
    if (watchdog_ != nullptr)
    {
        esp_timer_stop(watchdog_);
        esp_timer_delete(watchdog_);
        watchdog_ = nullptr;
    }
    resetOtaHandleAndSha();
}

void OtaWriter::Init(QueueHandle_t otaWriterQueue)
{
    // Idempotent: a second Init() would leak the first esp_timer handle.
    if (watchdog_ != nullptr)
    {
        ESP_LOGW(TAG, "Init() called twice — ignoring second call");
        return;
    }

    otaWriterQueue_ = otaWriterQueue;

    const esp_timer_create_args_t args = {
        .callback = &OtaWriter::watchdogTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_writer_watchdog",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &watchdog_);
    if (err != ESP_OK)
    {
        // Writer still serves happy-path transfers without the watchdog —
        // only stuck-recovery is lost. Mirrors OtaReceiver's choice.
        ESP_LOGE(TAG, "esp_timer_create(ota_writer_watchdog) failed: %s — watchdog disabled", esp_err_to_name(err));
        watchdog_ = nullptr;
    }

    ESP_LOGI(TAG, "OtaWriter initialized (watchdog idle threshold: %llums)", kWatchdogIdleUs / 1000ULL);
}

void OtaWriter::watchdogTimerCb(void *arg)
{
    auto self = static_cast<OtaWriter *>(arg);
    if (self->otaWriterQueue_ == nullptr)
    {
        return;
    }
    queue_ota_writer_msg_t msg{};
    msg.kind = OTA_WR_WATCHDOG_FIRE;
    // Non-blocking — a full queue means otaWriterTask is already wedged;
    // dropping the signal is the least-bad outcome.
    if (xQueueSend(self->otaWriterQueue_, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "watchdog: otaWriterQueue full, dropping WATCHDOG_FIRE signal");
    }
}

void OtaWriter::watchdogStart()
{
    if (watchdog_ == nullptr)
        return;
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaWriter::watchdogRestart()
{
    if (watchdog_ == nullptr)
        return;
    // esp_timer's one-shot has no native restart; stop-then-start.
    // esp_timer_stop on a non-running timer returns ESP_ERR_INVALID_STATE — benign.
    esp_timer_stop(watchdog_);
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: restart esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaWriter::watchdogStop()
{
    if (watchdog_ == nullptr)
        return;
    esp_timer_stop(watchdog_);
}

void OtaWriter::resetOtaHandleAndSha()
{
    if (otaHandle_ != 0)
    {
        // esp_ota_abort releases the handle. Return value isn't actionable
        // here — we're already on the abort path.
        esp_err_t err = esp_ota_abort(otaHandle_);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_ota_abort returned %s — partition may be in inconsistent state", esp_err_to_name(err));
        }
        otaHandle_ = 0;
    }
    // AstrOsSha256 owns no heap; just drop the active flag.
    shaActive_ = false;
    bulk_.reset();
    inactivePartition_ = nullptr;
    currentXferId_ = 0;
    memset(currentMasterMac_, 0, sizeof(currentMasterMac_));
    currentTotalSize_ = 0;
    memset(expectedSha256_, 0, sizeof(expectedSha256_));
    active_ = false;
}

void OtaWriter::process(queue_ota_writer_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_WR_BEGIN:
        handleBegin(msg);
        break;
    case OTA_WR_DATA:
        handleData(msg);
        break;
    case OTA_WR_END:
        handleEnd(msg);
        break;
    case OTA_WR_WATCHDOG_FIRE:
        handleWatchdogFire();
        break;
    default:
        ESP_LOGE(TAG, "process: unknown msg.kind=%d", (int)msg.kind);
        break;
    }
    // Convention (mirrors OtaReceiver::process / OtaForwarder::process):
    // process() owns the free contract. Callers (otaWriterTask) must NOT
    // free externally — doing so would double-free.
    freeOtaWriterMsg(&msg);
}

// ─── Stubbed handlers — Tasks 5/6/7/8 fill in the bodies ────────────────

void OtaWriter::handleBegin(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBegin: stubbed (Task 5) — xferId=%u totalSize=%u", msg.begin.xferId, msg.begin.totalSize);
}

void OtaWriter::handleData(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleData: stubbed (Task 6) — xferId=%u seq=%u", msg.data.xferId, msg.data.seq);
}

void OtaWriter::handleEnd(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleEnd: stubbed (Task 7) — xferId=%u chunks=%u", msg.end.xferId, msg.end.totalChunksSent);
}

void OtaWriter::handleWatchdogFire()
{
    ESP_LOGW(TAG, "handleWatchdogFire: stubbed (Task 8)");
}

// ─── Wire-emission helpers — Tasks 5/6/7 use these ──────────────────────

esp_err_t OtaWriter::sendBeginAck(const uint8_t mac[6], uint8_t xferId)
{
    OtaBeginAckPayload p{};
    p.xferId = xferId;
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendBeginNak(const uint8_t mac[6], uint8_t xferId, OtaBeginNakReason reason)
{
    OtaBeginNakPayload p{};
    p.xferId = xferId;
    p.reason = static_cast<uint8_t>(reason);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_BEGIN_NAK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendDataAck(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq,
                                 uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    OtaDataAckPayload p{};
    p.xferId = xferId;
    p.highestContiguousSeq = highestContiguousSeq;
    p.nextExpectedSeq = nextExpectedSeq;
    p.windowRemaining = windowRemaining;
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_DATA_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendDataNak(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq,
                                 uint32_t nextExpectedSeq, uint8_t windowRemaining, OtaDataNakReason reason)
{
    OtaDataNakPayload p{};
    p.xferId = xferId;
    p.highestContiguousSeq = highestContiguousSeq;
    p.nextExpectedSeq = nextExpectedSeq;
    p.windowRemaining = windowRemaining;
    p.reason = static_cast<uint8_t>(reason);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_DATA_NAK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendEndAck(const uint8_t mac[6], uint8_t xferId, OtaEndStatus status,
                                const uint8_t sha256Computed[32])
{
    OtaEndAckPayload p{};
    p.xferId = xferId;
    p.status = static_cast<uint8_t>(status);
    memcpy(p.sha256Computed, sha256Computed, 32);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_END_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}
```

- [ ] **Step 4: Verify both boards build clean**

Run: `pio run -e metro_s3 2>&1 | tail -20`
Expected: clean build. `OtaWriter` is now linked into the padawan image but isn't reachable until Task 3 wires the task + Task 4 reroutes dispatch.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -20`
Expected: clean build.

Run: `pio test -e test 2>&1 | tail -5`
Expected: no change from Task 1's total (no new native tests in T2 — `OtaWriter` is MIXED).

- [ ] **Step 5: Commit**

```bash
git add lib/OtaWriter/library.json lib/OtaWriter/README lib/OtaWriter/include/OtaWriter.hpp lib/OtaWriter/src/OtaWriter.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T2 — skeleton, watchdog, stubbed handlers

Lays down the OtaWriter MIXED lib: class declaration mirroring OtaForwarder's
shape, Init() two-step, process() dispatcher, idempotent resetOtaHandleAndSha,
10s idle watchdog (resolves design open-Q #4), and the five wire-emission
helpers (sendBeginAck/Nak, sendDataAck/Nak, sendEndAck) that wrap the
AstrOs_EspNow.sendOtaFrame binary TX path from M3.

Handler bodies are stubs that ESP_LOGW their kind — Tasks 5/6/7/8 fill them.
After T3 wires the task + T4 reroutes dispatch, the firmware will emit the
stub logs on OTA_BEGIN arrival; real handler bodies replace the logs in
later tasks.

M4 Task 2 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `main.cpp` wiring — queue, task, polling-pause gate

Creates `otaWriterQueue` (padawan only), spawns `otaWriterTask` pinned to core 1 (padawan only), extends the padawan polling-pause gate, plumbs the queue handle into the singleton + into `AstrOs_EspNow`. After this task, the padawan firmware boots with `otaWriterTask` draining an empty queue (no producer wired yet — Task 4 lights the producer side).

The role-gating discipline (forwarder allocations gated behind `isMasterNode`) is mirrored here for the padawan side (writer allocations gated behind `!isMasterNode`). Master nodes get neither the writer task nor the queue, saving ~4 KB of RAM.

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Read the current padawan polling-pause gate**

Read `src/main.cpp:460-495` to confirm the shape:

```bash
sed -n '460,495p' src/main.cpp
```

Expected: `pollingTimerCallback` with a master branch gated on `OtaReceiver::isActive() || OtaForwarder::isActive()` and a padawan branch with no OTA gate (today, padawan polling is just `handleMaintenanceTimerExpired`).

- [ ] **Step 2: Add the queue + task forward declarations**

In `src/main.cpp`, near the existing queue declarations (around line 82, after `otaForwarderQueue`), add:

```cpp
static QueueHandle_t otaWriterQueue;
```

Near the existing task forward declarations (around line 187, after `otaForwarderTask`), add:

```cpp
void otaWriterTask(void *arg);
```

Include the OtaWriter header near the other OTA-related includes (find where `<OtaForwarder.hpp>` is included; add):

```cpp
#include <OtaWriter.hpp>
```

- [ ] **Step 3: Create the queue (padawan only)**

Find the existing `otaForwarderQueue = xQueueCreate(...)` block (around line 303-318) which is gated on `isMasterNode.load()`. Immediately after that block, add the mirror for padawans:

```cpp
    // otaWriterQueue is padawan-only: master never receives OTA_BEGIN /
    // OTA_DATA / OTA_END (those are master→padawan directional frames).
    // Keeping the queue padawan-only saves the queue's 16-slot * sizeof
    // allocation on master nodes.
    if (!isMasterNode.load())
    {
        otaWriterQueue = xQueueCreate(16, sizeof(queue_ota_writer_msg_t));
        if (otaWriterQueue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create otaWriterQueue — aborting init");
            abort();
        }
    }
```

- [ ] **Step 4: Plumb the queue handle into AstrOs_OtaWriter + AstrOs_EspNow**

Find the existing `AstrOs_OtaForwarder.Init(otaForwarderQueue);` + `AstrOs_EspNow.setOtaForwarderQueue(otaForwarderQueue);` calls (around line 327-330, inside the `if (isMasterNode.load())` block). Below that closing brace, add the padawan-side mirror:

```cpp
    if (!isMasterNode.load())
    {
        AstrOs_OtaWriter.Init(otaWriterQueue);
        AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue);
    }
```

(The `setOtaWriterQueue` method is declared + implemented in Task 4. For T3 it does not exist yet — confirm by compiling at the end of T3; you'll get a missing-method error and you'll skip ahead to T4 to add it before T3 commits cleanly. Alternative: add a temporary forward-declaration stub in `AstrOsEspNowService.hpp` that does nothing; remove the stub when T4 implements the real one. The cleanest sequencing is to land T3's `main.cpp` changes WITHOUT the `setOtaWriterQueue` call, ship T4, then circle back to add the call. We'll take the linear path: T3 leaves the `setOtaWriterQueue` call out, T4 adds it.)

**Adjustment**: replace the snippet above with:

```cpp
    if (!isMasterNode.load())
    {
        AstrOs_OtaWriter.Init(otaWriterQueue);
        // setOtaWriterQueue wired in T4 once the method exists.
    }
```

T4 will edit this snippet to add the `setOtaWriterQueue` call.

- [ ] **Step 5: Spawn the task (padawan only)**

Find the existing `otaForwarderTask` spawn block (around line 231-238), which uses `xTaskCreatePinnedToCore` gated on `isMasterNode.load()`. Immediately below that block, add the mirror:

```cpp
    if (!isMasterNode.load())
    {
        // 8 KB stack (not 4 KB matching the receiver/forwarder family):
        // OtaWriter::handleEnd declares a 4 KB read-back buffer on the stack
        // alongside SHA-256 ctx + call frames. T7 code review flagged that
        // 4 KB + ~1 KB frames + FreeRTOS context exceeds a 4 KB stack.
        // Bench validation can tune this down if HWM shows comfortable margin.
        if (xTaskCreatePinnedToCore(&otaWriterTask, "ota_writer_task", 8192, (void *)otaWriterQueue, 6, NULL, 1) !=
            pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create otaWriterTask — aborting init");
            abort();
        }
    }
```

Note: stack is 8 KB (vs the 4 KB used by `otaReceiverTask` / `otaForwarderTask`). T7's code review flagged that handleEnd's 4 KB readback buffer + FreeRTOS context + call frames will overflow a 4 KB stack. Bench validation may tune this down if HWM shows comfortable margin (target ≥ 1 KB remaining); the design doc Task 144 captures this trade-off.

- [ ] **Step 6: Extend the padawan polling-pause gate**

Find the existing `pollingTimerCallback` (around line 460-490). Currently the master branch gates on:

```cpp
const bool otaActive = AstrOs_OtaReceiver.isActive() || AstrOs_OtaForwarder.isActive();
```

The padawan branch (look for `else` / non-master path inside the same callback) currently has no OTA gate. Add one symmetric to the master branch — the padawan also runs maintenance work that should pause while an OTA write is in flight, because `esp_ota_write`'s per-sector erase + write contends with SPI flash access on the same task affinity.

Locate the padawan branch (look for the comment block "// padawan: maintenance" or similar pattern; if not present, the padawan branch may be only the maintenance timer dispatch — read `pollingTimerCallback` end-to-end before editing). Modify the padawan branch to:

```cpp
    // Padawan-side OTA gate: writer is active while a transfer is in
    // flight. Pause maintenance work to keep SPI flash contention down
    // (esp_ota_write erase+program latency degrades when other tasks
    // hit the flash bus concurrently).
    if (AstrOs_OtaReceiver.isActive() || AstrOs_OtaWriter.isActive())
    {
        return;
    }
```

(If the padawan branch already has an `OtaReceiver::isActive()` gate from the Phase 3 serial-OTA work, extend it to OR with `OtaWriter::isActive()` instead of adding a duplicate `return`.)

- [ ] **Step 7: Add the task body**

Find the existing `otaForwarderTask` body (around line 1515-1530). Below it (or in the same neighborhood as the other OTA task bodies), add:

```cpp
void otaWriterTask(void *arg)
{
    auto queue = static_cast<QueueHandle_t>(arg);
    queue_ota_writer_msg_t msg;
    for (;;)
    {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            // process() internally calls freeOtaWriterMsg — do NOT free here.
            // Matches otaReceiverTask + otaForwarderTask convention.
            AstrOs_OtaWriter.process(msg);
        }
    }
}
```

`portMAX_DELAY` is used here matching `otaReceiverTask` and `otaForwarderTask`'s patterns — these tasks have nothing else to do between messages and should block indefinitely on the queue. (CLAUDE.md flags `portMAX_DELAY` as a known footgun "for new code"; the OTA task family uses it consistently and bench-tested clean. A future cleanup-pass plan can switch to `pdMS_TO_TICKS(...)` with idle-loop self-checks, but that's out of scope for M4.)

- [ ] **Step 8: Build both boards (and recognize the expected error)**

Run: `pio run -e metro_s3 2>&1 | tail -20`
Expected: BUILD FAILS with `'class AstrOsEspNow' has no member named 'setOtaWriterQueue'` — this is the linear-sequencing artifact from Step 4. **Continue to Task 4 immediately**; don't commit T3 yet.

Skip directly into Task 4. Task 4's commit message will say "T3 + T4 (rolled together due to sequencing)" and Step 7 of Task 4 commits both T3's main.cpp changes and T4's dispatcher changes in a single commit. This is intentional: T3's main.cpp changes alone don't compile, so they can't ship as a standalone commit.

(Alternative: skip the `setOtaWriterQueue` reference in T3 entirely and add it as the *only* main.cpp change of T4. We'll commit T3 as a standalone commit using this alternative; T4 then layers on top. **Use the alternative.** Remove the "setOtaWriterQueue wired in T4" comment from T3's Init block — T3 only does `AstrOs_OtaWriter.Init(otaWriterQueue)` for now.)

**Correction to Step 4**: T3's `main.cpp` edit reads:

```cpp
    if (!isMasterNode.load())
    {
        AstrOs_OtaWriter.Init(otaWriterQueue);
    }
```

No reference to `setOtaWriterQueue`. The setter call is added in T4.

Re-run: `pio run -e metro_s3 2>&1 | tail -20`
Expected: clean build now.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -20`
Expected: clean build.

Run: `pio test -e test 2>&1 | tail -5`
Expected: no change from T2's total.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T3 — main.cpp wiring (queue, task, polling-pause)

Creates otaWriterQueue (padawan only, 16 slots), spawns otaWriterTask
pinned to core 1 (padawan only, 4 KB stack matching otaReceiver/forwarder),
extends the padawan polling-pause gate to OR in OtaWriter::isActive().

Producer side (dispatcher) lights up in T4 — for now otaWriterTask drains
an empty queue. AstrOs_OtaWriter.Init() runs on padawan boot to create the
watchdog timer and hold the queue handle.

M4 Task 3 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `AstrOsEspNow` padawan-side OTA dispatch

Replaces M3's "M4 not wired" `ESP_LOGW` stub at `AstrOsEspNowService.cpp:400-409` with real parsing + queueing. Adds `setOtaWriterQueue` setter mirror to `setOtaForwarderQueue` from M3. The handler `routeOtaToWriter` mirrors M3's `routeOtaAckNakToForwarder` — parse the packet via M1's `parseOta*` free functions, fill an inline `queue_ota_writer_msg_t`, post non-blocking. **Special case: `OTA_DATA` requires a malloc + memcpy** because the parsed payload pointer is into the soon-to-be-freed packet buffer.

After this task the padawan firmware fully wires the producer → consumer pipeline. Sending `FW_DEPLOY_BEGIN` from a master + 1 padawan on the bench should now drive `OTA_BEGIN` → padawan's `OtaWriter::handleBegin` (still stubbed, just an `ESP_LOGW`). Tasks 5/6/7/8 fill in the handler bodies; this task makes the dispatch visible end-to-end.

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`
- Modify: `src/main.cpp` (one new line — `setOtaWriterQueue` call)

- [ ] **Step 1: Declare the queue setter + private helper**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`, find the existing `setOtaForwarderQueue` declaration (around the public section after `setOtaForwarderQueue(QueueHandle_t q)`). Add the mirror:

```cpp
    // Padawan-only. Setter for the otaWriterQueue used by the OTA arm of
    // the residual switch. nullptr leaves the dispatcher in the M3 "stub"
    // state (logs + drops). Master sets nullptr or skips the call entirely.
    void setOtaWriterQueue(QueueHandle_t q);
```

Find the existing `otaForwarderQueue_` private member (around line 52) — add the mirror just below:

```cpp
    QueueHandle_t otaWriterQueue_ = nullptr;
```

Find the existing private `routeOtaAckNakToForwarder` helper declaration. Add the mirror just below:

```cpp
    // Padawan-side OTA dispatcher. Parses OTA_BEGIN / OTA_DATA / OTA_END
    // via parseOta*, fills a queue_ota_writer_msg_t (OTA_DATA additionally
    // mallocs + memcpys the payload because parseOtaData returns a pointer
    // into the soon-to-be-freed packet buffer), posts to otaWriterQueue_.
    // Returns true if the packet was handled (queued or intentionally
    // dropped); false if a parse error means the dispatcher's residual
    // switch should log an unhandled-packet diagnostic.
    bool routeOtaToWriter(const uint8_t *src, const astros_packet_t &packet);
```

- [ ] **Step 2: Implement the setter**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, find the existing `setOtaForwarderQueue` implementation (around line 417-420). Just below it, add:

```cpp
void AstrOsEspNow::setOtaWriterQueue(QueueHandle_t q)
{
    this->otaWriterQueue_ = q;
}
```

- [ ] **Step 3: Replace the M3 stub with the real dispatch**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:400-409`, find the M3 stub:

```cpp
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

Replace it with:

```cpp
    case AstrOsPacketType::OTA_BEGIN:
    case AstrOsPacketType::OTA_DATA:
    case AstrOsPacketType::OTA_END:
        return this->routeOtaToWriter(src, packet);
```

Then, just below the existing `routeOtaAckNakToForwarder` function (after line 530 or wherever it ends — find the closing brace), add the new helper:

```cpp
bool AstrOsEspNow::routeOtaToWriter(const uint8_t *src, const astros_packet_t &packet)
{
    if (this->isMasterNode)
    {
        // Master receiving a master→padawan OTA frame is a protocol error
        // — log + drop. Returning true marks it handled so the residual
        // switch doesn't escalate to "no residual handler exists".
        ESP_LOGW(TAG, "OTA master→padawan packet type=%d received on master; dropping (wrong role)",
                 (int)packet.packetType);
        return true;
    }
    if (this->otaWriterQueue_ == nullptr)
    {
        ESP_LOGW(TAG, "OTA writer-bound packet type=%d received before otaWriterQueue_ set; dropping",
                 (int)packet.packetType);
        return true;
    }

    queue_ota_writer_msg_t m{};

    switch (packet.packetType)
    {
    case AstrOsPacketType::OTA_BEGIN:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaBegin(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_BEGIN parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_WR_BEGIN;
        memcpy(m.begin.srcMac, src, ESP_NOW_ETH_ALEN);
        m.begin.xferId = rec.xferId;
        m.begin.totalSize = rec.totalSize;
        m.begin.chunkSize = rec.chunkSize;
        m.begin.totalChunks = rec.totalChunks;
        memcpy(m.begin.sha256Expected, rec.sha256Expected, 32);
        m.begin.flags = rec.flags;
        break;
    }
    case AstrOsPacketType::OTA_DATA:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaData(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_DATA parse rejected (malformed wire bytes)");
            return false;
        }
        // CRITICAL: rec.payload points INTO the packet buffer that the
        // dispatcher returns from. We MUST copy the bytes before queueing,
        // otherwise the consumer will read freed memory. freeOtaWriterMsg
        // releases this malloc.
        uint8_t *payloadCopy = static_cast<uint8_t *>(malloc(rec.payloadLen));
        if (payloadCopy == nullptr)
        {
            ESP_LOGE(TAG, "OTA_DATA payload malloc(%u) failed — dropping chunk seq=%u",
                     (unsigned)rec.payloadLen, (unsigned)rec.seq);
            return true; // handled (dropped); master's tick-retransmit will re-send
        }
        memcpy(payloadCopy, rec.payload, rec.payloadLen);

        m.kind = OTA_WR_DATA;
        memcpy(m.data.srcMac, src, ESP_NOW_ETH_ALEN);
        m.data.xferId = rec.xferId;
        m.data.seq = rec.seq;
        m.data.payloadLen = rec.payloadLen;
        m.data.crc16 = rec.crc16;
        m.data.payload = payloadCopy;
        break;
    }
    case AstrOsPacketType::OTA_END:
    {
        auto rec = AstrOsEspNowProtocol::parseOtaEnd(packet);
        if (!rec.valid)
        {
            ESP_LOGW(TAG, "OTA_END parse rejected (malformed wire bytes)");
            return false;
        }
        m.kind = OTA_WR_END;
        memcpy(m.end.srcMac, src, ESP_NOW_ETH_ALEN);
        m.end.xferId = rec.xferId;
        m.end.totalChunksSent = rec.totalChunksSent;
        memcpy(m.end.sha256Final, rec.sha256Final, 32);
        break;
    }
    default:
        ESP_LOGE(TAG, "routeOtaToWriter: unexpected packet type %d", (int)packet.packetType);
        return false;
    }

    if (xQueueSend(this->otaWriterQueue_, &m, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGE(TAG, "otaWriterQueue full; dropping OTA packet type=%d", (int)packet.packetType);
        freeOtaWriterMsg(&m);
        // Return true (handled, dropped intentionally) so handleMessage's
        // residual switch doesn't escalate. The dropped chunk will be
        // reconstructed by the master's tick-driven retransmit.
        return true;
    }
    return true;
}
```

Add the header include for the new queue-message type near the existing OTA-related includes in `AstrOsEspNowService.cpp`:

```cpp
#include <OtaWriterQueueMessage.h>
```

- [ ] **Step 4: Plumb the queue handle in main.cpp**

In `src/main.cpp`, find the T3 padawan-side Init block:

```cpp
    if (!isMasterNode.load())
    {
        AstrOs_OtaWriter.Init(otaWriterQueue);
    }
```

Extend it with the setter call:

```cpp
    if (!isMasterNode.load())
    {
        AstrOs_OtaWriter.Init(otaWriterQueue);
        AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue);
    }
```

- [ ] **Step 5: Build both boards**

Run: `pio run -e metro_s3 2>&1 | tail -20`
Expected: clean build. The padawan dispatcher now decodes OTA_BEGIN/DATA/END into queue messages.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -20`
Expected: clean build.

Run: `pio test -e test 2>&1 | tail -5`
Expected: no change from T3's total (no native tests in T4 — the dispatch logic uses MIXED-layer ESP_LOG + xQueueSend that can't link in `[env:test]`).

- [ ] **Step 6: Smoke-test the dispatch on bench (optional but recommended before T5)**

Flash the metro_s3 image to a master + a padawan. Trigger `FW_DEPLOY_BEGIN` from the server UI. Monitor padawan serial:

```bash
pio device monitor -e metro_s3
```

Expected padawan-side log sequence:
1. `OtaWriter` init log on boot
2. After `FW_DEPLOY_BEGIN`: `handleBegin: stubbed (Task 5) — xferId=N totalSize=M` (the T2 stub log)
3. After master's BEGIN_ACK timeout fires (no real ACK from padawan since handleBegin is stubbed), master emits `FW_DEPLOY_DONE` with FAILED status. Master-side log: `forwarder_done: pad1=FAILED reason="begin_ack_timeout"`.

If the padawan log does NOT show the stubbed-handleBegin line, the dispatcher is misrouting. Verify:
- `parseOtaBegin` returns `valid=true` (add a temporary `ESP_LOGI` if needed)
- `otaWriterQueue_` is non-null on the padawan (the Init order is `Init() → setOtaWriterQueue()` — they run back-to-back in main.cpp)
- `routeOtaToWriter` is invoked (not the wrong-role drop log)

- [ ] **Step 7: Commit T3 + T4 (or commit T4 separately if T3 already shipped)**

If T3 was committed in isolation per the alternative path, this commit covers only T4's changes:

```bash
git add lib/AstrOsEspNow/src/AstrOsEspNowService.hpp lib/AstrOsEspNow/src/AstrOsEspNowService.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T4 — padawan-side OTA dispatch

Replaces M3's "M4 not wired" stub at AstrOsEspNowService.cpp:400-409 with
real parsing + queueing. parseOta{Begin,Data,End} produce records, fill
queue_ota_writer_msg_t, post to otaWriterQueue.

OTA_DATA additionally mallocs + memcpys the payload bytes because
parseOtaData returns a pointer into the soon-to-be-freed packet buffer
— skipping the copy would let the consumer read freed memory. The
malloc'd buffer is released by freeOtaWriterMsg after the consumer
(handleData in T6) calls esp_ota_write.

setOtaWriterQueue setter mirrors M3's setOtaForwarderQueue. main.cpp
wires the queue handle on padawan boot.

After this commit, the producer → consumer pipeline is fully wired but
handleBegin/Data/End are still stubs that ESP_LOGW their kind. T5/T6/T7
fill in the real bodies.

M4 Task 4 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `handleBegin` — partition lookup, esp_ota_begin, ACK or NAK

Implements `OtaWriter::handleBegin`. Reject if busy → `OTA_BEGIN_NAK BUSY`; look up inactive partition + size-check → `NAK NO_PARTITION` on failure; `esp_ota_begin` → `NAK BEGIN_FAILED` on failure; `BulkReceiver::begin` (set up seq tracking); init SHA; ACK + start watchdog. Every failure path routes through `resetOtaHandleAndSha()` so partial state doesn't leak across BEGINs.

**Files:**
- Modify: `lib/OtaWriter/src/OtaWriter.cpp`

- [ ] **Step 1: Replace the handleBegin stub**

In `lib/OtaWriter/src/OtaWriter.cpp`, find the T2 stub:

```cpp
void OtaWriter::handleBegin(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBegin: stubbed (Task 5) — xferId=%u totalSize=%u", msg.begin.xferId, msg.begin.totalSize);
}
```

Replace with the full implementation:

```cpp
void OtaWriter::handleBegin(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.begin.srcMac;
    const uint8_t xferId = msg.begin.xferId;

    if (active_)
    {
        ESP_LOGW(TAG, "handleBegin: xferId=%u arrived while xferId=%u is active — replying BUSY", xferId,
                 currentXferId_);
        sendBeginNak(mac, xferId, OtaBeginNakReason::BUSY);
        return;
    }

    // BulkReceiver wants a uint8_t xferId; rec.xferId is already a uint8_t
    // from the wire, no parse needed.

    // Inactive partition lookup. esp_ota_get_next_update_partition(NULL)
    // returns the slot that's NOT currently the boot partition. On a fresh
    // factory image where ota_data hasn't been written yet, ESP-IDF picks
    // ota_0; once we've ever booted from ota_0, this returns ota_1.
    inactivePartition_ = esp_ota_get_next_update_partition(NULL);
    if (inactivePartition_ == nullptr)
    {
        ESP_LOGE(TAG, "handleBegin: esp_ota_get_next_update_partition returned NULL — partition table is missing ota_0/ota_1");
        sendBeginNak(mac, xferId, OtaBeginNakReason::NO_PARTITION);
        return;
    }

    if (msg.begin.totalSize > inactivePartition_->size)
    {
        ESP_LOGW(TAG, "handleBegin: totalSize=%u exceeds partition '%s' size=%u — NAK NO_PARTITION",
                 (unsigned)msg.begin.totalSize, inactivePartition_->label, (unsigned)inactivePartition_->size);
        inactivePartition_ = nullptr;
        sendBeginNak(mac, xferId, OtaBeginNakReason::NO_PARTITION);
        return;
    }

    // esp_ota_begin allocates internal state + reserves the partition for
    // writing. OTA_SIZE_UNKNOWN tells it to erase only the sectors we
    // actually write (lazy per-sector erase inside esp_ota_write). Passing
    // the exact totalSize would let ESP-IDF erase the whole reserved span
    // up front — that's a one-shot erase of ~2 MB (8MB board) or ~6.4 MB
    // (16MB board), which can exceed the BEGIN_ACK timeout window. Lazy
    // per-sector erase keeps the M4 happy-path latency budget intact;
    // bench data via the M4 checkpoint validates this assumption
    // (resolves design open-Q #5).
    esp_err_t bErr = esp_ota_begin(inactivePartition_, OTA_SIZE_UNKNOWN, &otaHandle_);
    if (bErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleBegin: esp_ota_begin failed: %s — NAK BEGIN_FAILED", esp_err_to_name(bErr));
        otaHandle_ = 0;
        inactivePartition_ = nullptr;
        sendBeginNak(mac, xferId, OtaBeginNakReason::BEGIN_FAILED);
        return;
    }

    // BulkReceiver windowSize matches the master's BulkSender window (8) —
    // see lib/OtaForwarder/include/OtaForwarder.hpp:156's kWindowSize.
    constexpr uint8_t kWindowSize = 8;
    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kWindowSize);
    if (!br.valid)
    {
        ESP_LOGW(TAG, "handleBegin: BulkReceiver::begin rejected: reason=%d (totalSize=%u chunks=%u chunkSize=%u)",
                 (int)br.reason, (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks,
                 (unsigned)msg.begin.chunkSize);
        // esp_ota_begin succeeded but BulkReceiver setup failed —
        // resetOtaHandleAndSha clears the partial state.
        resetOtaHandleAndSha();
        sendBeginNak(mac, xferId, OtaBeginNakReason::BEGIN_FAILED);
        return;
    }

    AstrOsSha256_init(&shaCtx_);
    shaActive_ = true;

    // Latch per-transfer state.
    currentXferId_ = xferId;
    memcpy(currentMasterMac_, mac, 6);
    currentTotalSize_ = msg.begin.totalSize;
    memcpy(expectedSha256_, msg.begin.sha256Expected, 32);
    active_ = true;

    ESP_LOGI(TAG,
             "handleBegin accepted: xferId=%u totalSize=%u chunks=%u chunkSize=%u partition='%s' (size=%u, offset=0x%lx)",
             xferId, (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, (unsigned)msg.begin.chunkSize,
             inactivePartition_->label, (unsigned)inactivePartition_->size,
             (unsigned long)inactivePartition_->address);

    esp_err_t sendErr = sendBeginAck(mac, xferId);
    if (sendErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleBegin: sendBeginAck failed: %s — relying on master tick-retransmit", esp_err_to_name(sendErr));
        // Don't abort the transfer on ACK send failure — master's
        // BEGIN_ACK timeout (2 s) will fire if the ACK never lands, at
        // which point master retransmits OTA_BEGIN. Padawan-side state
        // stays committed; the second BEGIN will hit the BUSY path above
        // and return another ACK attempt.
    }
    watchdogStart();
}
```

- [ ] **Step 2: Verify both boards build**

Run: `pio run -e metro_s3 2>&1 | tail -15`
Expected: clean build.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -15`
Expected: clean build.

- [ ] **Step 3: Bench smoke-test (manual)**

Flash both boards, trigger `FW_DEPLOY_BEGIN`. Padawan log should now show:

```
I (...) OtaWriter: handleBegin accepted: xferId=N totalSize=M chunks=K ...
```

Master-side log should show `OTA_BEGIN_ACK` received from the padawan (handled by the M3 forwarder's `handleBeginAck`). Master then attempts to stream OTA_DATA frames; padawan's `handleData` is still stubbed, so chunks accumulate in the master's window until the data-ack timeout fires, at which point the master abandons the transfer.

The bench observation here is: ACK lands, master begins streaming, master eventually times out. **This validates the partition lookup + esp_ota_begin + ACK round trip**, which is the M4 risk the design doc flagged for early bench validation.

- [ ] **Step 4: Commit**

```bash
git add lib/OtaWriter/src/OtaWriter.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T5 — handleBegin implementation

Implements the BEGIN side of the OtaWriter state machine: busy check,
inactive-partition lookup via esp_ota_get_next_update_partition, size-check
against partition capacity, esp_ota_begin with OTA_SIZE_UNKNOWN (lazy
per-sector erase to stay inside the 2s BEGIN_ACK budget — resolves design
open-Q #5), BulkReceiver::begin with windowSize=8 (mirrors BulkSender),
AstrOsSha256_init, and OTA_BEGIN_ACK reply + watchdog start.

Every failure path routes through resetOtaHandleAndSha so partial state
(opened OTA handle, half-init'd BulkReceiver) doesn't leak across BEGINs.

handleData / handleEnd / handleWatchdogFire are still stubs — T6/T7/T8
fill them.

M4 Task 5 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `handleData` — BulkReceiver.onChunk, esp_ota_write, sha update, ACK or NAK

Implements `OtaWriter::handleData`. Inactive-receiver NAK if `!active_`; `BulkReceiver::onChunk` → if NAK, emit `OTA_DATA_NAK` with the receiver's hint; if ACK, `esp_ota_write` (on failure → NAK + reset), `AstrOsSha256_update`, `OTA_DATA_ACK`, `watchdogRestart`. This is the hot path; logging is intentionally sparse (one ESP_LOGD per accepted chunk, ESP_LOGW only on NAK paths).

**Files:**
- Modify: `lib/OtaWriter/src/OtaWriter.cpp`

- [ ] **Step 1: Replace the handleData stub**

In `lib/OtaWriter/src/OtaWriter.cpp`, find the T2 stub:

```cpp
void OtaWriter::handleData(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleData: stubbed (Task 6) — xferId=%u seq=%u", msg.data.xferId, msg.data.seq);
}
```

Replace with:

```cpp
void OtaWriter::handleData(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.data.srcMac;
    const uint8_t xferId = msg.data.xferId;
    const uint32_t seq = msg.data.seq;

    if (!active_)
    {
        // Inactive NAK: chunk arrived while no transfer is live. Use the
        // wire's xferId (not currentXferId_) since the master is still
        // tagging by its in-flight transfer.
        ESP_LOGW(TAG, "handleData: chunk for xferId=%u seq=%u arrived while inactive — NAK OUT_OF_ORDER",
                 xferId, seq);
        sendDataNak(mac, xferId, /*highestContiguousSeq=*/0, /*nextExpectedSeq=*/0,
                    /*windowRemaining=*/0, OtaDataNakReason::OUT_OF_ORDER);
        return;
    }

    auto cr = bulk_.onChunk(xferId, seq, msg.data.payloadLen, msg.data.crc16, msg.data.payload);

    if (cr.decision == AstrOsBulkTransport::Decision::NAK)
    {
        OtaDataNakReason wireReason = OtaDataNakReason::OUT_OF_ORDER;
        switch (cr.reason)
        {
        case AstrOsBulkTransport::NakReason::CRC:
            wireReason = OtaDataNakReason::CRC;
            break;
        case AstrOsBulkTransport::NakReason::SIZE:
            wireReason = OtaDataNakReason::SIZE;
            break;
        case AstrOsBulkTransport::NakReason::OUT_OF_ORDER:
            wireReason = OtaDataNakReason::OUT_OF_ORDER;
            break;
        case AstrOsBulkTransport::NakReason::NONE:
            // Shouldn't happen on a NAK decision; fall through to OUT_OF_ORDER
            // as the safest hint for the master.
            ESP_LOGW(TAG, "handleData: NAK with reason=NONE — wire-encoding as OUT_OF_ORDER");
            wireReason = OtaDataNakReason::OUT_OF_ORDER;
            break;
        }
        ESP_LOGW(TAG, "handleData: xferId=%u seq=%u NAK reason=%d (hcs=%u nes=%u wr=%u)", xferId, seq,
                 (int)wireReason, (unsigned)cr.highestContiguousSeq, (unsigned)cr.nextExpectedSeq,
                 (unsigned)cr.windowRemaining);
        sendDataNak(mac, xferId, cr.highestContiguousSeq, cr.nextExpectedSeq, cr.windowRemaining, wireReason);
        return;
    }

    // ACK path: BulkReceiver accepted the chunk. Write to flash, update
    // streaming SHA, reply ACK.
    esp_err_t wErr = esp_ota_write(otaHandle_, cr.payload, cr.payloadLen);
    if (wErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleData: esp_ota_write failed: %s — aborting transfer xferId=%u seq=%u",
                 esp_err_to_name(wErr), xferId, seq);
        // Send NAK with WRITE reason so master logs a distinct failure
        // signal (vs CRC / SIZE / OUT_OF_ORDER which suggest retransmit).
        // WRITE is terminal — master should abandon the padawan.
        sendDataNak(mac, xferId, cr.highestContiguousSeq, cr.nextExpectedSeq, cr.windowRemaining,
                    OtaDataNakReason::WRITE);
        resetOtaHandleAndSha();
        watchdogStop();
        return;
    }

    if (shaActive_)
    {
        AstrOsSha256_update(&shaCtx_, cr.payload, cr.payloadLen);
    }
    else
    {
        // Shouldn't happen — handleBegin sets shaActive_ on the same path
        // that opens the OTA handle. If we hit this branch the SHA stream
        // is desynchronized; the END-side compare will catch it as a
        // HASH_MISMATCH, but log loudly.
        ESP_LOGE(TAG, "handleData: shaActive_=false on accepted chunk — END will report HASH_MISMATCH");
    }

    ESP_LOGD(TAG, "handleData: xferId=%u seq=%u accepted (cum=%u next=%u wr=%u)", xferId, seq,
             (unsigned)cr.highestContiguousSeq, (unsigned)cr.nextExpectedSeq, (unsigned)cr.windowRemaining);

    esp_err_t sErr = sendDataAck(mac, xferId, cr.highestContiguousSeq, cr.nextExpectedSeq, cr.windowRemaining);
    if (sErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleData: sendDataAck failed: %s — master tick-retransmit will re-trigger",
                 esp_err_to_name(sErr));
    }
    watchdogRestart();
}
```

- [ ] **Step 2: Verify both boards build**

Run: `pio run -e metro_s3 2>&1 | tail -15`
Expected: clean build.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -15`
Expected: clean build.

- [ ] **Step 3: Bench smoke-test**

Flash both boards, trigger `FW_DEPLOY_BEGIN` with a small firmware (use the existing `pio run -e metro_s3` build artifact at `.pio/build/metro_s3/firmware.bin`, ~1.2 MB). Padawan log should now show a stream of `ESP_LOGD handleData` lines (only visible with `CORE_DEBUG_LEVEL=4`; otherwise rely on the master-side data-ack flow). Master streams to completion of the window-drain and then attempts `OTA_END`; padawan's `handleEnd` is still stubbed so the transfer hangs at the END phase. Master's END_ACK timeout (5 s) eventually fires and the transfer abandons with `FAILED("end_ack_timeout")`.

**The bench observation here is: the full data stream completes without padawan-side NAKs.** This validates `esp_ota_write` latency stays inside the per-chunk window budget — i.e., it confirms the resolution to design open-Q #5 (lazy per-sector erase is fast enough). If you see a flood of CRC NAKs, your padawan is dropping CRC checks under load — investigate; the M4 design assumes BulkReceiver's CRC check on the M1 wire-format CRC field is the only filter.

To monitor `esp_ota_write` time across the transfer, temporarily wrap the call with `int64_t t0 = esp_timer_get_time();` and `ESP_LOGI(TAG, "esp_ota_write took %lldus", esp_timer_get_time()-t0);`. Typical values:
- First write per sector: ~40 ms (8MB board), ~25 ms (16MB board)
- Subsequent writes within a sector: <1 ms

If any single write exceeds the BulkSender ackTimeout (400 ms) you'll see master-side retransmits. Bench-validate before T7 lands.

- [ ] **Step 4: Commit**

```bash
git add lib/OtaWriter/src/OtaWriter.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T6 — handleData implementation

Implements the data side of the OtaWriter state machine: inactive-receiver
NAK, BulkReceiver::onChunk dispatch, esp_ota_write on accept, streaming
AstrOsSha256_update, OTA_DATA_ACK with the receiver's hint fields, and
watchdog reset per accepted chunk.

NAK reasons from BulkReceiver (CRC / SIZE / OUT_OF_ORDER) map to wire
OtaDataNakReason 1:1. esp_ota_write failure emits OtaDataNakReason::WRITE
which the master treats as terminal (no retransmit recovers from a flash
write failure) and routes through resetOtaHandleAndSha to leave the writer
in IDLE.

Hot-path logging is ESP_LOGD per accepted chunk; ESP_LOGW only on NAK
paths to keep serial bandwidth available.

handleEnd / handleWatchdogFire still stubbed — T7/T8 fill them.

M4 Task 6 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: `handleEnd` — final SHA compare, esp_ota_end, read-back-rehash, OTA_END_ACK

Implements `OtaWriter::handleEnd`. The terminal verifier — three independent checks any of which abort the transfer:

1. **Streaming SHA matches expected**: catches in-flight corruption (CRC missed a multi-bit error, padawan-side write dropped bytes, etc.). On mismatch → `OTA_END_ACK HASH_MISMATCH`.
2. **`esp_ota_end` succeeds**: ESP-IDF's final image-validity check (size, magic bytes). On failure → `OTA_END_ACK WRITE_ERROR`.
3. **Read-back-rehash matches streaming SHA**: catches silent flash-write failures (bit-rot, programmed-but-not-stored bytes). 4 KB-at-a-time `esp_partition_read` (resolves design open-Q #6) + `AstrOsSha256_update`. On mismatch → `OTA_END_ACK WRITE_ERROR`.

If all three pass: `OTA_END_ACK OK`. **No `esp_ota_set_boot_partition` call** — M4 intentionally leaves the boot partition unchanged. The inactive partition holds verified bytes; PR set 2 will add the boot-table flip + reboot logic.

**Files:**
- Modify: `lib/OtaWriter/src/OtaWriter.cpp`

- [ ] **Step 1: Replace the handleEnd stub**

In `lib/OtaWriter/src/OtaWriter.cpp`, find the T2 stub:

```cpp
void OtaWriter::handleEnd(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleEnd: stubbed (Task 7) — xferId=%u chunks=%u", msg.end.xferId, msg.end.totalChunksSent);
}
```

Replace with:

```cpp
void OtaWriter::handleEnd(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.end.srcMac;
    const uint8_t xferId = msg.end.xferId;

    if (!active_)
    {
        ESP_LOGW(TAG, "handleEnd: xferId=%u arrived while inactive — replying WRITE_ERROR", xferId);
        // No reasonable SHA to echo; send all-zero buffer.
        uint8_t zero[32] = {0};
        sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, zero);
        return;
    }

    // BulkReceiver::onEnd validates totalChunksSent matches the BEGIN's
    // totalChunks. Mismatch indicates a protocol-level desync that we
    // can't recover from (the master sent fewer chunks than it claimed).
    auto er = bulk_.onEnd(xferId, msg.end.totalChunksSent);
    bool teardownRequested = AstrOsBulkTransport::shouldTeardownOnEndResult(er);
    if (er.status != AstrOsBulkTransport::EndResult::Status::OK)
    {
        ESP_LOGW(TAG, "handleEnd: BulkReceiver::onEnd rejected: status=%d reason=%d",
                 (int)er.status, (int)er.reason);
        uint8_t zero[32] = {0};
        sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, zero);
        if (teardownRequested)
        {
            resetOtaHandleAndSha();
            watchdogStop();
        }
        return;
    }

    // 1. Finalize streaming SHA.
    uint8_t streamedDigest[32];
    if (shaActive_)
    {
        AstrOsSha256_final(&shaCtx_, streamedDigest);
        shaActive_ = false;
    }
    else
    {
        ESP_LOGE(TAG, "handleEnd: shaActive_=false at finalize — emitting all-zero digest as HASH_MISMATCH");
        memset(streamedDigest, 0, sizeof(streamedDigest));
    }

    if (memcmp(streamedDigest, expectedSha256_, 32) != 0)
    {
        ESP_LOGE(TAG, "handleEnd: streaming SHA mismatch — replying HASH_MISMATCH (chunks=%u, totalSize=%u)",
                 (unsigned)msg.end.totalChunksSent, (unsigned)currentTotalSize_);
        sendEndAck(mac, xferId, OtaEndStatus::HASH_MISMATCH, streamedDigest);
        resetOtaHandleAndSha();
        watchdogStop();
        return;
    }

    // 2. esp_ota_end finalizes the OTA write. With secure boot disabled
    //    (our project's configuration), this validates the image header's
    //    magic byte + size against the partition. It does NOT change the
    //    boot table — that's PR set 2.
    esp_err_t eErr = esp_ota_end(otaHandle_);
    if (eErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleEnd: esp_ota_end failed: %s — replying WRITE_ERROR", esp_err_to_name(eErr));
        // esp_ota_end has already released the handle internally even on
        // failure (per IDF docs); zero the handle so resetOtaHandleAndSha
        // doesn't try to re-abort it.
        otaHandle_ = 0;
        sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, streamedDigest);
        resetOtaHandleAndSha();
        watchdogStop();
        return;
    }
    otaHandle_ = 0;

    // 3. Read-back-and-rehash. esp_partition_read pulls bytes back from
    //    flash and we re-stream them through AstrOsSha256. Mismatch means
    //    the flash silently corrupted bytes between esp_ota_write and
    //    esp_ota_end's finalize.
    //
    //    4 KB read buffer matches the flash sector size (resolves design
    //    open-Q #6). Stack-allocated — fits comfortably within the 4 KB
    //    otaWriterTask stack alongside the AstrOsSha256Ctx (~120 B). If
    //    the per-task stack check warns at 500 B remaining during the
    //    M4 bench, escalate via Step 4's bench validation.
    AstrOsSha256Ctx rbCtx;
    AstrOsSha256_init(&rbCtx);

    constexpr size_t kReadBufSize = 4096;
    uint8_t buf[kReadBufSize];
    bool readbackOk = true;
    for (size_t off = 0; off < currentTotalSize_; off += kReadBufSize)
    {
        size_t chunk = (currentTotalSize_ - off < kReadBufSize) ? (currentTotalSize_ - off) : kReadBufSize;
        esp_err_t rErr = esp_partition_read(inactivePartition_, off, buf, chunk);
        if (rErr != ESP_OK)
        {
            ESP_LOGE(TAG, "handleEnd: esp_partition_read at off=%zu len=%zu failed: %s", off, chunk,
                     esp_err_to_name(rErr));
            readbackOk = false;
            break;
        }
        AstrOsSha256_update(&rbCtx, buf, chunk);
    }

    uint8_t readbackDigest[32];
    AstrOsSha256_final(&rbCtx, readbackDigest);

    if (!readbackOk)
    {
        sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, streamedDigest);
        resetOtaHandleAndSha();
        watchdogStop();
        return;
    }

    if (memcmp(readbackDigest, streamedDigest, 32) != 0)
    {
        ESP_LOGE(TAG, "handleEnd: READ-BACK hash mismatch — flash write silently corrupted bytes; replying WRITE_ERROR");
        // Send the readback digest (not streamed) so master forensics can
        // see what's actually on flash vs what we expected.
        sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, readbackDigest);
        resetOtaHandleAndSha();
        watchdogStop();
        return;
    }

    // All three checks passed.
    ESP_LOGI(TAG, "handleEnd: transfer xferId=%u OK — %u bytes verified on partition '%s' (NB: boot partition unchanged; PR set 2 will flip)",
             xferId, (unsigned)currentTotalSize_, inactivePartition_->label);
    sendEndAck(mac, xferId, OtaEndStatus::OK, streamedDigest);

    resetOtaHandleAndSha();
    watchdogStop();
}
```

- [ ] **Step 2: Verify both boards build**

Run: `pio run -e metro_s3 2>&1 | tail -15`
Expected: clean build.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -15`
Expected: clean build.

- [ ] **Step 3: First full bench round trip (M4 merge bar)**

This is the **M4 merge-bar bench validation** — the design doc calls it out as the milestone gate.

Setup:
- 1 master node (any board) + 1 padawan node (any board)
- AstrOs.Server running on the local Pi or laptop with the Vue Firmware view loaded
- Server's `develop` branch checked out
- Firmware artifact: use `pio run -e metro_s3` output at `.pio/build/metro_s3/firmware.bin` (~1.2 MB — small enough to bench in <30 s)

Steps:
1. Boot both nodes; confirm master log shows `polling padawan pad1` every 2 s and padawan log shows POLL ACK responses
2. In Vue Firmware view: upload `firmware.bin`. Server sends `FW_TRANSFER_BEGIN` → `FW_CHUNK` x N → `FW_TRANSFER_END` to master (the existing serial-OTA path from Phase 3)
3. Master's `OtaReceiver` writes the firmware to `/sdcard/firmware/<sha-prefix>.bin`
4. In Vue Firmware view: click "Deploy" with order list `[pad1]`. Server sends `FW_DEPLOY_BEGIN`
5. Master's `OtaForwarder` (from M3) picks up the deploy, opens the staged file, sends `OTA_BEGIN` to pad1
6. **Padawan's `OtaWriter::handleBegin`** runs, returns `OTA_BEGIN_ACK`
7. Master streams `OTA_DATA` chunks for ~20-30 s; **padawan's `handleData`** writes them to inactive partition + updates SHA
8. Master sends `OTA_END`; **padawan's `handleEnd`** runs streaming-SHA compare → esp_ota_end → read-back-rehash → `OTA_END_ACK OK`
9. Master records `pad1=OK` in deploy results, emits `FW_DEPLOY_DONE` with `pad1=OK, master=FAILED("not_implemented_in_pr_set_1")` (master self-flash deferred per the protocol contract)

Verification:
- Vue Firmware view shows `pad1: OK` (or whatever the M3 forwarder labels it; M5 lights up the per-stage progress UI)
- Padawan serial log shows: `handleBegin accepted` → many `handleData` debug lines → `handleEnd: transfer xferId=N OK — M bytes verified on partition 'ota_X'`
- USB-attach the padawan (disconnect from mesh first to avoid clobbering the test):
  ```bash
  esptool.py --port /dev/ttyUSB1 read_flash 0x220000 0x180000 padawan_inactive.bin   # 8MB board ota_1
  # or:
  esptool.py --port /dev/ttyUSB1 read_flash 0x650000 0x140000 padawan_inactive.bin   # 16MB board ota_1
  ```
  Adjust offset based on which partition is "inactive" — read padawan log line `inactivePartition_='ota_X' offset=0x...` to confirm.
- `sha256sum padawan_inactive.bin` — must match `sha256sum .pio/build/metro_s3/firmware.bin`. (`read_flash` returns the full partition slot, so the padawan-side bytes will be followed by 0xFF padding to the slot's full size — the sha will differ unless you truncate. Compare the first `currentTotalSize_` bytes:
  ```bash
  dd if=padawan_inactive.bin of=padawan_image.bin bs=1 count=$(stat -c%s .pio/build/metro_s3/firmware.bin) 2>/dev/null
  sha256sum padawan_image.bin .pio/build/metro_s3/firmware.bin
  ```
  Both lines should produce identical sha.)

If the SHA matches: M4 merge bar passed. If not: investigate (likely candidates — payload copy bug from T4, write ordering, sector-misalignment in read-back-rehash).

- [ ] **Step 4: Bench-validate task stack high-water mark**

After step 3's full transfer, check the padawan log for any stack warning:

```
W (... TASK_HWM) ota_writer_task high water mark: NNN bytes
```

(This warning fires automatically at 500 B remaining — see CLAUDE.md's "Task stack sizes" convention.) If you see the warning, bump `otaWriterTask`'s stack from 4096 to 6144 in `main.cpp` Step 5 of T3 and re-flash. The 4 KB readback buffer in `handleEnd` Step 1 is the largest local — 6 KB stack covers it with headroom.

If no warning: confirm bench measurement by adding a temporary `ESP_LOGI(TAG, "uxTaskGetStackHighWaterMark=%d", uxTaskGetStackHighWaterMark(NULL));` at the bottom of `handleEnd`'s success branch and re-flash. Record the value in the QA plan (Task 9). Remove the temporary log before merge.

- [ ] **Step 5: Commit**

```bash
git add lib/OtaWriter/src/OtaWriter.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T7 — handleEnd implementation (M4 merge bar)

Implements the terminal verifier: three independent checks any of which
abort the transfer:

1. Streaming SHA matches expected (catches in-flight corruption)
2. esp_ota_end succeeds (ESP-IDF's image-header validity check)
3. Read-back-rehash matches streaming SHA (catches silent flash-write
   failures via 4 KB-at-a-time esp_partition_read — resolves design
   open-Q #6)

On all-pass: OTA_END_ACK OK. Bytes are verified on the inactive partition;
boot table is intentionally unchanged (PR set 2 will add the flip).

On any failure: appropriate OtaEndStatus (HASH_MISMATCH for streaming
SHA fail; WRITE_ERROR for esp_ota_end fail or readback mismatch) and
the writer resets to IDLE for the next BEGIN.

After this commit, the M4 merge-bar bench validates: master + 1 padawan
on bench, single 1.2 MB firmware deploy from server UI completes with
pad1=OK; USB-attach + esptool.py read_flash + sha256sum confirms bytes
match the source firmware.bin.

handleWatchdogFire still stubbed — T8 fills it.

M4 Task 7 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: `handleWatchdogFire` — idempotent abort on stuck transfer

Implements the recovery path when the 10 s idle watchdog fires. The fire signal means: a transfer was active, then 10 s passed without any chunk activity (link drop, master crash, all-NAK loop where no progress is being made). Idempotent — if `active_` is already false (e.g., a late watchdog fire after handleEnd already cleaned up), this is a no-op.

**Files:**
- Modify: `lib/OtaWriter/src/OtaWriter.cpp`

- [ ] **Step 1: Replace the handleWatchdogFire stub**

In `lib/OtaWriter/src/OtaWriter.cpp`, find the T2 stub:

```cpp
void OtaWriter::handleWatchdogFire()
{
    ESP_LOGW(TAG, "handleWatchdogFire: stubbed (Task 8)");
}
```

Replace with:

```cpp
void OtaWriter::handleWatchdogFire()
{
    if (!active_)
    {
        // Late fire: handleEnd or a NAK-side reset already cleaned up,
        // and the watchdog signal landed in the queue before watchdogStop
        // took effect. No-op — but log so a misfiring timer is visible.
        ESP_LOGI(TAG, "handleWatchdogFire: no active transfer; treating as late signal");
        return;
    }
    ESP_LOGE(TAG, "handleWatchdogFire: idle threshold (%llums) exceeded for xferId=%u — aborting transfer",
             kWatchdogIdleUs / 1000ULL, currentXferId_);
    // No reply to master — the watchdog fires when the master is unreachable
    // or has already abandoned the transfer (its own BEGIN_ACK / data-ack /
    // END_ACK timeouts will have triggered). Sending a NAK into a dead
    // mesh wastes bandwidth and risks confusing a fresh transfer that
    // happens to be using the same xferId from a different master incarnation.
    resetOtaHandleAndSha();
    watchdogStop();
}
```

- [ ] **Step 2: Verify both boards build**

Run: `pio run -e metro_s3 2>&1 | tail -15`
Expected: clean build.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -15`
Expected: clean build.

- [ ] **Step 3: Bench-validate the watchdog**

Negative-path bench (informal — won't gate M4 merge but should be sanity-checked before T9 commits):
1. Start a transfer on the bench setup from T7 step 3
2. Mid-transfer (around 50% chunks sent), power-cycle the padawan via its hardware switch
3. Master will retry the chunk a few times, then its data-ack timeout will fire and it'll move on to either `OTA_END` or abandon
4. Power the padawan back on. It boots cleanly into a fresh state (no active transfer)
5. Trigger a new `FW_DEPLOY_BEGIN`. The fresh transfer should complete normally — confirms that an aborted transfer doesn't leave the padawan in a stuck `active_=true` state across boot.

Watchdog-specific test (harder to reproduce without sniffer):
1. Modify master temporarily to **stop sending OTA_DATA chunks** mid-transfer (comment out `streamDrain` in OtaForwarder for one bench run). Don't commit this change.
2. Padawan receives BEGIN, replies BEGIN_ACK, awaits chunks
3. After 10 s the padawan watchdog fires; log shows `handleWatchdogFire: idle threshold (10000ms) exceeded for xferId=N — aborting transfer`
4. Padawan `isActive()` returns false; subsequent `FW_DEPLOY_BEGIN` works normally

Revert the master-side modification before committing.

- [ ] **Step 4: Commit**

```bash
git add lib/OtaWriter/src/OtaWriter.cpp
git commit -m "$(cat <<'EOF'
feat(ota-writer): T8 — handleWatchdogFire (idle abort + cleanup)

Implements the 10 s idle-watchdog recovery path: if no chunk activity
has occurred within the threshold, reset to IDLE via resetOtaHandleAndSha
and stop the watchdog. Idempotent — late fires (post-handleEnd) log + no-op.

No NAK is sent on watchdog fire: the master is by definition unreachable
or already self-abandoned (its own BEGIN_ACK / data-ack / END_ACK timeouts
have fired by 10 s). Sending into a dead mesh wastes bandwidth.

This closes the OtaWriter state machine: every active path now has a
clean exit (handleEnd success, handleEnd failure, handleData NAK / write
fail, handleWatchdogFire). All paths route through resetOtaHandleAndSha
so the writer is always ready for the next BEGIN.

M4 Task 8 of 9.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: QA plan + verification-before-completion gates + clang-format

Documents the M4 bench scenarios + the merge-bar verification gates in the canonical QA plan location. Creates the QA plan file `.docs/qa/ota-mesh-forward.md` (does not yet exist — M3 design called for it but M3 shipped without it; M4 lands the doc covering both M3 and M4 bench cases). Runs the per-milestone verification-before-completion checks.

**Files:**
- Create: `.docs/qa/ota-mesh-forward.md`

- [ ] **Step 1: Write the QA plan**

Create `.docs/qa/ota-mesh-forward.md`:

```markdown
# OTA Mesh-Forward QA Plan

Covers bench validation for PR set 1 (M1-M5) of the firmware OTA mesh-forward
work. Each milestone adds its own sub-section. M5 will extend with 2-padawan
and negative-path cases.

## Preconditions

- AstrOs.Server `develop` branch running, Firmware view loaded in browser
- At least one master node + one padawan node, paired via the existing
  ESP-NOW peer registration flow (master polling shows padawan responding to
  POLL every 2 s before starting any test)
- Firmware artifact for testing: any build output, e.g.
  `.pio/build/metro_s3/firmware.bin` (~1.2 MB)
- Padawan USB cable accessible (for post-test partition dumps via esptool.py)

## M3 — master OtaForwarder emits frames (no padawan handler yet)

This case was validated informally during M3 bench checkpoint; recorded here
for completeness.

1. Trigger `FW_DEPLOY_BEGIN` from server UI with order list `[pad1]`
2. Expected: master serial log shows `OTA_BEGIN` emission attempt to pad1
3. Expected: after ~2 s, master log shows `forwarder_done: pad1=FAILED reason="begin_ack_timeout"` (padawan returned no ACK because OtaWriter was stubbed)
4. Expected: server UI shows `pad1: FAILED` with the begin-ack-timeout reason

## M4 — single-padawan end-to-end

The M4 merge-bar test. **This case must pass for M4 PR to merge.**

### Setup
- 1 master + 1 padawan on bench
- Master has SD card with firmware staging directory writable
- Padawan has SD card (per [[project_all_nodes_have_sd]] — every node has SD)
- Both nodes flashed with the M4-candidate firmware

### Test case 1: happy path

1. Upload `firmware.bin` (≥ 1 MB, ≤ partition size) via server Firmware view
2. Confirm master serial log shows `OtaReceiver: handleEnd: success — renamed to /sdcard/firmware/<sha>.bin`
3. Click "Deploy" in server UI with order list `[pad1]`
4. Expected master log sequence:
   - `forwarder: start xferId=1 padawan=pad1`
   - `forwarder: emitted OTA_BEGIN xferId=1 to <pad1-mac>`
   - `forwarder: BEGIN_ACK received xferId=1 from <pad1-mac>`
   - Many `forwarder: streaming chunk seq=N` (or DEBUG-only)
   - `forwarder: END_ACK received xferId=1 status=OK`
   - `forwarder_done: pad1=OK`
5. Expected padawan log sequence:
   - `OtaWriter: handleBegin accepted: xferId=1 totalSize=N chunks=K chunkSize=128 partition='ota_X' (size=M, offset=0x...)`
   - Many `OtaWriter: handleData ... accepted` (DEBUG only) — no NAKs
   - `OtaWriter: handleEnd: transfer xferId=1 OK — N bytes verified on partition 'ota_X' (NB: boot partition unchanged; PR set 2 will flip)`
6. Expected server UI: `pad1: OK`, deploy summary shows 1/1 successful

### Test case 2: byte-level verification

After test case 1 succeeds:

1. Disconnect padawan from mesh (or simply note its USB serial port)
2. Connect padawan via USB
3. Dump the inactive partition:
   ```bash
   # 8 MB board (ota_1 starts at 0x220000, size 0x200000):
   esptool.py --port /dev/ttyUSB1 read_flash 0x220000 0x200000 padawan_dump.bin
   # 16 MB board (ota_1 starts at 0x650000, size 0x640000):
   esptool.py --port /dev/ttyUSB1 read_flash 0x650000 0x640000 padawan_dump.bin
   ```
   Adjust offset based on which partition the padawan log identified as inactive.
4. Truncate to actual firmware size + compare SHA:
   ```bash
   FW_SIZE=$(stat -c%s .pio/build/metro_s3/firmware.bin)
   dd if=padawan_dump.bin of=padawan_image.bin bs=1 count=$FW_SIZE 2>/dev/null
   sha256sum padawan_image.bin .pio/build/metro_s3/firmware.bin
   ```
   Both lines MUST produce identical sha. If they differ, M4 has a silent corruption bug — investigate before merging.

### Test case 3: padawan-side BUSY rejection

Validates that a duplicate BEGIN during an active transfer is rejected gracefully (vs corrupting state).

1. Start a deploy with order list `[pad1]`
2. While the chunk stream is in flight (within ~5 s of BEGIN), manually trigger a second `FW_DEPLOY_BEGIN` from the server (open a second browser tab; click Deploy again)
3. Expected padawan log: `handleBegin: xferId=2 arrived while xferId=1 is active — replying BUSY`
4. Expected master log: forwarder of second deploy logs `OTA_BEGIN_NAK reason=BUSY` and records pad1 as FAILED for that deploy
5. The first deploy continues to completion normally (pad1=OK for deploy #1)

### Test case 4: idle-watchdog recovery

Validates that a stuck transfer self-aborts within 10 s.

1. Modify master temporarily (do not commit): comment out the line in `OtaForwarder::streamDrain` that calls `bulk_.nextChunkToSend()`, so the master stops sending OTA_DATA after BEGIN_ACK
2. Rebuild + flash master only (`pio run -e metro_s3 -t upload`)
3. Trigger a deploy with order list `[pad1]`
4. Padawan receives BEGIN, ACKs, awaits chunks
5. Expected after ~10 s on padawan: `handleWatchdogFire: idle threshold (10000ms) exceeded for xferId=N — aborting transfer`
6. Confirm padawan `isActive()` returns false: trigger a fresh deploy; it must succeed (test case 1 happy path replay)
7. Revert the master-side modification

### Stack high-water mark check

After test case 1, add a temporary `ESP_LOGI(TAG, "ota_writer hwm=%d", uxTaskGetStackHighWaterMark(NULL));` at the bottom of `handleEnd`'s success branch. Re-flash padawan, re-run test case 1, record the value:

- Recorded value: _____ bytes remaining (target: ≥ 500 B)
- If < 500 B: bump `otaWriterTask` stack from 4096 to 6144 in main.cpp, re-test
- Remove the temporary log before committing T9

## M5 (placeholder)

To be filled in by M5's PR. Will cover:
- 2-padawan sequential deploy (both OK)
- 2-padawan deploy with one offline (master times out gracefully, marks FAILED, moves to next)
- 2-padawan deploy with mid-transfer power-cycle on one padawan (idle watchdog fires, padawan recovers cleanly on next BEGIN)
- FW_PROGRESS UI animation per padawan
```

- [ ] **Step 2: Run the per-milestone verification gates**

Per the design doc, M4 must pass these gates before PR opens:

1. **`pio test -e test` 100% pass**

   Run: `pio test -e test 2>&1 | tail -10`
   Expected: all tests pass; total = baseline + 6 new `OtaWriterQueueMsg.*` tests from T1.

2. **`pio run -e metro_s3` builds clean**

   Run: `pio run -e metro_s3 2>&1 | tail -10`
   Expected: clean build, no new warnings beyond what's in develop's baseline.

3. **`pio run -e lolin_d32_pro` builds clean**

   Run: `pio run -e lolin_d32_pro 2>&1 | tail -10`
   Expected: clean build.

4. **`clang-format` clean on changed files**

   Run:
   ```bash
   git diff --name-only --diff-filter=ACMR main...HEAD | grep -E '\.(cpp|hpp|h|c)$' | while read f; do
     echo "=== $f ==="
     clang-format --dry-run --Werror "$f" 2>&1 || echo "FORMAT FAIL: $f"
   done
   ```
   Expected: no `FORMAT FAIL` lines. If any file fails, run `clang-format -i <file>` and amend the relevant commit.

   Alternatively, if `.githooks/pre-commit` is active (`git config core.hooksPath .githooks` was run once), the hook auto-formats on commit — confirm by re-running step 4.

5. **Native-purity CI guard passes** — M4 does NOT add any PURE libs, so this is a smoke check that we didn't accidentally leak ESP-IDF includes into `lib_native/`. The guard runs in CI on PR.

   Local quick-check:
   ```bash
   grep -rE '#include[[:space:]]+["<](esp_|freertos/|driver/|nvs|mbedtls|sdkconfig)' lib_native/ 2>/dev/null && echo "PURITY FAIL" || echo "purity OK"
   ```
   Expected: `purity OK`.

6. **M4 single-padawan bench checkpoint** — T7 Step 3 + Step 4 already validated. Cross-reference the test cases in `.docs/qa/ota-mesh-forward.md` were exercised; record any deviations as follow-ups.

- [ ] **Step 3: Self-review the plan against the design doc**

Before opening the M4 PR, audit:

1. **Open-Q resolution coverage**: design open-Qs #4, #5, #6 are tagged for M4. Confirm each has a referenced disposition in the implementation:
   - #4 (watchdog 10 s) — `OtaWriter.hpp:kWatchdogIdleUs = 10ULL * 1000ULL * 1000ULL`, T2
   - #5 (per-sector erase) — `handleBegin` passes `OTA_SIZE_UNKNOWN` to `esp_ota_begin`, T5 commit message references this
   - #6 (readback chunk size 4 KB) — `handleEnd` declares `constexpr size_t kReadBufSize = 4096`, T7

2. **No `esp_ota_set_boot_partition` reference anywhere in OtaWriter** — confirm with `grep -n set_boot_partition lib/OtaWriter/src/OtaWriter.cpp` (must return nothing). PR set 2 ships that call; M4 must not.

3. **Single-task-state invariant holds**: `active_` is the only field read off-task (by `pollingTimerCallback`). All other state mutates only inside `process()` on `otaWriterTask`. The watchdog callback only does `xQueueSend`. Same discipline as `OtaReceiver` and `OtaForwarder`.

4. **`resetOtaHandleAndSha` is the only place `esp_ota_abort` runs** (with the exception of dtor). `grep -n esp_ota_abort lib/OtaWriter/src/OtaWriter.cpp` should return exactly one hit (inside resetOtaHandleAndSha).

- [ ] **Step 4: Commit + open PR**

```bash
git add .docs/qa/ota-mesh-forward.md
git commit -m "$(cat <<'EOF'
docs(ota-writer): T9 — QA plan for M3/M4 bench scenarios

Lands .docs/qa/ota-mesh-forward.md covering:
- M3 master-side OTA_BEGIN emission (historical, validated during M3)
- M4 single-padawan end-to-end (the merge-bar test): happy path,
  byte-level esptool.py read_flash verification, BUSY rejection,
  idle-watchdog recovery, stack high-water-mark check
- M5 placeholder for 2-padawan + negative-path cases

This is the canonical QA reference for the OTA mesh-forward bench
testing through PR set 1. M5's PR will extend the M5 section.

M4 Task 9 of 9 — closes the milestone.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Then open the PR (use the M3 PR's body as the template for structure):

```bash
git push -u origin <branch-name>
gh pr create --base develop --title "feat(ota): M4 — padawan OtaWriter (MIXED) + first end-to-end bench" --body "$(cat <<'EOF'
## Summary

- Adds `lib/OtaWriter/` MIXED lib (padawan-side counterpart to M3's `OtaForwarder`) that wraps the existing `BulkReceiver`, drives `esp_ota_*`, and verifies via streaming + read-back SHA-256.
- Extends `AstrOsEspNow` padawan-side dispatcher to decode `OTA_BEGIN` / `OTA_DATA` / `OTA_END` into `otaWriterQueue` messages (replacing M3's "M4 not wired" stub).
- New `otaWriterQueue` + `otaWriterTask` in `main.cpp`, padawan only, polling-pause gate extended.
- Resolves design open questions #4 (10 s watchdog), #5 (lazy per-sector erase via `OTA_SIZE_UNKNOWN`), #6 (4 KB readback chunks).
- **Does NOT** change the boot partition (`esp_ota_set_boot_partition` is PR set 2). Verified bytes land in the inactive partition; current boot is untouched.

## Test plan

- [x] `pio test -e test` — all native tests pass (baseline + 6 new `OtaWriterQueueMsg.*`)
- [x] `pio run -e metro_s3` clean build
- [x] `pio run -e lolin_d32_pro` clean build
- [x] `clang-format` clean on changed files
- [x] Native-purity guard passes
- [x] M4 bench: master + 1 padawan, single 1.2 MB firmware deploy from server UI → `pad1=OK` in `FW_DEPLOY_DONE`
- [x] esptool.py read_flash + sha256sum of padawan's inactive partition matches source `firmware.bin`
- [x] Test case 3: BUSY rejection on duplicate BEGIN
- [x] Test case 4: idle-watchdog recovery (10 s)
- [x] Stack high-water mark check on `ota_writer_task` ≥ 500 B remaining

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-review checklist (run after writing all task content)

1. **Spec coverage**:
   - [x] Design doc M4 section (515-535) — all 6 listed files covered
   - [x] OtaWriter pseudocode (215-325) — `handleBegin` (T5), `handleData` (T6), `handleEnd` (T7), `handleWatchdogFire` (T8), `resetOtaHandleAndSha` (T2)
   - [x] Open Qs #4, #5, #6 — disposition table at top of plan, referenced in tasks
   - [x] Padawan-side dispatcher OTA cases (design Open Q #7 follow-on) — T4
   - [x] Polling-pause gate extension — T3 Step 6
   - [x] Merge-bar bench validation — T7 Step 3 + T9 QA plan
   - [x] Per-milestone verification gates — T9 Step 2

2. **Placeholder scan**: no "TBD", "TODO", "fill in later", "add validation", or "similar to Task N" — every step has concrete code blocks and commands.

3. **Type consistency**:
   - `queue_ota_writer_msg_t` is the union typename in T1, T2 (process arg), T3 (xQueueCreate sizeof), T4 (dispatcher fills), T7 (Cb posts)
   - `OTA_WR_BEGIN` / `OTA_WR_DATA` / `OTA_WR_END` / `OTA_WR_WATCHDOG_FIRE` enum values used consistently
   - `freeOtaWriterMsg` (not `free_ota_writer_msg` or `freeOtaWrMsg`) — single spelling throughout
   - `AstrOs_OtaWriter` singleton spelling matches `AstrOs_OtaReceiver` / `AstrOs_OtaForwarder` precedent
   - `setOtaWriterQueue` mirrors `setOtaForwarderQueue`
   - `OtaEndStatus::OK / HASH_MISMATCH / WRITE_ERROR` — three values, no others added
   - `OtaBeginNakReason::BUSY / NO_PARTITION / BEGIN_FAILED` — three values, no others added
   - `OtaDataNakReason::CRC / SIZE / OUT_OF_ORDER / WRITE` — used in T6 NAK-reason switch; NONE never sent (it's the wire-rejected sentinel)




