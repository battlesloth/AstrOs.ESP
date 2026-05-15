# Firmware OTA Phase 2 — Bulk Transport State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Post-implementation amendments (commits `6123a6a`, `fe87535`, and the
> PR-toolkit cleanup pass on `feature/ota-phase2-bulk-transport`):** the
> original plan claimed `crc16_ccitt_false` was byte-identical to ESP-IDF's
> `esp_crc16_le`. That is wrong — `esp_crc16_le` is CRC-16/CCITT
> *reflected* with init=0 and produces different output. The correct
> ESP-IDF equivalent is `~esp_rom_crc16_be((uint16_t)~0xFFFF, buf, len)`.
> The plan also said the single-zero-byte CRC value is `0x1EF0`; the
> correct value is `0xE1F0` (nibbles transposed). The shipped README,
> `.hpp`, and tests have the corrected forms; the historical text in this
> plan is preserved with inline corrections (`~~strikethrough~~ →
> correction`) so the original intent is auditable. Test totals also moved
> (22 → 26 cases, 279 → 287 total) after `fe87535` added 4 strengthening
> tests. Final PR-toolkit pass on the same branch added `BeginResult`,
> `EndResult::Reason`, `ChunkResult` factory methods, totalSize/windowSize
> validation, and a `seq >= totalChunks` overflow guard — see the cleanup
> commits for the API redesign rationale.

**Goal:** Build `lib_native/AstrOsBulkTransport` — a PURE, native-testable state machine that consumes parsed `FW_CHUNK` records and produces ACK/NAK decisions with sliding-window backpressure. No firmware behavior change; this is the algorithmic core that Phase 3 will wire into the `AstrOsSerialMsgHandler` dispatch path.

**Architecture:** A small `BulkReceiver` class plus a standalone CRC-16/CCITT-FALSE helper. Sequential receive (no reorder buffer): every chunk must arrive with `seq == nextSeq_`; out-of-order chunks are NAK'd. `windowRemaining` returned on every ACK is always the configured `windowSize` — backpressure is a sender concern. CRC validation uses our own `crc16_ccitt_false` (PURE libs can't link `esp_crc16_le`). The lib does *not* touch payload bytes — `onChunk` receives a `const uint8_t*`/`uint16_t` pair and passes them straight back through `ChunkResult` on ACK so the caller can write them to wherever it wants (SD card in Phase 4, /dev/null in Phase 3).

**Tech Stack:** C++17, googletest, PlatformIO `[env:test]` native build. Mirrors the existing PURE-lib pattern established by `lib_native/AstrOsSerialProtocol` and `lib_native/AstrOsAnimationEngine`.

**Design doc:** `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md` (Phase 2 section)
**Phase 1 (already merged):** the `FW_*` wire format and the `parseFwChunk` parser that produces the records this state machine consumes.
**Phase context:** This is Phase 2 of 5. Phase 3 wires the `BulkReceiver` into the serial-handler task; Phase 4 adds SD writer + streaming SHA; Phase 5 hardens failure modes.

---

## File Structure

| Path | Purpose | Action |
|---|---|---|
| `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp` | Public API: enums, result structs, `BulkReceiver` class, `crc16_ccitt_false` decl | Create |
| `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp` | `BulkReceiver` method bodies + CRC impl | Create |
| `lib_native/AstrOsBulkTransport/README` | Purity rule + forbidden include prefixes (mirrors `AstrOsSerialProtocol/README`) | Create |
| `.github/workflows/pr-validation.yml` | Append `lib_native/AstrOsBulkTransport` to the `PURE_LIBS` array | Modify |
| `test/test_native/bulk_transport_tests.cpp` | Native tests for the state machine + CRC helper | Create |

No changes to `lib_native/AstrOsMessaging`, `lib_native/AstrOsSerialProtocol`, or any MIXED-side lib. PlatformIO auto-discovers libs under `lib_native/` via the `lib_extra_dirs = lib_native` setting in `platformio.ini:12` — no `library.json` is required (the existing PURE libs don't have one).

## Background notes for the implementer

- **`pio` is at `/home/jeff/.platformio/penv/bin/pio`** (not on default PATH). All test/build commands in this plan use the full path.
- **Run native tests with**: `/home/jeff/.platformio/penv/bin/pio test -e test`. The `--filter "test_native"` matches the test folder, not test names; matching by test name uses `--filter "*BulkTransport*"` against the GoogleTest binary.
- **The pre-commit hook runs clang-format** on staged C/C++ files. Expect it to reformat whitespace; the commit still succeeds.
- **The CI native-purity guard** (`.github/workflows/pr-validation.yml`) greps for `#include <(freertos/|esp_|driver/|nvs_|sdmmc_)…>` across every path in `PURE_LIBS`. Don't include any of those, ever. The lib has no need to — we're not touching FreeRTOS, ESP-IDF, or hardware. Standard C++17 only.
- **No exceptions.** CLAUDE.md and existing patterns: don't `throw`; don't `try/catch`. Return result structs.
- **CRC-16/CCITT-FALSE:** poly `0x1021`, init `0xFFFF`, no input reflection, no output reflection, no XOR-out. **NOT `esp_crc16_le`** — that ESP-IDF helper is CRC-16/CCITT (*reflected*, init=0) and produces different output. The matching ESP-IDF form is `~esp_rom_crc16_be((uint16_t)~0xFFFF, buf, len)`. Many online CRC calculators get this wrong (CCITT vs CCITT-FALSE vs XMODEM are all different) — the test in Task 2 uses the canonical "123456789" → `0x29B1` check vector.
- **`windowRemaining` semantics:** always returned as the configured `windowSize`. The receiver has zero state in-flight after a chunk is ACK'd (it's already validated and "consumed" by the caller). Backpressure is a sender-side optimization. Document this in the header.
- **`xferId` mismatch / not-active:** NAK with `reason=OUT_OF_ORDER`. The wire-level reason codes are `CRC | SIZE | OUT_OF_ORDER | FLASH_FULL`; we map all structural rejection to `OUT_OF_ORDER`. The internal `NakReason` enum stays 1:1 with the wire-level set.
- **`HASH_MISMATCH` in `EndResult::Status`:** reserved for the Phase 3+ MIXED layer that computes the hash. This Phase 2 implementation never returns `HASH_MISMATCH` — only `OK` or `IO_ERROR` (chunk-count mismatch).
- **The lib does not own payload memory.** `onChunk` takes `const uint8_t* payload, uint16_t payloadLen`; `ChunkResult` passes them straight back on ACK. Caller (Phase 3 OtaReceiver) is responsible for the buffer's lifetime — typically `payload` lives in a queue-message struct that the caller frees after processing.

