# Firmware OTA PR set 2 — Phase C — Master self-flash + `PadawanStatus::PENDING` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `master_self_flash_pending` placeholder with a real master self-flash that reuses `OtaWriter`'s flash machinery via a sentinel-message loopback, records the master row as `PadawanStatus::PENDING` in `FW_DEPLOY_DONE`, and reboots — letting the server resolve the row via the post-reboot self-POLL_ACK heartbeat.

**Architecture:** Master flash work runs on `otaWriterTask` (preserves OtaWriter's single-task-state invariant) via a new `OTA_WR_LOCAL_FLASH_REQ` sentinel posted from OtaForwarder. OtaWriter reports back via `OTA_FWD_LOCAL_FLASH_RESULT`. OtaForwarder defers the master row in the order-list walk via a `masterRowDeferred_` flag, processes it last via a new `Phase::MASTER_SELF_FLASHING`, then inserts the row at its `masterRowOriginalIndex_` so `FW_DEPLOY_DONE` preserves the operator-submitted order. The `PadawanStatus` enum gains a third value `PENDING` (wire string `"PENDING"`); the FW_DEPLOY_DONE serializer's validation is extended to accept it. Master `mark_app_valid_cancel_rollback` is wired into the existing self-POLL_ACK polling-timer call in `main.cpp`.

**Tech Stack:** ESP-IDF 5.x (`esp_ota_*`, `esp_partition_read`, `esp_restart`), FreeRTOS (queues, atomics), PlatformIO `[env:test]` GoogleTest native suite, `lib_native/` PURE libs.

**Design spec:** `.docs/plans/20260528-1234-firmware-ota-pr-set-2-phase-c-master-self-flash-design.md`

---

## File Structure

### Modified files

- `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` — add `PadawanStatus::PENDING = 2`.
- `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp` — extend `getFwDeployDone` status validation to accept `"PENDING"` alongside `"OK"` and `"FAILED"`.
- `lib/OtaWriter/include/OtaWriterQueueMessage.h` — add `OTA_WR_LOCAL_FLASH_REQ = N` kind + `local_flash_req` union arm.
- `lib/OtaWriter/include/OtaWriter.hpp` — declare `handleLocalFlashReq(msg)` private method.
- `lib/OtaWriter/src/OtaWriter.cpp` — implement `handleLocalFlashReq`; add dispatch case in `process()`.
- `lib/OtaForwarder/include/OtaForwarderQueueMessage.h` — add `OTA_FWD_LOCAL_FLASH_RESULT = N` kind + `local_flash_result` union arm.
- `lib/OtaForwarder/include/OtaForwarder.hpp` — add `Phase::MASTER_SELF_FLASHING`; add `masterRowDeferred_` + `masterRowOriginalIndex_` members; declare `startMasterSelfFlash()` + `handleLocalFlashResult(msg)` private methods.
- `lib/OtaForwarder/src/OtaForwarder.cpp` — master-row-deferral in `startNextPadawan`; padawan-loop-exhausted handler dispatches to `startMasterSelfFlash`; implement `startMasterSelfFlash` + `handleLocalFlashResult`; add dispatch case in `process()`; add `case MASTER_SELF_FLASHING` in `handleStatsFire`; add `case PadawanStatus::PENDING` in `emitDeployDoneAndReset` switch; reset `masterRowDeferred_` + `masterRowOriginalIndex_` in cleanup.
- `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp` — change `sendPollAckNak` return type from `void` to `bool`.
- `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` — return `xQueueSend` success.
- `src/main.cpp` — lift `!isMasterNode.load()` guards around OtaWriter queue/task/Init (keeping the guard around `AstrOs_EspNow.setOtaWriterQueue`); add `firstSelfPollAckSent_` + `mark_app_valid` at the master self-POLL_ACK call site.
- `test/test_native/astros_serial_messages_tests.cpp` — 2 new tests for PENDING round-trip + wire-string pin.
- `test/test_native/astros_ota_forwarder_tests.cpp` — extend `VersionConfirmStandIn` (or add a new sibling stand-in) with master-row deferral coverage; 3 new test cases.
- `.docs/qa/ota-upgrade-pr-set-2.md` — append Phase C section (preconditions, C.1–C.4 cases).

### Files intentionally not modified

- `lib_native/AstrOsEspNowPeers/include/espnow_peer.h` — NVS-persisted byte-for-byte; no changes needed.
- `lib/OtaReceiver/*` — Phase C uses `AstrOs_OtaReceiver.getLastFirmwarePath()` (already exists).
- `lib_native/AstrOsBulkTransport/*` — wire-protocol flash path is unchanged; master self-flash bypasses it.

---

## Task 1 — Add `PadawanStatus::PENDING` + extend serializer (TDD)

**Files:**
- Modify: `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`
- Modify: `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp:268-275`
- Modify: `test/test_native/astros_serial_messages_tests.cpp` (append 2 tests)

The enum addition is one line. The serializer needs its status validation extended from `(status != "OK" && status != "FAILED")` to also accept `"PENDING"`. Native-testable end-to-end via `getFwDeployDone`.

- [ ] **Step 1: Write the failing tests**

Append to `test/test_native/astros_serial_messages_tests.cpp` (near the existing `FW_DEPLOY_DONE` tests — search the file for `getFwDeployDone` to find the right neighbourhood). If no existing tests, create a new TEST block at the end of the file:

```cpp
TEST(FwDeployDone, PendingRowSerializesAndContainsPENDINGToken)
{
    AstrOsSerialMessageService msgSvc;
    std::vector<astros_fw_deploy_result_t> results = {
        {"AA:BB:CC:DD:EE:01", "OK", "1.2.3", ""},
        {"00:00:00:00:00:00", "PENDING", "", "awaiting_post_reboot_version"},
        {"AA:BB:CC:DD:EE:02", "FAILED", "", "version_unconfirmed"},
    };
    auto msg = msgSvc.getFwDeployDone("msg-99", "xfer-99", results);
    EXPECT_FALSE(msg.empty()) << "PENDING must not cause the serializer to reject";
    EXPECT_NE(msg.find("PENDING"), std::string::npos);
    EXPECT_NE(msg.find("OK"), std::string::npos);
    EXPECT_NE(msg.find("FAILED"), std::string::npos);
    EXPECT_NE(msg.find("00:00:00:00:00:00"), std::string::npos);
    EXPECT_NE(msg.find("awaiting_post_reboot_version"), std::string::npos);
}

TEST(FwDeployDone, AllPendingRowsAlsoSerializesCleanly)
{
    AstrOsSerialMessageService msgSvc;
    std::vector<astros_fw_deploy_result_t> results = {
        {"00:00:00:00:00:00", "PENDING", "", "awaiting_post_reboot_version"},
    };
    auto msg = msgSvc.getFwDeployDone("msg-100", "xfer-100", results);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("PENDING"), std::string::npos);
}
```

Also add this test that pins the PadawanStatus enum value (so a future enum reorder is caught):

```cpp
TEST(PadawanStatus, PendingValueIsTwo)
{
    EXPECT_EQ(static_cast<uint8_t>(AstrOsEspNowProtocol::PadawanStatus::OK), 0);
    EXPECT_EQ(static_cast<uint8_t>(AstrOsEspNowProtocol::PadawanStatus::FAILED), 1);
    EXPECT_EQ(static_cast<uint8_t>(AstrOsEspNowProtocol::PadawanStatus::PENDING), 2);
}
```

If `AstrOsEspNowProtocol.hpp` isn't already included, add `#include "AstrOsEspNowProtocol.hpp"` near the existing includes.

- [ ] **Step 2: Run tests to verify they fail**

Run: `~/.platformio/penv/bin/pio test -e test -f test_native --filter "*FwDeployDone*PendingRow*"`

Expected: COMPILE FAIL with "PendingValueIsTwo" referencing an undefined `PadawanStatus::PENDING` enum, OR runtime FAIL on the `PendingRowSerializesAndContainsPENDINGToken` test if PadawanStatus already compiles but the serializer rejects "PENDING" (returns empty string).

- [ ] **Step 3: Add PadawanStatus::PENDING enum value**

Edit `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`. Locate the `PadawanStatus` enum (around line 138):

Before:
```cpp
    enum class PadawanStatus : uint8_t
    {
        OK,
        FAILED,
    };
```

After:
```cpp
    enum class PadawanStatus : uint8_t
    {
        OK,
        FAILED,
        PENDING,   // master row: optimistic; server resolves via post-reboot self-POLL_ACK version match
    };
```

- [ ] **Step 4: Extend the FW_DEPLOY_DONE serializer validation**

Edit `lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp`. Locate the validation loop at line 268-275 in `getFwDeployDone`:

Before:
```cpp
    for (const auto &r : results)
    {
        // Enforced: status must be exactly "OK" or "FAILED".
        if (r.status != "OK" && r.status != "FAILED")
        {
            return "";
        }
    }
```

After:
```cpp
    for (const auto &r : results)
    {
        // Enforced: status must be exactly "OK", "FAILED", or "PENDING".
        // "PENDING" is produced exclusively by OtaForwarder for the master
        // self-flash row; the server resolves it via the post-reboot
        // self-POLL_ACK version match (or 90s timeout).
        if (r.status != "OK" && r.status != "FAILED" && r.status != "PENDING")
        {
            return "";
        }
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: all 478 tests pass (475 prior + 3 new). If the existing test count was different at branch creation, confirm only the 3 new tests changed the delta.

- [ ] **Step 6: Build both boards (PadawanStatus is used on master)**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 7: Commit**

```bash
git add lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp \
        lib_native/AstrOsMessaging/src/AstrOsSerialMessageService.cpp \
        test/test_native/astros_serial_messages_tests.cpp
git commit -m "$(cat <<'EOF'
feat(ota): add PadawanStatus::PENDING + accept it in FW_DEPLOY_DONE serializer

Phase C foundation. New enum value (=2) for the master self-flash row:
optimistic status emitted when local flash succeeds; server resolves to
OK on post-reboot self-POLL_ACK version match, or to FAILED on 90s
timeout. The FW_DEPLOY_DONE serializer's status-validation loop now
accepts "PENDING" alongside "OK" and "FAILED". 3 new native tests pin
the enum value, the round-trip, and the all-PENDING edge case.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Sentinel message kinds (`OTA_WR_LOCAL_FLASH_REQ` + `OTA_FWD_LOCAL_FLASH_RESULT`)

**Files:**
- Modify: `lib/OtaWriter/include/OtaWriterQueueMessage.h`
- Modify: `lib/OtaForwarder/include/OtaForwarderQueueMessage.h`

Type-level scaffolding for the cross-task loopback. No behavior change yet; just the message types. Handlers are stubbed in Tasks 3 and 4.

- [ ] **Step 1: Read the existing OtaWriter queue message header**

Run: `~/.platformio/penv/bin/pio run --target idedata 2>/dev/null; grep -n "enum.*ota_writer_msg_kind\|union\|OTA_WR_" lib/OtaWriter/include/OtaWriterQueueMessage.h | head -20`

(The grep is the actual goal; the `pio` is just to make sure the build database exists. If the grep finds no enum named like that, search for whatever the existing kind enum is called.)

Locate the existing enum (likely `enum ota_writer_msg_kind_t`) and the union of payload arms.

- [ ] **Step 2: Add `OTA_WR_LOCAL_FLASH_REQ` to the enum**

Find the existing enum in `lib/OtaWriter/include/OtaWriterQueueMessage.h`. Add a new value at the end (use the next sequential integer; if existing values are `OTA_WR_BEGIN = 0, OTA_WR_DATA = 1, ...`, add `OTA_WR_LOCAL_FLASH_REQ = <next>`). Match the existing style (with or without explicit numeric values).

Example shape — adapt to actual existing layout:

```c
typedef enum {
    OTA_WR_BEGIN = 0,
    OTA_WR_DATA = 1,
    OTA_WR_END = 2,
    OTA_WR_WATCHDOG_FIRE = 3,
    OTA_WR_STATS_FIRE = 4,
    OTA_WR_LOCAL_FLASH_REQ = 5,   // NEW: master self-flash request from OtaForwarder
} ota_writer_msg_kind_t;
```

- [ ] **Step 3: Add `local_flash_req` to the payload union**

In the same file, find the payload union (likely inside `queue_ota_writer_msg_t`). Add the new arm at the end:

```c
struct {
    char     firmwarePath[64];     // null-terminated; staged path on master
    uint32_t expectedSize;         // bytes (from stat())
    uint8_t  expectedSha256[32];   // for streaming-SHA comparison
} local_flash_req;
```

If the file has a `freeOtaWriterMsg` helper that lists which kinds have malloc'd union arms, add `OTA_WR_LOCAL_FLASH_REQ` to the comment listing kinds that do NOT need freeing (the path is inline, no malloc).

- [ ] **Step 4: Add `OTA_FWD_LOCAL_FLASH_RESULT` to OtaForwarder's enum**

Edit `lib/OtaForwarder/include/OtaForwarderQueueMessage.h`. Find the existing enum (likely `ota_forwarder_msg_kind_t`). Phase A added `OTA_FWD_VERSION_CONFIRM_TIMEOUT = 10` — add the next sequential value:

```c
OTA_FWD_LOCAL_FLASH_RESULT = 11,   // NEW: master self-flash result from OtaWriter
```

- [ ] **Step 5: Add `local_flash_result` to OtaForwarder's payload union**

In the same file, add the new arm at the end of the union:

```c
struct {
    uint8_t  status;                // OtaFlashStatus value (OK or FAILED)
    uint8_t  errorReasonLen;        // 0..62
    char     errorReason[63];       // populated on FAILED; not NUL-terminated
} local_flash_result;
```

If the file has a `freeOtaForwarderMsg` helper, add the new kind to the no-free list comment (inline string, no malloc).

- [ ] **Step 6: Build both boards (no logic change, type-level only)**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed (the new enum values + union arms are unused at this point but compile cleanly).

- [ ] **Step 7: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 478/478 still pass.

- [ ] **Step 8: Commit**

```bash
git add lib/OtaWriter/include/OtaWriterQueueMessage.h \
        lib/OtaForwarder/include/OtaForwarderQueueMessage.h
git commit -m "$(cat <<'EOF'
feat(ota): add OTA_WR_LOCAL_FLASH_REQ + OTA_FWD_LOCAL_FLASH_RESULT message kinds

Phase C scaffolding for the master self-flash loopback. OtaForwarder will
post OTA_WR_LOCAL_FLASH_REQ to otaWriterQueue (with firmware path +
expected size + expected SHA); OtaWriter will post OTA_FWD_LOCAL_FLASH_RESULT
back to otaForwarderQueue (with OK/FAILED + reason). Both arms are inline
(no malloc) so the free-helper comments are updated to list them as
no-free kinds. Handlers stubbed in next tasks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — `OtaWriter::handleLocalFlashReq` implementation

**Files:**
- Modify: `lib/OtaWriter/include/OtaWriter.hpp`
- Modify: `lib/OtaWriter/src/OtaWriter.cpp`

The handler runs on `otaWriterTask` (preserving the single-task-state invariant). It does the whole local-flash sequence inline: file open → esp_ota_begin → fread + esp_ota_write loop + SHA → esp_ota_end → read-back verify → esp_ota_set_boot_partition → result post.

No native test for this task — `OtaWriter` is in `lib_ignore` for `[env:test]` (same constraint Phase A hit). Bench validation in Task 10.

- [ ] **Step 1: Declare `handleLocalFlashReq` in the header**

Edit `lib/OtaWriter/include/OtaWriter.hpp`. In the private section near the other `handle*` declarations (around line 56-59):

Before:
```cpp
    void handleBegin(queue_ota_writer_msg_t &msg);
    void handleData(queue_ota_writer_msg_t &msg);
    void handleEnd(queue_ota_writer_msg_t &msg);
    void handleWatchdogFire();
```

After:
```cpp
    void handleBegin(queue_ota_writer_msg_t &msg);
    void handleData(queue_ota_writer_msg_t &msg);
    void handleEnd(queue_ota_writer_msg_t &msg);
    void handleWatchdogFire();
    // Phase C — master self-flash. Runs the full flash sequence inline on
    // otaWriterTask: open file, esp_ota_begin, fread + esp_ota_write loop
    // + streaming SHA, esp_ota_end, read-back verify, esp_ota_set_boot_partition.
    // Posts OTA_FWD_LOCAL_FLASH_RESULT back to otaForwarderQueue. Does NOT
    // call esp_restart — OtaForwarder handles the reboot after emitting
    // FW_DEPLOY_DONE.
    void handleLocalFlashReq(queue_ota_writer_msg_t &msg);
```

- [ ] **Step 2: Implement `handleLocalFlashReq` in the .cpp**

Edit `lib/OtaWriter/src/OtaWriter.cpp`. Add the new method near the other `handle*` implementations (a reasonable place is after `handleEnd` but before `handleWatchdogFire`). The method body:

```cpp
void OtaWriter::handleLocalFlashReq(queue_ota_writer_msg_t &msg)
{
    const char *path = msg.local_flash_req.firmwarePath;
    uint32_t expectedSize = msg.local_flash_req.expectedSize;
    const uint8_t *expectedSha = msg.local_flash_req.expectedSha256;

    ESP_LOGI(TAG, "handleLocalFlashReq: path=%s expectedSize=%u", path, expectedSize);

    // Helper to post the result back to otaForwarderQueue. Declared as a
    // lambda so the failure paths can call it before returning. We look up
    // the forwarder queue via the global singleton — OtaWriter doesn't hold
    // a direct handle (would create a circular dependency at construction).
    auto postResult = [](OtaFlashStatus status, const std::string &reason) {
        queue_ota_forwarder_msg_t out{};
        out.kind = OTA_FWD_LOCAL_FLASH_RESULT;
        out.local_flash_result.status = static_cast<uint8_t>(status);
        std::size_t n = std::min(reason.size(), sizeof(out.local_flash_result.errorReason));
        out.local_flash_result.errorReasonLen = static_cast<uint8_t>(n);
        std::memcpy(out.local_flash_result.errorReason, reason.data(), n);
        // AstrOs_OtaForwarder is the global singleton; expose its queue
        // via a new accessor (see step 3 below).
        QueueHandle_t q = AstrOs_OtaForwarder.getForwarderQueue();
        if (q == nullptr)
        {
            ESP_LOGE(TAG, "handleLocalFlashReq: otaForwarderQueue null; result dropped");
            return;
        }
        if (xQueueSend(q, &out, 0) != pdTRUE)
        {
            ESP_LOGE(TAG, "handleLocalFlashReq: otaForwarderQueue full; result dropped");
        }
    };

    active_.store(true);

    // ─── Step 1: open + size-check the firmware file ─────────────────
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: fopen(%s) failed", path);
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, "firmware_open_failed");
        return;
    }
    struct stat st{};
    if (stat(path, &st) != 0 || static_cast<uint32_t>(st.st_size) != expectedSize)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: size mismatch (stat=%ld expected=%u)",
                 (long)st.st_size, expectedSize);
        std::fclose(f);
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, "firmware_size_mismatch");
        return;
    }

    // ─── Step 2: open the inactive OTA partition ──────────────────────
    inactivePartition_ = esp_ota_get_next_update_partition(NULL);
    if (inactivePartition_ == nullptr)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: esp_ota_get_next_update_partition returned null");
        std::fclose(f);
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, "no_update_partition");
        return;
    }

    esp_err_t err = esp_ota_begin(inactivePartition_, OTA_SIZE_UNKNOWN, &otaHandle_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: esp_ota_begin failed: %s", esp_err_to_name(err));
        std::fclose(f);
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, esp_err_to_name(err));
        return;
    }

    // ─── Step 3: stream the file in 4 KB chunks ──────────────────────
    AstrOsSha256_init(&shaCtx_);
    shaActive_ = true;
    constexpr size_t kChunkBytes = 4096;
    uint8_t buf[kChunkBytes];
    size_t totalRead = 0;
    while (totalRead < expectedSize)
    {
        size_t want = std::min(kChunkBytes, static_cast<size_t>(expectedSize - totalRead));
        size_t got = std::fread(buf, 1, want, f);
        if (got != want)
        {
            ESP_LOGE(TAG, "handleLocalFlashReq: fread short (%zu of %zu) at offset %zu",
                     got, want, totalRead);
            std::fclose(f);
            esp_ota_abort(otaHandle_);
            resetOtaHandleAndSha();
            active_.store(false);
            postResult(OtaFlashStatus::FAILED, "firmware_read_short");
            return;
        }
        err = esp_ota_write(otaHandle_, buf, got);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "handleLocalFlashReq: esp_ota_write failed at offset %zu: %s",
                     totalRead, esp_err_to_name(err));
            std::fclose(f);
            esp_ota_abort(otaHandle_);
            resetOtaHandleAndSha();
            active_.store(false);
            postResult(OtaFlashStatus::FAILED, esp_err_to_name(err));
            return;
        }
        AstrOsSha256_update(&shaCtx_, buf, got);
        totalRead += got;
    }
    std::fclose(f);

    // ─── Step 4: finalize streaming SHA, compare to expected ─────────
    uint8_t streamedDigest[32];
    AstrOsSha256_final(&shaCtx_, streamedDigest);
    shaActive_ = false;
    if (std::memcmp(streamedDigest, expectedSha, 32) != 0)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: streaming SHA mismatch");
        esp_ota_abort(otaHandle_);
        resetOtaHandleAndSha();
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, "sha_mismatch");
        return;
    }

    // ─── Step 5: esp_ota_end (commits the writes; partition pointer not yet flipped) ──
    err = esp_ota_end(otaHandle_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: esp_ota_end failed: %s", esp_err_to_name(err));
        // esp_ota_end releases the handle internally even on failure.
        otaHandle_ = 0;
        resetOtaHandleAndSha();
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, esp_err_to_name(err));
        return;
    }
    otaHandle_ = 0;

    // ─── Step 6: read-back-rehash verify ─────────────────────────────
    AstrOsSha256Ctx rbCtx;
    AstrOsSha256_init(&rbCtx);
    constexpr size_t kRbBufSize = 4096;
    uint8_t rbBuf[kRbBufSize];
    for (size_t off = 0; off < expectedSize;)
    {
        size_t chunk = std::min(kRbBufSize, static_cast<size_t>(expectedSize - off));
        err = esp_partition_read(inactivePartition_, off, rbBuf, chunk);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "handleLocalFlashReq: esp_partition_read at off=%zu failed: %s",
                     off, esp_err_to_name(err));
            resetOtaHandleAndSha();
            active_.store(false);
            postResult(OtaFlashStatus::FAILED, esp_err_to_name(err));
            return;
        }
        AstrOsSha256_update(&rbCtx, rbBuf, chunk);
        off += chunk;
    }
    uint8_t readbackDigest[32];
    AstrOsSha256_final(&rbCtx, readbackDigest);
    if (std::memcmp(readbackDigest, expectedSha, 32) != 0)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: read-back SHA mismatch");
        resetOtaHandleAndSha();
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, "readback_mismatch");
        return;
    }

    // ─── Step 7: flip the boot partition pointer ─────────────────────
    err = esp_ota_set_boot_partition(inactivePartition_);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "handleLocalFlashReq: esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        resetOtaHandleAndSha();
        active_.store(false);
        postResult(OtaFlashStatus::FAILED, esp_err_to_name(err));
        return;
    }

    // ─── Step 8: success ─────────────────────────────────────────────
    ESP_LOGI(TAG, "handleLocalFlashReq: success — boot partition flipped");
    resetOtaHandleAndSha();
    active_.store(false);
    postResult(OtaFlashStatus::OK, "");
}
```

- [ ] **Step 3: Add `OtaForwarder::getForwarderQueue()` public accessor**

The `handleLocalFlashReq` lambda needs the OtaForwarder's queue. Add a public accessor.

Edit `lib/OtaForwarder/include/OtaForwarder.hpp`. In the public section near `isWireBusy`:

```cpp
    // Returns the otaForwarderQueue handle so OtaWriter can post
    // OTA_FWD_LOCAL_FLASH_RESULT to it from the local-flash path. Returns
    // nullptr before Init is called. Thread-safe (handle is set once at Init
    // and never changes thereafter).
    QueueHandle_t getForwarderQueue() const noexcept
    {
        return otaForwarderQueue_;
    }
