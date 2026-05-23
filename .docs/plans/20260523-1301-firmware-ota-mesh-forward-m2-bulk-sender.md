# Firmware OTA Mesh-Forward M2 — BulkSender PURE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a PURE `BulkSender` state machine alongside the existing `BulkReceiver` in `lib_native/AstrOsBulkTransport`, with sliding-window send semantics suitable for the master-side mesh forwarder that M3 will build on top. Zero firmware behavior change.

**Architecture:** New `BulkSender` class lives in the existing PURE lib. Tracks per-seq in-flight state in a fixed-size `std::array<InFlightEntry, MAX_WINDOW_SIZE>` (no heap). Each result type follows the existing `ChunkResult` discipline: `const`-fields + private constructor + static factories, with `[[nodiscard]]` enforced at the type level. Wire-stable enum values pinned via `static_assert` at file scope. The state machine has 5 states (`IDLE → AWAITING_BEGIN_ACK → STREAMING → DONE_OK | ABANDONED`); `AWAITING_END_ACK` is implicit ("STREAMING with all seqs confirmed").

**Tech Stack:** C++17, GoogleTest/GMock (native), PlatformIO `[env:test]` with `-std=gnu++2a`. PURE-lib: only `<cstdint>`, `<array>`, `<type_traits>`, `<cassert>` are needed beyond what `BulkReceiver` already pulls in.

---

## Context for the engineer

Read these first — they're load-bearing and will not be re-explained per task:

- **Design doc**: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md` — sections "Master side — BulkSender (PURE) + OtaForwarder (MIXED)" and "M2 — BulkSender PURE" are the contract.
- **Existing precedent**: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` and `.cpp`. `BulkReceiver` is the sister class. Mirror its discipline: result-type `const`-fields + private ctor + static factories (`ChunkResult` lines 87-147), `[[nodiscard]]` on every result struct (`BeginResult` line 154, `EndResult` line 181), file-scope `static_assert` pinning enum values (`AstrOsBulkTransport.cpp` lines 12-37 and 46-54), and per-method docstring explaining the invariants and error modes.
- **Test file**: `test/test_native/bulk_transport_tests.cpp` (717 lines). Read the first 100 lines to see the test patterns: helper functions in anonymous namespaces, `TEST(BulkTransport, DescriptiveCamelCaseName)` style, no parameterized tests, explicit field-by-field expectations.
- **Frozen wire contract**: `AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md` section B — window=8 for ESP-NOW, ACK timeout 400 ms, 3 retries.
- **Native-purity CI guard**: `.github/workflows/pr-validation.yml` enforces `lib_native/AstrOsBulkTransport` purity. M2 must not introduce any ESP-IDF / FreeRTOS / driver header.

## File structure

**Modified:**
- `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` — append `BulkSender` class + 7 new result types (`BeginSenderResult`, `BeginAckResult`, `SendResult`, `AckResult`, `NakResult`, `TickResult`, `EndAckResult`) + `InFlightEntry` struct + `MAX_WINDOW_SIZE` constant. All inside the existing `namespace AstrOsBulkTransport`.
- `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp` — append `BulkSender` method implementations + file-scope `static_assert` blocks pinning the new enum values.
- `test/test_native/bulk_transport_tests.cpp` — append new section block "BulkSender" with ~11 tests. Anonymous-namespace helper `drainSends` for happy-path window pumping.

No new files. No `lib/` (MIXED) changes. No `src/main.cpp` changes. No platformio.ini changes.

## Wire-format reference (cross-check against frozen contract)

| Field | Wire byte | Use in BulkSender |
|---|---|---|
| `OTA_DATA_ACK.highestContiguousSeq` (u32) | bytes 1-4 | Drives `onDataAck(xferId, cumulativeSeq)` |
| `OTA_DATA_NAK.nextExpectedSeq` (u32) | bytes 5-8 | Drives `onDataNak(xferId, nextExpectedSeq, reason)` |
| `OTA_DATA_NAK.reason` (u8) | byte 10 | `NakReason` already defined for BulkReceiver |
| `OTA_END_ACK.status` (u8) | byte 1 | `OtaEndStatus` from M1 (`OtaWirePayloads.hpp`); drives `onEndAck(xferId, status)` |

The PURE sender doesn't see ESP-NOW frame bytes directly — the MIXED layer in M3 will parse with M1's `parseOta*` free functions and call `BulkSender` methods with the unpacked values.

---

## Task 1: Result-type scaffolding + `BulkSender::begin` + `onBeginAck` + `reset`

Lay the foundation: enum-based state machine, `MAX_WINDOW_SIZE` constant, `BeginSenderResult` and `BeginAckResult` result types with full factory discipline, plus the BEGIN-side state transitions. After this task, the sender can validate parameters and transition `IDLE → AWAITING_BEGIN_ACK → STREAMING`.

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkSender — M2 wire-format-compatible sender state machine.
//
// Mirrors BulkReceiver's result-type discipline: const-fields + private
// constructor + static factories on each result, [[nodiscard]] enforced
// per-type, file-scope static_assert pinning of wire-stable enum values.
//=================================================================================================

TEST(BulkTransport, BulkSenderStartsInIdle)
{
    AstrOsBulkTransport::BulkSender s;
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}

TEST(BulkTransport, BulkSenderBeginAcceptsValidParams)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(/*xferId=*/7, /*totalChunks=*/100, /*chunkSize=*/128,
                     /*windowSize=*/8, /*ackTimeoutMs=*/400, /*maxRetries=*/3);
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::OK, r.reason);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroTotalChunks)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(/*xferId=*/7, /*totalChunks=*/0, /*chunkSize=*/128,
                     /*windowSize=*/8, /*ackTimeoutMs=*/400, /*maxRetries=*/3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_TOTAL_CHUNKS, r.reason);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroChunkSize)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 0, 8, 400, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_CHUNK_SIZE, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroWindowSize)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 128, 0, 400, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_WINDOW_SIZE, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroAckTimeout)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 128, 8, 0, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_ACK_TIMEOUT, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsZeroMaxRetries)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.begin(7, 100, 128, 8, 400, 0);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::ZERO_MAX_RETRIES, r.reason);
}

