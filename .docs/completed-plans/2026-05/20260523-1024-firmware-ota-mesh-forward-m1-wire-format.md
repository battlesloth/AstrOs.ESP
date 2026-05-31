# Firmware OTA Mesh-Forward M1 — ESP-NOW Wire Format Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the ESP-NOW OTA wire-format primitives (8 new packet types, packed payload structs, binary builders/parsers, and role-gated dispatch routing) so M2-M5 can build the actual mesh-forward behavior on top. Zero firmware behavior change.

**Architecture:** All changes are in PURE `lib_native/` code. New `AstrOsPacketType` enum values (OTA_BEGIN, OTA_BEGIN_ACK, OTA_BEGIN_NAK, OTA_DATA, OTA_DATA_ACK, OTA_DATA_NAK, OTA_END, OTA_END_ACK). Packed structs with `__attribute__((packed))` + `static_assert(sizeof(X) == N)` lock the on-wire byte layouts. A new `generateOtaPacket()` builder path skips the validator-string injection that the existing string-payload builders perform (OTA frames are binary, every byte counts). Existing `parsePacket()` learns to recognize OTA types and skip the validator-strip step. POD record types + parser free functions live in `AstrOsEspNowProtocol` and produce `valid=true/false` records for the future MIXED layer to consume. The existing `handlePacket` dispatcher gains OTA cases that return `UnsupportedType` (MIXED adapter handles directly in M3/M4) with `WrongRole` gating for direction violations.

**Tech Stack:** C++17, GoogleTest/GMock (native), PlatformIO `[env:test]` (native target with `-std=gnu++2a`).

---

## Context for the engineer

Read these first — they're load-bearing and they will not be re-explained per task:

- **Design doc**: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md` — the contract this plan implements. Section "Wire-format binding (PR set 1)" has the byte-layout table. Section "NAK reason enums" has the enums to mirror.
- **Cross-repo frozen contract**: `AstrOs.Server/.docs/completed_plans/2026/04/27/20260427-2202-firmware-ota-decomposition.md` section B — the canonical source for the binary frame layouts. Quote: "**Frames are binary packed structs, not US/RS-delimited** — every byte matters."
- **Existing builders/parsers**: `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.{hpp,cpp}` — see `generatePackets`/`parsePacket` for the *existing* string-payload pattern. The new binary path runs alongside, not over.
- **Existing dispatcher**: `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp` — see `handlePacket` for the role-gating pattern (line 244+). OTA cases follow the same `unsupportedOrWrongRole(roleMatches)` helper.
- **Existing FW_* parsers** (good precedent): `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp` lines 117-150 — the `Fw*Record` pattern with `bool valid` field is what `Ota*Record` structs will mirror.
- **Native-purity CI guard**: `.github/workflows/pr-validation.yml` enforces that `lib_native/AstrOsMessaging` and `lib_native/AstrOsEspNowProtocol` contain no ESP-IDF/FreeRTOS includes. M1 must not violate this — only `<cstdint>`, `<cstring>`, `<vector>`, `<string>`, `<optional>` etc.
- **Test runner**: native tests run via `pio test -e test`. Filter to specific suite with `pio test -e test --filter "*ota_espnow*"`.

## File structure

**Created:**
- `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp` — packed payload structs + reason/status enums + `static_assert` size gates. Single-purpose: defines the on-wire byte layouts.
- `test/test_native/astros_ota_espnow_messages_tests.cpp` — round-trip + reject tests for M1's new builders/parsers. Kept separate from `astros_espnow_messages_tests.cpp` so the OTA suite can be filtered independently.

**Modified:**
- `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp` — extend `AstrOsPacketType` enum (8 new values), extend `AstrOsENC` string table (8 new names), declare `generateOtaPacket()` builder, declare `isOtaPacketType()` helper.
- `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp` — extend `packetTypeMap` constructor with the 8 new entries, implement `generateOtaPacket()`, teach `parsePacket()` to recognize OTA types and skip validator stripping.
- `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` — declare 8 POD record types (`OtaBeginRecord`, etc.), declare 8 parser free functions, declare 8 handler functions.
- `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp` — implement parsers, implement handlers (all return `UnsupportedType` with role gating), extend `handlePacket` switch with 8 OTA cases.

**No changes to:**
- `src/main.cpp` (firmware unchanged)
- `lib/` (no MIXED-side changes in M1)
- `platformio.ini`, `partition_*.csv`

## Wire-format reference (cross-check against design doc)

| Type | Direction | Payload byte layout | sizeof |
|---|---|---|---|
| `OTA_BEGIN` | master → padawan | `u8 xferId; u32 totalSize; u16 chunkSize; u32 totalChunks; u8[32] sha256Expected; u8 flags;` | **44 B** |
| `OTA_BEGIN_ACK` | padawan → master | `u8 xferId;` | **1 B** |
| `OTA_BEGIN_NAK` | padawan → master | `u8 xferId; u8 reason;` (reason ∈ BUSY=0, NO_PARTITION=1, BEGIN_FAILED=2) | **2 B** |
| `OTA_DATA` (header) | master → padawan | `u8 xferId; u32 seq; u16 payloadLen; u16 crc16;` then `u8 payload[payloadLen]` | header **9 B**, payload variable |
| `OTA_DATA_ACK` | padawan → master | `u8 xferId; u32 highestContiguousSeq; u32 nextExpectedSeq; u8 windowRemaining;` | **10 B** |
| `OTA_DATA_NAK` | padawan → master | `u8 xferId; u32 highestContiguousSeq; u32 nextExpectedSeq; u8 windowRemaining; u8 reason;` (reason ∈ CRC=1, SIZE=2, OUT_OF_ORDER=3, WRITE=4) | **11 B** |
| `OTA_END` | master → padawan | `u8 xferId; u32 totalChunksSent; u8[32] sha256Final;` | **37 B** |
| `OTA_END_ACK` | padawan → master | `u8 xferId; u8 status; u8[32] sha256Computed;` (status ∈ OK=0, HASH_MISMATCH=1, WRITE_ERROR=2) | **34 B** |

All `u32`/`u16` fields are **little-endian** on the wire — ESP32 native byte order. No byte-swap is needed because the only consumers are also ESP32; the structs are `__attribute__((packed))` to ensure no compiler padding insertion.

---

## Task 1: Add 8 new OTA packet types to AstrOsPacketType enum + ENC constants

Wire up the enum values + string names. No payload-handling yet. After this task, the enum recognizes OTA types and `packetTypeMap` can produce a diagnostic string for each.

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp`
- Create: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Write the failing test**

Create `test/test_native/astros_ota_espnow_messages_tests.cpp`:

```cpp
#include <AstrOsMessaging.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

// M1 — Task 1: enum values + ENC string mapping for the 8 new OTA packet types.

TEST(OtaPacketTypes, EnumValuesPresent)
{
    // Compile-time enum checks. These will fail to compile if the enum is missing values.
    AstrOsPacketType types[] = {
        AstrOsPacketType::OTA_BEGIN,    AstrOsPacketType::OTA_BEGIN_ACK, AstrOsPacketType::OTA_BEGIN_NAK,
        AstrOsPacketType::OTA_DATA,     AstrOsPacketType::OTA_DATA_ACK,  AstrOsPacketType::OTA_DATA_NAK,
        AstrOsPacketType::OTA_END,      AstrOsPacketType::OTA_END_ACK,
    };
    EXPECT_EQ(8u, sizeof(types) / sizeof(types[0]));
}

TEST(OtaPacketTypes, GeneratePacketsReturnsEmptyForOtaTypes)
{
    // The existing string-payload path (generatePackets) is NOT how OTA frames get built.
    // It calls packetTypeMap[type] and returns empty on unknown types. After Task 1 the
    // OTA types are in the map, so generatePackets returns a non-empty vector if we
    // (incorrectly) call the string path — but the resulting packet would have the
    // validator-string in the payload, which is wrong for OTA. Task 3 adds the proper
    // binary builder. This test just locks in: "Task 1 makes the map lookup succeed."
    auto svc = AstrOsEspNowMessageService();
    auto packets = svc.generatePackets(AstrOsPacketType::OTA_BEGIN, "ignored");
    ASSERT_EQ(1u, packets.size());
    // Cleanup
    for (auto &p : packets)
        free(p.data);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: compile error like `'OTA_BEGIN' is not a member of 'AstrOsPacketType'`.

- [ ] **Step 3: Extend the enum + ENC constants**

In `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp`, add to the `AstrOsENC` namespace (after the existing `SERVO_TEST_ACK` line):

```cpp
    constexpr const static char *OTA_BEGIN = "OTA_BEGIN";
    constexpr const static char *OTA_BEGIN_ACK = "OTA_BEGIN_ACK";
    constexpr const static char *OTA_BEGIN_NAK = "OTA_BEGIN_NAK";
    constexpr const static char *OTA_DATA = "OTA_DATA";
    constexpr const static char *OTA_DATA_ACK = "OTA_DATA_ACK";
    constexpr const static char *OTA_DATA_NAK = "OTA_DATA_NAK";
    constexpr const static char *OTA_END = "OTA_END";
    constexpr const static char *OTA_END_ACK = "OTA_END_ACK";
```

In the same file, add to the `AstrOsPacketType` enum (after `SERVO_TEST_ACK`):

```cpp
    OTA_BEGIN,
    OTA_BEGIN_ACK,
    OTA_BEGIN_NAK,
    OTA_DATA,
    OTA_DATA_ACK,
    OTA_DATA_NAK,
    OTA_END,
    OTA_END_ACK,
```

In `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp` constructor, add (after the existing `SERVO_TEST_ACK` entry):

```cpp
    packetTypeMap[AstrOsPacketType::OTA_BEGIN] = AstrOsENC::OTA_BEGIN;
    packetTypeMap[AstrOsPacketType::OTA_BEGIN_ACK] = AstrOsENC::OTA_BEGIN_ACK;
    packetTypeMap[AstrOsPacketType::OTA_BEGIN_NAK] = AstrOsENC::OTA_BEGIN_NAK;
    packetTypeMap[AstrOsPacketType::OTA_DATA] = AstrOsENC::OTA_DATA;
    packetTypeMap[AstrOsPacketType::OTA_DATA_ACK] = AstrOsENC::OTA_DATA_ACK;
    packetTypeMap[AstrOsPacketType::OTA_DATA_NAK] = AstrOsENC::OTA_DATA_NAK;
    packetTypeMap[AstrOsPacketType::OTA_END] = AstrOsENC::OTA_END;
    packetTypeMap[AstrOsPacketType::OTA_END_ACK] = AstrOsENC::OTA_END_ACK;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -10`
Expected: `OtaPacketTypes.EnumValuesPresent` and `OtaPacketTypes.GeneratePacketsReturnsEmptyForOtaTypes` both PASS.

Also run full suite to confirm no regression:

Run: `pio test -e test 2>&1 | tail -10`
Expected: all existing 309+ tests + 2 new tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(messaging): add OTA_* packet type enum values + ENC names"
```

---

## Task 2: Add packed OTA payload structs with `static_assert` size gates

Lock the on-wire byte layouts. `__attribute__((packed))` prevents the compiler from inserting alignment padding. `static_assert(sizeof(X) == N)` at namespace scope fires at compile time if any layout drifts.

**Files:**
- Create: `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp`
- Modify: `lib_native/AstrOsMessaging/include/AstrOsMessaging.hpp` (umbrella header — add `#include "OtaWirePayloads.hpp"`)
- Modify: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/test_native/astros_ota_espnow_messages_tests.cpp` (after the Task 1 tests):

```cpp
// OtaWirePayloads.hpp is pulled in transitively via <AstrOsMessaging.hpp>
// once Step 3 below wires the umbrella header to include it.

// M1 — Task 2: packed payload structs with byte-offset assertions.

TEST(OtaWirePayloads, OtaBeginRoundTripsViaByteArray)
{
    OtaBeginPayload p{};
    p.xferId = 0x42;
    p.totalSize = 0x12345678;
    p.chunkSize = 128;
    p.totalChunks = 9600;
    for (int i = 0; i < 32; i++) p.sha256Expected[i] = static_cast<uint8_t>(i);
    p.flags = 0;

    // sizeof(OtaBeginPayload) is locked by static_assert in the header.
    // Here we just confirm that interpreting the struct as bytes preserves layout.
    uint8_t buf[sizeof(OtaBeginPayload)];
    std::memcpy(buf, &p, sizeof(p));

    EXPECT_EQ(0x42, buf[0]);  // xferId
    // u32 little-endian at offset 1: 0x12345678 → 78 56 34 12
    EXPECT_EQ(0x78, buf[1]);
    EXPECT_EQ(0x56, buf[2]);
    EXPECT_EQ(0x34, buf[3]);
    EXPECT_EQ(0x12, buf[4]);
    // u16 little-endian at offset 5: 128 → 80 00
    EXPECT_EQ(0x80, buf[5]);
    EXPECT_EQ(0x00, buf[6]);
    // u32 little-endian at offset 7: 9600 → 80 25 00 00
    EXPECT_EQ(0x80, buf[7]);
    EXPECT_EQ(0x25, buf[8]);
    EXPECT_EQ(0x00, buf[9]);
    EXPECT_EQ(0x00, buf[10]);
    // u8[32] sha256 at offset 11..42
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(i), buf[11 + i]);
    EXPECT_EQ(0u, buf[43]);  // flags
}

TEST(OtaWirePayloads, OtaDataNakReasonValuesMatchSpec)
{
    // Spec freezes these values: CRC=1, SIZE=2, OUT_OF_ORDER=3, WRITE=4.
    // Reordering or renumbering breaks the wire contract.
    EXPECT_EQ(1, static_cast<int>(OtaDataNakReason::CRC));
    EXPECT_EQ(2, static_cast<int>(OtaDataNakReason::SIZE));
    EXPECT_EQ(3, static_cast<int>(OtaDataNakReason::OUT_OF_ORDER));
    EXPECT_EQ(4, static_cast<int>(OtaDataNakReason::WRITE));
}

TEST(OtaWirePayloads, OtaBeginNakReasonValuesMatchSpec)
{
    EXPECT_EQ(0, static_cast<int>(OtaBeginNakReason::BUSY));
    EXPECT_EQ(1, static_cast<int>(OtaBeginNakReason::NO_PARTITION));
    EXPECT_EQ(2, static_cast<int>(OtaBeginNakReason::BEGIN_FAILED));
}