```

The `otaForwarderQueue_` member already exists; this is just a const getter.

- [ ] **Step 4: Add dispatch case in `OtaWriter::process()`**

Edit `lib/OtaWriter/src/OtaWriter.cpp`. Find `OtaWriter::process(queue_ota_writer_msg_t &msg)`. It already dispatches on `msg.kind` (a switch statement). Add a new case:

```cpp
case OTA_WR_LOCAL_FLASH_REQ:
    handleLocalFlashReq(msg);
    break;
```

Place it near the other handler cases. Match the existing switch style.

- [ ] **Step 5: Add includes if needed**

`handleLocalFlashReq` uses `std::fopen`, `stat`, `std::memcmp`, `std::min`. Confirm these are visible in the .cpp's includes. If not, add at the top with the other system includes:

```cpp
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
```

Also confirm `OtaForwarder.hpp` is visible (for the `AstrOs_OtaForwarder` symbol). If not, add `#include <OtaForwarder.hpp>` near the existing lib includes. If there's a circular dependency risk (OtaForwarder includes OtaWriter), forward-declare instead:

```cpp
// Forward declarations to avoid a circular include with OtaForwarder.
class OtaForwarder;
extern OtaForwarder AstrOs_OtaForwarder;
```

Use whichever pattern the codebase already uses elsewhere for OtaForwarder cross-references.