TEST(BulkTransport, BulkSenderBeginRejectsWindowTooLarge)
{
    AstrOsBulkTransport::BulkSender s;
    // MAX_WINDOW_SIZE = 16; windowSize = 17 must reject.
    auto r = s.begin(7, 100, 128, 17, 400, 3);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(AstrOsBulkTransport::BeginSenderResult::Reason::WINDOW_TOO_LARGE, r.reason);
}

TEST(BulkTransport, BulkSenderOnBeginAckAdvancesToStreaming)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    auto r = s.onBeginAck(/*xferId=*/7);
    EXPECT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());
}

TEST(BulkTransport, BulkSenderOnBeginAckRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    auto r = s.onBeginAck(/*xferId=*/99);
    EXPECT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::WRONG_XFER_ID, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());
}

TEST(BulkTransport, BulkSenderOnBeginAckRejectsBeforeBegin)
{
    AstrOsBulkTransport::BulkSender s;
    // No begin() called — status is IDLE.
    auto r = s.onBeginAck(7);
    EXPECT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::NOT_AWAITING_BEGIN_ACK, r.decision);
}

TEST(BulkTransport, BulkSenderResetReturnsToIdle)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());
    s.reset();
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::IDLE, s.status());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: compile errors — `BulkSender`, `BeginSenderResult`, `BeginAckResult` are not members of `AstrOsBulkTransport`.

- [ ] **Step 3: Add the new types to the header**

In `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`, append the following AFTER the existing `BulkReceiver` class definition but BEFORE the closing `}` of the `namespace AstrOsBulkTransport`:

```cpp
    // Maximum window size BulkSender supports. Covers both the
    // ESP-NOW path (window=8 per the frozen contract) and the
    // serial path (window=16 per the same contract). Bounds the
    // in-flight table and TickResult retransmit array so the
    // sender stays heap-free.
    constexpr uint8_t MAX_WINDOW_SIZE = 16;

    // One row of the BulkSender in-flight table. The sender keeps
    // up to MAX_WINDOW_SIZE entries; each tracks a single sent-but-
    // not-yet-acknowledged seq with its latest send-timestamp and
    // retry count. Public for tests; not part of the wire contract.
    struct InFlightEntry
    {
        uint32_t seq = 0;
        uint64_t sendTimestampMs = 0;
        uint8_t retryCount = 0;
        bool occupied = false;
    };

    // [[nodiscard]] mirrors BulkReceiver::BeginResult — silently
    // dropping the return value would leave the caller unable to
    // distinguish "transfer ready" from "begin rejected, sender
    // still IDLE", which is the silent-failure mode this attribute
    // closes.
    struct [[nodiscard]] BeginSenderResult
    {
        enum class Reason : uint8_t
        {
            OK = 0,
            ZERO_TOTAL_CHUNKS = 1,
            ZERO_CHUNK_SIZE = 2,
            ZERO_WINDOW_SIZE = 3,
            ZERO_ACK_TIMEOUT = 4,
            ZERO_MAX_RETRIES = 5,
            WINDOW_TOO_LARGE = 6 // windowSize > MAX_WINDOW_SIZE
        };
        bool valid = false;
        Reason reason = Reason::ZERO_TOTAL_CHUNKS;

        static BeginSenderResult ok()
        {
            return {true, Reason::OK};
        }
        static BeginSenderResult invalid(Reason r)
        {
            return {false, r};
        }
    };

    struct [[nodiscard]] BeginAckResult
    {
        enum class Decision : uint8_t
        {
            OK = 0,
            WRONG_XFER_ID = 1,
            NOT_AWAITING_BEGIN_ACK = 2
        };
        Decision decision = Decision::NOT_AWAITING_BEGIN_ACK;

        static BeginAckResult ok()
        {
            return {Decision::OK};
        }
        static BeginAckResult wrongXferId()
        {
            return {Decision::WRONG_XFER_ID};
        }
        static BeginAckResult notAwaiting()
        {
            return {Decision::NOT_AWAITING_BEGIN_ACK};
        }
    };

    // Sliding-window sender state machine for the firmware OTA path.
    // Used by the master-side OtaForwarder (M3) to push chunks to a
    // padawan and react to ACK/NAK responses. PURE — no I/O, no
    // FreeRTOS, no ESP-IDF. The MIXED caller does the actual byte
    // emission; this class tracks "what to send next" and "is the
    // transfer healthy".
    //
    // State machine:
    //   IDLE
    //     → AWAITING_BEGIN_ACK (via begin())
    //     → STREAMING (via onBeginAck OK)
    //         ⮨ self (per-chunk send/ack/nak/tick cycle)
    //         → DONE_OK (via onEndAck OK)
    //         → ABANDONED (via onEndAck != OK, OR tick(abandon=true))
    //
    // AWAITING_END_ACK is intentionally NOT a distinct state — it's
    // implicit ("STREAMING with all seqs confirmed and in-flight
    // table empty"). The MIXED caller checks SendResult::ALL_SENT
    // to know when it's time to send OTA_END.
    class BulkSender
    {
    public:
        enum class Status : uint8_t
        {
            IDLE = 0,
            AWAITING_BEGIN_ACK = 1,
            STREAMING = 2,
            DONE_OK = 3,
            ABANDONED = 4
        };

        [[nodiscard]] BeginSenderResult begin(uint8_t xferId, uint32_t totalChunks, uint16_t chunkSize,
                                              uint8_t windowSize, uint32_t ackTimeoutMs, uint8_t maxRetries);
        [[nodiscard]] BeginAckResult onBeginAck(uint8_t xferId);
        void reset();
        Status status() const
        {
            return status_;
        }

    private:
        uint8_t xferId_ = 0;
        uint32_t totalChunks_ = 0;
        uint16_t chunkSize_ = 0;
        uint8_t windowSize_ = 0;
        uint32_t ackTimeoutMs_ = 0;
        uint8_t maxRetries_ = 0;

        uint32_t nextSeqToSend_ = 0;
        uint32_t highestConfirmedSeq_ = 0;
        bool anyConfirmed_ = false; // disambiguates "seq 0 confirmed" from "nothing confirmed yet"

        std::array<InFlightEntry, MAX_WINDOW_SIZE> inFlight_{};

        Status status_ = Status::IDLE;
    };
```

