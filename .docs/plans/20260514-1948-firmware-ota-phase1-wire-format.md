# Firmware OTA Phase 1 — Wire format Implementation Plan

> **Status: COMPLETE.** All 14 tasks below were implemented and merged on branch
> `feature/ota-master-serial-receive`. Two multi-agent PR review passes (5 agents
> each) plus external GitHub review feedback drove four cleanup commits that
> hardened the parsers (strict-unsigned sign rejection, interior-empty list
> rejection, SHA-256 character-set validation) and tightened documentation.
> Final native-test count: 252 (190 baseline + 62 new). Both board builds clean.
> This document is kept as a historical implementation record.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the `FW_*` serial message types (BEGIN/CHUNK/END family + DEPLOY/BACKPRESSURE) to `lib_native/AstrOsMessaging` and route them through `lib_native/AstrOsSerialProtocol`, with native tests covering every builder and parser. No firmware behavior change yet — this phase is wire format only.

**Architecture:** Append new entries to `AstrOsSC::` string table, `AstrOsSerialMessageType` enum (values 30–40 per `.docs/protocol.md`), and `msgTypeMap`. Add five new builder methods on `AstrOsSerialMessageService` (the four ACK/NAK/END_ACK/DEPLOY_DONE master→server messages). Add POD record structs and four parser free-functions (the four server→master inbound messages). Extend `AstrOsInterfaceResponseType`, `mapResponseType`, and `decodeSerialMessage` to dispatch the four inbound FW_* types as single-command `DecodedCommand` entries carrying the raw payload as `message` (downstream parsing in Phase 3 by OtaReceiver).

**Tech Stack:** C++17, googletest, PlatformIO `[env:test]` native build, existing AstrOs separator macros (`UNIT_SEPARATOR=0x1F`, `RECORD_SEPARATOR=0x1E`, `GROUP_SEPARATOR=0x1D`).

**Design doc:** `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md`
**Protocol contract:** `.docs/protocol.md`
**Phase context:** This is Phase 1 of 5. Phases 2–5 are out of scope for this plan and get their own plans when work starts on them.

---

## File Structure

| Path | Purpose | Action |
|---|---|---|
| `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp` | Enum + constants + builder/parser declarations | Modify |
| `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp` | Builder + parser implementations, msgTypeMap | Modify |
| `lib/AstrOsQueueMessages/include/AstrOsInterfaceResponseMsg.hpp` | `AstrOsInterfaceResponseType` enum (4 new values) | Modify |
| `lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp` | `mapResponseType` master table + `decodeSerialMessage` dispatch | Modify |
| `test/test_native/astros_serial_messages_tests.cpp` | Native tests for builders + parsers | Modify |
| `test/test_native/astros_serial_protocol_tests.cpp` | Native tests for `mapResponseType` + `decodeSerialMessage` FW_* paths | Modify |

No new files created in Phase 1. All work extends existing PURE libs and tests.

## Background notes for the implementer

- **Enum gap is intentional.** `AstrOsSerialMessageType` currently runs sequentially 0..22 (SERVO_TEST_ACK). The cross-repo protocol reserves 23–29 for in-flight non-OTA additions. The new entries get explicit `= 30` through `= 40` to leave that gap. Don't renumber existing entries.
- **Hex strings are lowercase.** Per `.docs/protocol.md` § "Shared values", SHA-256 hex is 64 lowercase chars; CRC-16 hex is 4 lowercase chars. Tests use lowercase.
- **Base64 alphabet is safe.** Base64 doesn't include 0x1F (`UNIT_SEPARATOR`), so `splitString(payload, UNIT_SEPARATOR)` works correctly even on FW_CHUNK whose payload field is base64. Plus and slash are valid base64 chars and irrelevant here.
- **`getPollAck` style — pop trailing separator vs not.** `getRegistrationSyncAck` and `getDeployScript` append a trailing `RECORD_SEPARATOR` per item, then `pop_back()` after the loop. `getFwDeployDone` follows the same pattern (per-result records). The flat-fields builders (`getPollAck`, `getServoTest`) don't have a trailing separator. Follow whichever pattern matches the message shape.
- **Reason codes are SCREAMING_SNAKE strings.** `FW_CHUNK_NAK reason-code` per protocol: `CRC|SIZE|OUT_OF_ORDER|FLASH_FULL`. Pass through as plain string literals — don't introduce an enum for the wire form yet (`AstrOsBulkTransport` in Phase 2 will own that enum internally).
- **Status codes are snake_case.** `FW_TRANSFER_BEGIN_ACK` status: `OK | sd_full | busy | unsupported_version | io_error`. End-ACK status: `OK | HASH_MISMATCH | IO_ERROR` (these three are SCREAMING per protocol — different convention from BEGIN, intentional).
- **`generateHeader` requires the type be in `msgTypeMap`.** Adding `msgTypeMap` entries (Task 1) is what enables all subsequent builders to produce a valid header. `validateSerialMsg` also checks `msgTypeMap` membership.

---

### Task 1: Add `FW_*` enum values and string constants

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

- [x] **Step 1: Write a failing test that asserts `validateSerialMsg` accepts a hand-crafted FW_TRANSFER_BEGIN header.**

Append to `test/test_native/astros_serial_messages_tests.cpp` at the end of the file:

```cpp
//=================================================================================================
// FW_* recognition (Phase 1 wire format)
//=================================================================================================

namespace
{
    // Hand-craft a serial message that has only a header (no payload) for
    // a given FW_* type, using the same shape generateHeader produces.
    // We can't call the real generateHeader yet for FW_* types until the
    // msgTypeMap entries land in Task 1, so this helper builds the raw
    // string directly.
    std::string buildBareHeader(int typeInt, const char *validator, const std::string &msgId)
    {
        std::stringstream ss;
        ss << typeInt << RECORD_SEPARATOR << validator << RECORD_SEPARATOR << msgId << GROUP_SEPARATOR;
        return ss.str();
    }
} // namespace

TEST(SerialMessages, FwTransferBeginRecognized)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = buildBareHeader(30, "FW_TRANSFER_BEGIN", "mid-1");

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, validation.type);
    EXPECT_STREQ("mid-1", validation.msgId.c_str());
}

TEST(SerialMessages, FwTypesUseProtocolReservedRange)
{
    EXPECT_EQ(30, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_BEGIN));
    EXPECT_EQ(31, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK));
    EXPECT_EQ(32, static_cast<int>(AstrOsSerialMessageType::FW_CHUNK));
    EXPECT_EQ(33, static_cast<int>(AstrOsSerialMessageType::FW_CHUNK_ACK));
    EXPECT_EQ(34, static_cast<int>(AstrOsSerialMessageType::FW_CHUNK_NAK));
    EXPECT_EQ(35, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_END));
    EXPECT_EQ(36, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_END_ACK));
    EXPECT_EQ(37, static_cast<int>(AstrOsSerialMessageType::FW_DEPLOY_BEGIN));
    EXPECT_EQ(38, static_cast<int>(AstrOsSerialMessageType::FW_PROGRESS));
    EXPECT_EQ(39, static_cast<int>(AstrOsSerialMessageType::FW_DEPLOY_DONE));
    EXPECT_EQ(40, static_cast<int>(AstrOsSerialMessageType::FW_BACKPRESSURE));
}
```

