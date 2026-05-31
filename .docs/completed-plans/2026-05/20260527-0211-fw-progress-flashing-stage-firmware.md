# Firmware FW_PROGRESS Emission + OTA_FLASH_RESULT + M4 Placeholder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the firmware-side machinery so an M4 deploy renders honestly in the server UI: master emits `FW_PROGRESS` for SENDING / VERIFYING / FLASHING transitions, and the padawan reports its flash-commit outcome via a new `OTA_FLASH_RESULT` message (carrying `FLASH_NOT_IMPLEMENTED` as the M4 placeholder, after a 2 s delay to make the FLASHING stage visibly current in the UI).

**Architecture:** Two new wire pieces — `FW_PROGRESS` serial builder (M5 work pulled forward) and a new ESP-NOW `OTA_FLASH_RESULT` packet (padawan → master). New `AWAITING_FLASH_RESULT` phase in `OtaForwarder` between END_ACK arrival and `completeCurrentPadawan`, with a 10 s safety timeout. `OtaWriter::handleEnd` sends `OTA_END_ACK OK` immediately on verification success, then `vTaskDelay(2 s)`, then `OTA_FLASH_RESULT(FLASH_NOT_IMPLEMENTED)`. Padawan-side `vTaskDelay` is acceptable because the writer task has no pipelined work during the post-verify window.

**Tech Stack:** C++17, ESP-IDF (esp_ota_ops, esp_timer, FreeRTOS), PlatformIO. Native tests via [env:test] (gnu++2a + googletest) for the PURE pieces (`OtaWirePayloads`, `AstrOsSerialMessageService`); bench validation for the MIXED pieces (`OtaForwarder`, `OtaWriter`).

**Parent design:** [`.docs/plans/20260527-0143-firmware-ota-progress-emission-design.md`](./20260527-0143-firmware-ota-progress-emission-design.md)

**Sister-repo plan (ships first):** [`AstrOs.Server/.docs/plans/20260527-0210-fw-progress-flashing-stage-server.md`](../../../AstrOs.Server/.docs/plans/20260527-0210-fw-progress-flashing-stage-server.md)

**Branch:** This plan continues on `feature/ota-fw-progress-emission` (already created and holding the design doc commits `f02201d`, `f8200af`).

---

## Migration order — read first

This plan ships **second**. The sister AstrOs.Server PR must merge first so the server has `FwStage.Flashing` in its enum + state machine. If this firmware ships first, the server will log `FW_PROGRESS has unknown stage: FLASHING` and drop those messages — no crash, but the UI gap remains.

Verify before opening this PR: `gh -R battlesloth/AstrOs.Server pr view <server-pr-number> --json state` returns `MERGED`.

## Context for the engineer

Read these first — load-bearing, will not be re-explained per task:

- **Design doc**: `.docs/plans/20260527-0143-firmware-ota-progress-emission-design.md`. The "Architecture overview" ASCII diagram is the canonical sequence of wire events; the "Firmware emission cadence" table is the canonical FW_PROGRESS emit-point spec.
- **Wire format file** (M1, frozen): `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp`. New struct + enum get appended at the end of the existing enum/struct blocks following the `__attribute__((packed))` + `static_assert(sizeof…)` discipline.
- **Existing packet-type enum**: `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`. `AstrOsPacketType::OTA_FLASH_RESULT` gets added; existing values are wire-stable so the new value goes at the end (don't renumber).
- **M3 master-side ACK/NAK dispatch precedent**: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — find `routeOtaAckNakToForwarder`. This is the template for routing the new OTA_FLASH_RESULT to the master-side forwarder queue.
- **OtaForwarder phase/timer pattern**: `lib/OtaForwarder/include/OtaForwarder.hpp` — `Phase` enum + the `beginAckTimer_` / `endAckTimer_` / `tickTimer_` / `statsTimer_` triplet pattern. The new flash-result timer follows the same shape (create/start/stop/destroy + Cb posting a queue message).
- **OtaWriter handleEnd current shape**: `lib/OtaWriter/src/OtaWriter.cpp::handleEnd`. The 3 integrity gates currently lead directly to `sendEndAck(... OK ...)`. This plan splits that into "send END_ACK OK" then "delay 2 s" then "send OTA_FLASH_RESULT FLASH_NOT_IMPLEMENTED" — but the END_ACK OK send and gate logic itself is unchanged.
- **Pre-existing native test file for serial messages**: `test/test_native/astros_serial_messages_tests.cpp`. New FW_PROGRESS builder test goes here.
- **Pre-existing native test file for OTA wire payloads**: `test/test_native/astros_ota_espnow_messages_tests.cpp`. New OTA_FLASH_RESULT round-trip tests go here.
- **Pre-commit hook**: a clang-format pre-commit hook is opt-in (`git config core.hooksPath .githooks`). If activated, all staged C/C++ files get auto-formatted; the commit-prep steps in tasks below assume this is on. If not, format manually with `clang-format -i` on touched files before committing.

## File structure

**Modified:**
- `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp` — add `OtaFlashStatus` enum, `OtaFlashResultPayload` struct
- `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` — add `AstrOsPacketType::OTA_FLASH_RESULT` + `OtaFlashResultRecord` + `parseOtaFlashResult` declaration
- `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp` — implement `parseOtaFlashResult`; route the new packet type in any dispatch logic that lives here
- `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp` — declare `getFwProgress(...)` builder
- `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp` — implement `getFwProgress(...)`
- `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp` — declare `sendFwProgress(...)` wrapper
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` — implement `sendFwProgress(...)`
- `lib/AstrOsEspNow/src/AstrOsEspNowService.{hpp,cpp}` — add OTA_FLASH_RESULT dispatch to the master-side residual-switch arm
- `lib/OtaForwarder/include/OtaForwarder.hpp` — add `AWAITING_FLASH_RESULT` phase, `flashResultTimer_` handle + constants, `OTA_FWD_FLASH_RESULT` + `OTA_FWD_FLASH_RESULT_TIMEOUT` queue message kinds, `handleFlashResult` + `handleFlashResultTimeout` declarations
- `lib/OtaForwarder/include/OtaForwarderQueueMessage.h` — extend `ota_forwarder_msg_kind_t` with `OTA_FWD_FLASH_RESULT` + `OTA_FWD_FLASH_RESULT_TIMEOUT`; add `flash_result` arm to the union
- `lib/OtaForwarder/src/OtaForwarder.cpp` — add flash-result handler, timer create/start/stop in Init/dtor/state transitions; emit `FW_PROGRESS SENDING` (at startNextPadawan + during streamDrain throttled), `VERIFYING` (in emitOtaEndFrame), `FLASHING` (in handleEndAck OK path)
- `lib/OtaWriter/src/OtaWriter.cpp::handleEnd` — split END_ACK send from flash-result send via `vTaskDelay(pdMS_TO_TICKS(2000))` and `OtaFlashStatus::FLASH_NOT_IMPLEMENTED`
- `test/test_native/astros_ota_espnow_messages_tests.cpp` — native round-trip tests for OTA_FLASH_RESULT
- `test/test_native/astros_serial_messages_tests.cpp` — native round-trip test for FW_PROGRESS builder
- `.docs/protocol.md` — document `FwStage.FLASHING` + `OTA_FLASH_RESULT` payload + `OtaFlashStatus` values (lockstep with sister-repo update)
- `.docs/qa/ota-mesh-forward.md` — add bench-validation steps for the new UI behavior

**Not created:** No new lib directories. All changes extend existing PURE / MIXED libs.

---

## Tasks

### Task 0: Verify branch + plan commit

**Files:**
- This plan file

- [ ] **Step 1: Verify branch state**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git status -sb
git branch --show-current
```

Expected: branch is `feature/ota-fw-progress-emission`, working tree clean except this new plan file.

- [ ] **Step 2: Commit the plan**

```bash
git add .docs/plans/20260527-0211-fw-progress-flashing-stage-firmware.md
git commit -m "$(cat <<'EOF'
docs(ota): plan — firmware FW_PROGRESS emission + OTA_FLASH_RESULT

Implementation plan for the firmware half of the FW_PROGRESS emission
feature. Ships after the sister AstrOs.Server PR per the migration
order documented in the parent design.

See: .docs/plans/20260527-0143-firmware-ota-progress-emission-design.md
Server plan: AstrOs.Server/.docs/plans/20260527-0210-fw-progress-flashing-stage-server.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 1: Wire format — OTA_FLASH_RESULT payload + packet type + parser

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp`
- Modify: `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`
- Modify: `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp`
- Modify: `test/test_native/astros_ota_espnow_messages_tests.cpp`

- [ ] **Step 1: Add `OtaFlashStatus` enum + `OtaFlashResultPayload` struct**

Append to `lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp` after the existing `OtaEndAckPayload`:

```cpp
// ─── Upstream: flash-commit outcome (sent after the padawan's 2 s
//     post-verify delay; reports whether esp_ota_set_boot_partition
//     succeeded, or that flash is intentionally not implemented in this
//     firmware build (M4 placeholder)). ────────────────────────────────

enum class OtaFlashStatus : uint8_t
{
    OK = 0,                    // esp_ota_set_boot_partition succeeded (PR set 2)
    FLASH_NOT_IMPLEMENTED = 1, // M4 placeholder — flash step deliberately skipped
    FAILED = 2                 // esp_ota_set_boot_partition returned error (PR set 2)
};

// OTA_FLASH_RESULT payload. reasonLen is capped at 63 so the struct
// stays a fixed 66 bytes on the wire (well under the 180 B ESP-NOW
// fragmentation chunk size). Reason bytes are NOT NUL-terminated;
// the receiver must read exactly reasonLen bytes.
struct __attribute__((packed)) OtaFlashResultPayload
{
    uint8_t xferId;
    uint8_t status;     // OtaFlashStatus value
    uint8_t reasonLen;  // 0..63
    char reason[63];    // populated to reasonLen bytes; rest is don't-care
};
static_assert(sizeof(OtaFlashResultPayload) == 66, "OtaFlashResultPayload must be 66 bytes on the wire");
```

- [ ] **Step 2: Add `AstrOsPacketType::OTA_FLASH_RESULT` + record type + parser declaration**

Open `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`. Find the `AstrOsPacketType` enum and add `OTA_FLASH_RESULT` at the end (wire-stable — never renumber, only append):

```cpp
enum class AstrOsPacketType : uint8_t
{
    // ... existing variants unchanged ...
    OTA_END_ACK,         // (existing — keep its current value)
    // NEW:
    OTA_FLASH_RESULT,    // padawan → master flash-commit outcome
};
```

Then add a decoded-record type next to the existing `OtaEndAckRecord`:

```cpp
struct OtaFlashResultRecord
{
    uint8_t xferId;
    OtaFlashStatus status;
    std::string reason; // copied from wire bytes; empty when reasonLen == 0
    bool valid;
};
```

And declare the parser:

```cpp
OtaFlashResultRecord parseOtaFlashResult(const uint8_t *bytes, size_t len);
```

- [ ] **Step 3: Implement `parseOtaFlashResult`**

In `lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp`, add (mirroring the shape of an existing `parseOtaEndAck`):

```cpp
OtaFlashResultRecord parseOtaFlashResult(const uint8_t *bytes, size_t len)
{
    OtaFlashResultRecord r{};
    r.valid = false;
    if (bytes == nullptr || len < sizeof(OtaFlashResultPayload))
    {
        return r;
    }
    OtaFlashResultPayload p;
    std::memcpy(&p, bytes, sizeof(p));

    // Reject unknown status values up-front so the master never has to
    // disambiguate "padawan sent garbage" from "wire decoded fine but
    // the status is one we don't handle." Future-OtaFlashStatus values
    // need an explicit parser bump in lockstep.
    switch (static_cast<OtaFlashStatus>(p.status))
    {
    case OtaFlashStatus::OK:
    case OtaFlashStatus::FLASH_NOT_IMPLEMENTED:
    case OtaFlashStatus::FAILED:
        break;
    default:
        return r;
    }

    if (p.reasonLen > sizeof(p.reason))
    {
        return r;
    }

    r.xferId = p.xferId;
    r.status = static_cast<OtaFlashStatus>(p.status);
    r.reason.assign(p.reason, p.reason + p.reasonLen);
    r.valid = true;
    return r;
}
```

Include `<cstring>` if not already included.

- [ ] **Step 4: Write failing native round-trip tests**

Open `test/test_native/astros_ota_espnow_messages_tests.cpp` and append:

```cpp
TEST(OtaFlashResult, RoundTripOk)
{
    OtaFlashResultPayload p{};
    p.xferId = 5;
    p.status = static_cast<uint8_t>(OtaFlashStatus::OK);
    p.reasonLen = 0;

    auto r = parseOtaFlashResult(reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.xferId, 5);
    EXPECT_EQ(r.status, OtaFlashStatus::OK);
    EXPECT_EQ(r.reason, "");
}

TEST(OtaFlashResult, RoundTripFlashNotImplementedWithReason)
{
    OtaFlashResultPayload p{};
    p.xferId = 7;
    p.status = static_cast<uint8_t>(OtaFlashStatus::FLASH_NOT_IMPLEMENTED);
    const char *reason = "pr_set_1_placeholder";
    p.reasonLen = std::strlen(reason);
    std::memcpy(p.reason, reason, p.reasonLen);

    auto r = parseOtaFlashResult(reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.xferId, 7);
    EXPECT_EQ(r.status, OtaFlashStatus::FLASH_NOT_IMPLEMENTED);
    EXPECT_EQ(r.reason, "pr_set_1_placeholder");
}

TEST(OtaFlashResult, RoundTripFailedWithReason)
{
    OtaFlashResultPayload p{};
    p.xferId = 1;
    p.status = static_cast<uint8_t>(OtaFlashStatus::FAILED);
    const char *reason = "esp_ota_set_boot_partition: ESP_ERR_INVALID_STATE";
    p.reasonLen = std::strlen(reason);
    ASSERT_LE(p.reasonLen, sizeof(p.reason));
    std::memcpy(p.reason, reason, p.reasonLen);

    auto r = parseOtaFlashResult(reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.status, OtaFlashStatus::FAILED);
    EXPECT_EQ(r.reason, "esp_ota_set_boot_partition: ESP_ERR_INVALID_STATE");
}

TEST(OtaFlashResult, RejectsTruncatedBuffer)
{
    OtaFlashResultPayload p{};
    auto r = parseOtaFlashResult(reinterpret_cast<const uint8_t *>(&p), sizeof(p) - 1);
    EXPECT_FALSE(r.valid);
}

TEST(OtaFlashResult, RejectsUnknownStatus)
{
    OtaFlashResultPayload p{};
    p.status = 99;
    auto r = parseOtaFlashResult(reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    EXPECT_FALSE(r.valid);
}

TEST(OtaFlashResult, RejectsOversizedReasonLen)
{
    OtaFlashResultPayload p{};
    p.status = static_cast<uint8_t>(OtaFlashStatus::OK);
    p.reasonLen = sizeof(p.reason) + 1;
    auto r = parseOtaFlashResult(reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    EXPECT_FALSE(r.valid);
}
```

- [ ] **Step 5: Run native tests; expect 6 new failures**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
pio test -e test -f test_native --filter "*OtaFlashResult*"
```

Expected: 6 tests fail (parser doesn't exist yet at compile time → compile error, OR exists but returns invalid → assertion failures).

If you got compile errors instead of assertion failures, the earlier steps weren't done yet — go back.

- [ ] **Step 6: Re-run with parser implemented; expect 6 passes**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
pio test -e test -f test_native --filter "*OtaFlashResult*"
```

Expected: 6 passes.

- [ ] **Step 7: Full native suite sanity check**

```bash
pio test -e test
```

Expected: pre-existing test count + 6 new = clean pass.

- [ ] **Step 8: Both-board build sanity check**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build, no new warnings.

- [ ] **Step 9: Commit**

```bash
git add lib_native/AstrOsMessaging/src/OtaWirePayloads.hpp \
        lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp \
        lib_native/AstrOsEspNowProtocol/src/AstrOsEspNowProtocol.cpp \
        test/test_native/astros_ota_espnow_messages_tests.cpp
git commit -m "feat(ota): wire format — OTA_FLASH_RESULT payload + parser

Adds OtaFlashStatus enum (OK/FLASH_NOT_IMPLEMENTED/FAILED) and
OtaFlashResultPayload struct (xferId, status, reasonLen, reason[63] —
fixed 66 bytes packed). New AstrOsPacketType::OTA_FLASH_RESULT
variant for the ESP-NOW frame routing. parseOtaFlashResult validates
length, status value, and reasonLen <= 63.

Native round-trip tests cover OK, FLASH_NOT_IMPLEMENTED+reason,
FAILED+long-reason, truncated-buffer reject, unknown-status reject,
oversized-reasonLen reject. No emit/consume yet — wire format only.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Serial messaging — `getFwProgress` builder + `sendFwProgress` wrapper

**Files:**
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`
- Modify: `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`
- Modify: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`
- Modify: `test/test_native/astros_serial_messages_tests.cpp`

- [ ] **Step 1: Declare the builder**

In `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp`, alongside the other `get*` declarations, add:

```cpp
// FW_PROGRESS wire format:
//   FW_PROGRESS<header>transferId<US>controllerId<US>stage<US>bytesSent<US>totalBytes<US>detail
// stage is a literal string from the FwStage enum on the server side
// (e.g. "SENDING", "VERIFYING", "FLASHING"). The server's parser at
// astros_api/src/serial/message_handler.ts:308 reads parts[2] directly
// as the stage value, so the wire-encoding here is the string itself,
// not a numeric id.
std::string getFwProgress(std::string msgId,
                          std::string transferId,
                          std::string controllerId,
                          std::string stage,
                          uint32_t bytesSent,
                          uint32_t totalBytes,
                          std::string detail);
```

- [ ] **Step 2: Implement the builder**

In `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`, add (alongside `getFwTransferEndAck` / `getFwDeployDone`):

```cpp
std::string AstrOsSerialMessageService::getFwProgress(std::string msgId,
                                                      std::string transferId,
                                                      std::string controllerId,
                                                      std::string stage,
                                                      uint32_t bytesSent,
                                                      uint32_t totalBytes,
                                                      std::string detail)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_PROGRESS, msgId);
    ss << transferId << UNIT_SEPARATOR
       << controllerId << UNIT_SEPARATOR
       << stage << UNIT_SEPARATOR
       << bytesSent << UNIT_SEPARATOR
       << totalBytes << UNIT_SEPARATOR
       << detail;
    return ss.str();
}
```

- [ ] **Step 3: Declare + implement the `sendFwProgress` wrapper**

In `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`, alongside the other `send*Fw*` wrappers:

```cpp
void sendFwProgress(std::string transferId,
                    std::string controllerId,
                    std::string stage,
                    uint32_t bytesSent,
                    uint32_t totalBytes,
                    std::string detail);