Also add the `<array>` include at the top of the file with the other includes:

```cpp
#include <array>
#include <cstddef>
#include <cstdint>
```

- [ ] **Step 4: Implement the methods**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, append at the end of the `namespace AstrOsBulkTransport` block (BEFORE the closing `}`):

```cpp
    // BulkSender::Status integer values are API-stable and consumed by
    // switch statements in M3's OtaForwarder. Pin them so a future
    // reorder breaks the build, not the dispatch.
    static_assert(static_cast<uint8_t>(BulkSender::Status::IDLE) == 0);
    static_assert(static_cast<uint8_t>(BulkSender::Status::AWAITING_BEGIN_ACK) == 1);
    static_assert(static_cast<uint8_t>(BulkSender::Status::STREAMING) == 2);
    static_assert(static_cast<uint8_t>(BulkSender::Status::DONE_OK) == 3);
    static_assert(static_cast<uint8_t>(BulkSender::Status::ABANDONED) == 4);

    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::OK) == 0);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_TOTAL_CHUNKS) == 1);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_CHUNK_SIZE) == 2);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_WINDOW_SIZE) == 3);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_ACK_TIMEOUT) == 4);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::ZERO_MAX_RETRIES) == 5);
    static_assert(static_cast<uint8_t>(BeginSenderResult::Reason::WINDOW_TOO_LARGE) == 6);

    static_assert(static_cast<uint8_t>(BeginAckResult::Decision::OK) == 0);
    static_assert(static_cast<uint8_t>(BeginAckResult::Decision::WRONG_XFER_ID) == 1);
    static_assert(static_cast<uint8_t>(BeginAckResult::Decision::NOT_AWAITING_BEGIN_ACK) == 2);

    BeginSenderResult BulkSender::begin(uint8_t xferId, uint32_t totalChunks, uint16_t chunkSize, uint8_t windowSize,
                                        uint32_t ackTimeoutMs, uint8_t maxRetries)
    {
        // Reject protocol-illegal parameters in the order the contract
        // freezes them. Each rejection leaves the sender in IDLE so a
        // subsequent corrected begin() works cleanly. Order matters: a
        // future reorder of these guards would change which Reason a
        // caller sees for multi-fault input.
        if (totalChunks == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_TOTAL_CHUNKS);
        }
        if (chunkSize == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_CHUNK_SIZE);
        }
        if (windowSize == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_WINDOW_SIZE);
        }
        if (windowSize > MAX_WINDOW_SIZE)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::WINDOW_TOO_LARGE);
        }
        if (ackTimeoutMs == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_ACK_TIMEOUT);
        }
        if (maxRetries == 0)
        {
            reset();
            return BeginSenderResult::invalid(BeginSenderResult::Reason::ZERO_MAX_RETRIES);
        }

        xferId_ = xferId;
        totalChunks_ = totalChunks;
        chunkSize_ = chunkSize;
        windowSize_ = windowSize;
        ackTimeoutMs_ = ackTimeoutMs;
        maxRetries_ = maxRetries;
        nextSeqToSend_ = 0;
        highestConfirmedSeq_ = 0;
        anyConfirmed_ = false;
        for (auto &e : inFlight_)
        {
            e = InFlightEntry{};
        }
        status_ = Status::AWAITING_BEGIN_ACK;
        return BeginSenderResult::ok();
    }

    BeginAckResult BulkSender::onBeginAck(uint8_t xferId)
    {
        if (status_ != Status::AWAITING_BEGIN_ACK)
        {
            return BeginAckResult::notAwaiting();
        }
        if (xferId != xferId_)
        {
            return BeginAckResult::wrongXferId();
        }
        status_ = Status::STREAMING;
        return BeginAckResult::ok();
    }

    void BulkSender::reset()
    {
        xferId_ = 0;
        totalChunks_ = 0;
        chunkSize_ = 0;
        windowSize_ = 0;
        ackTimeoutMs_ = 0;
        maxRetries_ = 0;
        nextSeqToSend_ = 0;
        highestConfirmedSeq_ = 0;
        anyConfirmed_ = false;
        for (auto &e : inFlight_)
        {
            e = InFlightEntry{};
        }
        status_ = Status::IDLE;
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: all 12 new `BulkTransport.BulkSender*` tests PASS.

Full suite: `pio test -e test 2>&1 | tail -5`
Expected: existing baseline + 12 new tests pass. (Baseline is `pio test -e test`'s pre-M2 count on `develop` after M1 merge.)

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp \
        lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "feat(bulk-transport): add BulkSender begin/onBeginAck/reset + scaffolding"
```

---

## Task 2: `SendResult` + `nextChunkToSend` + in-flight table claim

Add the send-side primitive. `nextChunkToSend(nowMs)` claims an in-flight slot, records the send timestamp, advances `nextSeqToSend_`, and returns the seq to emit. Returns `WINDOW_FULL` when the in-flight count reaches `windowSize_`, `ALL_SENT` when `nextSeqToSend_ >= totalChunks_`, or `NOT_STREAMING` if state is wrong.

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
namespace
{
    // Helper: drive `nextChunkToSend` in a loop until WINDOW_FULL or ALL_SENT.
    // Records each emitted seq in `outSeqs`. Returns the final Decision.
    AstrOsBulkTransport::SendResult::Decision drainSends(AstrOsBulkTransport::BulkSender &s, uint64_t nowMs,
                                                          std::vector<uint32_t> &outSeqs)
    {
        for (;;)
        {
            auto r = s.nextChunkToSend(nowMs);
            if (r.decision == AstrOsBulkTransport::SendResult::Decision::SEND)
            {
                outSeqs.push_back(r.seq);
                continue;
            }
            return r.decision;
        }
    }
} // namespace

TEST(BulkTransport, BulkSenderNextChunkRejectedBeforeStreaming)
{
    AstrOsBulkTransport::BulkSender s;
    auto r = s.nextChunkToSend(/*nowMs=*/0);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::NOT_STREAMING, r.decision);
}