TEST(OtaWirePayloads, OtaEndStatusValuesMatchSpec)
{
    EXPECT_EQ(0, static_cast<int>(OtaEndStatus::OK));
    EXPECT_EQ(1, static_cast<int>(OtaEndStatus::HASH_MISMATCH));
    EXPECT_EQ(2, static_cast<int>(OtaEndStatus::WRITE_ERROR));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: compile error — `OtaWirePayloads.hpp` not found.

- [ ] **Step 3: Create the packed payload header**

Create `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

// On-wire byte layouts for ESP-NOW OTA frames (PR set 1).
//
// Direction: master → padawan = "downstream"; padawan → master = "upstream".
//
// All multi-byte integer fields are stored in native ESP32 byte order
// (little-endian). The only consumers of these structs are ESP32 nodes;
// no host-to-network conversion is required. __attribute__((packed))
// prevents the compiler from inserting alignment padding between fields,
// which would silently break wire compatibility.
//
// Each struct has a static_assert immediately after its definition that
// pins sizeof to the spec'd byte count. If any field type changes or the
// compiler inserts padding, the assertion fires at compile time.

// ─── Reason / status enums (wire-stable; values must not be renumbered) ──

enum class OtaBeginNakReason : uint8_t
{
    BUSY = 0,         // transfer already in flight
    NO_PARTITION = 1, // inactive partition missing or too small for totalSize
    BEGIN_FAILED = 2  // esp_ota_begin returned non-OK (MIXED layer only)
};

enum class OtaDataNakReason : uint8_t
{
    CRC = 1,
    SIZE = 2,
    OUT_OF_ORDER = 3,
    WRITE = 4 // esp_ota_write failed (MIXED layer only)
};

enum class OtaEndStatus : uint8_t
{
    OK = 0,
    HASH_MISMATCH = 1, // streamed SHA mismatch OR read-back-rehash mismatch
    WRITE_ERROR = 2    // chunk count mismatch OR esp_ota_end failed
};

// ─── Downstream frames (master → padawan) ────────────────────────────────

struct __attribute__((packed)) OtaBeginPayload
{
    uint8_t xferId;
    uint32_t totalSize;
    uint16_t chunkSize;
    uint32_t totalChunks;
    uint8_t sha256Expected[32];
    uint8_t flags; // bit0=enable-psram-buffer (reserved for future use); 0 in PR set 1
};
static_assert(sizeof(OtaBeginPayload) == 44, "OtaBeginPayload must be 44 bytes on the wire");

// OTA_DATA payload = header + variable-length firmware bytes.
// The MIXED layer reads payloadLen bytes immediately after the header.
struct __attribute__((packed)) OtaDataHeader
{
    uint8_t xferId;
    uint32_t seq;
    uint16_t payloadLen;
    uint16_t crc16; // CRC-16/CCITT-FALSE over [xferId..end of firmware-bytes]
                    // (matches the AstrOsBulkTransport::crc16_ccitt_false function)
};
static_assert(sizeof(OtaDataHeader) == 9, "OtaDataHeader must be 9 bytes on the wire");

struct __attribute__((packed)) OtaEndPayload
{
    uint8_t xferId;
    uint32_t totalChunksSent;
    uint8_t sha256Final[32];
};
static_assert(sizeof(OtaEndPayload) == 37, "OtaEndPayload must be 37 bytes on the wire");

// ─── Upstream frames (padawan → master) ──────────────────────────────────

struct __attribute__((packed)) OtaBeginAckPayload
{
    uint8_t xferId;
};
static_assert(sizeof(OtaBeginAckPayload) == 1, "OtaBeginAckPayload must be 1 byte on the wire");

struct __attribute__((packed)) OtaBeginNakPayload
{
    uint8_t xferId;
    uint8_t reason; // OtaBeginNakReason value
};
static_assert(sizeof(OtaBeginNakPayload) == 2, "OtaBeginNakPayload must be 2 bytes on the wire");

struct __attribute__((packed)) OtaDataAckPayload
{
    uint8_t xferId;
    uint32_t highestContiguousSeq;
    uint32_t nextExpectedSeq;
    uint8_t windowRemaining;
};
static_assert(sizeof(OtaDataAckPayload) == 10, "OtaDataAckPayload must be 10 bytes on the wire");

struct __attribute__((packed)) OtaDataNakPayload
{
    uint8_t xferId;
    uint32_t highestContiguousSeq;
    uint32_t nextExpectedSeq;
    uint8_t windowRemaining;
    uint8_t reason; // OtaDataNakReason value
};
static_assert(sizeof(OtaDataNakPayload) == 11, "OtaDataNakPayload must be 11 bytes on the wire");

struct __attribute__((packed)) OtaEndAckPayload
{
    uint8_t xferId;
    uint8_t status; // OtaEndStatus value
    uint8_t sha256Computed[32];
};
static_assert(sizeof(OtaEndAckPayload) == 34, "OtaEndAckPayload must be 34 bytes on the wire");
```

Then add `#include "OtaWirePayloads.hpp"` to the umbrella header `lib_native/AstrOsMessaging/include/AstrOsMessaging.hpp` so consumers can access the payload structs through the existing `<AstrOsMessaging.hpp>` include:

```cpp
#ifndef ASTROSMESSAGING_HPP
#define ASTROSMESSAGING_HPP

#include "AstrOsEspNowMessageService.hpp"
#include "AstrOsSerialMessageService.hpp"
#include "OtaWirePayloads.hpp"
#include "PacketTracker.hpp"

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: all OtaWirePayloads.* tests PASS.

Also run full suite to confirm no regression:

Run: `pio test -e test 2>&1 | tail -10`
Expected: all existing tests + 6 new OtaWirePayloads tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp \
        lib_native/AstrOsMessaging/include/AstrOsMessaging.hpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(messaging): add packed OTA wire-payload structs + reason enums"
```

---

## Task 3: Add `generateOtaPacket` binary builder

The existing `generatePackets` injects a validator string into the payload (eats ~12 bytes for "OTA_DATA" etc). OTA frames are binary and use every payload byte. `generateOtaPacket` builds a single-packet ESP-NOW frame with the raw payload bytes directly after the 20-byte AstrOs header.

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp`
- Modify: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/test_native/astros_ota_espnow_messages_tests.cpp`:

```cpp
// M1 — Task 3: generateOtaPacket builds a single-packet binary frame
// with no validator-string injection.

TEST(OtaPacketBuilder, GenerateOtaBeginProducesOneFrame)
{
    auto svc = AstrOsEspNowMessageService();

    OtaBeginPayload payload{};
    payload.xferId = 0x42;
    payload.totalSize = 0x12345678;
    payload.chunkSize = 128;
    payload.totalChunks = 9600;
    for (int i = 0; i < 32; i++) payload.sha256Expected[i] = static_cast<uint8_t>(i);
    payload.flags = 0;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN,
                                          reinterpret_cast<const uint8_t *>(&payload),
                                          sizeof(payload));
    ASSERT_EQ(1u, packets.size());
    ASSERT_EQ(20u + sizeof(payload), packets[0].size);

    uint8_t *p = packets[0].data;
    // bytes 0..15 = id (random — don't check content, just that they exist)
    EXPECT_EQ(1, p[16]); // packetNumber
    EXPECT_EQ(1, p[17]); // totalPackets
    EXPECT_EQ(static_cast<uint8_t>(AstrOsPacketType::OTA_BEGIN), p[18]);
    EXPECT_EQ(static_cast<uint8_t>(sizeof(payload)), p[19]); // payloadSize = 44

    // Payload bytes start at offset 20 with NO validator-string prefix.
    EXPECT_EQ(0x42, p[20]);  // xferId byte 0 of payload
    EXPECT_EQ(0x78, p[21]);  // totalSize byte 0 (LE)
    // ... (full bit-for-bit equality)
    EXPECT_EQ(0, std::memcmp(p + 20, &payload, sizeof(payload)));

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaPacketBuilder, GenerateOtaPacketRejectsNonOtaType)
{
    auto svc = AstrOsEspNowMessageService();
    uint8_t dummy[1] = {0};

    auto packets = svc.generateOtaPacket(AstrOsPacketType::BASIC, dummy, sizeof(dummy));
    EXPECT_EQ(0u, packets.size()); // empty vector signals rejection
}

TEST(OtaPacketBuilder, GenerateOtaPacketRejectsOversizedPayload)
{
    auto svc = AstrOsEspNowMessageService();
    uint8_t big[ASTROS_PACKET_PAYLOAD_SIZE + 1] = {0};

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, big, sizeof(big));
    EXPECT_EQ(0u, packets.size());
}

TEST(OtaPacketBuilder, GenerateOtaDataAckProducesTinyFrame)
{
    auto svc = AstrOsEspNowMessageService();
    OtaDataAckPayload ack{};
    ack.xferId = 7;
    ack.highestContiguousSeq = 1024;
    ack.nextExpectedSeq = 1025;
    ack.windowRemaining = 8;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_ACK,
                                          reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
    ASSERT_EQ(1u, packets.size());
    EXPECT_EQ(20u + 10u, packets[0].size); // header + 10B payload

    for (auto &pkt : packets) free(pkt.data);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: compile error — `generateOtaPacket` is not a member of `AstrOsEspNowMessageService`.

- [ ] **Step 3: Add the builder declaration**

In `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp`, add to the `public:` section of the class (after `generatePackets`):

```cpp
    // Binary-frame builder for OTA packets. Unlike generateEspNowMsg/generatePackets,
    // this path does NOT inject a validator-string prefix into the payload — the full
    // ASTROS_PACKET_PAYLOAD_SIZE budget is available for binary content. Always
    // produces exactly one packet (OTA frames fit in a single ESP-NOW transmission
    // by design). Returns an empty vector if `type` is not an OTA type or `len`
    // exceeds ASTROS_PACKET_PAYLOAD_SIZE.
    std::vector<astros_espnow_data_t> generateOtaPacket(AstrOsPacketType type, const uint8_t *payload, size_t len);
```

Add also (outside the class, near the existing declarations or above the class) a free helper:

```cpp
// True iff `type` is one of the 8 OTA packet types added in M1.
bool isOtaPacketType(AstrOsPacketType type);
```

- [ ] **Step 4: Implement the builder + helper**

In `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp`, add (at file scope, before the class methods):

```cpp
bool isOtaPacketType(AstrOsPacketType type)
{
    switch (type)
    {
    case AstrOsPacketType::OTA_BEGIN:
    case AstrOsPacketType::OTA_BEGIN_ACK:
    case AstrOsPacketType::OTA_BEGIN_NAK:
    case AstrOsPacketType::OTA_DATA:
    case AstrOsPacketType::OTA_DATA_ACK:
    case AstrOsPacketType::OTA_DATA_NAK:
    case AstrOsPacketType::OTA_END:
    case AstrOsPacketType::OTA_END_ACK:
        return true;
    default:
        return false;
    }
}
```

Then add the `generateOtaPacket` implementation (after `generatePackets`):

```cpp
std::vector<astros_espnow_data_t> AstrOsEspNowMessageService::generateOtaPacket(AstrOsPacketType type,
                                                                                  const uint8_t *payload, size_t len)
{
    std::vector<astros_espnow_data_t> packets;

    if (!isOtaPacketType(type))
    {
        return packets;
    }
    if (len > ASTROS_PACKET_PAYLOAD_SIZE)
    {
        return packets;
    }
    if (len > 0 && payload == nullptr)
    {
        return packets;
    }

    uint8_t *id = AstrOsEspNowMessageService::generateId();
    uint8_t *frame = (uint8_t *)malloc(20 + len);

    int packetNumber = 1;
    int totalPackets = 1;
    uint8_t typeByte = static_cast<uint8_t>(type);
    uint8_t payloadSize = static_cast<uint8_t>(len);

    int offset = 0;
    std::memcpy(frame, id, 16);
    offset += 16;
    std::memcpy(frame + offset, &packetNumber, 1);
    offset += 1;
    std::memcpy(frame + offset, &totalPackets, 1);
    offset += 1;
    std::memcpy(frame + offset, &typeByte, 1);
    offset += 1;
    std::memcpy(frame + offset, &payloadSize, 1);
    offset += 1;
    if (len > 0)
    {
        std::memcpy(frame + offset, payload, len);
    }

    free(id);

    astros_espnow_data_t data = {frame, 20 + len};
    packets.push_back(data);
    return packets;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: all OtaPacketBuilder.* tests PASS.

Run full suite for regression: `pio test -e test 2>&1 | tail -10`
Expected: all tests PASS.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(messaging): add generateOtaPacket binary-frame builder + isOtaPacketType helper"
```

---

## Task 4: Teach `parsePacket` to recognize OTA types (skip validator strip)

The existing parser strips a validator-string prefix from the payload (used by string-payload types). For OTA types, the payload IS the binary frame — no validator was injected. `parsePacket` needs to detect OTA types and skip the validator check, so the consumer sees `payload` pointing directly at the binary content and `payloadSize` equal to the wire length.

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp`
- Modify: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/test_native/astros_ota_espnow_messages_tests.cpp`:

```cpp
// M1 — Task 4: parsePacket recognizes OTA types and skips validator-string strip.

TEST(OtaPacketParser, ParseOtaBeginReturnsBinaryPayloadIntact)
{
    auto svc = AstrOsEspNowMessageService();

    OtaBeginPayload original{};
    original.xferId = 0xAB;
    original.totalSize = 1228800;
    original.chunkSize = 128;
    original.totalChunks = 9600;
    for (int i = 0; i < 32; i++) original.sha256Expected[i] = static_cast<uint8_t>(0xF0 | (i & 0x0F));
    original.flags = 0;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    ASSERT_EQ(1u, packets.size());

    auto parsed = svc.parsePacket(packets[0].data);

    EXPECT_EQ(AstrOsPacketType::OTA_BEGIN, parsed.packetType);
    EXPECT_EQ(static_cast<int>(sizeof(OtaBeginPayload)), parsed.payloadSize);
    // payload pointer points DIRECTLY at the OtaBeginPayload bytes (no validator stripped).
    EXPECT_EQ(0, std::memcmp(parsed.payload, &original, sizeof(original)));

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaPacketParser, ParseOtaDataAckReturnsBinaryPayloadIntact)
{
    auto svc = AstrOsEspNowMessageService();

    OtaDataAckPayload original{};
    original.xferId = 0x07;
    original.highestContiguousSeq = 4095;
    original.nextExpectedSeq = 4096;
    original.windowRemaining = 8;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_ACK,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    ASSERT_EQ(1u, packets.size());

    auto parsed = svc.parsePacket(packets[0].data);
    EXPECT_EQ(AstrOsPacketType::OTA_DATA_ACK, parsed.packetType);
    EXPECT_EQ(static_cast<int>(sizeof(OtaDataAckPayload)), parsed.payloadSize);
    EXPECT_EQ(0, std::memcmp(parsed.payload, &original, sizeof(original)));

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaPacketParser, ParseExistingStringTypeStillStripsValidator)
{
    // Regression: parsePacket must continue to strip the validator string
    // for non-OTA types like BASIC. (This guards against accidentally
    // routing the OTA path for everything.)
    auto svc = AstrOsEspNowMessageService();
    auto packets = svc.generatePackets(AstrOsPacketType::BASIC, "hello");
    ASSERT_EQ(1u, packets.size());

    auto parsed = svc.parsePacket(packets[0].data);
    EXPECT_EQ(AstrOsPacketType::BASIC, parsed.packetType);
    // After stripping "BASIC" + UNIT_SEPARATOR (5+1 = 6 bytes), payload is "hello" (5 bytes).
    EXPECT_EQ(5, parsed.payloadSize);
    EXPECT_EQ(0, std::memcmp(parsed.payload, "hello", 5));

    for (auto &pkt : packets) free(pkt.data);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: `OtaPacketParser.ParseOtaBeginReturnsBinaryPayloadIntact` and `OtaPacketParser.ParseOtaDataAckReturnsBinaryPayloadIntact` FAIL. Currently `parsePacket` runs `validatePacket` which sees the binary bytes don't match the validator-string "OTA_BEGIN" and sets packetType to UNKNOWN. The third test (`ParseExistingStringTypeStillStripsValidator`) should already PASS as a regression baseline.

- [ ] **Step 3: Update `parsePacket` to short-circuit on OTA types**

In `lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp`, replace the existing `parsePacket` body. Current:

```cpp
astros_packet_t AstrOsEspNowMessageService::parsePacket(uint8_t *packet)
{
    astros_packet_t parsedPacket;
    memcpy(parsedPacket.id, packet, 16);
    parsedPacket.packetNumber = packet[16];
    parsedPacket.totalPackets = packet[17];
    parsedPacket.packetType = (AstrOsPacketType)packet[18];
    parsedPacket.payloadSize = packet[19];
    parsedPacket.payload = packet + 20;

    auto validated = validatePacket(parsedPacket);
    if (validated == -1)
    {
        parsedPacket.packetType = AstrOsPacketType::UNKNOWN;
    }
    else // remove validator from payload
    {
        parsedPacket.payloadSize = ((int)packet[19]) - validated;
        parsedPacket.payload = packet + 20 + validated;
    }

    return parsedPacket;
}
```

Replace with:

```cpp
astros_packet_t AstrOsEspNowMessageService::parsePacket(uint8_t *packet)
{
    astros_packet_t parsedPacket;
    memcpy(parsedPacket.id, packet, 16);
    parsedPacket.packetNumber = packet[16];
    parsedPacket.totalPackets = packet[17];
    parsedPacket.packetType = (AstrOsPacketType)packet[18];
    parsedPacket.payloadSize = packet[19];
    parsedPacket.payload = packet + 20;

    if (isOtaPacketType(parsedPacket.packetType))
    {
        // OTA frames carry binary payloads with no validator-string prefix.
        // payload and payloadSize already point at the right bytes; skip
        // the validatePacket() call below which would mark the packet
        // UNKNOWN (the binary bytes won't match the validator string).
        return parsedPacket;
    }

    auto validated = validatePacket(parsedPacket);
    if (validated == -1)
    {
        parsedPacket.packetType = AstrOsPacketType::UNKNOWN;
    }
    else // remove validator from payload
    {
        parsedPacket.payloadSize = ((int)packet[19]) - validated;
        parsedPacket.payload = packet + 20 + validated;
    }

    return parsedPacket;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: all OtaPacketParser.* tests PASS.

Run full suite for regression: `pio test -e test 2>&1 | tail -10`
Expected: all existing 309+ tests + all OTA-suite tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsEspNowMessageService.cpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(messaging): parsePacket recognizes OTA types and skips validator strip"
```

---

## Task 5: Add POD record types + parser free functions in AstrOsEspNowProtocol

Mirrors the existing `FwTransferBeginRecord` + `parseFwTransferBegin` pattern in `AstrOsSerialMessageService`. Each parser takes a parsed `astros_packet_t` (output of `parsePacket`) and returns a `valid=true/false` POD record. Future MIXED layer (M3/M4) calls these parsers directly when its ESP-NOW RX callback sees an OTA packet type.

**Files:**
- Modify: `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`
- Modify: `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp`
- Modify: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/test_native/astros_ota_espnow_messages_tests.cpp`:

```cpp
#include <AstrOsEspNowProtocol.hpp>

// M1 — Task 5: POD record types + parsers in AstrOsEspNowProtocol.

TEST(OtaRecordParsers, ParseOtaBeginRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginPayload original{};
    original.xferId = 0x42;
    original.totalSize = 1228800;
    original.chunkSize = 128;
    original.totalChunks = 9600;
    for (int i = 0; i < 32; i++) original.sha256Expected[i] = static_cast<uint8_t>(i);
    original.flags = 0;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBegin(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(1228800u, rec.totalSize);
    EXPECT_EQ(128u, rec.chunkSize);
    EXPECT_EQ(9600u, rec.totalChunks);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(i), rec.sha256Expected[i]);
    EXPECT_EQ(0u, rec.flags);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginRejectsShortPayload)
{
    // Construct a malformed OTA_BEGIN with only 10 bytes of payload (not 44).
    auto svc = AstrOsEspNowMessageService();
    uint8_t shortPayload[10] = {0};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN, shortPayload, sizeof(shortPayload));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBegin(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    // OTA_DATA = 9-byte header + N bytes of firmware payload. Build one with 16 fw bytes.
    uint8_t frame[sizeof(OtaDataHeader) + 16];
    OtaDataHeader hdr{};
    hdr.xferId = 0x07;
    hdr.seq = 42;
    hdr.payloadLen = 16;
    hdr.crc16 = 0xABCD;
    std::memcpy(frame, &hdr, sizeof(hdr));
    for (int i = 0; i < 16; i++) frame[sizeof(hdr) + i] = static_cast<uint8_t>(0xA0 | i);

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, frame, sizeof(frame));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaData(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x07, rec.xferId);
    EXPECT_EQ(42u, rec.seq);
    EXPECT_EQ(16u, rec.payloadLen);
    EXPECT_EQ(0xABCDu, rec.crc16);
    // rec.payload points into the parsed packet's payload buffer.
    EXPECT_EQ(0xA0, rec.payload[0]);
    EXPECT_EQ(0xAF, rec.payload[15]);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataRejectsPayloadLenMismatch)
{
    // header.payloadLen = 32 but the actual packet only carries 16 bytes of payload.
    auto svc = AstrOsEspNowMessageService();
    uint8_t frame[sizeof(OtaDataHeader) + 16];
    OtaDataHeader hdr{};
    hdr.xferId = 1;
    hdr.seq = 0;
    hdr.payloadLen = 32; // lying — actual payload is 16 bytes
    hdr.crc16 = 0;
    std::memcpy(frame, &hdr, sizeof(hdr));
    std::memset(frame + sizeof(hdr), 0xCC, 16);

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, frame, sizeof(frame));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaData(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginAckRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginAckPayload original{0x42};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_ACK,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBeginAck(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginNakRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginNakPayload original{0x42, static_cast<uint8_t>(OtaBeginNakReason::NO_PARTITION)};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_NAK,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBeginNak(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(OtaBeginNakReason::NO_PARTITION, rec.reason);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginNakRejectsOutOfRangeReason)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginNakPayload bad{0x42, 99}; // 99 is not a valid OtaBeginNakReason
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_NAK,
                                          reinterpret_cast<const uint8_t *>(&bad), sizeof(bad));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBeginNak(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataAckRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaDataAckPayload original{0x07, 1023, 1024, 8};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_ACK,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaDataAck(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x07, rec.xferId);
    EXPECT_EQ(1023u, rec.highestContiguousSeq);
    EXPECT_EQ(1024u, rec.nextExpectedSeq);
    EXPECT_EQ(8, rec.windowRemaining);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataNakRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaDataNakPayload original{0x07, 1023, 1024, 8, static_cast<uint8_t>(OtaDataNakReason::CRC)};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_NAK,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaDataNak(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x07, rec.xferId);
    EXPECT_EQ(1023u, rec.highestContiguousSeq);
    EXPECT_EQ(1024u, rec.nextExpectedSeq);
    EXPECT_EQ(8, rec.windowRemaining);
    EXPECT_EQ(OtaDataNakReason::CRC, rec.reason);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaEndRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaEndPayload original{};
    original.xferId = 0x42;
    original.totalChunksSent = 9600;
    for (int i = 0; i < 32; i++) original.sha256Final[i] = static_cast<uint8_t>(0xC0 | (i & 0x0F));
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_END,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaEnd(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(9600u, rec.totalChunksSent);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(0xC0 | (i & 0x0F)), rec.sha256Final[i]);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaEndAckRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaEndAckPayload original{};
    original.xferId = 0x42;
    original.status = static_cast<uint8_t>(OtaEndStatus::OK);
    for (int i = 0; i < 32; i++) original.sha256Computed[i] = static_cast<uint8_t>(0xB0 | (i & 0x0F));
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_END_ACK,
                                          reinterpret_cast<const uint8_t *>(&original), sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaEndAck(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(OtaEndStatus::OK, rec.status);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(0xB0 | (i & 0x0F)), rec.sha256Computed[i]);

    for (auto &pkt : packets) free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaEndAckRejectsOutOfRangeStatus)
{
    auto svc = AstrOsEspNowMessageService();
    OtaEndAckPayload bad{};
    bad.xferId = 0x42;
    bad.status = 99; // not a valid OtaEndStatus
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_END_ACK,
                                          reinterpret_cast<const uint8_t *>(&bad), sizeof(bad));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaEndAck(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets) free(pkt.data);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -20`
Expected: compile errors — `parseOtaBegin`, `parseOtaData`, etc. are not members of `AstrOsEspNowProtocol`.

- [ ] **Step 3: Add POD record types + parser declarations to the protocol header**

In `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`, add (inside the `AstrOsEspNowProtocol` namespace, before the `handlePacket` declaration):

```cpp
    // ─── OTA records (M1 wire format) ────────────────────────────────────
    //
    // Each parser takes a parsed astros_packet_t (output of
    // AstrOsEspNowMessageService::parsePacket) and returns a POD record
    // with a `bool valid` field. valid=false means the payload length
    // didn't match the spec'd size, or a reason/status enum value was
    // out of range.
    //
    // These parsers are PURE — they do not mutate any state, do not
    // allocate, and operate only on the bytes already in the packet
    // buffer. The future MIXED layer (M3/M4) calls them from its ESP-NOW
    // RX callback to convert wire bytes into records before queueing.

    struct OtaBeginRecord
    {
        uint8_t xferId = 0;
        uint32_t totalSize = 0;
        uint16_t chunkSize = 0;
        uint32_t totalChunks = 0;
        uint8_t sha256Expected[32] = {0};
        uint8_t flags = 0;
        bool valid = false;
    };

    struct OtaBeginAckRecord
    {
        uint8_t xferId = 0;
        bool valid = false;
    };

    struct OtaBeginNakRecord
    {
        uint8_t xferId = 0;
        OtaBeginNakReason reason = OtaBeginNakReason::BUSY;
        bool valid = false;
    };

    // OtaDataRecord.payload is a pointer into the original parsed packet's
    // buffer. The caller MUST consume or copy the bytes before the buffer
    // is freed/reused. payloadLen is the spec'd length from the header;
    // parseOtaData rejects records where payloadLen doesn't match the
    // actual bytes-after-header count.
    struct OtaDataRecord
    {
        uint8_t xferId = 0;
        uint32_t seq = 0;
        uint16_t payloadLen = 0;
        uint16_t crc16 = 0;
        const uint8_t *payload = nullptr;
        bool valid = false;
    };

    struct OtaDataAckRecord
    {
        uint8_t xferId = 0;
        uint32_t highestContiguousSeq = 0;
        uint32_t nextExpectedSeq = 0;
        uint8_t windowRemaining = 0;
        bool valid = false;
    };

    struct OtaDataNakRecord
    {
        uint8_t xferId = 0;
        uint32_t highestContiguousSeq = 0;
        uint32_t nextExpectedSeq = 0;
        uint8_t windowRemaining = 0;
        OtaDataNakReason reason = OtaDataNakReason::CRC;
        bool valid = false;
    };

    struct OtaEndRecord
    {
        uint8_t xferId = 0;
        uint32_t totalChunksSent = 0;
        uint8_t sha256Final[32] = {0};
        bool valid = false;
    };

    struct OtaEndAckRecord
    {
        uint8_t xferId = 0;
        OtaEndStatus status = OtaEndStatus::OK;
        uint8_t sha256Computed[32] = {0};
        bool valid = false;
    };

    OtaBeginRecord parseOtaBegin(const astros_packet_t &packet);
    OtaBeginAckRecord parseOtaBeginAck(const astros_packet_t &packet);
    OtaBeginNakRecord parseOtaBeginNak(const astros_packet_t &packet);
    OtaDataRecord parseOtaData(const astros_packet_t &packet);
    OtaDataAckRecord parseOtaDataAck(const astros_packet_t &packet);
    OtaDataNakRecord parseOtaDataNak(const astros_packet_t &packet);
    OtaEndRecord parseOtaEnd(const astros_packet_t &packet);
    OtaEndAckRecord parseOtaEndAck(const astros_packet_t &packet);
```

No new includes needed — `OtaWirePayloads.hpp` is already pulled in transitively via the existing `#include <AstrOsMessaging.hpp>` (the umbrella header was wired in Task 2 to include it).

- [ ] **Step 4: Implement the parsers**

In `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp`, add at the end of the `AstrOsEspNowProtocol` namespace (before the closing brace):

```cpp
    OtaBeginRecord parseOtaBegin(const astros_packet_t &packet)
    {
        OtaBeginRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_BEGIN ||
            packet.payloadSize != static_cast<int>(sizeof(OtaBeginPayload)))
        {
            return rec; // valid stays false
        }
        const auto *p = reinterpret_cast<const OtaBeginPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.totalSize = p->totalSize;
        rec.chunkSize = p->chunkSize;
        rec.totalChunks = p->totalChunks;
        std::memcpy(rec.sha256Expected, p->sha256Expected, 32);
        rec.flags = p->flags;
        rec.valid = true;
        return rec;
    }

    OtaBeginAckRecord parseOtaBeginAck(const astros_packet_t &packet)
    {
        OtaBeginAckRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_BEGIN_ACK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaBeginAckPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaBeginAckPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.valid = true;
        return rec;
    }

    OtaBeginNakRecord parseOtaBeginNak(const astros_packet_t &packet)
    {
        OtaBeginNakRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_BEGIN_NAK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaBeginNakPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaBeginNakPayload *>(packet.payload);
        if (p->reason > static_cast<uint8_t>(OtaBeginNakReason::BEGIN_FAILED))
        {
            return rec; // out-of-range reason
        }
        rec.xferId = p->xferId;
        rec.reason = static_cast<OtaBeginNakReason>(p->reason);
        rec.valid = true;
        return rec;
    }

    OtaDataRecord parseOtaData(const astros_packet_t &packet)
    {
        OtaDataRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_DATA ||
            packet.payloadSize < static_cast<int>(sizeof(OtaDataHeader)))
        {
            return rec;
        }
        const auto *hdr = reinterpret_cast<const OtaDataHeader *>(packet.payload);
        // The actual firmware-bytes count is packet.payloadSize - sizeof(header).
        // hdr->payloadLen MUST equal that, else reject.
        const int actualBytes = packet.payloadSize - static_cast<int>(sizeof(OtaDataHeader));
        if (static_cast<int>(hdr->payloadLen) != actualBytes)
        {
            return rec;
        }
        rec.xferId = hdr->xferId;
        rec.seq = hdr->seq;
        rec.payloadLen = hdr->payloadLen;
        rec.crc16 = hdr->crc16;
        rec.payload = packet.payload + sizeof(OtaDataHeader);
        rec.valid = true;
        return rec;
    }

    OtaDataAckRecord parseOtaDataAck(const astros_packet_t &packet)
    {
        OtaDataAckRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_DATA_ACK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaDataAckPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaDataAckPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.highestContiguousSeq = p->highestContiguousSeq;
        rec.nextExpectedSeq = p->nextExpectedSeq;
        rec.windowRemaining = p->windowRemaining;
        rec.valid = true;
        return rec;
    }

    OtaDataNakRecord parseOtaDataNak(const astros_packet_t &packet)
    {
        OtaDataNakRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_DATA_NAK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaDataNakPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaDataNakPayload *>(packet.payload);
        if (p->reason < static_cast<uint8_t>(OtaDataNakReason::CRC) ||
            p->reason > static_cast<uint8_t>(OtaDataNakReason::WRITE))
        {
            return rec;
        }
        rec.xferId = p->xferId;
        rec.highestContiguousSeq = p->highestContiguousSeq;
        rec.nextExpectedSeq = p->nextExpectedSeq;
        rec.windowRemaining = p->windowRemaining;
        rec.reason = static_cast<OtaDataNakReason>(p->reason);
        rec.valid = true;
        return rec;
    }

    OtaEndRecord parseOtaEnd(const astros_packet_t &packet)
    {
        OtaEndRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_END ||
            packet.payloadSize != static_cast<int>(sizeof(OtaEndPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaEndPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.totalChunksSent = p->totalChunksSent;
        std::memcpy(rec.sha256Final, p->sha256Final, 32);
        rec.valid = true;
        return rec;
    }

    OtaEndAckRecord parseOtaEndAck(const astros_packet_t &packet)
    {
        OtaEndAckRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_END_ACK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaEndAckPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaEndAckPayload *>(packet.payload);
        if (p->status > static_cast<uint8_t>(OtaEndStatus::WRITE_ERROR))
        {
            return rec;
        }
        rec.xferId = p->xferId;
        rec.status = static_cast<OtaEndStatus>(p->status);
        std::memcpy(rec.sha256Computed, p->sha256Computed, 32);
        rec.valid = true;
        return rec;
    }
```

Add `#include <cstring>` at the top of the .cpp if not already present.

- [ ] **Step 5: Run test to verify it passes**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -25`
Expected: all OtaRecordParsers.* tests PASS.

Run full suite: `pio test -e test 2>&1 | tail -10`
Expected: all tests PASS.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp \
        lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(protocol): add Ota*Record POD types + parser free functions"
```

---

## Task 6: Extend `handlePacket` dispatcher with role-gated OTA cases

The existing dispatcher classifies packet types as `Ok` (handled and returned an InterfaceMessage), `Pending` (multi-packet, more to come), `InvalidPayload`, `WrongRole`, `UnsupportedType` (PURE layer doesn't extract; MIXED adapter handles directly), or `UnknownType` (packetType out of range). For OTA in M1, all 8 types route to `UnsupportedType` — the MIXED OtaForwarder/OtaWriter (M3/M4) handles them directly. But the dispatcher still enforces direction-of-travel: master receives only ACK/NAK (padawan→master); padawan receives only BEGIN/DATA/END (master→padawan). Wrong direction → `WrongRole`.

**Files:**
- Modify: `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp`
- Modify: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/test_native/astros_ota_espnow_messages_tests.cpp`:

```cpp
#include <PacketTracker.hpp>

// M1 — Task 6: handlePacket dispatcher recognizes OTA types and applies role gating.

namespace
{
astros_packet_t buildOtaPacketForDispatch(AstrOsPacketType type, const uint8_t *payload, size_t len,
                                            std::vector<astros_espnow_data_t> &keepAlive)
{
    auto svc = AstrOsEspNowMessageService();
    auto packets = svc.generateOtaPacket(type, payload, len);
    // Caller frees via keepAlive
    keepAlive.insert(keepAlive.end(), packets.begin(), packets.end());
    return svc.parsePacket(packets[0].data);
}
}

TEST(OtaDispatcher, MasterReceivesAckTypes_ReturnsUnsupportedType)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginAckPayload ack{0x42};
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN_ACK,
                                             reinterpret_cast<const uint8_t *>(&ack), sizeof(ack), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/true, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, result.status);

    for (auto &p : keepAlive) free(p.data);
}

TEST(OtaDispatcher, PadawanReceivesAckType_ReturnsWrongRole)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginAckPayload ack{0x42};
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN_ACK,
                                             reinterpret_cast<const uint8_t *>(&ack), sizeof(ack), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/false, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, result.status);

    for (auto &p : keepAlive) free(p.data);
}