Note: this test file already includes `<sstream>` indirectly via `<AstrOsMessaging.hpp>`'s transitive includes. If compilation fails, add `#include <sstream>` to the top of the test file.

- [x] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `AstrOsSerialMessageType::FW_TRANSFER_BEGIN` not declared.

- [x] **Step 3: Add string constants to `AstrOsSC::` namespace**

In `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`, after `constexpr const static char *SERVO_TEST_ACK = "SERVO_TEST_ACK";` (line 38), append:

```cpp
    constexpr const static char *FW_TRANSFER_BEGIN = "FW_TRANSFER_BEGIN";
    constexpr const static char *FW_TRANSFER_BEGIN_ACK = "FW_TRANSFER_BEGIN_ACK";
    constexpr const static char *FW_CHUNK = "FW_CHUNK";
    constexpr const static char *FW_CHUNK_ACK = "FW_CHUNK_ACK";
    constexpr const static char *FW_CHUNK_NAK = "FW_CHUNK_NAK";
    constexpr const static char *FW_TRANSFER_END = "FW_TRANSFER_END";
    constexpr const static char *FW_TRANSFER_END_ACK = "FW_TRANSFER_END_ACK";
    constexpr const static char *FW_DEPLOY_BEGIN = "FW_DEPLOY_BEGIN";
    constexpr const static char *FW_PROGRESS = "FW_PROGRESS";
    constexpr const static char *FW_DEPLOY_DONE = "FW_DEPLOY_DONE";
    constexpr const static char *FW_BACKPRESSURE = "FW_BACKPRESSURE";
```

- [x] **Step 4: Add enum values with explicit numeric assignments**

In the same file, replace the existing `AstrOsSerialMessageType` enum block (lines 41–66 currently) with this version. The existing entries stay in their current order with no explicit numbers (so the compiler still gives them 0..22); the new entries get explicit `= 30` through `= 40` to skip the 23–29 reservation gap.

```cpp
enum class AstrOsSerialMessageType
{
    UNKNOWN,
    REGISTRATION_SYNC, // from web server
    REGISTRATION_SYNC_ACK,
    POLL_ACK,
    POLL_NAK,
    DEPLOY_CONFIG, // from web server
    DEPLOY_CONFIG_ACK,
    DEPLOY_CONFIG_NAK,
    DEPLOY_SCRIPT, // from web server
    DEPLOY_SCRIPT_ACK,
    DEPLOY_SCRIPT_NAK,
    RUN_SCRIPT, // from web server
    RUN_SCRIPT_ACK,
    RUN_SCRIPT_NAK,
    PANIC_STOP,  // from web server
    RUN_COMMAND, // from web server
    RUN_COMMAND_ACK,
    RUN_COMMAND_NAK,
    FORMAT_SD, // from web server
    FORMAT_SD_ACK,
    FORMAT_SD_NAK,
    SERVO_TEST,
    SERVO_TEST_ACK,
    // Values 23–29 are reserved for in-flight non-OTA additions per
    // .docs/protocol.md. FW_* OTA types start at 30.
    FW_TRANSFER_BEGIN = 30,
    FW_TRANSFER_BEGIN_ACK = 31,
    FW_CHUNK = 32,
    FW_CHUNK_ACK = 33,
    FW_CHUNK_NAK = 34,
    FW_TRANSFER_END = 35,
    FW_TRANSFER_END_ACK = 36,
    FW_DEPLOY_BEGIN = 37,
    FW_PROGRESS = 38,
    FW_DEPLOY_DONE = 39,
    FW_BACKPRESSURE = 40,
};
```

- [x] **Step 5: Register all FW_* types in `msgTypeMap`**

In `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`, append to the initializer list inside the constructor (after the `{SERVO_TEST_ACK, …}` entry, line 34). Keep the trailing comma already on that line and add:

```cpp
        {AstrOsSerialMessageType::FW_TRANSFER_BEGIN, AstrOsSC::FW_TRANSFER_BEGIN},
        {AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK, AstrOsSC::FW_TRANSFER_BEGIN_ACK},
        {AstrOsSerialMessageType::FW_CHUNK, AstrOsSC::FW_CHUNK},
        {AstrOsSerialMessageType::FW_CHUNK_ACK, AstrOsSC::FW_CHUNK_ACK},
        {AstrOsSerialMessageType::FW_CHUNK_NAK, AstrOsSC::FW_CHUNK_NAK},
        {AstrOsSerialMessageType::FW_TRANSFER_END, AstrOsSC::FW_TRANSFER_END},
        {AstrOsSerialMessageType::FW_TRANSFER_END_ACK, AstrOsSC::FW_TRANSFER_END_ACK},
        {AstrOsSerialMessageType::FW_DEPLOY_BEGIN, AstrOsSC::FW_DEPLOY_BEGIN},
        {AstrOsSerialMessageType::FW_PROGRESS, AstrOsSC::FW_PROGRESS},
        {AstrOsSerialMessageType::FW_DEPLOY_DONE, AstrOsSC::FW_DEPLOY_DONE},
        {AstrOsSerialMessageType::FW_BACKPRESSURE, AstrOsSC::FW_BACKPRESSURE},
```

- [x] **Step 6: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS — including the two new tests `FwTransferBeginRecognized` and `FwTypesUseProtocolReservedRange`. All existing tests continue to pass.

- [x] **Step 7: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): add FW_* serial message type enum values

Adds FW_TRANSFER_BEGIN..FW_BACKPRESSURE (values 30-40) per the cross-repo
OTA wire-format contract in .docs/protocol.md. Registers each in
msgTypeMap so validateSerialMsg accepts properly-headed FW_* messages.
No builders or parsers yet — those land in subsequent tasks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [x] **Step 8: Update plan checkbox**