```

In `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`, find the existing `sendFwDeployDone(...)` for the precedent, then add:

```cpp
void AstrOsSerialMsgHandler::sendFwProgress(std::string transferId,
                                            std::string controllerId,
                                            std::string stage,
                                            uint32_t bytesSent,
                                            uint32_t totalBytes,
                                            std::string detail)
{
    // msgId is "na" for unsolicited push events — matches FW_CHUNK_ACK,
    // FW_CHUNK_NAK, POLL_ACK precedent.
    std::string msg = AstrOsSerialMessageService::getInstance().getFwProgress(
        "na", transferId, controllerId, stage, bytesSent, totalBytes, detail);
    queueResponse(msg);
}
```

(If your project's wrapper uses a different name than `queueResponse`, mirror the existing `sendFwDeployDone` body verbatim — same enqueue mechanism.)

- [ ] **Step 4: Write failing native round-trip test**

In `test/test_native/astros_serial_messages_tests.cpp`, append:

```cpp
TEST(AstrOsSerialMessageService, FwProgress_BuildsExpectedWireFormat)
{
    auto &svc = AstrOsSerialMessageService::getInstance();
    std::string msg = svc.getFwProgress(
        "msg-1", "xfer-42", "pad1", "SENDING", 1024, 4096, "");

    // Wire shape: <header><transferId><US><controllerId><US><stage><US><bytesSent><US><totalBytes><US><detail>
    // Verify the body field separators and values appear in order.
    EXPECT_NE(msg.find("xfer-42"), std::string::npos);
    EXPECT_NE(msg.find("pad1"), std::string::npos);
    EXPECT_NE(msg.find("SENDING"), std::string::npos);
    EXPECT_NE(msg.find("1024"), std::string::npos);
    EXPECT_NE(msg.find("4096"), std::string::npos);

    // Confirm field ordering by splitting on US after the header and
    // checking each field's index.
    auto headerEnd = msg.find(RECORD_SEPARATOR); // header ends with RS in the project convention
    ASSERT_NE(headerEnd, std::string::npos);
    std::string body = msg.substr(headerEnd + 1);

    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= body.size())
    {
        size_t pos = body.find(UNIT_SEPARATOR, start);
        if (pos == std::string::npos)
        {
            fields.push_back(body.substr(start));
            break;
        }
        fields.push_back(body.substr(start, pos - start));
        start = pos + 1;
    }
    ASSERT_EQ(fields.size(), 6u);
    EXPECT_EQ(fields[0], "xfer-42");
    EXPECT_EQ(fields[1], "pad1");
    EXPECT_EQ(fields[2], "SENDING");
    EXPECT_EQ(fields[3], "1024");
    EXPECT_EQ(fields[4], "4096");
    EXPECT_EQ(fields[5], "");
}