---

### Task 1: Scaffold the PURE lib + register with CI

**Files:**
- Create: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`
- Create: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Create: `lib_native/AstrOsBulkTransport/README`
- Modify: `.github/workflows/pr-validation.yml`

This task lays down the directory, an empty-but-valid header + source, the README, and the CI purity-guard registration. No functional code yet. The goal is "this compiles, CI accepts it, no tests fail" — a clean foundation for the subsequent TDD tasks.

- [ ] **Step 1: Create the README**

`lib_native/AstrOsBulkTransport/README`:

```
AstrOsBulkTransport
===================

Pure, native-testable state machine for the chunk-based bulk transport
used by the firmware OTA path. Consumes FW_CHUNK records (parsed by
lib_native/AstrOsMessaging) and produces ACK/NAK decisions, with
sliding-window backpressure metadata for the sender. Companion to the
FW_* wire format that Phase 1 added to AstrOsMessaging; consumed by
the MIXED OtaReceiver that Phase 3 will introduce.

Purity rule
-----------

This library is unit-tested on the native host under [env:test], so it
must not include any of:

    freertos/*, esp_*, driver/*, nvs_*, sdmmc_*, esp_vfs*, esp_now*, esp_wifi*

Enforced by the CI purity guard in .github/workflows/pr-validation.yml.

Error-channel convention
------------------------

The state machine returns ChunkResult / EndResult structs with explicit
Decision and Status fields. No exceptions, no logger injection. The
MIXED caller (Phase 3 OtaReceiver) is responsible for translating
results into wire-level FW_CHUNK_ACK / FW_CHUNK_NAK / FW_TRANSFER_END_ACK
messages and for any ESP_LOGW logging at the boundary.

CRC
---

This lib owns its own crc16_ccitt_false implementation: CRC-16/CCITT-FALSE
(poly 0x1021, init 0xFFFF, no input/output reflection, no XOR-out). This
is NOT byte-identical to esp_crc16_le — that ESP-IDF helper is the
*reflected* CCITT variant (init=0) and produces different output. The
matching ESP-IDF form is ~esp_rom_crc16_be((uint16_t)~0xFFFF, buf, len).
Canonical check vector: "123456789" → 0x29B1. The Phase 3 MIXED layer
should call the PURE crc16_ccitt_false directly rather than reaching for
the ESP-IDF helpers (none of which are byte-identical without the wrapper
above).
```

- [ ] **Step 2: Create the header skeleton**

`lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace AstrOsBulkTransport
{
    // Single-frame CRC-16/CCITT-FALSE. Poly 0x1021, init 0xFFFF, no input
    // reflection, no output reflection, no XOR-out. NOT esp_crc16_le (that
    // helper is the reflected variant, init=0). The matching ESP-IDF form
    // is ~esp_rom_crc16_be((uint16_t)~0xFFFF, buf, len). Standalone (not a
    // class method) so tests can pin behavior independently of the
    // receiver state machine.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);
} // namespace AstrOsBulkTransport
```

- [ ] **Step 3: Create the source skeleton**

`lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`:

```cpp
#include "AstrOsBulkTransport.hpp"

namespace AstrOsBulkTransport
{
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
        (void)data;
        (void)len;
        return 0; // implemented in Task 2
    }
} // namespace AstrOsBulkTransport
```

- [ ] **Step 4: Register with the CI purity guard**

In `.github/workflows/pr-validation.yml`, find the `PURE_LIBS=(` array (around line 33). Append `lib_native/AstrOsBulkTransport` as a new entry. The block should read:

```yaml
          PURE_LIBS=(
            lib_native/AstrOsMessaging
            lib_native/AstrOsUtility
            lib_native/AstrOsLogging
            lib_native/AstrOsSerialProtocol
            lib_native/AstrOsAnimationCommands
            lib_native/AstrOsAnimationEngine
            lib_native/AstrOsEspNowProtocol
            lib_native/AstrOsEspNowPeers
            lib_native/AstrOsBulkTransport
          )
```

- [ ] **Step 5: Verify the existing test suite still passes**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test`
Expected: 257/257 pass (Phase 1 baseline). The new lib has no tests yet but its presence under `lib_native/` should not break anything.

- [ ] **Step 6: Verify both boards still build**

```bash
/home/jeff/.platformio/penv/bin/pio run -e metro_s3
/home/jeff/.platformio/penv/bin/pio run -e lolin_d32_pro
```
Expected: both succeed.

- [ ] **Step 7: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib_native/AstrOsBulkTransport/ .github/workflows/pr-validation.yml
git commit -m "$(cat <<'EOF'
feat(bulk-transport): scaffold lib_native/AstrOsBulkTransport

Phase 2 of the OTA firmware work. Adds the PURE-lib skeleton plus the
CI purity-guard registration. Header declares only the standalone
crc16_ccitt_false helper as a placeholder return; class definition
and method bodies land in subsequent tasks. No tests yet — the next
task adds the CRC implementation under TDD.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `crc16_ccitt_false` implementation + tests

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Create: `test/test_native/bulk_transport_tests.cpp`

CRC-16/CCITT-FALSE: poly `0x1021`, init `0xFFFF`, no input/output reflection, no XOR-out. The canonical check vector is `"123456789"` (9 ASCII bytes) → `0x29B1`. Other useful pin-tests: empty input → `0xFFFF` (the init value); single zero byte → `0xE1F0`; `0xFF` byte → `0xFF00`.

- [ ] **Step 1: Create the test file with failing tests**

`test/test_native/bulk_transport_tests.cpp`:

```cpp
#include <AstrOsBulkTransport.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{
    // Helper: compute CRC over a string literal (excluding null terminator).
    uint16_t crc(const char *s)
    {
        return AstrOsBulkTransport::crc16_ccitt_false(reinterpret_cast<const uint8_t *>(s), std::strlen(s));
    }
} // namespace

//=================================================================================================
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no XOR-out)
//=================================================================================================

TEST(BulkTransport, Crc16EmptyInputReturnsInitValue)
{
    // Empty input: CRC equals the init value 0xFFFF.
    EXPECT_EQ(0xFFFFu, AstrOsBulkTransport::crc16_ccitt_false(nullptr, 0));
}

TEST(BulkTransport, Crc16CanonicalCheckVector)
{
    // "123456789" -> 0x29B1 per the canonical CCITT-FALSE test vector.
    EXPECT_EQ(0x29B1u, crc("123456789"));
}

TEST(BulkTransport, Crc16SingleZeroByte)
{
    // A single 0x00 byte: well-known CCITT-FALSE result 0xE1F0.
    const uint8_t data[] = {0x00};
    EXPECT_EQ(0xE1F0u, AstrOsBulkTransport::crc16_ccitt_false(data, 1));
}

TEST(BulkTransport, Crc16SingleFfByte)
{
    // A single 0xFF byte: well-known CCITT-FALSE result 0xFF00.
    const uint8_t data[] = {0xFF};
    EXPECT_EQ(0xFF00u, AstrOsBulkTransport::crc16_ccitt_false(data, 1));
}

TEST(BulkTransport, Crc16DeterministicAcrossCalls)
{
    // Same input on two separate invocations must produce the same output —
    // a regression guard for any future caching/state mistake.
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t first = AstrOsBulkTransport::crc16_ccitt_false(data, 4);
    uint16_t second = AstrOsBulkTransport::crc16_ccitt_false(data, 4);
    EXPECT_EQ(first, second);
}
```

Add `#include <cstring>` at the top of the file too, since the helper uses `std::strlen`.

- [ ] **Step 2: Run tests to verify they FAIL**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: most CRC tests fail (the placeholder returns `0`). The empty-input test happens to pass coincidentally (placeholder `return 0` ≠ `0xFFFF`, so it actually fails too).

- [ ] **Step 3: Implement `crc16_ccitt_false`**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, replace the placeholder implementation:

```cpp
#include "AstrOsBulkTransport.hpp"

namespace AstrOsBulkTransport
{
    // CRC-16/CCITT-FALSE. Bit-by-bit reference implementation:
    //   poly = 0x1021, init = 0xFFFF, refIn = false, refOut = false, xorOut = 0.
    // Table-based variants would be faster but the volume here (one chunk =
    // ~4 KB per frame, ~5 MB/s peak throughput) doesn't justify the static
    // table cost in a PURE lib that has no on-device performance pressure.
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
    {
        uint16_t crc = 0xFFFFu;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int bit = 0; bit < 8; bit++)
            {
                if (crc & 0x8000u)
                {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
                }
                else
                {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }
} // namespace AstrOsBulkTransport
```

- [ ] **Step 4: Run tests to verify they PASS**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 257 → 262 (5 new CRC tests). All pass.

- [ ] **Step 5: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
feat(bulk-transport): crc16_ccitt_false standalone helper

Bit-by-bit reference implementation of CRC-16/CCITT-FALSE (poly 0x1021,
init 0xFFFF, no reflection, no XOR-out). NOT esp_crc16_le — that helper
is the reflected variant and produces different output. Five native
tests cover the canonical "123456789" check vector, empty-input
init-value, single-zero/FF bytes, and call determinism.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Public type definitions (enums, result structs, `BulkReceiver` shape)

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`

This task adds the type definitions — `Decision`, `NakReason`, `ChunkResult`, `EndResult`, the `BulkReceiver` class declaration — but no method bodies yet. The lib will still build because the class is just a declaration. The point is to get the public API surface in place so subsequent tasks fill in one method at a time.

No tests yet — types are checked at compile time. The first behavioral test lands in Task 4.

- [ ] **Step 1: Add the type definitions**

In `lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp`, replace the existing namespace contents (currently just `crc16_ccitt_false`) with the full set:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace AstrOsBulkTransport
{
    // Single-frame CRC-16/CCITT-FALSE. Poly 0x1021, init 0xFFFF, no input
    // reflection, no output reflection, no XOR-out. NOT esp_crc16_le (that
    // helper is the reflected variant, init=0). The matching ESP-IDF form
    // is ~esp_rom_crc16_be((uint16_t)~0xFFFF, buf, len).
    uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

    enum class Decision
    {
        ACK,
        NAK
    };

    // Internal NAK reasons map 1:1 to the wire-level FW_CHUNK_NAK reason
    // codes (CRC | SIZE | OUT_OF_ORDER | FLASH_FULL). `WRONG_TRANSFER_ID`
    // and `NOT_ACTIVE` are NOT distinct enum entries — those structural
    // rejections all map to OUT_OF_ORDER, matching the wire protocol.
    enum class NakReason : uint8_t
    {
        NONE = 0,
        CRC = 1,
        SIZE = 2,
        OUT_OF_ORDER = 3,
        FLASH_FULL = 4
    };

    struct ChunkResult
    {
        Decision decision = Decision::NAK;
        uint32_t highestContiguousSeq = 0;
        uint32_t nextExpectedSeq = 0;
        uint8_t windowRemaining = 0;
        NakReason reason = NakReason::NONE;
        // On ACK only: the caller's input pointer + length pass straight
        // through, unmodified. BulkReceiver does not own this memory and
        // does not touch the bytes. nullptr/0 on NAK.
        const uint8_t *payload = nullptr;
        uint16_t payloadLen = 0;
    };

    struct EndResult
    {
        // HASH_MISMATCH is reserved for the MIXED-layer caller (Phase 3
        // OtaReceiver) that owns the streaming SHA-256 context. This
        // PURE state machine only knows about chunk counts; it returns
        // OK on matching totals and IO_ERROR on mismatch.
        enum class Status
        {
            OK,
            HASH_MISMATCH,
            IO_ERROR
        };
        Status status = Status::IO_ERROR;
    };

    // Sequential chunk-receive state machine for the firmware OTA path.
    // The receiver commits chunks strictly in seq order — sliding window
    // is a sender optimization, not a reorder buffer. After every ACK
    // the receiver reports `windowRemaining = windowSize_` because it
    // tracks no in-flight state (each ACK'd chunk is already consumed
    // by the caller).
    //
    // Usage:
    //   BulkReceiver r;
    //   r.begin(xferId, totalSize, totalChunks, chunkSize, windowSize);
    //   for each FW_CHUNK arrival:
    //       auto cr = r.onChunk(xferId, seq, len, crc16, payload);
    //       // caller acts on cr.decision (write bytes / send ACK / send NAK)
    //   auto er = r.onEnd(xferId, totalChunksSent);
    //   r.reset();  // ready for next transfer; safe to call anytime.
    class BulkReceiver
    {
      public:
        void begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks, uint16_t chunkSize, uint8_t windowSize);
        ChunkResult onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen, uint16_t crc16, const uint8_t *payload);
        EndResult onEnd(uint8_t xferId, uint32_t totalChunksSent);
        void reset();

      private:
        uint8_t xferId_ = 0;
        uint32_t nextSeq_ = 0;
        uint32_t totalSize_ = 0;
        uint32_t totalChunks_ = 0;
        uint16_t chunkSize_ = 0;
        uint8_t windowSize_ = 0;
        bool active_ = false;
    };
} // namespace AstrOsBulkTransport
```

- [ ] **Step 2: Verify the lib still compiles**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 262/262 pass — CRC tests still green, no new tests added yet. The `BulkReceiver` class is declared but its methods aren't called from anywhere, so the linker won't complain about missing definitions yet.

(If you get a linker error like "undefined reference to `AstrOsBulkTransport::BulkReceiver::begin`", that means a test file is already trying to use the class — check whether any later-task test was accidentally included.)

- [ ] **Step 3: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib_native/AstrOsBulkTransport/include/AstrOsBulkTransport.hpp
git commit -m "$(cat <<'EOF'
feat(bulk-transport): public type definitions + BulkReceiver shape

Adds Decision / NakReason / ChunkResult / EndResult / BulkReceiver to
the header. Method bodies land in subsequent tasks under TDD. NakReason
mirrors the wire-level FW_CHUNK_NAK reason codes 1:1 — structural
rejections (wrong xferId, not active) collapse to OUT_OF_ORDER on the
wire. EndResult::Status::HASH_MISMATCH is reserved for the MIXED-layer
caller; this PURE state machine only validates chunk counts.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: `BulkReceiver::begin` + `reset` + state inspection through `onChunk`

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

`begin` is a simple field initializer; `reset` is the inverse. Since neither method returns anything observable directly, we test them by observing `onChunk` behavior afterward — but `onChunk` isn't fully implemented yet. So we implement `begin`/`reset` together with the **minimum** `onChunk` needed for happy-path observation (the full reject paths come in Tasks 5–6). The minimal `onChunk` for Task 4 is: "if active and seq matches and payload is fine, accept it; otherwise NAK with `OUT_OF_ORDER`."

- [ ] **Step 1: Add the failing tests**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkReceiver::begin + reset + minimal onChunk happy path
//=================================================================================================

TEST(BulkTransport, BeginThenSingleChunkInOrderAcks)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/12, /*totalChunks=*/1, /*chunkSize=*/12, /*windowSize=*/16);

    const uint8_t payload[] = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
    uint16_t expectedCrc = AstrOsBulkTransport::crc16_ccitt_false(payload, sizeof(payload));

    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, sizeof(payload), expectedCrc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::NONE, result.reason);
    EXPECT_EQ(0u, result.highestContiguousSeq);
    EXPECT_EQ(1u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining); // matches the windowSize passed to begin()
    EXPECT_EQ(payload, result.payload);     // pointer passes through unmodified
    EXPECT_EQ(sizeof(payload), result.payloadLen);
}