TEST(BulkTransport, BulkSenderNextChunkAfterBeginAckEmitsSeqZero)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    auto r = s.nextChunkToSend(/*nowMs=*/1000);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, r.decision);
    EXPECT_EQ(0u, r.seq);
}

TEST(BulkTransport, BulkSenderFillsWindowThenStopsWithWindowFull)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/100, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    auto terminal = drainSends(s, 1000, sent);

    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::WINDOW_FULL, terminal);
    ASSERT_EQ(8u, sent.size());
    for (uint32_t i = 0; i < 8; i++)
    {
        EXPECT_EQ(i, sent[i]);
    }
}

TEST(BulkTransport, BulkSenderEmitsAllChunksWhenTotalIsBelowWindow)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, /*totalChunks=*/3, 128, /*windowSize=*/8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    auto terminal = drainSends(s, 1000, sent);

    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::ALL_SENT, terminal);
    ASSERT_EQ(3u, sent.size());
    EXPECT_EQ(0u, sent[0]);
    EXPECT_EQ(1u, sent[1]);
    EXPECT_EQ(2u, sent[2]);
}
```

Also ensure `#include <vector>` is at the top of the test file (it should already be there from the existing tests; if not, add it).

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: compile errors — `SendResult` and `BulkSender::nextChunkToSend` don't exist.

- [ ] **Step 3: Add `SendResult` and the method declaration**

In `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`, add the `SendResult` struct AFTER `BeginAckResult` and BEFORE the `BulkSender` class:

```cpp
    // Result of BulkSender::nextChunkToSend. On SEND, `seq` is the seq
    // to emit on the wire and the in-flight table now owns a slot for
    // it. Other Decisions leave the sender state unchanged.
    //
    // Const-fields + private constructor mirror ChunkResult's
    // discipline so a returned result cannot be mutated into an
    // invalid combination (e.g. WINDOW_FULL with a real seq).
    struct SendResult
    {
        enum class Decision : uint8_t
        {
            SEND = 0,          // emit `seq` on the wire
            WINDOW_FULL = 1,   // in-flight count == windowSize; ACK something first
            ALL_SENT = 2,      // nextSeqToSend_ == totalChunks_; nothing left to send
            NOT_STREAMING = 3  // status != STREAMING
        };

        const Decision decision;
        const uint32_t seq;

        static SendResult send(uint32_t s)
        {
            return SendResult(Decision::SEND, s);
        }
        static SendResult windowFull()
        {
            return SendResult(Decision::WINDOW_FULL, 0);
        }
        static SendResult allSent()
        {
            return SendResult(Decision::ALL_SENT, 0);
        }
        static SendResult notStreaming()
        {
            return SendResult(Decision::NOT_STREAMING, 0);
        }

    private:
        SendResult(Decision d, uint32_t s) : decision(d), seq(s)
        {
        }
    };
```

Add the declaration to the `BulkSender` `public:` section (after `onBeginAck`, before `reset`):

```cpp
        [[nodiscard]] SendResult nextChunkToSend(uint64_t nowMs);
```

- [ ] **Step 4: Implement `nextChunkToSend`**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, append to the `namespace AstrOsBulkTransport` block:

Add the static_asserts near the other BulkSender ones:

```cpp
    static_assert(static_cast<uint8_t>(SendResult::Decision::SEND) == 0);
    static_assert(static_cast<uint8_t>(SendResult::Decision::WINDOW_FULL) == 1);
    static_assert(static_cast<uint8_t>(SendResult::Decision::ALL_SENT) == 2);
    static_assert(static_cast<uint8_t>(SendResult::Decision::NOT_STREAMING) == 3);

    // SendResult immutability: every public field must be const so a
    // factory-produced result can't be mutated into an invalid combo
    // (WINDOW_FULL with a meaningful seq, etc.). Mirrors ChunkResult's
    // discipline.
    static_assert(std::is_const_v<decltype(SendResult::decision)>);
    static_assert(std::is_const_v<decltype(SendResult::seq)>);
    static_assert(!std::is_copy_assignable_v<SendResult>);
    static_assert(std::is_copy_constructible_v<SendResult>);
```

Add the method implementation:

```cpp
    SendResult BulkSender::nextChunkToSend(uint64_t nowMs)
    {
        if (status_ != Status::STREAMING)
        {
            return SendResult::notStreaming();
        }

        // ALL_SENT check first: if every chunk has been launched into
        // the in-flight table (or already evicted by ACK), there's
        // nothing left to claim. The MIXED caller treats ALL_SENT
        // (plus an empty in-flight table) as the signal to send OTA_END.
        if (nextSeqToSend_ >= totalChunks_)
        {
            return SendResult::allSent();
        }

        // Count occupied slots. WINDOW_FULL fires when we've already
        // launched `windowSize_` chunks that haven't been acked yet.
        // O(MAX_WINDOW_SIZE) linear scan; trivial at 16 entries.
        uint8_t occupied = 0;
        for (const auto &e : inFlight_)
        {
            if (e.occupied)
            {
                occupied++;
            }
        }
        if (occupied >= windowSize_)
        {
            return SendResult::windowFull();
        }

        // Claim the first unoccupied slot. The slot index is internal;
        // callers identify chunks by seq, not slot.
        for (auto &e : inFlight_)
        {
            if (!e.occupied)
            {
                e.seq = nextSeqToSend_;
                e.sendTimestampMs = nowMs;
                e.retryCount = 0;
                e.occupied = true;
                const uint32_t emittedSeq = nextSeqToSend_;
                nextSeqToSend_++;
                return SendResult::send(emittedSeq);
            }
        }

        // Unreachable: if `occupied < windowSize_ <= MAX_WINDOW_SIZE`,
        // at least one slot is free. The early-return above already
        // returned WINDOW_FULL in the full case.
        assert(false && "BulkSender::nextChunkToSend: in-flight table full despite occupied < windowSize_");
        std::abort();
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: 4 new tests PASS.

Full suite: `pio test -e test 2>&1 | tail -5`
Expected: existing tests + cumulative new tests pass (12 from T1 + 4 from T2 = 16 new).

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp \
        lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "feat(bulk-transport): add BulkSender::nextChunkToSend + SendResult"
```

---