TEST(AstrOsSerialMessageService, FwProgress_FlashingStageRoundTrip)
{
    auto &svc = AstrOsSerialMessageService::getInstance();
    std::string msg = svc.getFwProgress(
        "msg-2", "xfer-7", "pad2", "FLASHING", 10000, 10000, "pr_set_1_placeholder");

    EXPECT_NE(msg.find("FLASHING"), std::string::npos);
    EXPECT_NE(msg.find("pr_set_1_placeholder"), std::string::npos);
}
```

Note: if the existing test file uses a different header-terminator pattern (`'\n'`, ETX, etc.) instead of `RECORD_SEPARATOR`, adapt the `headerEnd` logic to match. Read 2-3 sibling tests in the file before writing this one to match style.

- [ ] **Step 5: Run + verify failing → implement → passing**

```bash
pio test -e test --filter "*FwProgress*"
```

First run: 2 failures (builder doesn't exist → compile error). After implementing Steps 1-3: 2 passes.

- [ ] **Step 6: Full native suite + both-board build sanity check**

```bash
pio test -e test
pio run -e metro_s3
pio run -e lolin_d32_pro
```

- [ ] **Step 7: Commit**

```bash
git add lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp \
        lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "feat(serial): FW_PROGRESS builder + sendFwProgress wrapper

Adds the firmware-side counterpart to the server's existing
FW_PROGRESS consumer (message_handler.ts:301-332). Wire shape:
  FW_PROGRESS<header><transferId><US><controllerId><US><stage>
              <US><bytesSent><US><totalBytes><US><detail>