In `.docs/plans/20260514-1948-firmware-ota-phase1-wire-format.md`, change `### Task 1: Add FW_* enum values and string constants` heading-line marker (or the "tasks completed" tracker if added later) to indicate this task is done. (Optional — if you add a top-of-file checklist later, check the box. Otherwise, the commit message is the artifact of record.)

---

### Task 2: `getFwTransferBeginAck` builder

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape per `.docs/protocol.md`: `transfer-id<US>status`.

- [x] **Step 1: Write the failing test**

Append to `test/test_native/astros_serial_messages_tests.cpp`:

```cpp
TEST(SerialMessages, FwTransferBeginAckOkMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwTransferBeginAck("mid-2", "7", "OK");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK, validation.type);
    EXPECT_STREQ("mid-2", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(2u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("OK", payloadParts[1]);
}

TEST(SerialMessages, FwTransferBeginAckSdFullMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwTransferBeginAck("mid-3", "7", "sd_full");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(2u, payloadParts.size());
    EXPECT_EQ("sd_full", payloadParts[1]);
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `getFwTransferBeginAck` not declared.

- [x] **Step 3: Declare the builder in the header**

In `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`, inside the `class AstrOsSerialMessageService` public section, after `getBasicAckNak(...)` declaration (around line 101–102), append:

```cpp
    // FW_* outbound builders (master → server)
    std::string getFwTransferBeginAck(std::string msgId, std::string transferId, std::string status);
```

- [x] **Step 4: Implement the builder**

In `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`, after `getBasicAckNak` (around line 162), append:

```cpp
/// @brief generates FW_TRANSFER_BEGIN_ACK reply. Payload shape per .docs/protocol.md:
///        transfer-id<US>status where status is "OK" or a snake_case rejection code
///        (sd_full, busy, unsupported_version, io_error).
/// @param msgId echo of the BEGIN's msgId
/// @param transferId transfer id assigned by the server (server's choice; opaque to us)
/// @param status "OK" on success, otherwise a snake_case rejection code
/// @return serial message
std::string AstrOsSerialMessageService::getFwTransferBeginAck(std::string msgId, std::string transferId,
                                                               std::string status)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK, msgId);
    ss << transferId << UNIT_SEPARATOR << status;
    return ss.str();
}
```

- [x] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): getFwTransferBeginAck builder

Payload: transfer-id<US>status. Status is "OK" or a snake_case rejection
code per .docs/protocol.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `getFwChunkAck` builder

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape: `transfer-id<US>highest-contiguous-seq<US>next-expected-seq<US>window-remaining`.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, FwChunkAckMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkAck("7", 41, 42, 14);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_CHUNK_ACK, validation.type);
    // FW_CHUNK_ACK is a server-bound unsolicited per-chunk reply; msgId is "na"
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("41", payloadParts[1]);
    EXPECT_EQ("42", payloadParts[2]);
    EXPECT_EQ("14", payloadParts[3]);
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `getFwChunkAck` not declared.

- [x] **Step 3: Declare and implement**

Header — append after `getFwTransferBeginAck`:

```cpp
    std::string getFwChunkAck(std::string transferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                              uint8_t windowRemaining);
```

Source — append after `getFwTransferBeginAck`:

```cpp
/// @brief generates FW_CHUNK_ACK reply. Payload shape:
///        transfer-id<US>highest-contiguous-seq<US>next-expected-seq<US>window-remaining.
/// @param transferId transfer id
/// @param highestContiguousSeq highest seq we've committed in order
/// @param nextExpectedSeq the seq we want next (always highestContiguousSeq + 1 in our impl)
/// @param windowRemaining how many more in-flight frames the sender may have
/// @return serial message
std::string AstrOsSerialMessageService::getFwChunkAck(std::string transferId, uint32_t highestContiguousSeq,
                                                       uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_CHUNK_ACK, "na");
    ss << transferId << UNIT_SEPARATOR << std::to_string(highestContiguousSeq) << UNIT_SEPARATOR
       << std::to_string(nextExpectedSeq) << UNIT_SEPARATOR << std::to_string(static_cast<unsigned>(windowRemaining));
    return ss.str();
}
```

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): getFwChunkAck builder

Payload: transfer-id<US>highest-contiguous-seq<US>next-expected-seq<US>
window-remaining. Sent per-chunk, msgId="na".

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: `getFwChunkNak` builder

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape: `transfer-id<US>last-good-seq<US>reason-code` where reason-code is `CRC | SIZE | OUT_OF_ORDER | FLASH_FULL`.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, FwChunkNakCrcMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 40, "CRC");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_CHUNK_NAK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("40", payloadParts[1]);
    EXPECT_EQ("CRC", payloadParts[2]);
}

TEST(SerialMessages, FwChunkNakOutOfOrderMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 40, "OUT_OF_ORDER");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("OUT_OF_ORDER", payloadParts[2]);
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `getFwChunkNak` not declared.

- [x] **Step 3: Declare and implement**

Header — append:

```cpp
    std::string getFwChunkNak(std::string transferId, uint32_t lastGoodSeq, std::string reasonCode);
```

Source — append:

```cpp
/// @brief generates FW_CHUNK_NAK reply. Payload shape:
///        transfer-id<US>last-good-seq<US>reason-code.
/// @param transferId transfer id
/// @param lastGoodSeq the last seq we committed (server resumes from N+1)
/// @param reasonCode "CRC" | "SIZE" | "OUT_OF_ORDER" | "FLASH_FULL"
/// @return serial message
std::string AstrOsSerialMessageService::getFwChunkNak(std::string transferId, uint32_t lastGoodSeq,
                                                       std::string reasonCode)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_CHUNK_NAK, "na");
    ss << transferId << UNIT_SEPARATOR << std::to_string(lastGoodSeq) << UNIT_SEPARATOR << reasonCode;
    return ss.str();
}
```

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): getFwChunkNak builder

Payload: transfer-id<US>last-good-seq<US>reason-code where reason-code is
one of CRC | SIZE | OUT_OF_ORDER | FLASH_FULL.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: `getFwTransferEndAck` builder

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape: `transfer-id<US>status<US>computed-sha256-hex` where status is `OK | HASH_MISMATCH | IO_ERROR`.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, FwTransferEndAckOkMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto computedHex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    auto value = msgSvc.getFwTransferEndAck("mid-9", "7", "OK", computedHex);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_TRANSFER_END_ACK, validation.type);
    EXPECT_STREQ("mid-9", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("OK", payloadParts[1]);
    EXPECT_EQ(computedHex, payloadParts[2]);
}

TEST(SerialMessages, FwTransferEndAckHashMismatchMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto computedHex = "0000000000000000000000000000000000000000000000000000000000000001";
    auto value = msgSvc.getFwTransferEndAck("mid-9", "7", "HASH_MISMATCH", computedHex);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("HASH_MISMATCH", payloadParts[1]);
    EXPECT_EQ(computedHex, payloadParts[2]);
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `getFwTransferEndAck` not declared.

- [x] **Step 3: Declare and implement**

Header — append:

```cpp
    std::string getFwTransferEndAck(std::string msgId, std::string transferId, std::string status,
                                    std::string computedSha256Hex);