TEST(OtaDispatcher, PadawanReceivesBeginType_ReturnsUnsupportedType)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginPayload begin{};
    begin.xferId = 0x42;
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN,
                                             reinterpret_cast<const uint8_t *>(&begin), sizeof(begin), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/false, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, result.status);

    for (auto &p : keepAlive) free(p.data);
}

TEST(OtaDispatcher, MasterReceivesBeginType_ReturnsWrongRole)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginPayload begin{};
    begin.xferId = 0x42;
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN,
                                             reinterpret_cast<const uint8_t *>(&begin), sizeof(begin), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/true, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, result.status);

    for (auto &p : keepAlive) free(p.data);
}

TEST(OtaDispatcher, AllUpstreamTypesGateMasterOnly)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    // All padawan→master types: master receives → UnsupportedType; padawan receives → WrongRole.
    AstrOsPacketType upstreamTypes[] = {
        AstrOsPacketType::OTA_BEGIN_ACK, AstrOsPacketType::OTA_BEGIN_NAK,
        AstrOsPacketType::OTA_DATA_ACK,  AstrOsPacketType::OTA_DATA_NAK,
        AstrOsPacketType::OTA_END_ACK,
    };

    for (auto type : upstreamTypes)
    {
        // payload bytes don't matter for dispatch — we never parse them in M1.
        uint8_t dummy[34] = {0};
        size_t len = (type == AstrOsPacketType::OTA_END_ACK)      ? 34u
                     : (type == AstrOsPacketType::OTA_DATA_ACK)    ? 10u
                     : (type == AstrOsPacketType::OTA_DATA_NAK)    ? 11u
                     : (type == AstrOsPacketType::OTA_BEGIN_NAK)   ? 2u
                                                                   : 1u; // OTA_BEGIN_ACK
        auto parsed = buildOtaPacketForDispatch(type, dummy, len, keepAlive);

        auto masterResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, true, 0);
        auto padawanResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, false, 0);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, masterResult.status)
            << "type=" << static_cast<int>(type);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, padawanResult.status)
            << "type=" << static_cast<int>(type);
    }

    for (auto &p : keepAlive) free(p.data);
}