TEST(BulkTransport, OnChunkBeforeBeginNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    // No begin() called.
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, 4, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
}

TEST(BulkTransport, ResetReturnsToInactive)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/100, /*totalChunks=*/1, /*chunkSize=*/100, /*windowSize=*/8);
    r.reset();

    // After reset, onChunk should behave the same as if begin() had never been called.
    const uint8_t payload[] = {0x01, 0x02};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 2);
    auto result = r.onChunk(/*xferId=*/7, /*seq=*/0, 2, crc, payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
}

TEST(BulkTransport, BeginAfterEndReusesReceiver)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t firstPayload[] = {'a', 'b', 'c', 'd'};
    uint16_t firstCrc = AstrOsBulkTransport::crc16_ccitt_false(firstPayload, 4);
    auto first = r.onChunk(7, 0, 4, firstCrc, firstPayload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, first.decision);

    // Reset, begin a new transfer with a different xferId, and accept its first chunk.
    r.reset();
    r.begin(/*xferId=*/9, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t secondPayload[] = {'e', 'f', 'g', 'h'};
    uint16_t secondCrc = AstrOsBulkTransport::crc16_ccitt_false(secondPayload, 4);
    auto second = r.onChunk(9, 0, 4, secondCrc, secondPayload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, second.decision);
    EXPECT_EQ(0u, second.highestContiguousSeq);
    EXPECT_EQ(1u, second.nextExpectedSeq);
}
```

- [ ] **Step 2: Run tests to verify they FAIL**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: link errors (`undefined reference to BulkReceiver::begin`, etc.) — the methods are declared but have no definitions yet.

- [ ] **Step 3: Implement `begin`, `reset`, and a minimal `onChunk`**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, append after the CRC implementation:

```cpp
    void BulkReceiver::begin(uint8_t xferId, uint32_t totalSize, uint32_t totalChunks, uint16_t chunkSize,
                              uint8_t windowSize)
    {
        xferId_ = xferId;
        nextSeq_ = 0;
        totalSize_ = totalSize;
        totalChunks_ = totalChunks;
        chunkSize_ = chunkSize;
        windowSize_ = windowSize;
        active_ = true;
    }

    void BulkReceiver::reset()
    {
        xferId_ = 0;
        nextSeq_ = 0;
        totalSize_ = 0;
        totalChunks_ = 0;
        chunkSize_ = 0;
        windowSize_ = 0;
        active_ = false;
    }

    ChunkResult BulkReceiver::onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen, uint16_t crc16,
                                       const uint8_t *payload)
    {
        ChunkResult result{};
        result.decision = Decision::NAK;
        result.reason = NakReason::OUT_OF_ORDER;
        // highestContiguousSeq / nextExpectedSeq / windowRemaining default to 0 for the not-active path.

        if (!active_ || xferId != xferId_ || seq != nextSeq_)
        {
            // Subsequent tasks: explicit handling for CRC / SIZE / etc. For
            // now, anything other than a matching in-order chunk on an
            // active transfer NAKs as OUT_OF_ORDER.
            return result;
        }

        // Happy path: ACK and advance.
        // CRC + SIZE validation come in Task 5/6; for now we trust the inputs
        // to keep this task's diff focused on begin/reset/state.
        (void)crc16;
        (void)payloadLen;
        result.decision = Decision::ACK;
        result.reason = NakReason::NONE;
        result.highestContiguousSeq = seq;
        result.nextExpectedSeq = seq + 1;
        result.windowRemaining = windowSize_;
        result.payload = payload;
        result.payloadLen = payloadLen;
        nextSeq_++;
        return result;
    }