Stage is a literal string (e.g. \"SENDING\", \"VERIFYING\",
\"FLASHING\") — matches the server's string-valued FwStage enum.

Builder lives in AstrOsSerialMessageService (PURE). Wrapper in
AstrOsSerialMsgHandler (MIXED) enqueues via the existing
queueResponse path; msgId is \"na\" matching the unsolicited-push
convention used by FW_CHUNK_ACK/NAK.

Native round-trip tests pin field ordering + SENDING/FLASHING values.

No emit site yet — that lands in the OtaForwarder task.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: AstrOsEspNow — dispatch inbound OTA_FLASH_RESULT to forwarder queue

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` (find the routeOta* helpers + the residual switch)
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp` (if a new helper is added)

- [ ] **Step 1: Locate the existing master-side OTA ACK/NAK route**

```bash
grep -n "routeOtaAckNakToForwarder\|OTA_END_ACK\|case AstrOsPacketType::OTA_" lib/AstrOsEspNow/src/AstrOsEspNowService.cpp | head -20
```

Note the function/switch location handling `OTA_BEGIN_ACK` / `OTA_BEGIN_NAK` / `OTA_DATA_ACK` / `OTA_DATA_NAK` / `OTA_END_ACK`. The new `OTA_FLASH_RESULT` joins this group — master receives it from a padawan, parses it, and posts a queue message to `otaForwarderQueue`.

- [ ] **Step 2: Add the OTA_FLASH_RESULT case**

In the same switch (or helper function), add a case mirroring the OTA_END_ACK handling. Pseudo-code (adapt to the actual file structure you found in Step 1):

```cpp
case AstrOsPacketType::OTA_FLASH_RESULT:
{
    auto rec = parseOtaFlashResult(packet.payload, packet.payloadLen);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "OTA_FLASH_RESULT parse failed from " MACSTR, MAC2STR(src));
        return;
    }
    if (otaForwarderQueue_ == nullptr)
    {
        ESP_LOGW(TAG, "OTA_FLASH_RESULT received but otaForwarderQueue_ is null (not master?)");
        return;
    }
    queue_ota_forwarder_msg_t msg{};
    msg.kind = OTA_FWD_FLASH_RESULT;
    std::memcpy(msg.flash_result.srcMac, src, sizeof(msg.flash_result.srcMac));
    msg.flash_result.xferId = rec.xferId;
    msg.flash_result.status = static_cast<uint8_t>(rec.status);
    // Copy reason into a malloc'd buffer; consumer free's it via
    // freeOtaForwarderMsg per the queue ownership convention.
    if (!rec.reason.empty())
    {
        msg.flash_result.reasonLen = rec.reason.size();
        msg.flash_result.reason = static_cast<char *>(std::malloc(rec.reason.size()));
        if (msg.flash_result.reason == nullptr)
        {
            ESP_LOGE(TAG, "OTA_FLASH_RESULT: malloc reason copy failed; dropping");
            return;
        }
        std::memcpy(msg.flash_result.reason, rec.reason.data(), rec.reason.size());
    }
    else
    {
        msg.flash_result.reasonLen = 0;
        msg.flash_result.reason = nullptr;
    }
    if (xQueueSend(otaForwarderQueue_, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "OTA_FLASH_RESULT: otaForwarderQueue full; dropping");
        if (msg.flash_result.reason)
        {
            std::free(msg.flash_result.reason);
        }
    }
    break;
}
```

The exact macros (`MACSTR`, `MAC2STR`, `TAG`) and queue-message field names depend on what Task 4 defines. Order: if Task 4 hasn't been completed yet at the time you reach this step, jump down and do Task 4 first, then return here. The two tasks define interlocking surface area.

**Decision point:** combine Tasks 3 and 4 into one commit if you're doing them sequentially, since neither compiles without the other.

- [ ] **Step 3: Build sanity check**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build. If you get "undefined `OTA_FWD_FLASH_RESULT`" errors, Task 4 hasn't landed yet.

- [ ] **Step 4: Commit (or roll into Task 4's commit)**

```bash
git add lib/AstrOsEspNow/src/AstrOsEspNowService.cpp lib/AstrOsEspNow/src/AstrOsEspNowService.hpp
git commit -m "feat(ota): route inbound OTA_FLASH_RESULT to otaForwarderQueue

Master-side dispatch for the new padawan→master flash-commit-outcome
packet. Parses via parseOtaFlashResult, posts an
OTA_FWD_FLASH_RESULT queue message with srcMac + payload contents +
malloc'd reason copy. Consumer-frees the reason buffer per the
queue ownership convention (see freeOtaForwarderMsg).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: OtaForwarder — AWAITING_FLASH_RESULT phase + flash-result handler + 10 s safety timer

**Files:**
- Modify: `lib/OtaForwarder/include/OtaForwarder.hpp`
- Modify: `lib/OtaForwarder/include/OtaForwarderQueueMessage.h`
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

- [ ] **Step 1: Extend the queue-message kind enum + union**

In `lib/OtaForwarder/include/OtaForwarderQueueMessage.h`, add to `ota_forwarder_msg_kind_t`:

```c
OTA_FWD_FLASH_RESULT = 8,              // padawan→master flash-commit outcome
OTA_FWD_FLASH_RESULT_TIMEOUT = 9       // safety timer fire — padawan never reported
```

Add a new arm to the union:

```c
struct
{
    uint8_t srcMac[6];
    uint8_t xferId;
    uint8_t status;        // OtaFlashStatus value (see OtaWirePayloads.hpp)
    uint8_t reasonLen;     // 0..63
    char *reason;          // malloc'd; freed in freeOtaForwarderMsg on this kind
} flash_result;
```

Update `freeOtaForwarderMsg` to free `flash_result.reason` when kind is `OTA_FWD_FLASH_RESULT`:

```c
if (m->kind == OTA_FWD_FLASH_RESULT && m->flash_result.reason != NULL)
{
    free(m->flash_result.reason);
    m->flash_result.reason = NULL;
}
```

`OTA_FWD_FLASH_RESULT_TIMEOUT` carries no heap pointers (timer fire just needs the kind discriminator).

- [ ] **Step 2: Add the new forwarder phase**

In `lib/OtaForwarder/include/OtaForwarder.hpp`, find the `Phase` enum (likely near the top of the private section) and insert:

```cpp
enum class Phase : uint8_t {
    IDLE,
    AWAITING_BEGIN_ACK,
    STREAMING,
    AWAITING_END_ACK,
    AWAITING_FLASH_RESULT,   // NEW — between END_ACK OK and completeCurrentPadawan
    BETWEEN_PADAWANS,
};
```

Add a timer handle + constants:

```cpp
esp_timer_handle_t flashResultTimer_ = nullptr;
static constexpr uint64_t kFlashResultTimeoutUs = 10ULL * 1000ULL * 1000ULL;  // 10 s
```

Declare the handler + timer methods:

```cpp
void handleFlashResult(queue_ota_forwarder_msg_t &msg);
void handleFlashResultTimeout();
void flashResultTimerStart();
void flashResultTimerStop();
static void flashResultTimerCb(void *arg);
```

- [ ] **Step 3: Implement timer create/destroy in `Init` + destructor**

Following the existing `statsTimer_` pattern in `OtaForwarder.cpp`:

In `Init(...)` (after the `statsTimer_` creation block):

```cpp
esp_timer_create_args_t flashResultArgs = {
    .callback = &OtaForwarder::flashResultTimerCb,
    .arg = this,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "ota_fwd_flashres",
    .skip_unhandled_events = true,
};
ESP_ERROR_CHECK(esp_timer_create(&flashResultArgs, &flashResultTimer_));
```

In the destructor's timer-cleanup loop, add `&flashResultTimer_` to the brace-init list (the existing pattern at the top of `~OtaForwarder()`).

- [ ] **Step 4: Implement the timer Cb, start/stop, and handler bodies**

Add near the other timer methods in `OtaForwarder.cpp`:

```cpp
void OtaForwarder::flashResultTimerStart()
{
    if (!flashResultTimer_) return;
    esp_timer_stop(flashResultTimer_);
    esp_err_t err = esp_timer_start_once(flashResultTimer_, kFlashResultTimeoutUs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "flashResultTimerStart: esp_timer_start_once failed: %s — "
                      "no safety net; padawan crash during pre-flash delay would block deploy",
                 esp_err_to_name(err));
    }
}

void OtaForwarder::flashResultTimerStop()
{
    if (flashResultTimer_)
    {
        esp_timer_stop(flashResultTimer_);
    }
}

void OtaForwarder::flashResultTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_) return;
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_FLASH_RESULT_TIMEOUT;
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}

void OtaForwarder::handleFlashResult(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_FLASH_RESULT)
    {
        ESP_LOGW(TAG, "handleFlashResult: unexpected arrival in phase=%d; dropping", (int)phase_);
        return;
    }
    if (msg.flash_result.xferId != currentXferId_)
    {
        ESP_LOGW(TAG, "handleFlashResult: xferId mismatch (got %u, expected %u); dropping",
                 (unsigned)msg.flash_result.xferId, (unsigned)currentXferId_);
        return;
    }
    flashResultTimerStop();

    OtaFlashStatus status = static_cast<OtaFlashStatus>(msg.flash_result.status);
    std::string reason(msg.flash_result.reason ? msg.flash_result.reason : "",
                       msg.flash_result.reasonLen);

    PadawanStatus padawanStatus = PadawanStatus::FAILED;
    std::string err = reason;
    std::string finalVersion = "";
    switch (status)
    {
    case OtaFlashStatus::OK:
        padawanStatus = PadawanStatus::OK;
        err = "";
        // finalVersion stays empty in M4-vs-PR-set-2 transition window;
        // PR set 2 will populate via a separate post-reboot path.
        break;
    case OtaFlashStatus::FLASH_NOT_IMPLEMENTED:
        padawanStatus = PadawanStatus::FAILED;
        if (err.empty()) err = "flash_not_implemented";
        break;
    case OtaFlashStatus::FAILED:
        padawanStatus = PadawanStatus::FAILED;
        if (err.empty()) err = "flash_failed";
        break;
    }

    ESP_LOGI(TAG, "Flash result for %s: status=%d reason='%s'",
             currentControllerId_.c_str(), (int)status, err.c_str());
    results_.push_back({currentControllerId_, padawanStatus, finalVersion, err});

    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}

void OtaForwarder::handleFlashResultTimeout()
{
    if (phase_ != Phase::AWAITING_FLASH_RESULT)
    {
        return; // stale fire after we already completed
    }
    ESP_LOGW(TAG, "Flash-result timeout for %s; recording FAILED",
             currentControllerId_.c_str());
    results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "flash_result_timeout"});
    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}
```

- [ ] **Step 5: Wire the new kinds into `process()`**

In `OtaForwarder::process(...)`, add cases:

```cpp
case OTA_FWD_FLASH_RESULT:
    handleFlashResult(msg);
    break;