TEST(OtaDispatcher, AllDownstreamTypesGatePadawanOnly)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    // All master→padawan types: padawan receives → UnsupportedType; master receives → WrongRole.
    // For OTA_DATA we send a minimal 9-byte header + 0-byte payload (payloadLen=0 in header).
    OtaDataHeader emptyDataHdr{};
    uint8_t otaBeginBuf[sizeof(OtaBeginPayload)] = {0};
    uint8_t otaEndBuf[sizeof(OtaEndPayload)] = {0};

    struct {
        AstrOsPacketType type;
        const uint8_t *payload;
        size_t len;
    } downstream[] = {
        {AstrOsPacketType::OTA_BEGIN, otaBeginBuf, sizeof(otaBeginBuf)},
        {AstrOsPacketType::OTA_DATA, reinterpret_cast<const uint8_t *>(&emptyDataHdr), sizeof(emptyDataHdr)},
        {AstrOsPacketType::OTA_END, otaEndBuf, sizeof(otaEndBuf)},
    };

    for (const auto &d : downstream)
    {
        auto parsed = buildOtaPacketForDispatch(d.type, d.payload, d.len, keepAlive);
        auto padawanResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, false, 0);
        auto masterResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, true, 0);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, padawanResult.status)
            << "type=" << static_cast<int>(d.type);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, masterResult.status)
            << "type=" << static_cast<int>(d.type);
    }

    for (auto &p : keepAlive) free(p.data);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -25`
Expected: dispatcher tests FAIL because the existing `handlePacket` returns `UnknownType` (default case) for OTA types.

- [ ] **Step 3: Extend the dispatcher switch**

In `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp`, locate the `handlePacket` function (around line 244+). Inside the switch, AFTER the existing `REGISTRATION_REQ` block and BEFORE the `CONFIG` case, add the OTA dispatch cases:

```cpp
        // OTA frames are handled by the MIXED OtaForwarder (master) / OtaWriter
        // (padawan) directly via parseOta* free functions; the PURE dispatcher
        // returns UnsupportedType with role gating to enforce direction-of-travel.
        // Master receives ACK/NAK from padawans; padawan receives BEGIN/DATA/END
        // from master.
        case AstrOsPacketType::OTA_BEGIN_ACK:
        case AstrOsPacketType::OTA_BEGIN_NAK:
        case AstrOsPacketType::OTA_DATA_ACK:
        case AstrOsPacketType::OTA_DATA_NAK:
        case AstrOsPacketType::OTA_END_ACK:
            return unsupportedOrWrongRole(isMasterNode);
        case AstrOsPacketType::OTA_BEGIN:
        case AstrOsPacketType::OTA_DATA:
        case AstrOsPacketType::OTA_END:
            return unsupportedOrWrongRole(!isMasterNode);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e test --filter "*ota_espnow*" 2>&1 | tail -25`
Expected: all OtaDispatcher.* tests PASS.

Run full suite: `pio test -e test 2>&1 | tail -10`
Expected: all 309+ existing tests + ~30 new OTA tests PASS.

Also confirm both boards still build:

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: `SUCCESS`.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: `SUCCESS`.

- [ ] **Step 5: Confirm clang-format clean on changed files**

Run: `git diff --name-only develop...HEAD | grep -E '\.(hpp|cpp|h)$' | xargs -I {} clang-format --dry-run --Werror {}`
Expected: no output (all changed C/C++ files are formatted). The `develop...HEAD` form lists files touched across the whole branch.

If any file fails, run `clang-format -i <file>` and stage/commit the result as a fixup.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(protocol): role-gated OTA dispatch (UnsupportedType for MIXED layer)"
```