## Task 3: `AckResult` + `NakResult` + `onDataAck` + `onDataNak`

Add cumulative-ACK and NAK handling. `onDataAck` advances `highestConfirmedSeq_` and evicts confirmed in-flight slots. `onDataNak` rewinds `nextSeqToSend_` and evicts seqs >= the NAK's nextExpectedSeq, so the next `nextChunkToSend` call resends from the right point.

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
TEST(BulkTransport, BulkSenderOnDataAckAdvancesHighestConfirmed)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);
    ASSERT_EQ(8u, sent.size()); // window full at 0..7

    // Cumulative ACK up through seq 3: frees slots for 0..3.
    auto r = s.onDataAck(7, /*cumulativeSeq=*/3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, r.decision);
    EXPECT_EQ(4u, r.newlyConfirmedCount); // seqs 0, 1, 2, 3 newly confirmed

    // Now nextChunkToSend should emit seq 8 (window frees 4 slots).
    auto sr = s.nextChunkToSend(1100);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, sr.decision);
    EXPECT_EQ(8u, sr.seq);
}

TEST(BulkTransport, BulkSenderRejectsStaleAck)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);

    ASSERT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, s.onDataAck(7, 5).decision);
    // Stale: cumulativeSeq=3 already covered by the previous ACK at 5.
    auto r = s.onDataAck(7, 3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::STALE, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataAckRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);
    auto r = s.onDataAck(/*xferId=*/99, 3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::WRONG_XFER_ID, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataAckRejectsBeforeStreaming)
{
    AstrOsBulkTransport::BulkSender s;
    // No begin / onBeginAck — status is IDLE.
    auto r = s.onDataAck(7, 0);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::NOT_STREAMING, r.decision);
}

TEST(BulkTransport, BulkSenderOnDataNakRewindsToNextExpectedSeq)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);
    ASSERT_EQ(8u, sent.size()); // sent 0..7

    // Receiver NAKs with nextExpectedSeq=3 — meaning seqs 0..2 are OK,
    // but seq 3 needs retransmission (and 4..7 will be discarded by
    // the receiver since they're out of order).
    auto r = s.onDataNak(7, /*nextExpectedSeq=*/3, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::OK, r.decision);
    EXPECT_EQ(3u, r.nextSeqToResend);

    // The next send should re-emit seq 3 (not 8).
    auto sr = s.nextChunkToSend(1100);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::SEND, sr.decision);
    EXPECT_EQ(3u, sr.seq);
}

TEST(BulkTransport, BulkSenderOnDataNakRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);
    auto r = s.onDataNak(/*xferId=*/99, 3, AstrOsBulkTransport::NakReason::CRC);
    EXPECT_EQ(AstrOsBulkTransport::NakResult::Decision::WRONG_XFER_ID, r.decision);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: compile errors — `AckResult`, `NakResult`, `BulkSender::onDataAck`, `BulkSender::onDataNak` don't exist.

- [ ] **Step 3: Add `AckResult` and `NakResult` to the header**

In `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`, add AFTER `SendResult` and BEFORE the `BulkSender` class:

```cpp
    // Result of BulkSender::onDataAck. On OK, `newlyConfirmedCount` is
    // the number of in-flight slots freed by this cumulative ACK —
    // useful when the caller wants to know "did room open up that
    // wasn't there before?" without diffing internal state.
    struct AckResult
    {
        enum class Decision : uint8_t
        {
            OK = 0,
            WRONG_XFER_ID = 1,
            NOT_STREAMING = 2,
            STALE = 3 // cumulativeSeq <= highestConfirmedSeq_ (already covered)
        };

        const Decision decision;
        const uint32_t newlyConfirmedCount;

        static AckResult ok(uint32_t n)
        {
            return AckResult(Decision::OK, n);
        }
        static AckResult wrongXferId()
        {
            return AckResult(Decision::WRONG_XFER_ID, 0);
        }
        static AckResult notStreaming()
        {
            return AckResult(Decision::NOT_STREAMING, 0);
        }
        static AckResult stale()
        {
            return AckResult(Decision::STALE, 0);
        }

    private:
        AckResult(Decision d, uint32_t n) : decision(d), newlyConfirmedCount(n)
        {
        }
    };

    // Result of BulkSender::onDataNak. On OK, `nextSeqToResend` is the
    // seq that the next `nextChunkToSend` call will emit (mirrors the
    // wire-level `nextExpectedSeq` field from OTA_DATA_NAK).
    struct NakResult
    {
        enum class Decision : uint8_t
        {
            OK = 0,
            WRONG_XFER_ID = 1,
            NOT_STREAMING = 2
        };

        const Decision decision;
        const uint32_t nextSeqToResend;

        static NakResult ok(uint32_t s)
        {
            return NakResult(Decision::OK, s);
        }
        static NakResult wrongXferId()
        {
            return NakResult(Decision::WRONG_XFER_ID, 0);
        }
        static NakResult notStreaming()
        {
            return NakResult(Decision::NOT_STREAMING, 0);
        }

    private:
        NakResult(Decision d, uint32_t s) : decision(d), nextSeqToResend(s)
        {
        }
    };
```

Add to the `BulkSender` `public:` section:

```cpp
        [[nodiscard]] AckResult onDataAck(uint8_t xferId, uint32_t cumulativeSeq);
        [[nodiscard]] NakResult onDataNak(uint8_t xferId, uint32_t nextExpectedSeq, NakReason reason);
```

- [ ] **Step 4: Implement the methods**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, add the static_asserts:

```cpp
    static_assert(static_cast<uint8_t>(AckResult::Decision::OK) == 0);
    static_assert(static_cast<uint8_t>(AckResult::Decision::WRONG_XFER_ID) == 1);
    static_assert(static_cast<uint8_t>(AckResult::Decision::NOT_STREAMING) == 2);
    static_assert(static_cast<uint8_t>(AckResult::Decision::STALE) == 3);

    static_assert(static_cast<uint8_t>(NakResult::Decision::OK) == 0);
    static_assert(static_cast<uint8_t>(NakResult::Decision::WRONG_XFER_ID) == 1);
    static_assert(static_cast<uint8_t>(NakResult::Decision::NOT_STREAMING) == 2);

    static_assert(std::is_const_v<decltype(AckResult::decision)>);
    static_assert(std::is_const_v<decltype(AckResult::newlyConfirmedCount)>);
    static_assert(!std::is_copy_assignable_v<AckResult>);

    static_assert(std::is_const_v<decltype(NakResult::decision)>);
    static_assert(std::is_const_v<decltype(NakResult::nextSeqToResend)>);
    static_assert(!std::is_copy_assignable_v<NakResult>);
```