```

Note: the public `EndResult BulkReceiver::onEnd(...)` is declared in the header but not defined yet. As long as nothing calls it, the linker doesn't care. (If your toolchain does complain, add a stub: `EndResult BulkReceiver::onEnd(uint8_t, uint32_t) { return {}; }` and remove it in Task 7.)

- [ ] **Step 4: Run tests to verify they PASS**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 262 → 266 (4 new tests). All pass.

- [ ] **Step 5: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
feat(bulk-transport): begin + reset + minimal onChunk happy path

BulkReceiver state-machine skeleton: begin() initializes the active
transfer; reset() returns to inactive; onChunk() ACKs an in-order
matching chunk on an active transfer and NAKs with OUT_OF_ORDER for
the not-active / wrong-xferId / wrong-seq cases. CRC and SIZE
validation come in subsequent tasks. Four tests cover the happy path,
not-active rejection, post-reset rejection, and begin-after-end reuse.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: `BulkReceiver::onChunk` — CRC + SIZE rejection

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

CRC validation: receiver recomputes `crc16_ccitt_false(payload, payloadLen)` and rejects with `NakReason::CRC` if the recomputed value doesn't match the `crc16` argument.

SIZE validation: `payloadLen` must equal `chunkSize_` for all chunks except possibly the last one. The last chunk may be shorter if `totalSize_` isn't a clean multiple of `chunkSize_`. The current chunk's expected length is:

```
remainingSize = totalSize_ - (seq * chunkSize_);
expectedLen = min(chunkSize_, remainingSize)
```

`SIZE` NAK fires if `payloadLen != expectedLen`.

Both new NAK paths report `highestContiguousSeq = nextSeq_ - 1` (last committed seq), `nextExpectedSeq = nextSeq_` (where the sender should resume), `windowRemaining = windowSize_`. The first chunk's `highestContiguousSeq` is reported as `0` when `nextSeq_ == 0` — that's a known one-off: the wire-level meaning is "I've committed nothing yet"; downstream clients treat `highest=0, next=0` as the initial-state marker.

- [ ] **Step 1: Add the failing tests**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkReceiver::onChunk — CRC + SIZE rejection
//=================================================================================================

TEST(BulkTransport, OnChunkBadCrcNaks)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t realCrc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    auto result = r.onChunk(7, 0, 4, /*crc16=*/static_cast<uint16_t>(realCrc ^ 0xFFFFu), payload);

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::CRC, result.reason);
    // No chunk committed yet -> reported as 0/0.
    EXPECT_EQ(0u, result.highestContiguousSeq);
    EXPECT_EQ(0u, result.nextExpectedSeq);
    EXPECT_EQ(16u, result.windowRemaining);
}

TEST(BulkTransport, OnChunkPayloadLenMismatchNaksSize)
{
    AstrOsBulkTransport::BulkReceiver r;
    // totalSize = 4096, chunkSize = 1024 -> 4 chunks of 1024 bytes each.
    r.begin(/*xferId=*/7, /*totalSize=*/4096, /*totalChunks=*/4, /*chunkSize=*/1024, /*windowSize=*/16);

    // Sender claims this is chunk 0 with len=500 — wrong; should be 1024.
    std::vector<uint8_t> payload(500, 0xAA);
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload.data(), payload.size());
    auto result = r.onChunk(7, 0, static_cast<uint16_t>(payload.size()), crc, payload.data());

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::SIZE, result.reason);
}

TEST(BulkTransport, OnChunkLastShortChunkAcks)
{
    AstrOsBulkTransport::BulkReceiver r;
    // totalSize = 2500, chunkSize = 1024 -> chunks of 1024, 1024, 452.
    r.begin(/*xferId=*/7, /*totalSize=*/2500, /*totalChunks=*/3, /*chunkSize=*/1024, /*windowSize=*/16);

    // Commit chunks 0 and 1.
    std::vector<uint8_t> fullChunk(1024, 0xCC);
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullChunk.data(), fullChunk.size());
    auto r0 = r.onChunk(7, 0, 1024, fullCrc, fullChunk.data());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r0.decision);
    auto r1 = r.onChunk(7, 1, 1024, fullCrc, fullChunk.data());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r1.decision);

    // Last chunk: 452 bytes — the short tail.
    std::vector<uint8_t> tail(452, 0xDD);
    uint16_t tailCrc = AstrOsBulkTransport::crc16_ccitt_false(tail.data(), tail.size());
    auto r2 = r.onChunk(7, 2, 452, tailCrc, tail.data());
    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, r2.decision);
    EXPECT_EQ(2u, r2.highestContiguousSeq);
    EXPECT_EQ(3u, r2.nextExpectedSeq);
}

TEST(BulkTransport, OnChunkWrongPayloadLenOnLastChunkNaksSize)
{
    AstrOsBulkTransport::BulkReceiver r;
    // totalSize = 2500, chunkSize = 1024 -> last chunk expected = 452 bytes.
    r.begin(/*xferId=*/7, /*totalSize=*/2500, /*totalChunks=*/3, /*chunkSize=*/1024, /*windowSize=*/16);

    std::vector<uint8_t> fullChunk(1024, 0xCC);
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullChunk.data(), fullChunk.size());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 1024, fullCrc, fullChunk.data()).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 1024, fullCrc, fullChunk.data()).decision);

    // Last chunk: sender sends 1024 bytes but only 452 are expected.
    std::vector<uint8_t> wrongTail(1024, 0xDD);
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(wrongTail.data(), wrongTail.size());
    auto result = r.onChunk(7, 2, 1024, crc, wrongTail.data());

    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::SIZE, result.reason);
}
```