```

Source — append:

```cpp
/// @brief generates FW_TRANSFER_END_ACK reply. Payload shape:
///        transfer-id<US>status<US>computed-sha256-hex where status is
///        OK | HASH_MISMATCH | IO_ERROR (SCREAMING per .docs/protocol.md).
/// @param msgId echo of the END's msgId
/// @param transferId transfer id
/// @param status OK | HASH_MISMATCH | IO_ERROR
/// @param computedSha256Hex 64 lowercase hex chars of master's computed hash
/// @return serial message
std::string AstrOsSerialMessageService::getFwTransferEndAck(std::string msgId, std::string transferId,
                                                              std::string status, std::string computedSha256Hex)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_TRANSFER_END_ACK, msgId);
    ss << transferId << UNIT_SEPARATOR << status << UNIT_SEPARATOR << computedSha256Hex;
    return ss.str();
}
```

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): getFwTransferEndAck builder

Payload: transfer-id<US>status<US>computed-sha256-hex where status is
OK | HASH_MISMATCH | IO_ERROR.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: `getFwDeployDone` builder

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape per `.docs/protocol.md`: `transfer-id<US>per-controller-result-list` where each result is `controllerId<US>OK|FAILED<US>finalVersion<US>errorOrEmpty`, **RS-separated between results**. Note: this means the transfer-id sits at the start of the first record (the first 4-field block is preceded by transfer-id<US>). Read the protocol grammar carefully:

```
FW_DEPLOY_DONE:  transfer-id<US>per-controller-result-list
result-list = result[RS]result[RS]...
result      = controllerId<US>OK|FAILED<US>finalVersion<US>errorOrEmpty
```

So the wire payload becomes: `7<US>core<US>FAILED<US><US>not_implemented<RS>dome<US>FAILED<US><US>not_implemented`. The transfer-id is prepended once, then the result records are RS-joined.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, FwDeployDoneAllFailedMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::vector<astros_fw_deploy_result_t> results;
    results.push_back({"core", "FAILED", "", "not_implemented"});
    results.push_back({"dome", "FAILED", "", "not_implemented"});

    auto value = msgSvc.getFwDeployDone("mid-d", "7", results);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_DEPLOY_DONE, validation.type);
    EXPECT_STREQ("mid-d", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto resultRecords = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);
    ASSERT_EQ(2u, resultRecords.size());

    // First record starts with transfer-id, then the first result's 4 fields:
    auto firstParts = AstrOsStringUtils::splitString(resultRecords[0], UNIT_SEPARATOR);
    ASSERT_EQ(5u, firstParts.size());
    EXPECT_EQ("7", firstParts[0]);
    EXPECT_EQ("core", firstParts[1]);
    EXPECT_EQ("FAILED", firstParts[2]);
    EXPECT_EQ("", firstParts[3]);
    EXPECT_EQ("not_implemented", firstParts[4]);

    auto secondParts = AstrOsStringUtils::splitString(resultRecords[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, secondParts.size());
    EXPECT_EQ("dome", secondParts[0]);
    EXPECT_EQ("FAILED", secondParts[1]);
    EXPECT_EQ("", secondParts[2]);
    EXPECT_EQ("not_implemented", secondParts[3]);
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `astros_fw_deploy_result_t` not declared.

- [x] **Step 3: Declare the result struct and the builder**

Header — append after the existing `astros_serial_msg_validation_t` struct (around line 83):

```cpp
typedef struct
{
    std::string controllerId;
    std::string status;       // "OK" or "FAILED"
    std::string finalVersion; // may be empty
    std::string errorOrEmpty; // may be empty
} astros_fw_deploy_result_t;
```

Header — append in the public class section after `getFwTransferEndAck`:

```cpp
    std::string getFwDeployDone(std::string msgId, std::string transferId,
                                std::vector<astros_fw_deploy_result_t> results);
```

- [x] **Step 4: Implement the builder**

Source — append:

```cpp
/// @brief generates FW_DEPLOY_DONE. Payload shape per .docs/protocol.md:
///        transfer-id<US>per-controller-result-list, RS-separated results
///        of controllerId<US>OK|FAILED<US>finalVersion<US>errorOrEmpty.
///        Transfer-id is prepended once; it precedes the first result inline,
///        which therefore has 5 US-separated fields on the wire while subsequent
///        results have 4.
/// @param msgId echo of the originating msgId (the DEPLOY_BEGIN that triggered this)
/// @param transferId transfer id
/// @param results per-target result records
/// @return serial message
std::string AstrOsSerialMessageService::getFwDeployDone(std::string msgId, std::string transferId,
                                                          std::vector<astros_fw_deploy_result_t> results)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_DEPLOY_DONE, msgId);
    ss << transferId;
    for (const auto &r : results)
    {
        ss << UNIT_SEPARATOR << r.controllerId << UNIT_SEPARATOR << r.status << UNIT_SEPARATOR << r.finalVersion
           << UNIT_SEPARATOR << r.errorOrEmpty << RECORD_SEPARATOR;
    }
    std::string message = ss.str();
    if (!results.empty())
    {
        message.pop_back(); // strip the trailing RECORD_SEPARATOR
    }
    return message;
}
```

- [x] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): getFwDeployDone builder + result struct

Payload: transfer-id<US>(controllerId<US>status<US>version<US>error)
RS-joined. Transfer-id prepends the first result inline. Used by master
to report per-target deploy outcomes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: `FwTransferBeginRecord` + `parseFwTransferBegin`

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape per `.docs/protocol.md`: `transfer-id<US>total-size<US>sha256-hex<US>chunk-size<US>target-list` where `target-list = controllerId[RS]controllerId[RS]…`. Note the RS *inside* the payload — that means the payload spans more than one "record" by the validateSerialMsg view. We parse it as one logical payload: split on UNIT_SEPARATOR to get the first 5 fields, where the 5th field itself is the RS-joined target list.

Reality check: `validateSerialMsg` puts everything after `GROUP_SEPARATOR` into `result.payload` as a single string. RS inside that string is fine — we control how we parse.

The first 4 US-separated fields are simple. The 5th US-separated field is the RS-joined target list, which we then RS-split.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, ParseFwTransferBeginHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "1234567" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR
            << "4096" << UNIT_SEPARATOR
            << "core" << RECORD_SEPARATOR << "dome" << RECORD_SEPARATOR << "master";

    auto rec = parseFwTransferBegin(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    EXPECT_EQ(1234567u, rec.totalSize);
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", rec.sha256Hex);
    EXPECT_EQ(4096u, rec.chunkSize);
    ASSERT_EQ(3u, rec.targetIds.size());
    EXPECT_EQ("core", rec.targetIds[0]);
    EXPECT_EQ("dome", rec.targetIds[1]);
    EXPECT_EQ("master", rec.targetIds[2]);
}

TEST(SerialMessages, ParseFwTransferBeginTooFewFields)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "1234567"; // only 2 fields
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginNonNumericSize)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "abc" << UNIT_SEPARATOR
            << "deadbeef" << UNIT_SEPARATOR
            << "4096" << UNIT_SEPARATOR
            << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginEmptyTargetList)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "100" << UNIT_SEPARATOR
            << "deadbeef" << UNIT_SEPARATOR
            << "4096" << UNIT_SEPARATOR
            << ""; // empty target list
    auto rec = parseFwTransferBegin(payload.str());
    // An empty target-list is malformed (server should always send at least one)
    EXPECT_FALSE(rec.valid);
}
```