case OTA_FWD_FLASH_RESULT_TIMEOUT:
    handleFlashResultTimeout();
    break;
```

- [ ] **Step 6: Modify the END_ACK OK handler to transition to AWAITING_FLASH_RESULT instead of calling `completeCurrentPadawan`**

Find `handleEndAck` in `OtaForwarder.cpp`. Currently on OK arrival it calls `completeCurrentPadawan()`. Change to:

```cpp
// On OK, the padawan has verified but not yet committed. Enter the
// AWAITING_FLASH_RESULT phase and wait for OTA_FLASH_RESULT to land.
// kFlashResultTimeoutUs is the safety bound on padawan misbehavior
// during the pre-flash delay window.
phase_ = Phase::AWAITING_FLASH_RESULT;
flashResultTimerStart();
// FW_PROGRESS FLASHING emission lands in Task 5; this commit just
// wires the phase transition.
```

Existing HASH_MISMATCH / WRITE_ERROR paths still take the immediate-FAILED route (they call `abortCurrentPadawan` or similar) — those don't enter AWAITING_FLASH_RESULT.

- [ ] **Step 7: Ensure abort paths stop the flash-result timer**

In `abortCurrentPadawan` (and any other abort/reset path), add `flashResultTimerStop();` alongside the other timer-stops.

- [ ] **Step 8: Build + bench-prep sanity check**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build, no new warnings. No native tests for MIXED-lib state machine — verified at bench in Task 8.

- [ ] **Step 9: Commit (or combine with Task 3's commit if you did them together)**

```bash
git add lib/OtaForwarder/include/OtaForwarder.hpp \
        lib/OtaForwarder/include/OtaForwarderQueueMessage.h \
        lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "feat(ota-forwarder): AWAITING_FLASH_RESULT phase + 10 s safety timer

New phase between AWAITING_END_ACK and BETWEEN_PADAWANS. On END_ACK
OK arrival the forwarder transitions to AWAITING_FLASH_RESULT and
arms a 10 s safety timer; OTA_FLASH_RESULT receipt (mapped via the
new OTA_FWD_FLASH_RESULT queue kind) records the padawan's outcome
and advances to the next padawan. Timer fire records
'flash_result_timeout' and proceeds — covers padawan crash during
the pre-flash delay window so the deploy can't block forever.

Wire-format builder + sender wrappers also added: getFwProgress in
AstrOsSerialMessageService (PURE) and sendFwProgress in
AstrOsSerialMsgHandler (MIXED). Emit sites land in the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: OtaForwarder — emit FW_PROGRESS at SENDING / VERIFYING / FLASHING boundaries

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`
- Modify: `lib/OtaForwarder/include/OtaForwarder.hpp` (add a `lastProgressBytesSent_` field for throttling)

- [ ] **Step 1: Add the throttle-tracking field**

In `OtaForwarder.hpp` private section:

```cpp
// FW_PROGRESS SENDING throttle: emit on every >=5% byte advance.
// Reset to 0 in startNextPadawan; updated in streamDrain.
uint32_t lastProgressBytesSent_ = 0;
```

In `startNextPadawan()` (where the existing `stats…_ = 0;` reset block lives), add:

```cpp
lastProgressBytesSent_ = 0;
```

- [ ] **Step 2: Emit SENDING on first-byte and on every >=5% advance**

In `startNextPadawan` after `emitOtaBeginFrame()` (or wherever the transition to STREAMING completes — find the existing FW_PROGRESS-equivalent emit-point or use the point right after `tickTimer_` starts):

```cpp
AstrOs_SerialMsgHandler.sendFwProgress(
    deployTransferId_,
    currentControllerId_,
    "SENDING",
    /*bytesSent=*/0,
    /*totalBytes=*/firmwareTotalSize_,
    /*detail=*/"");
```

In `streamDrain` (find the loop that increments per-chunk progress — likely around the `seq` advance after a successful `sendOtaFrame`), add throttled emission:

```cpp
// Emit SENDING FW_PROGRESS every >=5% of firmwareTotalSize_ bytes-sent
// advance. firstChunk emission already happened in startNextPadawan.
// Throttle threshold is 5% of total; fractional bytes computed inline
// to avoid floating-point in the hot path.
uint32_t bytesSent = static_cast<uint32_t>(seq) * static_cast<uint32_t>(kChunkSize);
if (bytesSent > firmwareTotalSize_) bytesSent = firmwareTotalSize_;  // cap on last chunk
uint32_t fivePct = firmwareTotalSize_ / 20;
if (fivePct == 0) fivePct = 1;  // degenerate small-firmware safety
if (bytesSent >= lastProgressBytesSent_ + fivePct)
{
    lastProgressBytesSent_ = bytesSent;
    AstrOs_SerialMsgHandler.sendFwProgress(
        deployTransferId_, currentControllerId_, "SENDING",
        bytesSent, firmwareTotalSize_, "");
}
```

Adapt variable names if `seq` / `kChunkSize` are named differently in the current `streamDrain` body — use whatever the file uses.

- [ ] **Step 3: Emit VERIFYING in `emitOtaEndFrame`**

Find `emitOtaEndFrame()`. Right after the `sendOtaFrame(..., AstrOsPacketType::OTA_END, ...)` call succeeds (not the failure branch):

```cpp
AstrOs_SerialMsgHandler.sendFwProgress(
    deployTransferId_, currentControllerId_, "VERIFYING",
    firmwareTotalSize_, firmwareTotalSize_, "");
```

Reasoning: VERIFYING fires when the master *sends* OTA_END, not when END_ACK arrives back. This is what makes the verify stage visibly current in the UI while the padawan is running its 3 integrity gates (~100 ms) + 2 s delay (Task 6).

- [ ] **Step 4: Emit FLASHING in `handleEndAck` OK path**

In the `handleEndAck` OK arm (just before the `flashResultTimerStart()` call added in Task 4):

```cpp
AstrOs_SerialMsgHandler.sendFwProgress(
    deployTransferId_, currentControllerId_, "FLASHING",
    firmwareTotalSize_, firmwareTotalSize_, "");