- [ ] **Step 2: Run tests to verify they FAIL**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: the new tests fail (the current `onChunk` ignores CRC/SIZE — bad CRC + wrong length both pass through as ACK).

- [ ] **Step 3: Add SIZE + CRC checks to `onChunk`**

In `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`, replace the body of `onChunk` with:

```cpp
    ChunkResult BulkReceiver::onChunk(uint8_t xferId, uint32_t seq, uint16_t payloadLen, uint16_t crc16,
                                       const uint8_t *payload)
    {
        ChunkResult result{};
        result.decision = Decision::NAK;
        // Reported window: always windowSize_ on every reply. The receiver
        // tracks no in-flight state.
        result.windowRemaining = active_ ? windowSize_ : 0;

        // Structural rejections first: not-active, wrong xferId, out-of-seq.
        // All collapse to OUT_OF_ORDER per the wire-level reason-code set.
        if (!active_ || xferId != xferId_ || seq != nextSeq_)
        {
            result.reason = NakReason::OUT_OF_ORDER;
            // highestContiguousSeq / nextExpectedSeq default to 0 on the
            // not-active path; on an active mismatch they should reflect
            // the receiver's actual state so the sender can resync.
            if (active_)
            {
                result.highestContiguousSeq = (nextSeq_ == 0) ? 0 : (nextSeq_ - 1);
                result.nextExpectedSeq = nextSeq_;
            }
            return result;
        }

        // SIZE: compute the expected length for THIS seq. All chunks are
        // chunkSize_ bytes except possibly the last one (totalSize_ may not
        // be a clean multiple of chunkSize_).
        uint32_t expectedLen = chunkSize_;
        uint32_t committedBytes = static_cast<uint32_t>(seq) * chunkSize_;
        if (committedBytes + chunkSize_ > totalSize_)
        {
            expectedLen = totalSize_ - committedBytes;
        }
        if (payloadLen != expectedLen)
        {
            result.reason = NakReason::SIZE;
            result.highestContiguousSeq = (nextSeq_ == 0) ? 0 : (nextSeq_ - 1);
            result.nextExpectedSeq = nextSeq_;
            return result;
        }

        // CRC: recompute over the payload and compare.
        uint16_t computed = crc16_ccitt_false(payload, payloadLen);
        if (computed != crc16)
        {
            result.reason = NakReason::CRC;
            result.highestContiguousSeq = (nextSeq_ == 0) ? 0 : (nextSeq_ - 1);
            result.nextExpectedSeq = nextSeq_;
            return result;
        }

        // ACK and advance.
        result.decision = Decision::ACK;
        result.reason = NakReason::NONE;
        result.highestContiguousSeq = seq;
        result.nextExpectedSeq = seq + 1;
        result.payload = payload;
        result.payloadLen = payloadLen;
        nextSeq_++;
        return result;
    }
```