- [x] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `parseFwTransferBegin` not declared, `FwTransferBeginRecord` not declared.

- [x] **Step 3: Declare the record struct and the parser**

Header — append after `astros_fw_deploy_result_t`:

```cpp
typedef struct
{
    std::string transferId;
    uint32_t totalSize;
    std::string sha256Hex;
    uint16_t chunkSize;
    std::vector<std::string> targetIds;
    bool valid;
} FwTransferBeginRecord;
```

Header — append as a free function declaration at the bottom of the file, **outside** the class but **inside** the include guard. After the closing `};` of the class (line 115 in current file) add:

```cpp
// Free parsers for inbound FW_* payloads. Live alongside the
// AstrOsSerialMessageService class because they share the wire
// grammar in this file. Pure C++; no allocations beyond the
// returned struct's members.
FwTransferBeginRecord parseFwTransferBegin(const std::string &payload);
```

- [x] **Step 4: Implement the parser**

Source — append at the bottom of `AstrOsSerialMessageService.cpp`:

```cpp
FwTransferBeginRecord parseFwTransferBegin(const std::string &payload)
{
    FwTransferBeginRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 5)
    {
        return rec;
    }

    // size + chunk-size: parse with stoul, catch parse failure via errno.
    // The codebase avoids exceptions on the embedded target (CLAUDE.md), so
    // use strtoul which signals errors via errno + endptr.
    errno = 0;
    char *endptr = nullptr;
    auto totalSize = std::strtoul(parts[1].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[1].c_str() || *endptr != '\0')
    {
        return rec;
    }
    errno = 0;
    endptr = nullptr;
    auto chunkSize = std::strtoul(parts[3].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[3].c_str() || *endptr != '\0' || chunkSize > 0xFFFFu)
    {
        return rec;
    }

    // sha256-hex must be exactly 64 lowercase hex chars
    if (parts[2].size() != 64)
    {
        return rec;
    }

    // target-list: RS-split. Reject if empty (no targets).
    auto targets = AstrOsStringUtils::splitString(parts[4], RECORD_SEPARATOR);
    if (targets.empty() || (targets.size() == 1 && targets[0].empty()))
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.totalSize = static_cast<uint32_t>(totalSize);
    rec.sha256Hex = parts[2];
    rec.chunkSize = static_cast<uint16_t>(chunkSize);
    rec.targetIds = std::move(targets);
    rec.valid = true;
    return rec;
}
```

Add `#include <cerrno>` and `#include <cstdlib>` at the top of the .cpp file if not already present (check existing includes — `<cstring>` is there but `<cstdlib>` may not be).

- [x] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): parseFwTransferBegin + FwTransferBeginRecord

Parses transfer-id<US>total-size<US>sha256-hex<US>chunk-size<US>RS-target-list
into a POD record. Rejects malformed sizes, wrong-length hash, empty target
list. Uses strtoul (not stoi) to avoid exceptions per the embedded
no-exceptions convention in CLAUDE.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: `FwChunkRecord` + `parseFwChunk`

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape: `transfer-id<US>seq<US>payload-len<US>base64-bytes<US>crc16-hex`. Note: the parser does **not** base64-decode — it just extracts fields. Decoding happens in the MIXED handler in Phase 3. CRC-16 hex is exactly 4 lowercase hex chars.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, ParseFwChunkHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "42" << UNIT_SEPARATOR
            << "12" << UNIT_SEPARATOR
            << "SGVsbG8gV29ybGQh" << UNIT_SEPARATOR // base64 of "Hello World!" (12 bytes)
            << "abcd";

    auto rec = parseFwChunk(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    EXPECT_EQ(42u, rec.seq);
    EXPECT_EQ(12u, rec.payloadLen);
    EXPECT_EQ("SGVsbG8gV29ybGQh", rec.base64Payload);
    EXPECT_EQ(0xabcdu, rec.crc16);
}

TEST(SerialMessages, ParseFwChunkTooFewFields)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "12"; // 3 fields
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkBadCrcHex)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "42" << UNIT_SEPARATOR
            << "12" << UNIT_SEPARATOR
            << "SGVsbG8gV29ybGQh" << UNIT_SEPARATOR
            << "xyz1"; // not hex
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkCrcHexWrongLength)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "42" << UNIT_SEPARATOR
            << "12" << UNIT_SEPARATOR
            << "SGVsbG8gV29ybGQh" << UNIT_SEPARATOR
            << "abc"; // 3 chars, not 4
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}
```

- [x] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `parseFwChunk` not declared.

- [x] **Step 3: Declare the record struct and parser**

Header — append after `FwTransferBeginRecord`:

```cpp
typedef struct
{
    std::string transferId;
    uint32_t seq;
    uint16_t payloadLen;
    std::string base64Payload; // not decoded here — Phase 3 MIXED handler decodes
    uint16_t crc16;
    bool valid;
} FwChunkRecord;
```

Header — append after `parseFwTransferBegin` declaration:

```cpp
FwChunkRecord parseFwChunk(const std::string &payload);
```

- [x] **Step 4: Implement**

Source — append:

```cpp
namespace
{
    // Parses exactly 4 lowercase hex chars into a uint16_t.
    // Returns false on length mismatch or non-hex character.
    bool parseHex16(const std::string &hex, uint16_t &out)
    {
        if (hex.size() != 4)
        {
            return false;
        }
        uint16_t v = 0;
        for (char c : hex)
        {
            uint8_t nibble = 0;
            if (c >= '0' && c <= '9')
                nibble = c - '0';
            else if (c >= 'a' && c <= 'f')
                nibble = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
                nibble = 10 + (c - 'A');
            else
                return false;
            v = static_cast<uint16_t>((v << 4) | nibble);
        }
        out = v;
        return true;
    }
} // namespace