Add the method implementations:

```cpp
    AckResult BulkSender::onDataAck(uint8_t xferId, uint32_t cumulativeSeq)
    {
        if (status_ != Status::STREAMING)
        {
            return AckResult::notStreaming();
        }
        if (xferId != xferId_)
        {
            return AckResult::wrongXferId();
        }
        // Stale check: cumulativeSeq must strictly advance from the
        // previous high-water mark. The `anyConfirmed_` flag
        // disambiguates "no ACK yet seen" (any cumulativeSeq is fresh)
        // from "seq 0 already confirmed" (only cumulativeSeq > 0 is
        // fresh).
        if (anyConfirmed_ && cumulativeSeq <= highestConfirmedSeq_)
        {
            return AckResult::stale();
        }

        const uint32_t prev = anyConfirmed_ ? highestConfirmedSeq_ + 1 : 0;
        highestConfirmedSeq_ = cumulativeSeq;
        anyConfirmed_ = true;

        // Evict all in-flight slots whose seq <= cumulativeSeq.
        for (auto &e : inFlight_)
        {
            if (e.occupied && e.seq <= cumulativeSeq)
            {
                e = InFlightEntry{};
            }
        }

        const uint32_t newlyConfirmed = cumulativeSeq + 1 - prev;
        return AckResult::ok(newlyConfirmed);
    }

    NakResult BulkSender::onDataNak(uint8_t xferId, uint32_t nextExpectedSeq, NakReason /*reason*/)
    {
        if (status_ != Status::STREAMING)
        {
            return NakResult::notStreaming();
        }
        if (xferId != xferId_)
        {
            return NakResult::wrongXferId();
        }

        // Rewind: future sends start from nextExpectedSeq. Evict all
        // in-flight slots whose seq >= nextExpectedSeq — they were
        // either NAK'd directly or discarded by the receiver as out-
        // of-order. The MIXED layer doesn't need to know which slot
        // belongs to which seq; it just calls nextChunkToSend in a
        // loop after a NAK.
        //
        // Reason is currently unused by the state machine (the wire
        // layer logs it in M3); it's in the signature so future
        // reason-aware retry policies don't break the API.
        nextSeqToSend_ = nextExpectedSeq;
        for (auto &e : inFlight_)
        {
            if (e.occupied && e.seq >= nextExpectedSeq)
            {
                e = InFlightEntry{};
            }
        }
        return NakResult::ok(nextExpectedSeq);
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: 6 new tests PASS.

Full suite: `pio test -e test 2>&1 | tail -5`
Expected: all tests still pass.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp \
        lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "feat(bulk-transport): add BulkSender onDataAck + onDataNak"
```

---

## Task 4: `TickResult` + `tick` + retry/abandon

Add timer-driven retransmission. `tick(nowMs)` scans the in-flight table; for each occupied slot whose `sendTimestampMs + ackTimeoutMs_ <= nowMs`, it increments `retryCount` (if not over the cap), resets `sendTimestampMs` to `nowMs`, and appends the seq to `TickResult::retransmitSeqs`. If any slot's `retryCount` would exceed `maxRetries_`, transition to `ABANDONED` and set `TickResult::abandon = true`.

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
TEST(BulkTransport, BulkSenderTickReturnsNothingWhenNoTimeoutsFired)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, /*ackTimeoutMs=*/400, /*maxRetries=*/3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, /*nowMs=*/1000, sent);
    ASSERT_EQ(8u, sent.size());

    auto t = s.tick(/*nowMs=*/1100); // only 100 ms passed — under the 400 ms timeout
    EXPECT_EQ(0, t.count);
    EXPECT_FALSE(t.abandon);
}

TEST(BulkTransport, BulkSenderTickRetransmitsTimedOutSeqs)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 1000, sent);

    auto t = s.tick(/*nowMs=*/1500); // 500 ms passed — past the 400 ms timeout
    EXPECT_EQ(8, t.count);
    EXPECT_FALSE(t.abandon);
    // All 8 in-flight seqs should be flagged for retransmission.
    for (uint8_t i = 0; i < 8; i++)
    {
        EXPECT_EQ(i, t.retransmitSeqs[i]);
    }
}

TEST(BulkTransport, BulkSenderTickAbandonsAfterMaxRetries)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, /*ackTimeoutMs=*/400, /*maxRetries=*/2).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    std::vector<uint32_t> sent;
    drainSends(s, 0, sent);

    // First tick at 500 ms: retryCount 0 → 1 for every seq.
    auto t1 = s.tick(500);
    EXPECT_EQ(8, t1.count);
    EXPECT_FALSE(t1.abandon);

    // Second tick at 1000 ms: retryCount 1 → 2 for every seq.
    auto t2 = s.tick(1000);
    EXPECT_EQ(8, t2.count);
    EXPECT_FALSE(t2.abandon);

    // Third tick at 1500 ms: retryCount would go 2 → 3 (> maxRetries=2).
    // Abandon, no further retransmissions.
    auto t3 = s.tick(1500);
    EXPECT_TRUE(t3.abandon);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::ABANDONED, s.status());
}