- [ ] **Step 6: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 7: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 478/478 still pass (no new tests; existing tests untouched).

- [ ] **Step 8: Commit**

```bash
git add lib/OtaWriter/include/OtaWriter.hpp \
        lib/OtaWriter/src/OtaWriter.cpp \
        lib/OtaForwarder/include/OtaForwarder.hpp
git commit -m "$(cat <<'EOF'
feat(ota): OtaWriter::handleLocalFlashReq — master self-flash on otaWriterTask

Phase C core. Runs the full self-flash sequence inline on otaWriterTask
(preserves OtaWriter's single-task-state invariant): file open + size
check, esp_ota_begin, fread + esp_ota_write loop with streaming SHA,
esp_ota_end, read-back-rehash verify, esp_ota_set_boot_partition.
Posts OTA_FWD_LOCAL_FLASH_RESULT back to otaForwarderQueue via the new
OtaForwarder::getForwarderQueue() accessor. Does NOT call esp_restart —
OtaForwarder orchestrates the reboot after emitting FW_DEPLOY_DONE.

Every failure path routes through "report FAILED, no reboot" — partition
flip is the last step, so any earlier failure leaves the bootloader
pointing at the running image. Wire-protocol path
(handleBegin/handleData/handleEnd) untouched; this is a separate code
path that bypasses BulkReceiver and the OTA_*_ACK sends entirely.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — OtaForwarder `Phase::MASTER_SELF_FLASHING` scaffold + stubs

**Files:**
- Modify: `lib/OtaForwarder/include/OtaForwarder.hpp`
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

Type-level scaffold: add the new phase, the two new members, declare stub methods, add dispatch + switch cases. Real logic lands in Tasks 5 and 6.

- [ ] **Step 1: Extend the Phase enum**

Edit `lib/OtaForwarder/include/OtaForwarder.hpp`. Locate the existing `enum class Phase` (Phase A had it ending with `AWAITING_VERSION_CONFIRMED = 5, BETWEEN_PADAWANS = 6`):

Before:
```cpp
    enum class Phase : uint8_t
    {
        IDLE = 0,
        AWAITING_BEGIN_ACK = 1,
        STREAMING = 2,
        AWAITING_END_ACK = 3,
        AWAITING_FLASH_RESULT = 4,
        AWAITING_VERSION_CONFIRMED = 5,
        BETWEEN_PADAWANS = 6,
    };