FwChunkRecord parseFwChunk(const std::string &payload)
{
    FwChunkRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 5)
    {
        return rec;
    }

    errno = 0;
    char *endptr = nullptr;
    auto seq = std::strtoul(parts[1].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[1].c_str() || *endptr != '\0')
    {
        return rec;
    }
    errno = 0;
    endptr = nullptr;
    auto plen = std::strtoul(parts[2].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[2].c_str() || *endptr != '\0' || plen > 0xFFFFu)
    {
        return rec;
    }

    uint16_t crc = 0;
    if (!parseHex16(parts[4], crc))
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.seq = static_cast<uint32_t>(seq);
    rec.payloadLen = static_cast<uint16_t>(plen);
    rec.base64Payload = parts[3];
    rec.crc16 = crc;
    rec.valid = true;
    return rec;
}
```

- [x] **Step 5: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): parseFwChunk + FwChunkRecord + hex16 helper

Parses transfer-id<US>seq<US>payload-len<US>base64-payload<US>crc16-hex.
Does not base64-decode (that's the MIXED handler's job in Phase 3).
Adds a small parseHex16 helper for the 4-char CRC field.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: `FwTransferEndRecord` + `parseFwTransferEnd`

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape: `transfer-id<US>total-chunks<US>final-sha256-hex`.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, ParseFwTransferEndHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "9400" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    auto rec = parseFwTransferEnd(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    EXPECT_EQ(9400u, rec.totalChunks);
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", rec.finalSha256Hex);
}

TEST(SerialMessages, ParseFwTransferEndWrongHashLength)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "9400" << UNIT_SEPARATOR
            << "deadbeef"; // 8 chars, not 64
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndTooFewFields)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400";
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}
```

- [x] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL — `parseFwTransferEnd` not declared.

- [x] **Step 3: Declare and implement**

Header — append after `FwChunkRecord`:

```cpp
typedef struct
{
    std::string transferId;
    uint32_t totalChunks;
    std::string finalSha256Hex;
    bool valid;
} FwTransferEndRecord;
```

Header — append after `parseFwChunk` decl:

```cpp
FwTransferEndRecord parseFwTransferEnd(const std::string &payload);
```

Source — append:

```cpp
FwTransferEndRecord parseFwTransferEnd(const std::string &payload)
{
    FwTransferEndRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 3)
    {
        return rec;
    }

    errno = 0;
    char *endptr = nullptr;
    auto totalChunks = std::strtoul(parts[1].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[1].c_str() || *endptr != '\0')
    {
        return rec;
    }

    if (parts[2].size() != 64)
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.totalChunks = static_cast<uint32_t>(totalChunks);
    rec.finalSha256Hex = parts[2];
    rec.valid = true;
    return rec;
}
```

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): parseFwTransferEnd + FwTransferEndRecord

Parses transfer-id<US>total-chunks<US>final-sha256-hex. Validates 64-char
hash length and numeric total-chunks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: `FwDeployBeginRecord` + `parseFwDeployBegin`

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Test: `test/test_native/astros_serial_messages_tests.cpp`

Payload shape: `transfer-id<US>order-list` where `order-list = controllerId[RS]controllerId[RS]…`.

- [x] **Step 1: Write the failing test**

```cpp
TEST(SerialMessages, ParseFwDeployBeginHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "core" << RECORD_SEPARATOR << "dome" << RECORD_SEPARATOR << "master";

    auto rec = parseFwDeployBegin(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    ASSERT_EQ(3u, rec.orderIds.size());
    EXPECT_EQ("core", rec.orderIds[0]);
    EXPECT_EQ("dome", rec.orderIds[1]);
    EXPECT_EQ("master", rec.orderIds[2]);
}

TEST(SerialMessages, ParseFwDeployBeginEmptyOrderList)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "";
    auto rec = parseFwDeployBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwDeployBeginTooFewFields)
{
    auto rec = parseFwDeployBegin("7"); // no separator, no order list
    EXPECT_FALSE(rec.valid);
}
```

- [x] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: FAIL.

- [x] **Step 3: Declare and implement**

Header — append after `FwTransferEndRecord`:

```cpp
typedef struct
{
    std::string transferId;
    std::vector<std::string> orderIds;
    bool valid;
} FwDeployBeginRecord;
```

Header — append decl:

```cpp
FwDeployBeginRecord parseFwDeployBegin(const std::string &payload);
```

Source — append:

```cpp
FwDeployBeginRecord parseFwDeployBegin(const std::string &payload)
{
    FwDeployBeginRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 2)
    {
        return rec;
    }

    auto orderIds = AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);
    if (orderIds.empty() || (orderIds.size() == 1 && orderIds[0].empty()))
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.orderIds = std::move(orderIds);
    rec.valid = true;
    return rec;
}
```

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(messaging): parseFwDeployBegin + FwDeployBeginRecord

Parses transfer-id<US>order-list where order-list is RS-joined controller
ids. Empty order-list is rejected.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: Add FW_* values to `AstrOsInterfaceResponseType`

**Files:**
- Modify: `lib/AstrOsQueueMessages/include/AstrOsInterfaceResponseMsg.hpp`

Four new values for the four inbound FW_* types master receives. No test for this task — the enum is consumed by `mapResponseType` (Task 12) and `decodeSerialMessage` (Task 13), whose tests will fail if any of these are missing.

- [x] **Step 1: Add the enum values**

In `lib/AstrOsQueueMessages/include/AstrOsInterfaceResponseMsg.hpp`, append before the closing `};` of the enum (after `SEND_SERVO_TEST_ACK`):

```cpp
    ,
    FW_TRANSFER_BEGIN,
    FW_CHUNK,
    FW_TRANSFER_END,
    FW_DEPLOY_BEGIN
```