---

## Task 7: Update plan checklist + close M1

Final housekeeping commit that checks off M1 in the design doc's task list and (optionally) adds an M1 entry to MEMORY.md if anything surprising came up during implementation. No code changes.

**Files:**
- Modify: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md` (no — design doc has the per-milestone bullet but is not a task checklist)
- Modify: `.docs/plans/20260523-1024-firmware-ota-mesh-forward-m1-wire-format.md` (this file — check off all `- [ ]` boxes as work completes)

- [x] **Step 1: Verify all tasks above checked off**

Open this file and confirm every `- [ ]` from Tasks 1-6 is now `- [x]`. If any are still unchecked, do not proceed — go back and complete them.

- [x] **Step 2: Final verification**

Run: `pio test -e test 2>&1 | tail -10`
Expected: 100% pass, includes ~30 new OTA-suite tests.

Run: `pio run -e metro_s3 2>&1 | tail -5`
Expected: SUCCESS.

Run: `pio run -e lolin_d32_pro 2>&1 | tail -5`
Expected: SUCCESS.

Run: `git log --oneline HEAD~6..HEAD`
Expected: 6 commits, one per task, all on `feature/ota-mesh-forward-m1-wire-format`.

- [x] **Step 3: Confirm `lib_native/` purity**

Run: `grep -RnE 'esp_|freertos|driver/|<esp_|<freertos' lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp || echo "PURE OK"`
Expected: `PURE OK` — no ESP-IDF/FreeRTOS includes leaked into PURE libs.

- [x] **Step 4: Ready to open PR**

M1 is feature-complete. Branch `feature/ota-mesh-forward-m1-wire-format` carries 7 commits (1 design doc + 6 implementation). Open a PR targeting `develop`:

```bash
gh pr create --title "feat(ota): M1 — ESP-NOW OTA wire format" --body "$(cat <<'EOF'
## Summary