- [ ] **Step 4: Run tests to verify they PASS**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 266 → 270 (4 new tests). All pass including the existing happy-path test from Task 4.

- [ ] **Step 5: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
feat(bulk-transport): onChunk CRC + SIZE validation

onChunk recomputes crc16_ccitt_false over the payload and rejects
with NakReason::CRC on mismatch. Payload-len is checked against the
expected size for the current seq, including the short-last-chunk
case (totalSize % chunkSize != 0). Both rejection paths report
highestContiguousSeq / nextExpectedSeq so the sender can resync.
4 new tests cover bad CRC, wrong payload length, correct short last
chunk, and wrong-sized last chunk.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: `BulkReceiver::onChunk` — duplicate + wrong-xferId state reporting

**Files:**
- Modify: `test/test_native/bulk_transport_tests.cpp`

The behaviour for wrong-xferId already collapses to `OUT_OF_ORDER` in Task 4's code. Duplicate-seq (the sender retransmits seq=0 after we've already ACK'd it and moved to nextSeq_=1) also collapses to `OUT_OF_ORDER` since `seq != nextSeq_` triggers the structural-reject branch. This task adds explicit test coverage for both behaviours — no source changes needed if Task 5 is correct, just additional regression guards.

If the assertion in Step 3 fails, the production logic from Task 5 needs to be re-examined; only add source code if a test reveals a bug.

- [ ] **Step 1: Add the duplicate + wrong-xferId tests**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkReceiver::onChunk — duplicate + wrong-xferId rejection
//=================================================================================================

TEST(BulkTransport, OnChunkDuplicateSeqNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    // ACK seq=0 cleanly.
    auto first = r.onChunk(7, 0, 4, crc, payload);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, first.decision);
    EXPECT_EQ(1u, first.nextExpectedSeq);

    // Sender retransmits seq=0 — receiver has already moved on.
    auto duplicate = r.onChunk(7, 0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, duplicate.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, duplicate.reason);
    // Reports we last committed seq=0, expect seq=1 next.
    EXPECT_EQ(0u, duplicate.highestContiguousSeq);
    EXPECT_EQ(1u, duplicate.nextExpectedSeq);
}

TEST(BulkTransport, OnChunkSkipForwardNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    // Sender jumps to seq=1 without sending seq=0.
    auto skipped = r.onChunk(7, 1, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, skipped.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, skipped.reason);
    EXPECT_EQ(0u, skipped.nextExpectedSeq);
}

TEST(BulkTransport, OnChunkWrongXferIdNaksOutOfOrder)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);

    // Send a chunk claiming xferId=9 on a receiver bound to xferId=7.
    auto result = r.onChunk(/*xferId=*/9, 0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::NAK, result.decision);
    EXPECT_EQ(AstrOsBulkTransport::NakReason::OUT_OF_ORDER, result.reason);
    EXPECT_EQ(0u, result.nextExpectedSeq);

    // The receiver state must not have been corrupted: a correct chunk for
    // xferId=7 still gets ACK'd cleanly afterward.
    auto recovery = r.onChunk(7, 0, 4, crc, payload);
    EXPECT_EQ(AstrOsBulkTransport::Decision::ACK, recovery.decision);
    EXPECT_EQ(1u, recovery.nextExpectedSeq);
}
```

- [ ] **Step 2: Run tests to verify they PASS immediately**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 270 → 273. All new tests pass on first attempt because the structural-reject branch in Task 5's `onChunk` already collapses these cases to `OUT_OF_ORDER`. If any fail, *do not* "fix" them by editing the production code without first re-reading it carefully — the test is more likely to be wrong than the implementation, given Task 5 already covered the logic.

- [ ] **Step 3: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
test(bulk-transport): duplicate + skip-forward + wrong-xferId coverage

Adds three regression tests for the structural-reject path in onChunk:
duplicate seq, skip-forward seq, and wrong xferId all NAK as
OUT_OF_ORDER. Also verifies that a wrong-xferId attempt does not
corrupt receiver state — a subsequent correct chunk still ACKs
cleanly. No source changes; behavior was already correct from Task 5.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: `BulkReceiver::onEnd` — happy path

**Files:**
- Modify: `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp`
- Modify: `test/test_native/bulk_transport_tests.cpp`

`onEnd` validates that the transfer is complete: `active_ == true`, `xferId == xferId_`, and `totalChunksSent == totalChunks_` (the value the sender reports must match what was declared in `begin()`), AND `nextSeq_ == totalChunks_` (the receiver actually saw and ACK'd every chunk). All matching → `Status::OK`. The caller is responsible for `reset()`ing the receiver afterward.

This task covers the OK path only; the IO_ERROR path lands in Task 8.

- [ ] **Step 1: Add the failing test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkReceiver::onEnd — happy path
//=================================================================================================

TEST(BulkTransport, OnEndAfterAllChunksReturnsOk)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 4, crc, payload).decision);

    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/2);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::OK, end.status);
}
```

- [ ] **Step 2: Run test to verify it FAILS**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: link error if Task 4 didn't include a stub for `onEnd`. Or, if Task 4's stub returned `{}` (default `Status::IO_ERROR`), the test fails with `Expected: equality of these values: AstrOsBulkTransport::EndResult::Status::OK ... Actual: 2 (IO_ERROR)`.

- [ ] **Step 3: Implement `onEnd` happy path**

Append to `lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp` (or replace the stub from Task 4):