Note the leading comma — the existing last entry `SEND_SERVO_TEST_ACK` does not have a trailing comma. Adding the comma is necessary; reorder so the leading comma sits on its own to keep the diff clean. The final file region should read:

```cpp
    SERVO_TEST,
    SEND_SERVO_TEST,
    SERVO_TEST_ACK,
    SEND_SERVO_TEST_ACK,
    FW_TRANSFER_BEGIN,
    FW_CHUNK,
    FW_TRANSFER_END,
    FW_DEPLOY_BEGIN
};
```

(That is: add the trailing comma to `SEND_SERVO_TEST_ACK`, then list the four new entries.)

- [x] **Step 2: Build to confirm compilation**

Run: `pio test -e test --filter "*serial_messages*"`
Expected: PASS — no test added but the enum addition must not break the existing test binary.

Also run a board build to confirm the firmware still compiles:

Run: `pio run -e metro_s3`
Expected: build succeeds.

- [x] **Step 3: Commit**

```bash
git add lib/AstrOsQueueMessages/include/AstrOsInterfaceResponseMsg.hpp
git commit -m "$(cat <<'EOF'
feat(messaging): add FW_* values to AstrOsInterfaceResponseType

Adds FW_TRANSFER_BEGIN, FW_CHUNK, FW_TRANSFER_END, FW_DEPLOY_BEGIN —
the four inbound types master receives. Consumed by mapResponseType
and decodeSerialMessage in the next two tasks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Extend `mapResponseType` master table

**Files:**
- Modify: `lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp`
- Test: `test/test_native/astros_serial_protocol_tests.cpp`

The master-side path of `mapResponseType` lists every server→master type. Padawan path is unchanged — padawans don't receive FW_* over serial.

- [x] **Step 1: Write the failing test**

Append to `test/test_native/astros_serial_protocol_tests.cpp` in the `MapResponseTypeMasterTable` test block, OR add a new dedicated test:

```cpp
TEST(SerialProtocol, MapResponseTypeMasterFwTable)
{
    using namespace AstrOsSerialProtocol;
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_BEGIN,
              mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_CHUNK, mapResponseType(AstrOsSerialMessageType::FW_CHUNK, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_END,
              mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_END, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_DEPLOY_BEGIN,
              mapResponseType(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, true));
}

TEST(SerialProtocol, MapResponseTypePadawanFwAllUnknown)
{
    using namespace AstrOsSerialProtocol;
    // Padawans don't receive FW_* over serial; they get only ESP-NOW.
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN,
              mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsSerialMessageType::FW_CHUNK, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN,
              mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_END, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN,
              mapResponseType(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, false));
}
```

- [x] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*serial_protocol*"`
Expected: FAIL — master cases return UNKNOWN.

- [x] **Step 3: Extend the master-side switch in `mapResponseType`**

In `lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp`, in the `mapResponseType` function's master-side `switch` block (lines 124–144), add four cases before the `default:` line:

```cpp
            case AstrOsSerialMessageType::FW_TRANSFER_BEGIN:
                return AstrOsInterfaceResponseType::FW_TRANSFER_BEGIN;
            case AstrOsSerialMessageType::FW_CHUNK:
                return AstrOsInterfaceResponseType::FW_CHUNK;
            case AstrOsSerialMessageType::FW_TRANSFER_END:
                return AstrOsInterfaceResponseType::FW_TRANSFER_END;
            case AstrOsSerialMessageType::FW_DEPLOY_BEGIN:
                return AstrOsInterfaceResponseType::FW_DEPLOY_BEGIN;
```

Do not modify the padawan-side switch — it correctly returns UNKNOWN for FW_* by default.

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_protocol*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp \
        test/test_native/astros_serial_protocol_tests.cpp
git commit -m "$(cat <<'EOF'
feat(protocol): map FW_* inbound types on master path

mapResponseType returns the matching FW_* InterfaceResponseType for
each of the four inbound server→master FW_* messages. Padawan side
returns UNKNOWN — padawans don't receive FW_* over serial.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Extend `decodeSerialMessage` to dispatch FW_* types

**Files:**
- Modify: `lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp`
- Test: `test/test_native/astros_serial_protocol_tests.cpp`

`decodeSerialMessage` produces a `DecodedCommand` carrying enough for the MIXED handler to route. For FW_* the structured parsing happens later (in Phase 3 OtaReceiver), so this dispatch packs the *raw payload* into `cmd.message` and sets `responseType` from `mapResponseType`. Tests verify single-command output and that the payload survives intact.

- [x] **Step 1: Write the failing test**

Append to `test/test_native/astros_serial_protocol_tests.cpp`:

```cpp
// ---------------- FW_* dispatch ----------------

TEST(SerialProtocol, DecodeFwTransferBeginProducesSingleCommandWithRawPayload)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR
            << "1234567" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR
            << "4096" << UNIT_SEPARATOR
            << "core" << RECORD_SEPARATOR << "dome";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, "mid-1", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());
    const auto &cmd = result.commands[0];
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_BEGIN, cmd.responseType);
    EXPECT_EQ("mid-1", cmd.msgId);
    EXPECT_EQ("", cmd.peerMac);
    EXPECT_EQ("", cmd.peerName);
    EXPECT_EQ(raw, cmd.message); // raw payload passes through unchanged
}

TEST(SerialProtocol, DecodeFwChunkProducesSingleCommand)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "0" << UNIT_SEPARATOR << "4" << UNIT_SEPARATOR << "AQID" << UNIT_SEPARATOR
            << "abcd";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_CHUNK, "mid-c", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_CHUNK, result.commands[0].responseType);
    EXPECT_EQ(raw, result.commands[0].message);
}

TEST(SerialProtocol, DecodeFwTransferEndProducesSingleCommand)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_END, "mid-e", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_END, result.commands[0].responseType);
    EXPECT_EQ(raw, result.commands[0].message);
}

TEST(SerialProtocol, DecodeFwDeployBeginProducesSingleCommand)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << "dome";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, "mid-d", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_DEPLOY_BEGIN, result.commands[0].responseType);
    EXPECT_EQ(raw, result.commands[0].message);
}

TEST(SerialProtocol, DecodeFwTransferBeginEmptyPayloadRejects)
{
    using namespace AstrOsSerialProtocol;
    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, "mid-1", "");
    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(DecodeRejectReason::EMPTY_PAYLOAD, result.rejects[0].reason);
}
```

- [x] **Step 2: Run tests to verify they fail**