```

After:
```cpp
    enum class Phase : uint8_t
    {
        IDLE = 0,
        AWAITING_BEGIN_ACK = 1,
        STREAMING = 2,
        AWAITING_END_ACK = 3,
        AWAITING_FLASH_RESULT = 4,
        AWAITING_VERSION_CONFIRMED = 5,
        MASTER_SELF_FLASHING = 6,   // Phase C — master local-flash in flight; OtaWriter is running
        BETWEEN_PADAWANS = 7,
    };
```

(Internal enum — renumber is safe. No wire exposure.)

- [ ] **Step 2: Add `masterRowDeferred_` + `masterRowOriginalIndex_` members**

In the same header, in the private section near other per-deploy state, add:

```cpp
    // Phase C — master row deferral. Master always self-flashes last per
    // the cross-milestone design; these track that the row was seen + at
    // what index, so the result inserts at the original position when
    // handleLocalFlashResult fires (preserves operator-submitted order in
    // FW_DEPLOY_DONE).
    bool masterRowDeferred_ = false;
    size_t masterRowOriginalIndex_ = 0;
```

- [ ] **Step 3: Declare the two new private methods**

In the same header, in the private methods section (near `abortCurrentPadawan` and `finishCurrentPadawanAndAdvance`):

```cpp
    // Phase C — master self-flash machinery.
    void startMasterSelfFlash();
    void handleLocalFlashResult(queue_ota_forwarder_msg_t &msg);
```

- [ ] **Step 4: Stub both methods in the .cpp**

Edit `lib/OtaForwarder/src/OtaForwarder.cpp`. Add stubs near the other phase-handler methods. Real bodies land in Tasks 5 and 6:

```cpp
void OtaForwarder::startMasterSelfFlash()
{
    // Stub; populated in Task 6.
}

void OtaForwarder::handleLocalFlashResult(queue_ota_forwarder_msg_t &msg)
{
    // Stub; populated in Task 6.
    (void)msg;
}
```

- [ ] **Step 5: Add dispatch case in `process()`**

In the same .cpp, find `OtaForwarder::process(queue_ota_forwarder_msg_t &msg)`. Add a new case in the dispatch switch (near `OTA_FWD_VERSION_CONFIRM_TIMEOUT`):

```cpp
case OTA_FWD_LOCAL_FLASH_RESULT:
    handleLocalFlashResult(msg);
    break;
```

- [ ] **Step 6: Add `MASTER_SELF_FLASHING` case in `handleStatsFire`**

In the same .cpp, find the phase-string switch inside `handleStatsFire` (search for `case Phase::AWAITING_VERSION_CONFIRMED` to locate it). Add a new case:

```cpp
case Phase::MASTER_SELF_FLASHING:
    return "MASTER_SELF_FLASHING";
```

This silences `-Wswitch` and gives the stats log a readable phase string.

- [ ] **Step 7: Add `PadawanStatus::PENDING` case in `emitDeployDoneAndReset` switch**

In the same .cpp, find the `switch (r.status)` block in `emitDeployDoneAndReset` (around line 826). Insert a new case BEFORE the `default`:

Before:
```cpp
case PadawanStatus::FAILED:
    statusStr = "FAILED";
    break;
default:
    ESP_LOGE(TAG, "Unmapped PadawanStatus=%d; defaulting to FAILED", (int)r.status);
    ...
```

After:
```cpp
case PadawanStatus::FAILED:
    statusStr = "FAILED";
    break;
case PadawanStatus::PENDING:
    statusStr = "PENDING";
    break;
default:
    ESP_LOGE(TAG, "Unmapped PadawanStatus=%d; defaulting to FAILED", (int)r.status);
    ...
```

- [ ] **Step 8: Reset deferral state in `emitDeployDoneAndReset`**

In the same `emitDeployDoneAndReset` body (after the `wire` push loop, around the existing `deployMsgId_.clear()` block), add the two new resets:

```cpp
    deployMsgId_.clear();
    deployTransferId_.clear();
    orderList_.clear();
    nextOrderIdx_ = 0;
    results_.clear();
    masterRowDeferred_ = false;          // NEW
    masterRowOriginalIndex_ = 0;          // NEW
    phase_ = Phase::IDLE;
    active_.store(false);
    wireBusy_.store(false);
```

- [ ] **Step 9: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 10: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 478/478 still pass.

- [ ] **Step 11: Commit**

```bash
git add lib/OtaForwarder/include/OtaForwarder.hpp \
        lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "$(cat <<'EOF'
feat(ota): add Phase::MASTER_SELF_FLASHING scaffold + stubs (Phase C)

Type-level scaffolding for master self-flash orchestration. Adds: Phase
enum value (shifts BETWEEN_PADAWANS from 6 to 7), masterRowDeferred_ +
masterRowOriginalIndex_ members, startMasterSelfFlash +
handleLocalFlashResult stubs, dispatch case for OTA_FWD_LOCAL_FLASH_RESULT,
MASTER_SELF_FLASHING case in handleStatsFire phase-string switch, and
PadawanStatus::PENDING case in the emitDeployDoneAndReset wire-status
switch (so a stray push of PENDING in the deferral logic doesn't trip
the "unmapped status" default). Deferral state is reset in
emitDeployDoneAndReset alongside the other per-deploy fields.

Behavioral wiring lands in Tasks 5 and 6.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — OtaForwarder master-row deferral in `startNextPadawan`

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

Replace the existing master-row-skip block (which records `FAILED("master_self_flash_pending")`) with the deferral logic. Update the padawan-loop-exhausted handler to dispatch to `startMasterSelfFlash` (which is still a stub — real logic in Task 6).

- [ ] **Step 1: Replace the existing master-row-skip block**

In `lib/OtaForwarder/src/OtaForwarder.cpp`, find the master-row-skip block (currently around line 580-586, search for `master_self_flash_pending`):

Before:
```cpp
        // Skip the master self-flash entry — encoded as the all-zero MAC by
        // the Pi-side FW_DEPLOY_BEGIN convention (see the matching
        // "00:00:00:00:00:00" / "master" pair in
        // astros_serial_protocol_tests fixtures). Self-flash isn't wired
        // yet; record FAILED so the deploy advances to the padawans.
        if (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "00:00:00:00:00:00")
        {
            results_.push_back({orderList_[nextOrderIdx_], PadawanStatus::FAILED, "", "master_self_flash_pending"});
            nextOrderIdx_++;
            continue;
        }
```

After:
```cpp
        // Phase C — defer master row until after all padawan rows complete.
        // Master always self-flashes last per the cross-milestone design.
        // Record nothing now — the deferred row's result gets inserted at
        // this original index in handleLocalFlashResult so the
        // FW_DEPLOY_DONE row order matches the operator-submitted order
        // list. Master MAC sentinel is the all-zero MAC per the Pi-side
        // FW_DEPLOY_BEGIN convention.
        if (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "00:00:00:00:00:00")
        {
            masterRowDeferred_ = true;
            masterRowOriginalIndex_ = nextOrderIdx_;
            nextOrderIdx_++;
            continue;
        }
```

- [ ] **Step 2: Update the padawan-loop-exhausted handler**

In the same function, find the block that fires when the order list is exhausted (currently around line 589-592, search for `nextOrderIdx_ >= orderList_.size()`):

Before:
```cpp
        if (nextOrderIdx_ >= orderList_.size())
        {
            emitDeployDoneAndReset();
            return;
        }
```

After:
```cpp
        if (nextOrderIdx_ >= orderList_.size())
        {
            if (masterRowDeferred_)
            {
                // Padawan loop done; now do the master self-flash. Result
                // dispatch + DEPLOY_DONE happen in handleLocalFlashResult.
                startMasterSelfFlash();
                return;
            }
            emitDeployDoneAndReset();
            return;
        }
```