```cpp
    EndResult BulkReceiver::onEnd(uint8_t xferId, uint32_t totalChunksSent)
    {
        EndResult result{};
        result.status = EndResult::Status::IO_ERROR;

        if (!active_ || xferId != xferId_)
        {
            return result;
        }
        if (totalChunksSent != totalChunks_)
        {
            return result;
        }
        if (nextSeq_ != totalChunks_)
        {
            return result;
        }

        result.status = EndResult::Status::OK;
        return result;
    }
```

- [ ] **Step 4: Run test to verify it PASSES**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 273 → 274. The OK case passes.

- [ ] **Step 5: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib_native/AstrOsBulkTransport/src/AstrOsBulkTransport.cpp \
        test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
feat(bulk-transport): onEnd happy path returns OK

After begin + all chunks ACK'd in order, onEnd(matchingXferId,
totalChunks) returns Status::OK. Reject paths (not active / wrong
xferId / chunk-count mismatch) collapse to IO_ERROR; explicit tests
for those follow in the next task.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: `BulkReceiver::onEnd` — IO_ERROR reject paths

**Files:**
- Modify: `test/test_native/bulk_transport_tests.cpp`

Test the four IO_ERROR paths Task 7's implementation introduced: not-active, wrong xferId, sender-reported total mismatch, receiver-actual chunk count mismatch. No source changes if Task 7's implementation is correct; the assertions verify each guard fires.

- [ ] **Step 1: Add the failing tests**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkReceiver::onEnd — IO_ERROR reject paths
//=================================================================================================

TEST(BulkTransport, OnEndBeforeBeginReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    // No begin() called.
    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/1);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

TEST(BulkTransport, OnEndWrongXferIdReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/4, /*totalChunks=*/1, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);

    // Caller claims this END is for a different xferId.
    auto end = r.onEnd(/*xferId=*/9, /*totalChunksSent=*/1);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

TEST(BulkTransport, OnEndSenderTotalMismatchReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 1, 4, crc, payload).decision);

    // Sender claims 3 chunks were sent, but begin() said 2.
    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/3);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}

TEST(BulkTransport, OnEndReceiverShortChunkCountReturnsIoError)
{
    AstrOsBulkTransport::BulkReceiver r;
    r.begin(/*xferId=*/7, /*totalSize=*/8, /*totalChunks=*/2, /*chunkSize=*/4, /*windowSize=*/16);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payload, 4);
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, r.onChunk(7, 0, 4, crc, payload).decision);
    // Only one chunk was sent; sender claims 2 to match the declared total.

    auto end = r.onEnd(/*xferId=*/7, /*totalChunksSent=*/2);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::IO_ERROR, end.status);
}
```

- [ ] **Step 2: Run tests to verify they PASS immediately**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: 274 → 278. All new tests pass on first attempt — the guards in Task 7's `onEnd` already handle each path.

- [ ] **Step 3: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
test(bulk-transport): onEnd IO_ERROR reject paths

Four regression tests cover the IO_ERROR paths through onEnd:
not-active, wrong xferId, sender-reported total mismatch, and
receiver-actual chunk-count short. No source changes; behavior
was already correct from Task 7.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: End-to-end multi-chunk integration test

**Files:**
- Modify: `test/test_native/bulk_transport_tests.cpp`

A single end-to-end test that drives a realistic transfer: 10 chunks of varying sizes, all in order, with one CRC failure mid-stream followed by a successful retransmit, then `onEnd`. This is the "if you only run one BulkReceiver test on the bench rig, run this one" smoke test — it exercises every code path through `begin → many onChunks → onEnd → OK`.

- [ ] **Step 1: Add the integration test**

Append to `test/test_native/bulk_transport_tests.cpp`:

```cpp
//=================================================================================================
// BulkReceiver — end-to-end happy path with one mid-stream CRC retransmit
//=================================================================================================

TEST(BulkTransport, EndToEndTenChunkTransferWithMidStreamRetransmit)
{
    constexpr uint32_t kTotalSize = 9500;
    constexpr uint16_t kChunkSize = 1024;
    constexpr uint32_t kTotalChunks = 10; // 9 full chunks + 1 short (9500 - 9216 = 284 bytes)
    constexpr uint8_t kXferId = 42;
    constexpr uint8_t kWindowSize = 16;

    AstrOsBulkTransport::BulkReceiver r;
    r.begin(kXferId, kTotalSize, kTotalChunks, kChunkSize, kWindowSize);

    // 9 full chunks + 1 short tail.
    std::vector<uint8_t> fullPayload(kChunkSize, 0xA5);
    uint16_t fullCrc = AstrOsBulkTransport::crc16_ccitt_false(fullPayload.data(), fullPayload.size());

    for (uint32_t seq = 0; seq < kTotalChunks - 1; seq++)
    {
        if (seq == 5)
        {
            // Mid-stream CRC error: sender sends correct payload with a corrupted CRC field.
            auto nakResult =
                r.onChunk(kXferId, seq, kChunkSize, static_cast<uint16_t>(fullCrc ^ 0x00FFu), fullPayload.data());
            ASSERT_EQ(AstrOsBulkTransport::Decision::NAK, nakResult.decision);
            ASSERT_EQ(AstrOsBulkTransport::NakReason::CRC, nakResult.reason);
            EXPECT_EQ(4u, nakResult.highestContiguousSeq);
            EXPECT_EQ(5u, nakResult.nextExpectedSeq);
            // Sender retransmits seq=5 with the correct CRC.
        }
        auto okResult = r.onChunk(kXferId, seq, kChunkSize, fullCrc, fullPayload.data());
        ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, okResult.decision) << "ACK failed at seq=" << seq;
        ASSERT_EQ(seq + 1, okResult.nextExpectedSeq);
    }

    // Final short chunk: 9500 - 9 * 1024 = 9500 - 9216 = 284 bytes.
    constexpr uint16_t kTailLen = 284;
    std::vector<uint8_t> tail(kTailLen, 0xC3);
    uint16_t tailCrc = AstrOsBulkTransport::crc16_ccitt_false(tail.data(), tail.size());
    auto tailResult = r.onChunk(kXferId, kTotalChunks - 1, kTailLen, tailCrc, tail.data());
    ASSERT_EQ(AstrOsBulkTransport::Decision::ACK, tailResult.decision);
    EXPECT_EQ(kTotalChunks - 1, tailResult.highestContiguousSeq);
    EXPECT_EQ(kTotalChunks, tailResult.nextExpectedSeq);

    // End of transfer.
    auto endResult = r.onEnd(kXferId, kTotalChunks);
    EXPECT_EQ(AstrOsBulkTransport::EndResult::Status::OK, endResult.status);

    r.reset();
}
```

- [ ] **Step 2: Run tests to verify the new test PASSES**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test --filter "test_native"`
Expected: prior-test-count + 1. The integration test passes — and if any of the prior tasks left a subtle bug, this is likely where it surfaces.