TEST(BulkTransport, BulkSenderTickReturnsZeroOnInactive)
{
    AstrOsBulkTransport::BulkSender s;
    // Never called begin — IDLE.
    auto t = s.tick(10000);
    EXPECT_EQ(0, t.count);
    EXPECT_FALSE(t.abandon);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: compile errors — `TickResult`, `BulkSender::tick` don't exist.

- [ ] **Step 3: Add `TickResult` and the method declaration**

In `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`, add AFTER `NakResult` and BEFORE the `BulkSender` class:

```cpp
    // Result of BulkSender::tick. `retransmitSeqs[0..count-1]` are the
    // seqs whose ackTimeout fired and need re-emission on the wire.
    // `abandon` is set when any seq's retryCount would exceed
    // maxRetries — at that point the sender transitions to ABANDONED
    // and stops issuing further retransmit lists.
    //
    // Bounded by MAX_WINDOW_SIZE so this struct stays heap-free. A
    // single tick can produce at most `windowSize` retransmissions
    // (the in-flight table is the upper bound).
    struct TickResult
    {
        std::array<uint32_t, MAX_WINDOW_SIZE> retransmitSeqs;
        uint8_t count = 0;
        bool abandon = false;
    };
```

Add to the `BulkSender` `public:` section:

```cpp
        [[nodiscard]] TickResult tick(uint64_t nowMs);
```

Note: `TickResult` does NOT use the const-fields-private-ctor pattern because the fields are written by `tick` directly (count up to count, then return). The other result types are pure observations of an event; `TickResult` is structurally an output buffer. Convention: zero-initialize the array and trust `count`.

- [ ] **Step 4: Implement `tick`**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, add the method implementation:

```cpp
    TickResult BulkSender::tick(uint64_t nowMs)
    {
        TickResult result{};
        if (status_ != Status::STREAMING)
        {
            return result;
        }

        for (auto &e : inFlight_)
        {
            if (!e.occupied)
            {
                continue;
            }
            // Timeout fired iff (nowMs - sendTimestampMs) >= ackTimeoutMs_.
            // Use subtraction in uint64_t so wrap-around isn't a concern at
            // typical millisecond scales; if a future caller passes nowMs <
            // sendTimestampMs (clock went backwards), the underflow wraps
            // to a huge value and the timeout looks "fired" — acceptable
            // failure mode (retransmits a slot that wasn't actually timed
            // out). The MIXED caller is responsible for monotonic time.
            if (nowMs - e.sendTimestampMs < ackTimeoutMs_)
            {
                continue;
            }

            // Retry count check: if incrementing would exceed maxRetries_,
            // abandon the whole transfer immediately. The wire-level
            // contract says "abort this padawan, move to next" — at the
            // PURE layer we just transition to ABANDONED.
            if (e.retryCount + 1 > maxRetries_)
            {
                status_ = Status::ABANDONED;
                result.abandon = true;
                result.count = 0;
                return result;
            }

            e.retryCount++;
            e.sendTimestampMs = nowMs;
            result.retransmitSeqs[result.count] = e.seq;
            result.count++;
        }
        return result;
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: 4 new tests PASS.

Full suite: `pio test -e test 2>&1 | tail -5`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp \
        lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "feat(bulk-transport): add BulkSender::tick + retry/abandon"
```

---

## Task 5: `EndAckResult` + `onEndAck` + end-to-end integration test

Add the terminal-state transitions. `onEndAck(xferId, OtaEndStatus)` validates the transfer ID, then transitions to `DONE_OK` if status==OK or `ABANDONED` otherwise. Plus one end-to-end integration test that drives a sender through a full happy-path transfer using all the M2 primitives together.

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
TEST(BulkTransport, BulkSenderOnEndAckOkTransitionsToDoneOk)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    auto r = s.onEndAck(7, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::DONE_OK, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::DONE_OK, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckHashMismatchAbandons)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    auto r = s.onEndAck(7, OtaEndStatus::HASH_MISMATCH);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::ABANDONED, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::ABANDONED, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckWriteErrorAbandons)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);

    auto r = s.onEndAck(7, OtaEndStatus::WRITE_ERROR);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::ABANDONED, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::ABANDONED, s.status());
}

TEST(BulkTransport, BulkSenderOnEndAckRejectsWrongXferId)
{
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(7, 100, 128, 8, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(7).decision);
    auto r = s.onEndAck(/*xferId=*/99, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::WRONG_XFER_ID, r.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());
}

TEST(BulkTransport, BulkSenderEndToEndHappyPath)
{
    // Drives the full state machine through a small transfer with the
    // exact API call pattern M3's OtaForwarder will use.
    AstrOsBulkTransport::BulkSender s;
    ASSERT_TRUE(s.begin(/*xferId=*/42, /*totalChunks=*/4, 128, /*windowSize=*/4, 400, 3).valid);
    ASSERT_EQ(AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK, s.status());

    ASSERT_EQ(AstrOsBulkTransport::BeginAckResult::Decision::OK, s.onBeginAck(42).decision);
    ASSERT_EQ(AstrOsBulkTransport::BulkSender::Status::STREAMING, s.status());

    // Pump nextChunkToSend until ALL_SENT.
    std::vector<uint32_t> sent;
    auto term = drainSends(s, 1000, sent);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::WINDOW_FULL, term);
    ASSERT_EQ(4u, sent.size()); // window=4 fills before reaching ALL_SENT

    // Receiver cumulatively ACKs all 4.
    auto ackR = s.onDataAck(42, /*cumulativeSeq=*/3);
    EXPECT_EQ(AstrOsBulkTransport::AckResult::Decision::OK, ackR.decision);
    EXPECT_EQ(4u, ackR.newlyConfirmedCount);

    // Try to send more — should report ALL_SENT (since all 4 chunks were already emitted).
    auto sendR = s.nextChunkToSend(1100);
    EXPECT_EQ(AstrOsBulkTransport::SendResult::Decision::ALL_SENT, sendR.decision);

    // Send OTA_END, receiver replies OK.
    auto endR = s.onEndAck(42, OtaEndStatus::OK);
    EXPECT_EQ(AstrOsBulkTransport::EndAckResult::Decision::DONE_OK, endR.decision);
    EXPECT_EQ(AstrOsBulkTransport::BulkSender::Status::DONE_OK, s.status());
}
```

Note: the tests use `OtaEndStatus` from M1 (`OtaWirePayloads.hpp`). That's pulled in transitively via `<AstrOsBulkTransport.hpp>` once we add the include below.

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: compile errors — `EndAckResult`, `BulkSender::onEndAck`, `OtaEndStatus` not visible.

- [ ] **Step 3: Add `EndAckResult` and pull in M1's wire payloads**

In `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`, add the include at the top (after the existing `<cstdint>` etc.):

```cpp
#include <OtaWirePayloads.hpp>
```

This pulls in `OtaEndStatus` from M1's PURE wire-format header. Both libs are in `lib_native/`, so the include resolves cleanly through PlatformIO's `lib_extra_dirs = lib_native` setting.

Add the `EndAckResult` struct AFTER `TickResult` and BEFORE the `BulkSender` class:

```cpp
    // Result of BulkSender::onEndAck. Terminal transitions only —
    // after DONE_OK or ABANDONED the sender stays in that state until
    // reset(). WRONG_XFER_ID and NOT_STREAMING leave state unchanged.
    struct EndAckResult
    {
        enum class Decision : uint8_t
        {
            DONE_OK = 0,
            ABANDONED = 1,
            WRONG_XFER_ID = 2,
            NOT_STREAMING = 3
        };

        const Decision decision;

        static EndAckResult doneOk()
        {
            return EndAckResult(Decision::DONE_OK);
        }
        static EndAckResult abandoned()
        {
            return EndAckResult(Decision::ABANDONED);
        }
        static EndAckResult wrongXferId()
        {
            return EndAckResult(Decision::WRONG_XFER_ID);
        }
        static EndAckResult notStreaming()
        {
            return EndAckResult(Decision::NOT_STREAMING);
        }

    private:
        explicit EndAckResult(Decision d) : decision(d)
        {
        }
    };