- [ ] **Step 3: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed. (Behavioral effect: deploys that include the master row now silently advance through the padawan loop with no recorded master result until Task 6 wires up `startMasterSelfFlash` — but since `startMasterSelfFlash` is a stub that does nothing, master-included deploys will currently emit DEPLOY_DONE with NO master row at all. That's intentional for this task; Task 6 fills it in.)

Actually correction — since `startMasterSelfFlash` is a stub that does nothing and never emits DEPLOY_DONE, the deploy state machine will hang in `Phase::IDLE` waiting forever after the padawan loop. **Don't deploy master-included orders between Tasks 5 and 6.** The intermediate state ships only briefly between commits.

- [ ] **Step 4: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 478/478 still pass.

- [ ] **Step 5: Commit**

```bash
git add lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "$(cat <<'EOF'
feat(ota): defer master row in OtaForwarder order-list walk (Phase C)

Replace the master_self_flash_pending placeholder skip with proper
deferral: when the all-zero MAC is encountered, set masterRowDeferred_
+ masterRowOriginalIndex_ and continue. When the padawan loop exhausts,
dispatch to startMasterSelfFlash (currently a stub — real logic lands
next task). NOTE: this commit on its own leaves master-included deploys
hanging in IDLE after the padawan loop completes. Don't deploy with
master in the order list until Task 6 lands the real
startMasterSelfFlash + handleLocalFlashResult bodies.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — OtaForwarder `startMasterSelfFlash` + `handleLocalFlashResult` + native tests

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`
- Modify: `test/test_native/astros_ota_forwarder_tests.cpp`

Real implementations replace the stubs from Task 4. Add stand-in tests for the deferral + result-handling logic (since OtaForwarder is in `lib_ignore` for the native test env, same constraint Phase A hit).

- [ ] **Step 1: Replace `startMasterSelfFlash` stub with real implementation**

In `lib/OtaForwarder/src/OtaForwarder.cpp`:

```cpp
void OtaForwarder::startMasterSelfFlash()
{
    ESP_LOGI(TAG, "startMasterSelfFlash: beginning master self-flash");

    // ─── Resolve staged firmware path ───────────────────────────────
    auto firmwarePathOpt = AstrOs_OtaReceiver.getLastFirmwarePath();
    if (!firmwarePathOpt.has_value())
    {
        ESP_LOGW(TAG, "startMasterSelfFlash: no staged firmware; recording FAILED");
        insertMasterRow(PadawanStatus::FAILED, "", "no_firmware");
        emitDeployDoneAndReset();
        return;
    }
    const std::string &firmwarePath = *firmwarePathOpt;

    // ─── stat() for size ────────────────────────────────────────────
    struct stat st{};
    if (stat(firmwarePath.c_str(), &st) != 0)
    {
        ESP_LOGE(TAG, "startMasterSelfFlash: stat(%s) failed", firmwarePath.c_str());
        insertMasterRow(PadawanStatus::FAILED, "", "firmware_stat_failed");
        emitDeployDoneAndReset();
        return;
    }
    uint32_t expectedSize = static_cast<uint32_t>(st.st_size);

    // ─── Compute SHA-256 ────────────────────────────────────────────
    // Same one-shot helper startCurrentPadawan uses (search for "computeFileSha256"
    // or equivalent in this file). If no helper exists yet, factor it out from
    // startCurrentPadawan into a private method computeFileSha256(path, outSha).
    uint8_t expectedSha[32];
    if (!computeFileSha256(firmwarePath, expectedSha))
    {
        ESP_LOGE(TAG, "startMasterSelfFlash: SHA computation failed");
        insertMasterRow(PadawanStatus::FAILED, "", "firmware_sha_failed");
        emitDeployDoneAndReset();
        return;
    }

    // ─── Build request + post to otaWriterQueue ─────────────────────
    queue_ota_writer_msg_t req{};
    req.kind = OTA_WR_LOCAL_FLASH_REQ;
    std::strncpy(req.local_flash_req.firmwarePath, firmwarePath.c_str(),
                 sizeof(req.local_flash_req.firmwarePath) - 1);
    req.local_flash_req.firmwarePath[sizeof(req.local_flash_req.firmwarePath) - 1] = '\0';
    req.local_flash_req.expectedSize = expectedSize;
    std::memcpy(req.local_flash_req.expectedSha256, expectedSha, 32);

    QueueHandle_t writerQueue = AstrOs_OtaWriter.getWriterQueue();
    if (writerQueue == nullptr)
    {
        ESP_LOGE(TAG, "startMasterSelfFlash: AstrOs_OtaWriter queue null — not init on master?");
        insertMasterRow(PadawanStatus::FAILED, "", "writer_queue_null");
        emitDeployDoneAndReset();
        return;
    }

    phase_ = Phase::MASTER_SELF_FLASHING;
    currentControllerId_ = "00:00:00:00:00:00";  // for downstream logging identity

    if (xQueueSend(writerQueue, &req, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "startMasterSelfFlash: otaWriterQueue full — recording FAILED");
        insertMasterRow(PadawanStatus::FAILED, "", "writer_queue_full");
        phase_ = Phase::IDLE;
        emitDeployDoneAndReset();
        return;
    }
}
```

- [ ] **Step 2: Replace `handleLocalFlashResult` stub with real implementation**

In the same .cpp, replace the stub:

```cpp
void OtaForwarder::handleLocalFlashResult(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::MASTER_SELF_FLASHING)
    {
        ESP_LOGW(TAG, "handleLocalFlashResult: ignored — phase=%d", (int)phase_);
        return;
    }

    OtaFlashStatus status = static_cast<OtaFlashStatus>(msg.local_flash_result.status);
    std::string reason(msg.local_flash_result.errorReason,
                       msg.local_flash_result.errorReasonLen);

    if (status == OtaFlashStatus::OK)
    {
        // Optimistic PENDING — server resolves to OK on post-reboot
        // self-POLL_ACK version match (or to FAILED on 90 s timeout).
        // Master reboots before the server can do that resolution; the
        // post-reboot heartbeat is the signal.
        insertMasterRow(PadawanStatus::PENDING, "", "awaiting_post_reboot_version");
        emitDeployDoneAndReset();  // emits DEPLOY_DONE with full results vector

        ESP_LOGI(TAG, "Master self-flash complete; rebooting in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        // not reached
    }

    // FAILED: master partition was NOT switched; bootloader still points
    // at the running image. Record + emit DEPLOY_DONE; no reboot.
    ESP_LOGE(TAG, "Master self-flash failed: %s", reason.c_str());
    insertMasterRow(PadawanStatus::FAILED, "", reason);
    emitDeployDoneAndReset();
}
```

- [ ] **Step 3: Add the `insertMasterRow` helper as a private method**

In `lib/OtaForwarder/include/OtaForwarder.hpp`, in the private methods section:

```cpp
    // Insert the master row at masterRowOriginalIndex_ (clamped to
    // results_.size()) so FW_DEPLOY_DONE preserves the operator-submitted
    // order. Called twice in handleLocalFlashResult (OK path + FAILED path)
    // so the logic is centralized.
    void insertMasterRow(PadawanStatus status, const std::string &finalVersion,
                         const std::string &errorReason);
```

In `lib/OtaForwarder/src/OtaForwarder.cpp`, implement near `handleLocalFlashResult`:

```cpp
void OtaForwarder::insertMasterRow(PadawanStatus status, const std::string &finalVersion,
                                   const std::string &errorReason)
{
    size_t idx = std::min(masterRowOriginalIndex_, results_.size());
    results_.insert(results_.begin() + idx,
                    {"00:00:00:00:00:00", status, finalVersion, errorReason});
}
```

- [ ] **Step 4: Add `OtaWriter::getWriterQueue()` accessor**

`startMasterSelfFlash` calls `AstrOs_OtaWriter.getWriterQueue()`. Add the accessor.

Edit `lib/OtaWriter/include/OtaWriter.hpp`, in the public section near `isActive`:

```cpp
    // Returns the otaWriterQueue handle so OtaForwarder can post
    // OTA_WR_LOCAL_FLASH_REQ to it from the master self-flash path. Returns
    // nullptr before Init is called (e.g., on a node where OtaWriter wasn't
    // initialized). Thread-safe (handle is set once at Init).
    QueueHandle_t getWriterQueue() const noexcept
    {
        return otaWriterQueue_;
    }
```

- [ ] **Step 5: Factor out `computeFileSha256` if not already a helper**

Search the existing `OtaForwarder.cpp` for the SHA-256 file-hashing block in `startNextPadawan` (search for `AstrOsSha256_init` or `computeFileSha256`). If a `computeFileSha256(path, outSha)` private method already exists, the call in `startMasterSelfFlash` (Step 1) just works.

If the SHA computation is inline in `startNextPadawan`, extract it:

In `OtaForwarder.hpp` private methods:

```cpp
    // Computes SHA-256 of a file on disk. Returns false on fopen/fread
    // failure. Used by startNextPadawan (for padawan deploy) and
    // startMasterSelfFlash (for master self-flash).
    bool computeFileSha256(const std::string &path, uint8_t outSha[32]) const;
```

In `OtaForwarder.cpp`:

```cpp
bool OtaForwarder::computeFileSha256(const std::string &path, uint8_t outSha[32]) const
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return false;
    }
    AstrOsSha256Ctx ctx;
    AstrOsSha256_init(&ctx);
    constexpr size_t kBufSize = 512;  // matches existing convention in startNextPadawan
    uint8_t buf[kBufSize];
    size_t got;
    while ((got = std::fread(buf, 1, kBufSize, f)) > 0)
    {
        AstrOsSha256_update(&ctx, buf, got);
    }
    bool readOk = !std::ferror(f);
    std::fclose(f);
    if (!readOk)
    {
        return false;
    }
    AstrOsSha256_final(&ctx, outSha);
    return true;
}
```

Then update `startNextPadawan`'s existing SHA-compute block to call `computeFileSha256(firmwarePath, expectedSha)` instead of inlining the loop. Match the call-site error-handling pattern (push `firmware_sha_failed`, advance).

- [ ] **Step 6: Write the failing native tests via stand-in**

In `test/test_native/astros_ota_forwarder_tests.cpp`, add three new test cases extending the existing stand-in pattern (`VersionConfirmStandIn` from Phase A). The master-row-deferral logic is in `startNextPadawan` and `handleLocalFlashResult` — both are in production, both can be mirrored in a sibling stand-in `MasterRowDeferralStandIn`.

Add the stand-in class near `VersionConfirmStandIn`:

```cpp
// Mirrors OtaForwarder::startNextPadawan's master-row-deferral block and
// OtaForwarder::handleLocalFlashResult + insertMasterRow. Mirror code; if
// production drifts, this stand-in must be updated too. Documented as
// known limitation; bench QA in .docs/qa/ota-upgrade-pr-set-2.md is the
// on-target backstop.
class MasterRowDeferralStandIn
{
public:
    struct Result
    {
        std::string controllerId;
        AstrOsEspNowProtocol::PadawanStatus status;
        std::string finalVersion;
        std::string errorReason;
    };

    void deployOrder(std::vector<std::string> order)
    {
        orderList_ = std::move(order);
        nextOrderIdx_ = 0;
        results_.clear();
        masterRowDeferred_ = false;
        masterRowOriginalIndex_ = 0;
        deployActive_ = true;
    }

    // Advance one position through the order list (mirrors the loop inside
    // startNextPadawan; padawan rows are simulated as immediately-OK for
    // test brevity, since we're only testing the master-row mechanics).
    bool stepOnce()
    {
        if (!deployActive_ || nextOrderIdx_ >= orderList_.size())
        {
            return false;
        }
        if (orderList_[nextOrderIdx_] == "00:00:00:00:00:00")
        {
            masterRowDeferred_ = true;
            masterRowOriginalIndex_ = nextOrderIdx_;
            nextOrderIdx_++;
            return true;
        }
        // Simulate padawan row completing OK.
        results_.push_back({orderList_[nextOrderIdx_],
                            AstrOsEspNowProtocol::PadawanStatus::OK,
                            "1.2.3", ""});
        nextOrderIdx_++;
        return true;
    }

    bool padawanLoopExhausted() const
    {
        return nextOrderIdx_ >= orderList_.size();
    }

    bool masterDeferred() const
    {
        return masterRowDeferred_;
    }

    // Mirrors insertMasterRow + handleLocalFlashResult OK path.
    void completeMasterFlashOK()
    {
        insertMasterRow(AstrOsEspNowProtocol::PadawanStatus::PENDING, "",
                        "awaiting_post_reboot_version");
        deployActive_ = false;
    }

    // Mirrors insertMasterRow + handleLocalFlashResult FAILED path.
    void completeMasterFlashFailed(const std::string &reason)
    {
        insertMasterRow(AstrOsEspNowProtocol::PadawanStatus::FAILED, "", reason);
        deployActive_ = false;
    }

    const std::vector<Result> &results() const
    {
        return results_;
    }

private:
    void insertMasterRow(AstrOsEspNowProtocol::PadawanStatus status,
                         const std::string &finalVersion,
                         const std::string &errorReason)
    {
        size_t idx = std::min(masterRowOriginalIndex_, results_.size());
        results_.insert(results_.begin() + idx,
                        {"00:00:00:00:00:00", status, finalVersion, errorReason});
    }

    std::vector<std::string> orderList_;
    size_t nextOrderIdx_ = 0;
    std::vector<Result> results_;
    bool masterRowDeferred_ = false;
    size_t masterRowOriginalIndex_ = 0;
    bool deployActive_ = false;
};
```

Then add three test cases:

```cpp
TEST(MasterRowDeferral, MasterFirst_InsertsAtIndex0)
{
    MasterRowDeferralStandIn s;
    s.deployOrder({"00:00:00:00:00:00", "AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"});

    // Step through the deploy. Master gets deferred; padawan A + B run.
    while (s.stepOnce()) {}
    EXPECT_TRUE(s.padawanLoopExhausted());
    EXPECT_TRUE(s.masterDeferred());
    EXPECT_EQ(s.results().size(), 2u);  // only padawans recorded so far

    s.completeMasterFlashOK();

    ASSERT_EQ(s.results().size(), 3u);
    EXPECT_EQ(s.results()[0].controllerId, "00:00:00:00:00:00");  // master at index 0
    EXPECT_EQ(s.results()[0].status, AstrOsEspNowProtocol::PadawanStatus::PENDING);
    EXPECT_EQ(s.results()[1].controllerId, "AA:BB:CC:DD:EE:01");
    EXPECT_EQ(s.results()[2].controllerId, "AA:BB:CC:DD:EE:02");
}

TEST(MasterRowDeferral, MasterMiddle_InsertsAtOriginalIndex)
{
    MasterRowDeferralStandIn s;
    s.deployOrder({"AA:BB:CC:DD:EE:01", "00:00:00:00:00:00", "AA:BB:CC:DD:EE:02"});

    while (s.stepOnce()) {}
    s.completeMasterFlashOK();

    ASSERT_EQ(s.results().size(), 3u);
    EXPECT_EQ(s.results()[0].controllerId, "AA:BB:CC:DD:EE:01");
    EXPECT_EQ(s.results()[1].controllerId, "00:00:00:00:00:00");  // master inserted at index 1
    EXPECT_EQ(s.results()[1].status, AstrOsEspNowProtocol::PadawanStatus::PENDING);
    EXPECT_EQ(s.results()[2].controllerId, "AA:BB:CC:DD:EE:02");
}

TEST(MasterRowDeferral, MasterLast_AppendsAtEnd)
{
    MasterRowDeferralStandIn s;
    s.deployOrder({"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02", "00:00:00:00:00:00"});

    while (s.stepOnce()) {}
    s.completeMasterFlashOK();

    ASSERT_EQ(s.results().size(), 3u);
    EXPECT_EQ(s.results()[0].controllerId, "AA:BB:CC:DD:EE:01");
    EXPECT_EQ(s.results()[1].controllerId, "AA:BB:CC:DD:EE:02");
    EXPECT_EQ(s.results()[2].controllerId, "00:00:00:00:00:00");
    EXPECT_EQ(s.results()[2].status, AstrOsEspNowProtocol::PadawanStatus::PENDING);
}

TEST(MasterRowDeferral, MasterFlashFailed_InsertsFAILEDAtOriginalIndex)
{
    MasterRowDeferralStandIn s;
    s.deployOrder({"AA:BB:CC:DD:EE:01", "00:00:00:00:00:00"});

    while (s.stepOnce()) {}
    s.completeMasterFlashFailed("ESP_FAIL");

    ASSERT_EQ(s.results().size(), 2u);
    EXPECT_EQ(s.results()[1].controllerId, "00:00:00:00:00:00");
    EXPECT_EQ(s.results()[1].status, AstrOsEspNowProtocol::PadawanStatus::FAILED);
    EXPECT_EQ(s.results()[1].errorReason, "ESP_FAIL");
}
```

If `AstrOsEspNowProtocol.hpp` isn't already included, add `#include "AstrOsEspNowProtocol.hpp"` near the top.

- [ ] **Step 7: Run tests to verify they pass**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 482/482 (478 prior + 4 new). All four new MasterRowDeferral cases pass.

- [ ] **Step 8: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 9: Commit**

```bash
git add lib/OtaForwarder/include/OtaForwarder.hpp \
        lib/OtaForwarder/src/OtaForwarder.cpp \
        lib/OtaWriter/include/OtaWriter.hpp \
        test/test_native/astros_ota_forwarder_tests.cpp
git commit -m "$(cat <<'EOF'
feat(ota): real startMasterSelfFlash + handleLocalFlashResult (Phase C)

Replaces the Task 4 stubs with the full master self-flash orchestration:
startMasterSelfFlash resolves the staged firmware path, stats for size,
computes SHA-256, builds and posts OTA_WR_LOCAL_FLASH_REQ to
otaWriterQueue (via new AstrOs_OtaWriter.getWriterQueue() accessor).
handleLocalFlashResult on OK inserts the master row as PENDING at its
original order-list index, emits FW_DEPLOY_DONE, sleeps 500ms (UART
drain), esp_restart. On FAILED inserts as FAILED and emits DEPLOY_DONE
without rebooting.

Factored out computeFileSha256 from startNextPadawan into a private
method so both deploy paths share the SHA helper. insertMasterRow
helper centralizes the order-preserving insert.

Four new native tests via MasterRowDeferralStandIn (mirrors production
control flow): master-first, master-middle, master-last, master-failed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7 — `sendPollAckNak` returns `bool`

**Files:**
- Modify: `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`
- Modify: `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`

Change the return type from `void` to `bool`. Returns `true` if `xQueueSend` to `serialQueue` succeeded; `false` on queue-full. Existing call sites that don't care about the return remain compatible (C++ accepts discarded return values).

- [ ] **Step 1: Change the header signature**

Edit `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp`. Locate the existing declaration of `sendPollAckNak` (search for the function name). Change `void` to `bool`:

Before:
```cpp
    void sendPollAckNak(std::string mac, std::string name, std::string fingerprint,
                        std::string firmwareVersion, std::string variant, bool isAck);
```

After:
```cpp
    // Returns true if the serial-queue post succeeded; false if the queue was
    // full and the message was dropped. Most callers discard the return (the
    // self-POLL_ACK message is best-effort). main.cpp's master polling code
    // checks the return to gate firstSelfPollAckSent_ for OTA rollback.
    bool sendPollAckNak(std::string mac, std::string name, std::string fingerprint,
                        std::string firmwareVersion, std::string variant, bool isAck);
```

- [ ] **Step 2: Change the implementation to return success**

Edit `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`. Find the existing `sendPollAckNak` body (around line 175). Locate the `xQueueSend` block at the end:

Before:
```cpp
void AstrOsSerialMsgHandler::sendPollAckNak(std::string mac, std::string name, std::string fingerprint,
                                            std::string firmwareVersion, std::string variant, bool isAck)
{
    // ... [build response] ...

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}
```

After:
```cpp
bool AstrOsSerialMsgHandler::sendPollAckNak(std::string mac, std::string name, std::string fingerprint,
                                            std::string firmwareVersion, std::string variant, bool isAck)
{
    // ... [build response — unchanged] ...

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
        return false;
    }
    return true;
}
```

- [ ] **Step 3: Verify existing call sites still compile**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro 2>&1 | grep -i "error\|warning" | head -10`