- [ ] **Step 3: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add test/test_native/bulk_transport_tests.cpp
git commit -m "$(cat <<'EOF'
test(bulk-transport): end-to-end 10-chunk transfer with CRC retransmit

Drives a full realistic transfer: 10 chunks (9 full + 1 short tail),
one mid-stream CRC failure followed by a successful retransmit, then
onEnd → OK. Exercises the complete begin → many onChunks → onEnd path
in one test — the canonical smoke test if only one BulkReceiver test
were run.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Final verification + open PR

**Files:** None modified — verification only.

- [ ] **Step 1: Run the full native test suite**

Run: `/home/jeff/.platformio/penv/bin/pio test -e test`
Expected: 261 → 287 (26 new BulkTransport tests, including the 4 strengthening cases added late by commit `fe87535`). All pass.

- [ ] **Step 2: Build both board firmware variants**

```bash
/home/jeff/.platformio/penv/bin/pio run -e metro_s3
/home/jeff/.platformio/penv/bin/pio run -e lolin_d32_pro
```
Expected: both succeed. Phase 2 doesn't change firmware behavior; this just confirms the new lib doesn't accidentally break the on-device build (e.g., via header transitive includes that confuse the IDF build).

- [ ] **Step 3: Confirm CI purity guard is happy**

The CI guard greps the source tree for forbidden include patterns. Simulate it locally:

```bash
grep -rEn '#include[[:space:]]*[<"](freertos/|esp_|driver/|nvs_|sdmmc_)' lib_native/AstrOsBulkTransport
```
Expected: no output. If anything matches, the offending include must move out of the PURE lib.

- [ ] **Step 4: clang-format check on changed files**

```bash
find lib_native/AstrOsBulkTransport test/test_native/bulk_transport_tests.cpp \
    -name '*.hpp' -o -name '*.cpp' -o -name '*.h' | \
    xargs clang-format --dry-run --Werror
```
Expected: no output. (The pre-commit hook should have already kept this clean.)

- [ ] **Step 5: Push branch + open PR**

The branch `feature/ota-phase2-bulk-transport` was cut from `develop` before this plan started. To push and open the PR:

```bash
git push -u origin feature/ota-phase2-bulk-transport

gh pr create --base develop --title "Firmware OTA Phase 2 — bulk-transport state machine" --body "$(cat <<'EOF'
## Summary

Phase 2 of the ESP-side OTA firmware work. Adds the
\`lib_native/AstrOsBulkTransport\` PURE state machine that consumes
parsed \`FW_CHUNK\` records (from Phase 1's \`parseFwChunk\`) and
produces ACK/NAK decisions with sliding-window backpressure metadata.
**No firmware behavior change yet** — this phase is purely a new PURE
lib with native tests. Phase 3 wires the BulkReceiver into the
serial-handler dispatch path.

Design: \`.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md\`
Plan: \`.docs/plans/20260514-2300-firmware-ota-phase2-bulk-transport.md\`

Adds:
- New PURE lib \`lib_native/AstrOsBulkTransport\` with \`BulkReceiver\` class + \`crc16_ccitt_false\` standalone helper
- \`Decision\`, \`NakReason\`, \`ChunkResult\`, \`EndResult\` public types
- Sequential chunk-receive state machine: in-order commit, structural rejections collapse to OUT_OF_ORDER, CRC + SIZE validation, short-last-chunk support
- 26 new native tests covering CRC check vectors, in-order happy path, every rejection (CRC / SIZE / OUT_OF_ORDER / wrong-xferId / not-active / duplicate / skip-forward), `begin()` input-validation guards (zero-`chunkSize`, zero-`totalChunks`, `totalSize` consistency, zero-`windowSize`), onEnd OK and per-reason IO_ERROR paths, plus a 10-chunk end-to-end integration test with mid-stream CRC retransmit
- CI purity-guard registration for the new lib in \`.github/workflows/pr-validation.yml\`

## Test plan

- [x] \`pio test -e test\` passes (287/287 — Phase 2 added 26 tests on top of develop's 261)
- [x] \`pio run -e metro_s3\` builds clean
- [x] \`pio run -e lolin_d32_pro\` builds clean
- [x] CI native-purity grep finds no forbidden includes in the new lib
- [x] clang-format clean on changed files

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

(`git push` and `gh pr create` need to run from your VS Code terminal where auth is established.)

---

## Self-review notes (already applied to this plan)

- **Spec coverage:** every Phase 2 deliverable in the design doc is covered:
  - PURE lib scaffolding + README + CI registration (Task 1) ✓
  - `crc16_ccitt_false` (Task 2) ✓
  - Type definitions + `BulkReceiver` shape (Task 3) ✓
  - `begin` / `reset` / minimal `onChunk` (Task 4) ✓
  - `onChunk` CRC + SIZE validation (Task 5) ✓
  - `onChunk` duplicate / skip / wrong-xferId test coverage (Task 6) ✓
  - `onEnd` OK + IO_ERROR (Tasks 7 + 8) ✓
  - End-to-end integration test (Task 9) ✓
  - Final verification + PR (Task 10) ✓
  - **Post-implementation hardening from PR-toolkit pass (not a numbered task)**: `begin()` input-validation guards (zero `chunkSize`/`totalChunks`/`windowSize`, `totalSize` ≈ `totalChunks * chunkSize` consistency); `seq >= totalChunks_` overflow guard on the SIZE math; `BeginResult` return value; `EndResult::Reason` enum (distinct IO_ERROR causes); `ChunkResult` factory methods preventing invalid field combinations; `writeResumePoint` helper folding the duplicated first-chunk-NAK contract into one place. ✓
- **Placeholder scan:** the original plan was free of TBDs at task-execution time. The amendment header above flags the specific claims (CRC interop, single-zero-byte value, 22/279 test counts) that were superseded by the PR-toolkit cleanup; surgical corrections in-body keep the plan re-executable.
- **Type consistency:** `BulkReceiver`, `ChunkResult`, `EndResult`, `Decision`, `NakReason` are used consistently across all tasks. `crc16_ccitt_false` lowercase-underscore naming is used everywhere.
- **Scope check:** 10 tasks. Above the CLAUDE.md ~8-task warning, but each task is small (≤2 files, ≤30 min) and Phase 2 is genuinely one coherent unit — splitting into "scaffold" and "state machine" sub-phases would just add cross-PR coordination cost without value. If the scope grows past 10, escalate.