```

This makes the flash row visibly current while the padawan is in its 2 s pre-flash delay.

- [ ] **Step 5: Build + bench-prep sanity check**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add lib/OtaForwarder/include/OtaForwarder.hpp lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "feat(ota-forwarder): emit FW_PROGRESS at SENDING / VERIFYING / FLASHING boundaries

SENDING fires at startNextPadawan (first-byte) and on every >=5% of
firmwareTotalSize_ bytes-sent advance in streamDrain (throttled to
keep the serial channel quiet on small images).

VERIFYING fires on emitOtaEndFrame — i.e., when the master sends
OTA_END, not when END_ACK arrives back. This lights up the verify
row in the UI while the padawan runs its 3 integrity gates and the
2 s pre-flash delay.

FLASHING fires on END_ACK OK arrival. The flash row is visible
while the padawan is in its pre-flash delay window before sending
OTA_FLASH_RESULT.

Per the parent design's UX requirement: each stage row gets a
visible-duration window before transitioning to the next, so the
operator sees the lifecycle progress instead of a fast flicker.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: OtaWriter — handleEnd splits END_ACK OK from delayed FLASH_NOT_IMPLEMENTED

**Files:**
- Modify: `lib/OtaWriter/src/OtaWriter.cpp::handleEnd`
- Modify: `lib/OtaWriter/include/OtaWriter.hpp` (small — declare a `sendFlashResult` helper if useful)

- [ ] **Step 1: Read the existing handleEnd flow**

```bash
grep -n "handleEnd\|sendEndAck\|esp_ota_end\|all gates" lib/OtaWriter/src/OtaWriter.cpp | head -15
```

Find the success path — the point where the 3 integrity gates have all passed and the current code calls `sendEndAck(...,  OtaEndStatus::OK, ...)`.

- [ ] **Step 2: Insert the delay + flash-result send after the existing END_ACK OK send**

After the existing `sendEndAck(... OK ...)` call, insert:

```cpp
// M4 PLACEHOLDER — see .docs/plans/20260527-0143-firmware-ota-progress-emission-design.md
//
// Verification passed. Send OTA_END_ACK OK immediately (above) so the
// master can emit FW_PROGRESS FLASHING and the UI flash row lights up.
// Then deliberately delay 2 s so that flash row is visibly current
// before reporting the M4 placeholder failure.
//
// PR set 2 replaces this block with:
//   - vTaskDelay(pdMS_TO_TICKS(2000));
//   - esp_err_t err = esp_ota_set_boot_partition(inactivePartition_);
//   - sendFlashResult(mac, xferId,
//                     err == ESP_OK ? OtaFlashStatus::OK : OtaFlashStatus::FAILED,
//                     err == ESP_OK ? "" : esp_err_to_name(err));
//   - if (err == ESP_OK) esp_restart();
//
// The vTaskDelay below mirrors the natural PR-set-2 cadence so server
// timing assumptions tested against M4 hold unchanged against PR set 2.
vTaskDelay(pdMS_TO_TICKS(2000));
sendFlashResult(mac, xferId, OtaFlashStatus::FLASH_NOT_IMPLEMENTED, "pr_set_1_placeholder");
```

(`mac` and `xferId` here are whatever local variables `handleEnd` already has in scope — match the names from the existing `sendEndAck` call right above the insertion.)

- [ ] **Step 3: Add the `sendFlashResult` private helper**

In `OtaWriter.hpp` (private section, near `sendEndAck`):

```cpp
// Send OTA_FLASH_RESULT — emitted from handleEnd after the 2 s
// pre-flash delay. Mirrors sendEndAck's shape: builds the wire
// payload, calls AstrOs_EspNow.sendOtaFrame, logs at warn on send
// failure (matches the discipline in logSendResult).
esp_err_t sendFlashResult(const uint8_t mac[6], uint8_t xferId,
                          OtaFlashStatus status, const std::string &reason);
```

Implement in `OtaWriter.cpp`:

```cpp
esp_err_t OtaWriter::sendFlashResult(const uint8_t mac[6], uint8_t xferId,
                                     OtaFlashStatus status, const std::string &reason)
{
    OtaFlashResultPayload payload{};
    payload.xferId = xferId;
    payload.status = static_cast<uint8_t>(status);
    payload.reasonLen = std::min<size_t>(reason.size(), sizeof(payload.reason));
    if (payload.reasonLen > 0)
    {
        std::memcpy(payload.reason, reason.data(), payload.reasonLen);
    }
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_FLASH_RESULT,
                                      reinterpret_cast<const uint8_t *>(&payload),
                                      sizeof(payload));
}
```

- [ ] **Step 4: Watchdog interaction**

The padawan's `OtaWriter` has a 10 s idle watchdog. `vTaskDelay(2000)` won't trip it (watchdog is silence-based, but the END_ACK OK send before the delay restarts it). However, the `vTaskDelay` blocks the otaWriterTask — the watchdog timer fires via `esp_timer` which posts to the queue. The queue post during the delay accumulates; on wake the task processes them. If a stale `OTA_WR_WATCHDOG_FIRE` arrived just before the delay started and gets processed after, the late-fire `!active_` guard handles it (already in place per the M4 design).

Verify the watchdog reset happens *before* the delay — i.e., `watchdogStop()` (or equivalent) should run on END_ACK send so the timer doesn't fire mid-delay. Confirm via:

```bash
grep -n "watchdogStop\|watchdog\|resetOtaHandleAndSha" lib/OtaWriter/src/OtaWriter.cpp | head -20
```

If `resetOtaHandleAndSha()` is called at the start of `handleEnd`'s success path: good — it stops the watchdog. If not, add `watchdogStop();` before the delay.

After `sendFlashResult`, the padawan's per-transfer state can be torn down: call `resetOtaHandleAndSha()` to release everything (matches the existing OK-path teardown). Confirm this is the existing post-OK behavior — if so, just leave it where it is. If the existing handleEnd already called `resetOtaHandleAndSha` *before* sendEndAck, that ordering is wrong — it needs to be after sendFlashResult (so `currentXferId_` etc. stay valid for the in-flight send).

- [ ] **Step 5: Build sanity check**

```bash
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add lib/OtaWriter/include/OtaWriter.hpp lib/OtaWriter/src/OtaWriter.cpp
git commit -m "feat(ota-writer): handleEnd — send END_ACK OK, delay 2 s, then OTA_FLASH_RESULT

M4 placeholder behavior: verification gates pass, padawan sends
OTA_END_ACK OK so master can emit FW_PROGRESS FLASHING, then waits
2 s so the UI flash row is visibly current before sending
OTA_FLASH_RESULT(FLASH_NOT_IMPLEMENTED, 'pr_set_1_placeholder').

The delay deliberately mirrors PR set 2's eventual cadence (where
the padawan will run esp_ota_set_boot_partition in this window).
Server-side timing assumptions tested against M4 hold unchanged
against PR set 2.

vTaskDelay is acceptable here — the writer task has no pipelined
work in this window (OTA is single-shot per transfer; watchdog has
already been stopped via the END_ACK-side teardown). Watchdog
won't fire mid-delay because END_ACK send resets it. Stale
WATCHDOG_FIRE arriving from before the delay started is handled
by the existing late-signal guard.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Update `.docs/protocol.md` + QA plan

**Files:**
- Modify: `.docs/protocol.md`
- Modify: `.docs/qa/ota-mesh-forward.md`

- [ ] **Step 1: Update protocol.md**

Mirror the changes the sister AstrOs.Server PR made to its copy. Add:

- `FLASHING` to the Stage enum section, with the "between Verifying and Rebooting" placement
- New "OTA_FLASH_RESULT" subsection in the ESP-NOW frame catalog with the payload shape (xferId / status / reasonLen / reason[63])
- `OtaFlashStatus` enum values (OK / FLASH_NOT_IMPLEMENTED / FAILED)
- Note that M4 firmware emits FLASH_NOT_IMPLEMENTED as a placeholder; PR set 2 replaces it with the real esp_ota_set_boot_partition outcome.