Expected: no errors. C++ allows discarded return values silently; the callers in `main.cpp` and other places that currently call `sendPollAckNak(...)` without checking the result continue to work unchanged.

- [ ] **Step 4: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 5: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 482/482 still pass.

- [ ] **Step 6: Commit**

```bash
git add lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp \
        lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp
git commit -m "$(cat <<'EOF'
refactor(serial): sendPollAckNak returns bool for queue-post success

Phase C prep. main.cpp's master polling code needs to gate
firstSelfPollAckSent_ on actual serial-queue success (mirrors padawan-side
discipline of acting on send success, not "function returned"). Bool
return surfaces xQueueSend pdTRUE/pdFALSE without changing behavior for
any existing caller — C++ silently discards unchecked returns. The
existing failure path (ESP_LOGW + free) is unchanged; we just also
report false up to the caller.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8 — Lift master OtaWriter init guards in `main.cpp`

**Files:**
- Modify: `src/main.cpp`

OtaWriter is currently padawan-only at `main.cpp:337-364`. Phase C needs it initialized on master too so `startMasterSelfFlash` has a queue to post to. Lift the init guards. KEEP the guard around `AstrOs_EspNow.setOtaWriterQueue` — master never receives wire OTA_BEGIN/OTA_DATA/OTA_END from a peer, so the ESP-NOW dispatch path stays padawan-only.

- [ ] **Step 1: Locate the existing OtaWriter init block**

Run: `grep -n "otaWriterQueue\|otaWriterTask\|OtaWriter.Init\|setOtaWriterQueue" src/main.cpp | head -20`

Identify the lines that:
- Create `otaWriterQueue` (xQueueCreate)
- Create `otaWriterTask` (xTaskCreatePinnedToCore)
- Call `AstrOs_OtaWriter.Init(otaWriterQueue)`
- Call `AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue)`

The first three are inside `if (!isMasterNode.load()) { ... }` blocks. The fourth is also inside one (and must STAY inside one).

- [ ] **Step 2: Lift the three init guards, keep the fourth**

Edit `src/main.cpp`. For each of the three blocks that init OtaWriter queue / task / Init, remove the `if (!isMasterNode.load())` wrapper but keep the code:

Before (illustrative — actual block boundaries vary):
```cpp
    if (!isMasterNode.load())
    {
        otaWriterQueue = xQueueCreate(16, sizeof(queue_ota_writer_msg_t));
        if (otaWriterQueue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create otaWriterQueue — aborting init");
            // ...
        }
    }