```

Add to the `BulkSender` `public:` section:

```cpp
        [[nodiscard]] EndAckResult onEndAck(uint8_t xferId, OtaEndStatus status);
```

- [ ] **Step 4: Implement `onEndAck`**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, add the static_asserts:

```cpp
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::DONE_OK) == 0);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::ABANDONED) == 1);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::WRONG_XFER_ID) == 2);
    static_assert(static_cast<uint8_t>(EndAckResult::Decision::NOT_STREAMING) == 3);

    static_assert(std::is_const_v<decltype(EndAckResult::decision)>);
    static_assert(!std::is_copy_assignable_v<EndAckResult>);
```

Add the method implementation:

```cpp
    EndAckResult BulkSender::onEndAck(uint8_t xferId, OtaEndStatus status)
    {
        if (status_ != Status::STREAMING)
        {
            return EndAckResult::notStreaming();
        }
        if (xferId != xferId_)
        {
            return EndAckResult::wrongXferId();
        }

        // OK is the only success status; HASH_MISMATCH and WRITE_ERROR
        // both abandon. The MIXED layer (M3) logs the specific status
        // for diagnostics; the state machine only cares about
        // success-vs-abandon.
        if (status == OtaEndStatus::OK)
        {
            status_ = Status::DONE_OK;
            return EndAckResult::doneOk();
        }
        status_ = Status::ABANDONED;
        return EndAckResult::abandoned();
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*test_native*" 2>&1 | tail -20`
Expected: 5 new tests PASS (4 onEndAck cases + 1 end-to-end integration).

Full suite: `pio test -e test 2>&1 | tail -5`
Expected: all tests pass. M2 adds **31 new tests** total across T1-T5 (12 + 4 + 6 + 4 + 5).

Build both boards to confirm the firmware still compiles:

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp \
        lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "feat(bulk-transport): add BulkSender::onEndAck + end-to-end test"
```

---

## Task 6: Final verification + PR-ready housekeeping

NO code changes — verification only. Confirms M2 is complete, gates pass, and the branch is ready to push.

**Files:** none

- [ ] **Step 1: Final test suite run**

```bash
pio test -e test 2>&1 | tail -10
```

Expected: 100% pass. M2 adds 31 new tests; total should be (M1-baseline) + 31.

- [ ] **Step 2: Both board builds**

```bash
pio run -e metro_s3 2>&1 | tail -5
pio run -e lolin_d32_pro 2>&1 | tail -5
```

Expected: both SUCCESS.

- [ ] **Step 3: clang-format check on the branch**

```bash
git diff --name-only develop...HEAD | grep -E '\.(hpp|cpp|h)$' \
  | xargs -I {} /home/jeff/.platformio/penv/bin/clang-format --dry-run --Werror {} 2>&1
```

Expected: empty output (all changed files clang-format clean). If anything fails, run `clang-format -i <file>` and commit a fixup before continuing.

- [ ] **Step 4: PURE-lib purity sweep**

```bash
grep -RnE 'esp_|freertos/|driver/|<esp_|<freertos|<driver' \
    lib_native/AstrOsBulkTransport/include/ \
    lib_native/AstrOsBulkTransport/src/ \
    | grep -v ' *//' || echo "PURE OK"
```

Expected: `PURE OK` — no ESP-IDF / FreeRTOS / driver includes leaked into the PURE tree. (Doc comments mentioning `esp_*` are filtered out by `grep -v ' *//'`.)

- [ ] **Step 5: Branch summary**

```bash
git log --oneline develop..HEAD
```

Expected: 5 commits, one per T1-T5, all on `feature/ota-mesh-forward-m2-bulk-sender`.

- [ ] **Step 6: Report PR-ready status**

M2 is feature-complete. Branch carries 5 commits implementing `BulkSender` as a PURE state machine, with 31 new native tests covering happy paths, every rejection path, and the full end-to-end transfer flow. Ready for `git push` and `gh pr create` from the user's authenticated VS Code terminal. **Do NOT push from this session** — that's the user's job per the project's command-execution convention.

---

## Out of scope for M2 (do NOT add)

- Any MIXED-side wiring (`lib/` changes) — M3
- `OtaForwarder` — M3
- ESP-NOW binary TX path — M3
- Pause-polling integration — M3
- Padawan-side `OtaWriter` — M4
- `FW_PROGRESS` emission — M5
- `FW_DEPLOY_BEGIN` orchestration — M5

## Verification gates per the brief

- `pio test -e test` — 100% pass with 31 new BulkSender tests
- `pio run -e metro_s3` — clean
- `pio run -e lolin_d32_pro` — clean
- `clang-format` — clean on changed files
- Native-purity CI guard (`.github/workflows/pr-validation.yml`) — passes (PURE lib unchanged in classification; no new includes leak)