Run: `pio test -e test --filter "*serial_protocol*"`
Expected: FAIL — `decodeSerialMessage` falls through to the default case (UNKNOWN_TYPE reject) for FW_* inputs.

- [x] **Step 3: Add a dispatcher for FW_* in `decodeSerialMessage`**

In `lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp`, inside the anonymous namespace at the top, after `decodeBasicCommand`, add:

```cpp
        void decodeFwInbound(DecodeResult &result, AstrOsSerialMessageType type, const std::string &msgId,
                             const std::string &payload)
        {
            // FW_* inbound payloads are not parsed here — the MIXED OtaReceiver
            // owns structured parsing via parseFwTransferBegin / parseFwChunk /
            // parseFwTransferEnd / parseFwDeployBegin. We just route the raw
            // payload through with the matching responseType so the handler
            // task can hand it to OtaReceiver.
            const auto responseType = mapResponseType(type, /*isMaster=*/true);
            appendCommand(result, responseType, msgId, "", "", payload);
        }
```

In `decodeSerialMessage`, add four cases to the `switch (type)` block before `default:`:

```cpp
        case AstrOsSerialMessageType::FW_TRANSFER_BEGIN:
        case AstrOsSerialMessageType::FW_CHUNK:
        case AstrOsSerialMessageType::FW_TRANSFER_END:
        case AstrOsSerialMessageType::FW_DEPLOY_BEGIN:
            decodeFwInbound(result, type, msgId, payload);
            break;
```

The empty-payload guard at the top of `decodeSerialMessage` already covers FW_* — it appends an `EMPTY_PAYLOAD` reject for any non-REGISTRATION_SYNC type with empty payload. No change needed there.

- [x] **Step 4: Run tests to verify they pass**

Run: `pio test -e test --filter "*serial_protocol*"`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add lib_native/AstrOsSerialProtocol/src/AstrOsSerialProtocol.cpp \
        test/test_native/astros_serial_protocol_tests.cpp
git commit -m "$(cat <<'EOF'
feat(protocol): dispatch FW_* inbound types as single-command results

decodeSerialMessage produces a single DecodedCommand carrying the raw
payload as cmd.message for FW_TRANSFER_BEGIN / FW_CHUNK /
FW_TRANSFER_END / FW_DEPLOY_BEGIN. Structured parsing is the MIXED
OtaReceiver's job (Phase 3); this layer only routes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 14: Final verification + open PR

**Files:** None modified — verification only.

- [x] **Step 1: Run the full native test suite**

Run: `pio test -e test`
Expected: 100% pass. Note the new test count (was 190 before; should be ~220+ after Phase 1).

- [x] **Step 2: Build both board firmware variants**

Run in parallel (separate terminals or sequentially):

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both succeed with no new warnings related to the FW_* changes.

- [x] **Step 3: Verify clang-format clean on changed files**

The pre-commit hook should have caught this if active. To double-check:

```bash
find lib_native/AstrOsMessaging lib_native/AstrOsSerialProtocol lib/AstrOsQueueMessages -name '*.hpp' -o -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
```

Expected: no output (clang-format clean).

- [x] **Step 4: Push branch + open PR**

The branch is `feature/ota-master-serial-receive` (already cut when the design doc was committed). Push and open the PR.

```bash
git push -u origin feature/ota-master-serial-receive
gh pr create --base develop --title "Firmware OTA Phase 1 — wire format" --body "$(cat <<'EOF'
## Summary

Phase 1 of the ESP-side OTA firmware work. Adds the FW_* serial message
types (BEGIN/CHUNK/END family + DEPLOY/PROGRESS/DONE/BACKPRESSURE) to
the native-testable messaging libs. No firmware behavior change yet —
wire format only.

Design: `.docs/plans/20260514-1941-firmware-ota-esp-master-serial-receive-design.md`
Plan: `.docs/plans/20260514-1948-firmware-ota-phase1-wire-format.md`
Protocol: `.docs/protocol.md`

Adds:
- 11 new `AstrOsSerialMessageType` enum values (30-40), 11 string constants
- 5 builders: `getFwTransferBeginAck`, `getFwChunkAck`, `getFwChunkNak`,
  `getFwTransferEndAck`, `getFwDeployDone`
- 4 POD record structs + 4 parser free-functions for the inbound types
- 4 new `AstrOsInterfaceResponseType` values
- `mapResponseType` master-table entries for FW_* inbound types
- `decodeSerialMessage` dispatch for FW_* (raw payload pass-through to MIXED)
- Native tests for every builder, every parser (happy + reject paths), and
  every new mapping/dispatch entry

## Test plan

- [x] `pio test -e test` passes 100%
- [x] `pio run -e metro_s3` builds clean
- [x] `pio run -e lolin_d32_pro` builds clean
- [x] CI native-purity guard passes (no new ESP-IDF includes in PURE libs)
- [x] clang-format clean on changed files

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Expected: PR is created and CI runs. Watch:
- native unit tests
- both-board build matrix
- AstrOsMessaging native-purity guard
- clang-format on changed files

All four must pass before merge.

- [x] **Step 5: Update plan progress at top of this file**

(Optional, if a top-level checklist was added.) Add a `## Status` section noting Phase 1 PR opened.

---

## Self-review notes (already applied to this plan)

- **Spec coverage**: every Phase 1 deliverable in the design doc is covered:
  - Enum + constants + map (Task 1) ✓
  - Five builders (Tasks 2-6) ✓
  - Four parsers + record structs (Tasks 7-10) ✓
  - InterfaceResponseType extension (Task 11) ✓
  - `mapResponseType` extension (Task 12) ✓
  - `decodeSerialMessage` dispatch (Task 13) ✓
  - Final verification (Task 14) ✓
- **Placeholder scan**: no TBDs, no "add appropriate error handling", every step contains executable code or a concrete command.
- **Type consistency**: `FwTransferBeginRecord` / `FwChunkRecord` / `FwTransferEndRecord` / `FwDeployBeginRecord` names are used consistently. `astros_fw_deploy_result_t` follows the existing `astros_*_t` POD-typedef convention. `parseFwTransferBegin` / `parseFwChunk` / `parseFwTransferEnd` / `parseFwDeployBegin` are the four parser names, matching their record types.
- **Scope check**: 14 tasks is at the upper edge of the "8-task warning" in CLAUDE.md but each task is small (≤3 file changes, ≤30 min). Splitting Phase 1 further would mean splitting a single builder across PRs which adds ceremony without value. The remaining four phases are separately planned.