```

After:
```cpp
    // OtaWriter queue is needed on BOTH master (for self-flash via
    // OTA_WR_LOCAL_FLASH_REQ from OtaForwarder) and padawan (for
    // wire-driven OTA_BEGIN/OTA_DATA/OTA_END). The dispatch into this
    // queue from incoming ESP-NOW frames is gated separately via
    // setOtaWriterQueue (padawan-only).
    otaWriterQueue = xQueueCreate(16, sizeof(queue_ota_writer_msg_t));
    if (otaWriterQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create otaWriterQueue — aborting init");
        // ...
    }
```

Apply the same de-guarding to the `xTaskCreatePinnedToCore` block for `otaWriterTask` and the `AstrOs_OtaWriter.Init(otaWriterQueue)` call.

For `AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue)`, KEEP the guard — master should not have ESP-NOW dispatch into OtaWriter:

```cpp
    if (!isMasterNode.load())
    {
        // Padawan-only: route incoming wire OTA frames (OTA_BEGIN/DATA/END)
        // into OtaWriter. Master uses the loopback (OTA_WR_LOCAL_FLASH_REQ
        // posted by OtaForwarder) instead.
        AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue);
    }
```

- [ ] **Step 3: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 4: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 482/482.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat(ota): init OtaWriter on master too (Phase C)

Phase C needs OtaWriter available on master so startMasterSelfFlash can
post OTA_WR_LOCAL_FLASH_REQ to its queue. Lift the !isMasterNode guards
around otaWriterQueue creation, otaWriterTask creation, and
AstrOs_OtaWriter.Init. KEEP the guard around setOtaWriterQueue — master
should never have incoming wire OTA frames routed into OtaWriter (those
are master-to-padawan only; the master uses the loopback path instead).

On master, OtaWriter's active_ stays false until startMasterSelfFlash
posts a request, so the polling-pause gate at main.cpp:520 is correctly
inert until master actually self-flashes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9 — Master `firstSelfPollAckSent_` + `mark_app_valid` in `main.cpp`

**Files:**
- Modify: `src/main.cpp`

Mirror the padawan-side Phase A pattern at the master's self-POLL_ACK call site (around line 543). Static atomic + compare_exchange + `esp_ota_mark_app_valid_cancel_rollback` on first successful queue post. Discriminate log levels: INFO on success, suppressed for the expected `ESP_ERR_NOT_FOUND` (no rollback pending on subsequent boots), WARN for unexpected returns.

No native test — hardware-only behavior. Bench-validated in Task 10 (case C.4).

- [ ] **Step 1: Locate the master self-POLL_ACK call site**

Run: `grep -n "sendPollAckNak.*00:00:00:00:00:00\|sendPollAckNak.*master" src/main.cpp`

Expected: one match around line 543 (the master self-poll inside the polling-timer callback).

- [ ] **Step 2: Wrap the call with firstSelfPollAckSent_ + mark_valid**

Edit `src/main.cpp`. Find the block:

Before:
```cpp
            // Master self-POLL_ACK: report own build variant so the server can
            // pick the right firmware asset at flash time.
            AstrOs_SerialMsgHandler.sendPollAckNak("00:00:00:00:00:00", "master", std::string(fingerprint),
                                                   AstrOsConstants::Version, AstrOsConstants::Variant, true);
```

After:
```cpp
            // Master self-POLL_ACK: report own build variant so the server can
            // pick the right firmware asset at flash time.
            static std::atomic<bool> firstSelfPollAckSent_{false};
            bool serialQueued = AstrOs_SerialMsgHandler.sendPollAckNak(
                "00:00:00:00:00:00", "master", std::string(fingerprint),
                AstrOsConstants::Version, AstrOsConstants::Variant, true);

            if (serialQueued)
            {
                // First successful self-POLL_ACK queue post post-reboot proves:
                // NVS read worked (fingerprint), serial queue alive, message-
                // service serialization alive, FreeRTOS scheduler healthy.
                // Enough evidence to cancel auto-rollback for a freshly-flashed
                // master image. mark_valid is documented as safe to call
                // unconditionally; returns ESP_ERR_NOT_FOUND if no rollback was
                // pending (the normal subsequent-boot case).
                bool expected = false;
                if (firstSelfPollAckSent_.compare_exchange_strong(expected, true))
                {
                    esp_err_t markErr = esp_ota_mark_app_valid_cancel_rollback();
                    if (markErr == ESP_OK)
                    {
                        ESP_LOGI(TAG, "Master OTA rollback cancelled — running image is now valid");
                    }
                    else if (markErr != ESP_ERR_NOT_FOUND)
                    {
                        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback returned %s",
                                 esp_err_to_name(markErr));
                    }
                }
            }
```

- [ ] **Step 3: Add includes if needed**

The new code uses `std::atomic` and `esp_ota_mark_app_valid_cancel_rollback`. Confirm these are visible at the top of `main.cpp`:

```cpp
#include <atomic>
#include <esp_ota_ops.h>
```

Add either if missing. (Both are likely already present — `std::atomic` is used for `isMasterNode`, `esp_ota_ops.h` is used elsewhere for OTA work.)

- [ ] **Step 4: Build both boards**

Run: `~/.platformio/penv/bin/pio run -e lolin_d32_pro && ~/.platformio/penv/bin/pio run -e metro_s3`

Expected: both succeed.

- [ ] **Step 5: Native suite still passes**

Run: `~/.platformio/penv/bin/pio test -e test`

Expected: 482/482.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat(ota): master mark_app_valid on first successful self-POLL_ACK (Phase C)

Mirrors padawan-side Phase A pattern (AstrOsEspNow::sendPollAck) on the
master's self-POLL_ACK serial-TX call site in the polling timer.
firstSelfPollAckSent_ static atomic gated by compare_exchange_strong;
on first true success of serialQueued, calls
esp_ota_mark_app_valid_cancel_rollback. Log-level discrimination: INFO
on genuine cancel, suppressed for the expected ESP_ERR_NOT_FOUND (no
rollback pending on subsequent boots), WARN for unexpected returns.

Closes one of the deferred follow-up items from Phase A's PR-toolkit
review (log-level discrimination for the mark_valid call path).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10 — Bench QA plan Phase C section + server pre-merge gate note

**Files:**
- Modify: `.docs/qa/ota-upgrade-pr-set-2.md`

Append Phase C bench cases + spare-board precondition. Add a pre-merge gate note about verifying server `FW_DEPLOY_DONE` parser handles `"PENDING"`.

- [ ] **Step 1: Append the Phase C section**

Edit `.docs/qa/ota-upgrade-pr-set-2.md`. After the existing Phase A section, append:

```markdown
## Phase C — Master self-flash + `PadawanStatus::PENDING`