First milestone of the firmware OTA mesh-forward work (PR set 1 of the cross-repo decomposition).

- Adds 8 new `AstrOsPacketType` enum values (OTA_BEGIN/_ACK/_NAK, OTA_DATA/_ACK/_NAK, OTA_END/_ACK)
- Adds packed wire-payload structs with `static_assert(sizeof(X) == N)` byte-offset gates
- Adds `generateOtaPacket` binary-frame builder (skips the validator-string injection used by string-payload types)
- Teaches `parsePacket` to recognize OTA types and skip validator strip
- Adds POD record types + parser free functions in `AstrOsEspNowProtocol`
- Extends `handlePacket` dispatcher with role-gated OTA cases (master/padawan direction-of-travel enforcement)

No firmware behavior change. PURE-lib-only changes; native-purity guard passes. ~30 new native tests, all green. Both boards build clean.

Design doc: `.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md`
M1 plan: `.docs/plans/20260523-1024-firmware-ota-mesh-forward-m1-wire-format.md`

Next: M2 (BulkSender PURE state machine).

## Test plan

- [ ] `pio test -e test` 100% pass
- [ ] `pio run -e metro_s3` clean
- [ ] `pio run -e lolin_d32_pro` clean
- [ ] clang-format clean on changed files
- [ ] native-purity CI guard passes

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

NOTE: PR creation requires the user's authenticated VS Code terminal per the project's command-execution memory.

---

## Out of scope for M1 (do NOT add)

- Any MIXED-side changes (no `lib/` edits in M1)
- Any `BulkSender` work (M2)
- Any `OtaForwarder` / `OtaWriter` classes (M3 / M4)
- Any `FW_DEPLOY_BEGIN` routing changes in `AstrOsSerialMsgHandler` (M3)
- Any queue creation in `src/main.cpp` (M3 / M4)
- Any CRC computation (the CRC field is wire-stable in M1 — actual CRC verification is M2's `BulkReceiver` consumer)
- `OTA_COMMIT` / `OTA_COMMIT_ACK` packet types (PR set 2)
- Server-side or Vue changes (cross-repo coordination: none required)