The two copies must stay in lockstep — per the comment at `astros_api/src/models/firmware/firmware_messages.ts:3-4`, drift between them is a known hazard.

- [ ] **Step 2: Extend the QA plan**

Open `.docs/qa/ota-mesh-forward.md` and add a new test case at the end:

```markdown
## M4 — UI deploy with FW_PROGRESS emission + flash-not-implemented placeholder

**Precondition**: Master + 1 padawan flashed with this PR. Server-side PR
`feature/fw-progress-flashing-stage` merged.

1. Trigger a firmware deploy from the server's Firmware view.
2. Observe the UI:
   - **transfer** row turns yellow ("current") with running byte
     percentage, then green ✓ when streaming completes.
   - **verify** row turns yellow within a second of transfer completing,
     stays yellow for ~2 s.
   - **flash** row turns yellow when verify completes, stays yellow for
     ~2 s.
   - **flash** row turns red ! with status "Flash failed".
   - **reboot** row remains idle.
   - Result-bar reads "⚠ <padawan-label> failed during Flash" with detail
     line showing reason `flash_not_implemented`.

3. Repeat with 2 padawans. Each padawan's flash row turns red after its
   own 2 s window (sequential per-padawan deploy).

4. Expected master-side log lines (interleaved with existing OTA_STATS_TX):
   - `OtaForwarder: FW_PROGRESS SENDING bytesSent=… totalBytes=…`
   - `OtaForwarder: FW_PROGRESS VERIFYING bytesSent=N totalBytes=N` (after OTA_END sent)
   - `OtaForwarder: FW_PROGRESS FLASHING bytesSent=N totalBytes=N` (after OTA_END_ACK OK)
   - `OtaForwarder: Flash result for <padawan>: status=1 reason='pr_set_1_placeholder'`
   - `OtaForwarder: FW_DEPLOY_DONE: 2 targets, transferId=…`

5. Negative case — kill the padawan power immediately after it sends
   OTA_END_ACK OK (e.g., during the 2 s delay window). Master records
   `flash_result_timeout` after 10 s; UI shows flash failed with detail
   `flash_result_timeout`.
```

- [ ] **Step 3: Commit**

```bash
git add .docs/protocol.md .docs/qa/ota-mesh-forward.md
git commit -m "docs(ota): document FLASHING stage + OTA_FLASH_RESULT + M4 bench plan

Protocol doc adds the new FLASHING stage and OTA_FLASH_RESULT
payload shape — kept in lockstep with the sister AstrOs.Server
copy. QA plan adds the 2-padawan bench case covering the
verify→flash→fail UX timing assumptions and the negative
flash-result-timeout path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Bench validation + push + PR

- [ ] **Step 1: Confirm sister server PR is merged**

```bash
gh -R battlesloth/AstrOs.Server pr list --head feature/fw-progress-flashing-stage --json number,state,mergedAt
```

Expected: `state=MERGED`. If not, **stop** — pushing the firmware PR now would result in M4 deploys logging `FW_PROGRESS has unknown stage: FLASHING` on the server, no UI fix.

- [ ] **Step 2: Bench validation per .docs/qa/ota-mesh-forward.md "M4 — UI deploy"**

Flash master + 1 padawan. Pull the latest server. Run a deploy from the Firmware view. Observe the UI per the QA plan steps. Verify each row's timing matches the design's expectations.

If something looks wrong, **do not skip to the push**. The bench is the contract.

- [ ] **Step 3: Run native suite + both-board builds once more**

```bash
pio test -e test
pio run -e metro_s3
pio run -e lolin_d32_pro
```

- [ ] **Step 4: Push the branch**

```bash
git push -u origin feature/ota-fw-progress-emission
```

Auth-dependent — run in VS Code terminal if Claude Code's bash environment hits an auth wall.

- [ ] **Step 5: Open PR**

```bash
gh pr create --title "feat(ota): FW_PROGRESS emission + OTA_FLASH_RESULT + M4 flash placeholder" --body "$(cat <<'EOF'
## Summary

Firmware half of the FW_PROGRESS emission feature. Pairs with the AstrOs.Server PR (already merged) that added FwStage.Flashing + state-machine transitions.

After this lands, an M4 firmware deploy renders honestly in the server UI: ✓ download ✓ transfer ✓ verify ! flash idle-reboot, with detail `flash_not_implemented`. The previous "Flash failed: illegal flash-job transition: SENDING → VERSION_CONFIRMED" UI bug is resolved.

## Design

`.docs/plans/20260527-0143-firmware-ota-progress-emission-design.md`

## Implementation plan

`.docs/plans/20260527-0211-fw-progress-flashing-stage-firmware.md`

## What ships

- `OtaFlashStatus` enum + `OtaFlashResultPayload` struct (new ESP-NOW packet type `OTA_FLASH_RESULT`, padawan → master)
- `getFwProgress` builder (PURE) + `sendFwProgress` wrapper (MIXED)
- OtaForwarder: new `AWAITING_FLASH_RESULT` phase + 10 s safety timer; emits `FW_PROGRESS` at SENDING (first-byte + per-5% throttled), VERIFYING (on `emitOtaEndFrame`), FLASHING (on `OTA_END_ACK OK`)
- OtaWriter: `handleEnd` sends `OTA_END_ACK OK` immediately, delays 2 s (mirrors PR set 2's pre-flash-flip cadence), then sends `OTA_FLASH_RESULT(FLASH_NOT_IMPLEMENTED, "pr_set_1_placeholder")`
- 6 new native tests (round-trip of `OtaFlashResultPayload` parse/build, including reject cases) + 2 new FW_PROGRESS builder tests
- `.docs/protocol.md` + `.docs/qa/ota-mesh-forward.md` updated

## Test plan

- [ ] CI: PR validation (native tests, both-board build matrix, native-purity guard, clang-format)
- [ ] Bench (the M4 merge bar) — see `.docs/qa/ota-mesh-forward.md` "M4 — UI deploy" section
- [ ] Bench negative — padawan power-cut during 2 s pre-flash delay; master records `flash_result_timeout`
- [ ] UI: 2-padawan deploy renders sequential `✓ transfer ✓ verify ! flash` per padawan with ~2 s visible flash window

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-review checklist (run before declaring complete)

- [ ] All 8 tasks completed and committed
- [ ] Native test suite passes (`pio test -e test`)
- [ ] Both boards build clean (`pio run -e metro_s3`, `pio run -e lolin_d32_pro`)
- [ ] Pre-commit clang-format hook is on (or files manually formatted)
- [ ] Sister server PR is merged
- [ ] Bench test renders expected UI sequence on a 2-padawan deploy
- [ ] PR opened with the right base branch (`develop`)