### Pre-merge gate

Before this Phase C firmware merges, confirm the AstrOs.Server's
`FW_DEPLOY_DONE` parser handles `status="PENDING"` without crashing.
Three possible server behaviors:

| Server behavior | Phase C firmware ship status |
|---|---|
| Permissive: renders as "unknown row" or similar | OK to ship firmware first |
| Strict but defaults to FAILED | OK to ship; master row shows "Failed" until server PR lands |
| Crashes / rejects the whole DEPLOY_DONE | **BLOCKING** — sequence the server PR first |

The server-side Phase C follow-up (`Finalizing` deploy state +
self-POLL_ACK version watcher + 90 s timeout + UI spinner on PENDING
rows) is a separate PR.

### Preconditions

- A **spare** master-role board with USB tether. **Do not test on the
  production master** — use a reconfigured spare so a brick doesn't take
  down the fleet. Either lolin_d32_pro or metro_s3 can be reconfigured as
  a master via the existing serial-driven `isMasterNode` toggle (see
  `main.cpp:1711-1717`).
- At least one padawan board on the previous firmware (so C.1's deploy
  exercises both the padawan loop and master self-flash).
- The padawan and spare-master boards have already been re-flashed once
  via USB to install the rollback-enabled bootloader from Phase A's
  `862503b` (per Phase A precondition).
- Pi connected to master via USB serial at 115200 baud.
- AstrOs.Server running.
- A "next" firmware build with a bumped `VERSION` so the .bin's
  `esp_app_desc_t.version` differs from what the boards are currently
  running.

### Test case C.1 — Master self-flash happy path

1. From the server UI, upload the next .bin to the master.
2. Trigger a deploy targeting `["00:00:00:00:00:00", "<padawan_mac>"]`
   (master + 1 padawan; order list with master first to also verify
   ordering preservation).
3. **Expected serial-log progression on the spare master**:
   - Padawan loop runs first (master row deferred, no log noise yet).
   - Padawan completes: `Version confirmed for <mac>: '<new_version>' ...`
   - Master starts self-flash: `startMasterSelfFlash: beginning master self-flash`.
   - OtaWriter logs the flash sequence: `handleLocalFlashReq: ...` →
     `handleLocalFlashReq: success — boot partition flipped`.
   - OtaForwarder: `Master self-flash complete; rebooting in 500ms`.
   - DEPLOY_DONE emitted: master row = PENDING (at index 0), padawan row = OK (at index 1).
   - Master reboots; comes up on new firmware.
   - First self-POLL_ACK (~2 s post-boot): `Master OTA rollback cancelled — running image is now valid`.
4. **Server-side verification** (with server-side Phase C PR landed): deploy
   record initially shows master as PENDING; resolves to OK after the
   post-reboot self-POLL_ACK lands.
5. **Without server-side Phase C PR**: master row stays PENDING in the
   UI indefinitely — this is the documented operational state for the
   firmware-first ship window.

### Test case C.2 — Master flash failure injection

1. Apply temporary debug patch in `OtaWriter::handleLocalFlashReq` just
   before `esp_ota_set_boot_partition`:
   ```cpp
   err = ESP_FAIL;  // FORCED FAILURE
   goto failed;  // adapt to local control flow — see existing failure pattern
   ```
   (or simply replace the next `if (err != ESP_OK)` branch with an
   unconditional FAILED post.)
2. Build and flash the spare master with the patched firmware.
3. From the server, trigger a deploy targeting `["00:00:00:00:00:00"]`
   (master-only — minimizes test surface).
4. **Expected**:
   - OtaWriter never calls `esp_ota_set_boot_partition`.
   - OtaWriter posts FAILED result with reason `"ESP_FAIL"`.
   - OtaForwarder logs `Master self-flash failed: ESP_FAIL`.
   - DEPLOY_DONE emitted with master row = FAILED.
   - Master does NOT reboot; still running the old firmware.
5. Revert the debug patch.

### Test case C.3 — Master boot-crash + auto-rollback

The highest-stakes test. Validates that the rollback safety net (active
since Phase A's `862503b`) actually catches a bad master image.

1. Apply temporary debug patch in `app_main`:
   ```cpp
   void app_main() {
       abort();  // FORCED CRASH
       // ... rest unchanged
   }
   ```
2. Build with a bumped VERSION so the .bin is identifiable.
3. Stage on the spare master via the server; deploy targeting
   `["00:00:00:00:00:00"]` (master-only).
4. **Expected**:
   - Master self-flash succeeds locally; DEPLOY_DONE emitted with
     master row = PENDING; master reboots.
   - New image immediately aborts.
   - Bootloader detects PENDING_VERIFY image crashed → reverts to old
     partition on next boot.
   - Master comes up running OLD firmware (verify via serial banner
     version).
   - Self-POLL_ACK starts firing again reporting OLD version.
   - Server-side (with Phase C server PR): PENDING resolves to FAILED on
     90 s `post_reboot_timeout`.
   - Master is still functional, just on the old image.
5. **If the master DOES NOT come up on old firmware** (rollback failed —
   shouldn't happen with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`), the
   spare master is bricked. Recover via
   `.docs/qa/ota-upgrade-recovery-via-usb.md`. File a bug; this is a
   regression of Phase A's `862503b`.
6. Revert the debug patch.

### Test case C.4 — `firstSelfPollAckSent_` fires exactly once

Validates the mark_valid wiring.

1. Normal boot of the spare master (no flash). Observe serial log.
2. **Expected**: exactly one `Master OTA rollback cancelled — running image is now valid`
   log message — OR silent (no message) if the boot was not from a
   PENDING_VERIFY state (subsequent boots after C.1 success path).
3. Wait through 5 poll cycles (~10 s). **Expected**: NO additional
   rollback-path log messages.

### Recovery

If C.3 or any other test bricks the spare master AND auto-rollback also
fails, USB-recover via `.docs/qa/ota-upgrade-recovery-via-usb.md`. The
procedure already covers both supported boards.
```

- [ ] **Step 2: Commit**

```bash
git add .docs/qa/ota-upgrade-pr-set-2.md
git commit -m "$(cat <<'EOF'
docs(ota): Phase C bench QA plan + server-side pre-merge gate

Bench test plan for Phase C's hardware-only behavior (master self-flash,
PENDING wire status, master rollback on crash). Four test cases:
- C.1 master happy path with order-preservation verification
- C.2 forced flash failure (no reboot, master stays on old firmware)
- C.3 master boot-crash + auto-rollback (highest stakes; validates the
  rollback safety net actually engages with bootloader rollback config)
- C.4 firstSelfPollAckSent_ fires exactly once

Spare-board precondition documented: do NOT test on production master.
Pre-merge gate added: confirm AstrOs.Server FW_DEPLOY_DONE parser
handles status="PENDING" before firmware ships (or sequence server PR
first if not).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review checklist (for the writer)

- [x] **Spec coverage** — verified each design section maps to tasks:
  - Loopback wire architecture (design §1) → Tasks 2, 3
  - OtaForwarder state machine + deferral (design §2) → Tasks 4, 5, 6
  - Wire format `PadawanStatus::PENDING` (design §3) → Task 1
  - Master `mark_app_valid` trigger (design §4) → Tasks 7, 9
  - Master OtaWriter initialization (design §5) → Task 8
  - Bench plan (design §6) → Task 10
  - Server pre-merge gate → Task 10
- [x] **No placeholders** — every step has either exact code or exact
  command. Two areas explicitly call out adapt-to-local:
  - Task 2 Step 1's grep is to find the actual enum/union shape (the
    file's exact layout varies)
  - Task 8 Step 2's exact block boundaries vary depending on how the
    existing `if (!isMasterNode.load())` blocks are structured
- [x] **Type consistency** — `Phase::MASTER_SELF_FLASHING`, `masterRowDeferred_`,
  `masterRowOriginalIndex_`, `insertMasterRow`, `startMasterSelfFlash`,
  `handleLocalFlashResult`, `OTA_WR_LOCAL_FLASH_REQ`,
  `OTA_FWD_LOCAL_FLASH_RESULT`, `getForwarderQueue`, `getWriterQueue`,
  `computeFileSha256`, `firstSelfPollAckSent_`, `PadawanStatus::PENDING`
  all named consistently across tasks. Status strings `"OK"`, `"FAILED"`,
  `"PENDING"` consistent between Task 1 (serializer validation) and
  Task 4 (wire-string switch case).
- [x] **Commit cadence** — 10 commits total, consistent with the established
  `feat(ota): / refactor(ota): / docs(ota):` prefix style.
