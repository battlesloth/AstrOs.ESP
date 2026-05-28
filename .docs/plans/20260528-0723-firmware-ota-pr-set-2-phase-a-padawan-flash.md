# Firmware OTA PR set 2 — Phase A — Padawan flash + master version-confirm gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the PR-set-1 placeholder in `OtaWriter::handleEnd` with a real `esp_ota_set_boot_partition` + `esp_restart` flash commit, add an auto-rollback safety net keyed on first-POLL_ACK-post-reboot, and gate master-side SUCCESS on a new `AWAITING_VERSION_CONFIRMED` heartbeat-based phase.

**Architecture:** Two-sided change. Padawan side: replace the placeholder block at `lib/OtaWriter/src/OtaWriter.cpp:603-628`, then add a `firstPollAckSent_` flag-driven `esp_ota_mark_app_valid_cancel_rollback` call inside `AstrOsEspNow::sendPollAck` so the new image only commits itself after proving it can talk back. Master side: parse `esp_app_desc_t` from the staged `.bin` to learn the expected new version (new PURE helper in `lib_native/AstrOsUtility`), store per-peer last-known version in `AstrOsEspNow`, add a new `Phase::AWAITING_VERSION_CONFIRMED` state in `OtaForwarder` that ticks every 1 s comparing the peer's reported version to the expected version, and times out at 15 s with `FAILED("version_unconfirmed")`.

**Tech Stack:** ESP-IDF 5.x (`esp_ota_*` APIs), FreeRTOS (timers + mutexes), PlatformIO `[env:test]` GoogleTest native suite, `lib_native/` PURE libs.

**Design spec:** `.docs/plans/20260528-0709-firmware-ota-pr-set-2-design.md`

---

## File Structure

### New files
- `lib_native/AstrOsUtility/include/AstrOsEspAppDescParser.hpp` — PURE header declaring the parser namespace + result struct.
- `lib_native/AstrOsUtility/src/AstrOsEspAppDescParser.cpp` — PURE source implementing the parse.
- `test/test_native/astros_esp_app_desc_parser_tests.cpp` — native GoogleTest cases for the parser.
- `.docs/qa/ota-upgrade-pr-set-2.md` — bench plan; this PR adds the Phase A section.
- `.docs/qa/ota-upgrade-recovery-via-usb.md` — USB-recovery procedure for a bricked board.

### Modified files
- `lib/OtaWriter/src/OtaWriter.cpp` — replace the placeholder block (lines 603-628).
- `lib/AstrOsEspNow/include/AstrOsEspNowService.hpp` — add `getPeerVersion(mac) → std::string` accessor declaration, add private `peerVersions_` map and `firstPollAckSent_` flag.
- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — store version in `handlePollAck` under `peersMutex`; implement `getPeerVersion`; call `esp_ota_mark_app_valid_cancel_rollback` on the first successful POLL_ACK send.
- `lib/OtaForwarder/include/OtaForwarder.hpp` — add `Phase::AWAITING_VERSION_CONFIRMED` enum value, `expectedNewVersion_` member, `versionConfirmTimer_` handle, new private methods.
- `lib/OtaForwarder/src/OtaForwarder.cpp` — parse expected version in `startCurrentPadawan`; transition to `AWAITING_VERSION_CONFIRMED` on `OTA_FLASH_RESULT(OK)` (replacing immediate result-record); tick-driven version-match polling; 15 s timeout handler.
- `test/test_native/astros_ota_forwarder_tests.cpp` — extend with state-machine cases for the new phase (happy path, timeout, wrong version).

### Files intentionally not modified
- `lib_native/AstrOsEspNowPeers/include/espnow_peer.h` — `espnow_peer_t` is byte-for-byte NVS-persisted; no field additions. Version goes in a parallel runtime-only map in `AstrOsEspNow`.

---

## Task 1 — EspAppDescParser PURE helper (TDD)

**Files:**
- Create: `lib_native/AstrOsUtility/include/AstrOsEspAppDescParser.hpp`
- Create: `lib_native/AstrOsUtility/src/AstrOsEspAppDescParser.cpp`
- Create: `test/test_native/astros_esp_app_desc_parser_tests.cpp`

Parses the ESP-IDF `esp_app_desc_t` block from a raw `.bin` buffer. Layout (stable since IDF 3.x): 24 B `esp_image_header_t` (first byte `0xE9`), then 8 B `esp_image_segment_header_t`, then 256 B `esp_app_desc_t` (magic word `0xABCD5432` little-endian, then secure-version u32, then 8 B reserved, then `version[32]` null-terminated ASCII, then `project_name[32]`, then `time[16]`, then `date[16]`, then `idf_ver[32]`, then 32 B app_elf_sha256, then 80 B reserved). We only need to extract `version[32]`.

- [ ] **Step 1: Write the failing test file**

Create `test/test_native/astros_esp_app_desc_parser_tests.cpp`:

```cpp
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include "AstrOsEspAppDescParser.hpp"

namespace
{
    // Minimal synthetic .bin prefix: image header (1st byte = 0xE9, rest zeroed),
    // segment header (all zero), app desc (magic + secure_ver + 8B reserved + version).
    // Total prefix size needed: 24 + 8 + 4 + 4 + 8 + 32 = 80 bytes.
    constexpr std::size_t kPrefixSize = 80;

    std::array<uint8_t, kPrefixSize> makeValidPrefix(const char *version)
    {
        std::array<uint8_t, kPrefixSize> buf{};
        buf[0] = 0xE9; // esp_image_header_t magic
        // segment header (offset 24) — content unused
        // app desc magic at offset 32, little-endian 0xABCD5432
        buf[32] = 0x32;
        buf[33] = 0x54;
        buf[34] = 0xCD;
        buf[35] = 0xAB;
        // version[32] starts at offset 32 + 4 (magic) + 4 (secure_ver) + 8 (reserved) = 48
        std::memcpy(buf.data() + 48, version, std::strlen(version));
        return buf;
    }
}

TEST(EspAppDescParser, ParsesValidVersion)
{
    auto buf = makeValidPrefix("1.2.3-test");
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.version, "1.2.3-test");
    EXPECT_TRUE(result.error.empty());
}

TEST(EspAppDescParser, RejectsBadImageMagic)
{
    auto buf = makeValidPrefix("1.0.0");
    buf[0] = 0x00; // corrupt esp_image_header magic
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.version.empty());
    EXPECT_NE(result.error.find("image_magic"), std::string::npos);
}

TEST(EspAppDescParser, RejectsBadAppDescMagic)
{
    auto buf = makeValidPrefix("1.0.0");
    buf[32] = 0x00; // corrupt app_desc magic
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("app_desc_magic"), std::string::npos);
}

TEST(EspAppDescParser, RejectsTruncatedInput)
{
    auto buf = makeValidPrefix("1.0.0");
    auto result = AstrOsEspAppDescParser::parse(buf.data(), 40); // less than 80
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("truncated"), std::string::npos);
}

TEST(EspAppDescParser, HandlesEmptyVersionString)
{
    auto buf = makeValidPrefix("");
    auto result = AstrOsEspAppDescParser::parse(buf.data(), buf.size());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.version, "");
}
```

- [ ] **Step 2: Run the tests and verify they fail (header doesn't exist)**

Run: `pio test -e test -f test_native --filter "*esp_app_desc*"`

Expected: COMPILE FAIL with `fatal error: AstrOsEspAppDescParser.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `lib_native/AstrOsUtility/include/AstrOsEspAppDescParser.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AstrOsEspAppDescParser
{
    struct Result
    {
        bool ok = false;
        std::string version;
        std::string error;
    };

    // Parse esp_app_desc_t from the leading bytes of an ESP32 firmware .bin.
    // Expects at least 80 bytes: 24 B image header + 8 B segment header +
    // 4 B app_desc magic + 4 B secure_version + 8 B reserved + 32 B version.
    //
    // Validates:
    //   buf[0] == 0xE9                 (esp_image_header magic)
    //   buf[32..35] == 32 54 CD AB     (esp_app_desc magic_word, little-endian 0xABCD5432)
    //
    // Reads version[32] starting at offset 48, treated as a null-terminated
    // ASCII string (truncated at first '\0' or at 32 bytes).
    Result parse(const uint8_t *buf, std::size_t len);
}
```

- [ ] **Step 4: Implement the parser**

Create `lib_native/AstrOsUtility/src/AstrOsEspAppDescParser.cpp`:

```cpp
#include "AstrOsEspAppDescParser.hpp"

#include <cstring>

namespace AstrOsEspAppDescParser
{
    namespace
    {
        constexpr std::size_t kImageHeaderSize = 24;
        constexpr std::size_t kSegmentHeaderSize = 8;
        constexpr std::size_t kAppDescMagicOffset = kImageHeaderSize + kSegmentHeaderSize; // 32
        constexpr std::size_t kVersionOffset = kAppDescMagicOffset + 4 + 4 + 8;            // 48
        constexpr std::size_t kVersionMaxLen = 32;
        constexpr std::size_t kRequiredLen = kVersionOffset + kVersionMaxLen;              // 80

        constexpr uint8_t kImageMagic = 0xE9;
        // 0xABCD5432 little-endian = 32 54 CD AB
        constexpr uint8_t kAppDescMagic[4] = {0x32, 0x54, 0xCD, 0xAB};
    }

    Result parse(const uint8_t *buf, std::size_t len)
    {
        Result r;
        if (buf == nullptr || len < kRequiredLen)
        {
            r.error = "truncated";
            return r;
        }
        if (buf[0] != kImageMagic)
        {
            r.error = "image_magic";
            return r;
        }
        if (std::memcmp(buf + kAppDescMagicOffset, kAppDescMagic, 4) != 0)
        {
            r.error = "app_desc_magic";
            return r;
        }

        const char *versionField = reinterpret_cast<const char *>(buf + kVersionOffset);
        std::size_t versionLen = ::strnlen(versionField, kVersionMaxLen);
        r.version.assign(versionField, versionLen);
        r.ok = true;
        return r;
    }
}
```

- [ ] **Step 5: Run the tests and verify they pass**

Run: `pio test -e test -f test_native --filter "*esp_app_desc*"`

Expected: 5/5 PASS.

- [ ] **Step 6: Commit**

```bash
git add lib_native/AstrOsUtility/include/AstrOsEspAppDescParser.hpp \
        lib_native/AstrOsUtility/src/AstrOsEspAppDescParser.cpp \
        test/test_native/astros_esp_app_desc_parser_tests.cpp
git commit -m "$(cat <<'EOF'
feat(ota): add EspAppDescParser PURE helper for reading firmware version from .bin

Phase A foundation — master uses this to learn the expected post-reboot
version of the staged firmware so it can gate VERSION_CONFIRMED on a
heartbeat match. Native-testable: 5 GoogleTest cases cover valid parse,
bad image magic, bad app_desc magic, truncated input, empty version.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Padawan: replace placeholder with real flash commit

**Files:**
- Modify: `lib/OtaWriter/src/OtaWriter.cpp:603-628`

The placeholder calls `sendFlashResult(... FLASH_NOT_IMPLEMENTED ...)` after a 2 s `vTaskDelay`. Real implementation: keep the 2 s delay (preserves visible FLASHING row cadence the server state machine was tested against), then call `esp_ota_set_boot_partition`, report real OK/FAILED status, and on success reboot after a small UART-flush delay.

No native test — this is hardware-only behavior. Bench validation in Task 10.

- [ ] **Step 1: Replace the placeholder block**

At `lib/OtaWriter/src/OtaWriter.cpp:601` the file currently reads:

```cpp
    logSendResult("handleEnd OK END_ACK", sendEndAck(mac, xferId, OtaEndStatus::OK, streamedDigest));

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
    // The vTaskDelay below mirrors the natural PR set 2 cadence so server
    // timing assumptions tested against M4 hold unchanged against PR set 2.
    ESP_LOGI(TAG, "handleEnd: M4 placeholder — sleeping 2s before OTA_FLASH_RESULT(FLASH_NOT_IMPLEMENTED)");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_err_t flashErr = sendFlashResult(mac, xferId, OtaFlashStatus::FLASH_NOT_IMPLEMENTED, "pr_set_1_placeholder");
    if (flashErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleEnd: sendFlashResult failed: %s", esp_err_to_name(flashErr));
    }

    resetOtaHandleAndSha();
}
```

Replace it with:

```cpp
    logSendResult("handleEnd OK END_ACK", sendEndAck(mac, xferId, OtaEndStatus::OK, streamedDigest));

    // Verification passed. END_ACK OK has gone on the wire so the master can
    // emit FW_PROGRESS FLASHING and the UI flash row lights up. Delay 2 s
    // for visible cadence (matches the timing the server state machine was
    // tested against in PR set 1), then flip the boot partition and reboot.
    ESP_LOGI(TAG, "handleEnd: sleeping 2s before esp_ota_set_boot_partition");
    vTaskDelay(pdMS_TO_TICKS(2000));

    esp_err_t bootErr = esp_ota_set_boot_partition(inactivePartition_);
    if (bootErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleEnd: esp_ota_set_boot_partition failed: %s", esp_err_to_name(bootErr));
        esp_err_t flashErr = sendFlashResult(mac, xferId, OtaFlashStatus::FAILED, esp_err_to_name(bootErr));
        if (flashErr != ESP_OK)
        {
            ESP_LOGW(TAG, "handleEnd: sendFlashResult(FAILED) failed: %s", esp_err_to_name(flashErr));
        }
        resetOtaHandleAndSha();
        return;
    }

    ESP_LOGI(TAG, "handleEnd: boot partition flipped; reporting OK and rebooting");
    esp_err_t flashErr = sendFlashResult(mac, xferId, OtaFlashStatus::OK, "");
    if (flashErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleEnd: sendFlashResult(OK) failed: %s — proceeding to reboot anyway", esp_err_to_name(flashErr));
    }

    // 200 ms gives the ESP-NOW send queue + UART log a chance to drain on
    // the wire before esp_restart blows away task state. The master tolerates
    // missing FLASH_RESULT via its flashResultTimer fallback, but we want the
    // common case to deliver it.
    vTaskDelay(pdMS_TO_TICKS(200));
    resetOtaHandleAndSha();
    esp_restart();
    // not reached
}
```

- [ ] **Step 2: Verify the placeholder header reference is gone**

Run: `grep -n "FLASH_NOT_IMPLEMENTED\|pr_set_1_placeholder\|M4 placeholder\|M4 PLACEHOLDER" lib/OtaWriter/src/OtaWriter.cpp`

Expected: zero matches. (The enum value `FLASH_NOT_IMPLEMENTED` itself stays defined in `OtaWirePayloads.hpp` for wire-compatibility; only the call site goes away.)

- [ ] **Step 3: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed with no new warnings.

- [ ] **Step 4: Commit**

```bash
git add lib/OtaWriter/src/OtaWriter.cpp
git commit -m "$(cat <<'EOF'
feat(ota): replace OtaWriter flash placeholder with real esp_ota_set_boot_partition + reboot

Phase A core change. handleEnd's success path now: (1) sends END_ACK OK,
(2) waits 2s to keep FLASHING row visible at the PR-set-1 cadence,
(3) calls esp_ota_set_boot_partition, (4) reports real OK / FAILED with
esp_err_to_name, (5) on success waits 200ms for UART/ESP-NOW flush then
esp_restart. The FLASH_NOT_IMPLEMENTED enum value remains in the wire
format for backward-compat decoding; no padawan emits it anymore.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Padawan: rollback safety net via first-POLL_ACK mark_app_valid

**Files:**
- Modify: `lib/AstrOsEspNow/include/AstrOsEspNowService.hpp`
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

Add a `firstPollAckSent_` atomic flag. In `AstrOsEspNow::sendPollAck`, after a successful `esp_now_send` return, on the `false→true` transition of that flag, call `esp_ota_mark_app_valid_cancel_rollback()`. The ESP-IDF function is documented as idempotent (no-op if running image isn't in `PENDING_VERIFY`), so no state-check guard needed.

- [ ] **Step 1: Add the flag declaration in the header**

Locate the private section of `class AstrOsEspNow` in `lib/AstrOsEspNow/include/AstrOsEspNowService.hpp`. Add near the existing peer-tracking fields:

```cpp
    // Rollback safety net: on the first successful POLL_ACK send post-reboot,
    // call esp_ota_mark_app_valid_cancel_rollback so a newly-flashed image
    // only commits itself after proving it can talk back. If the new image
    // crashes before reaching this point, the bootloader auto-reverts on
    // next boot.
    std::atomic<bool> firstPollAckSent_{false};
```

If `<atomic>` is not already included in the header, add `#include <atomic>` near the other system includes at the top.

- [ ] **Step 2: Wire the mark_app_valid call in sendPollAck**

Locate `AstrOsEspNow::sendPollAck` in `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` (around line 920 currently). The function ends with:

```cpp
    if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending poll ack");
        result = false;
    }

    free(data.data);
    free(destMac);

    return result;
}
```

Replace with:

```cpp
    esp_err_t sendErr = esp_now_send(destMac, data.data, data.size);
    if (sendErr != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending poll ack");
        result = false;
    }
    else
    {
        // First successful POLL_ACK send post-reboot proves: NVS read worked
        // (fingerprint), ESP-NOW peer table is valid, version + variant
        // strings are reachable, and the send queue accepted the frame. That's
        // enough evidence to cancel the auto-rollback for a freshly-flashed
        // image. The mark_valid call is a no-op when the running image isn't
        // in PENDING_VERIFY, so no extra guard is needed.
        bool expected = false;
        if (firstPollAckSent_.compare_exchange_strong(expected, true))
        {
            esp_err_t markErr = esp_ota_mark_app_valid_cancel_rollback();
            if (markErr == ESP_OK)
            {
                ESP_LOGI(TAG, "OTA rollback cancelled — running image is now valid");
            }
            else
            {
                ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback returned %s "
                              "(safe to ignore if image was not in PENDING_VERIFY)",
                         esp_err_to_name(markErr));
            }
        }
    }

    free(data.data);
    free(destMac);

    return result;
}
```

Add `#include "esp_ota_ops.h"` near the other ESP-IDF includes at the top of `AstrOsEspNowService.cpp` if it isn't already present (grep first: `grep esp_ota_ops lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`).

- [ ] **Step 3: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed.

- [ ] **Step 4: Native test guard (purity check)**

The `firstPollAckSent_` atomic and `esp_ota_*` calls live in MIXED code only — `lib/AstrOsEspNow`, not `lib_native/`. Confirm the purity guard still passes:

Run: `pio test -e test`

Expected: existing native suite passes (no new failures, no new tests in this task).

- [ ] **Step 5: Commit**

```bash
git add lib/AstrOsEspNow/include/AstrOsEspNowService.hpp \
        lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
git commit -m "$(cat <<'EOF'
feat(ota): cancel OTA auto-rollback on first successful POLL_ACK send

Phase A safety net. New firmware images boot in PENDING_VERIFY state; if
they crash before calling esp_ota_mark_app_valid_cancel_rollback, the
bootloader auto-reverts on next boot. We trigger the cancel from inside
AstrOsEspNow::sendPollAck on the first successful esp_now_send. Proving
ESP-NOW transmit works is a strong signal the new image came up healthy
enough to do real work. mark_valid is documented as idempotent on already-
valid images, so no extra guard is needed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — Master: per-peer version storage + getPeerVersion accessor

**Files:**
- Modify: `lib/AstrOsEspNow/include/AstrOsEspNowService.hpp`
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

`espnow_peer_t` is byte-for-byte NVS-persisted; we cannot add fields to it. Use a parallel runtime-only map keyed by MAC, populated in `handlePollAck`, read by the new `getPeerVersion` accessor. Guarded by the existing `peersMutex`.

- [x] **Step 1: Add the map and accessor declarations**

In `lib/AstrOsEspNow/include/AstrOsEspNowService.hpp`:

Add `#include <unordered_map>` near the top includes if not present.

In the public section (where `getPeers()` already lives), add:

```cpp
    // Returns the last-known firmware version string reported by the given
    // peer in its most recent POLL_ACK, or an empty string if the peer is
    // unknown or has never been polled successfully. Thread-safe (acquires
    // peersMutex internally). MAC is the canonical "XX:XX:XX:XX:XX:XX"
    // uppercase string used elsewhere in AstrOsEspNow.
    std::string getPeerVersion(const std::string &macString) const;
```

In the private section (where `peersMutex` and other peer-related fields live), add:

```cpp
    // Parallel storage for per-peer last-known firmware version. Not in
    // espnow_peer_t because that struct is NVS-persisted byte-for-byte;
    // version is runtime-only (refreshed every POLL_ACK).
    std::unordered_map<std::string, std::string> peerVersions_;
```

- [x] **Step 2: Implement getPeerVersion**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, add the method (anywhere after the other `AstrOsEspNow::` member functions; near `getPeers` is a natural home):

```cpp
std::string AstrOsEspNow::getPeerVersion(const std::string &macString) const
{
    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getPeerVersion: failed to acquire peersMutex within 1s");
        return "";
    }
    std::string out;
    auto it = this->peerVersions_.find(macString);
    if (it != this->peerVersions_.end())
    {
        out = it->second;
    }
    xSemaphoreGive(this->peersMutex);
    return out;
}
```

- [x] **Step 3: Populate peerVersions_ in handlePollAck**

Locate `AstrOsEspNow::handlePollAck` in the same file (around line 964 currently). After `markPollAckReceived` succeeds but before releasing the mutex, store the version. Find this block:

```cpp
    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "handlePollAck: failed to acquire peersMutex within 1s");
        return false;
    }
    bool known = this->peers.markPollAckReceived(padawanMac);
    xSemaphoreGive(this->peersMutex);
```

Replace with:

```cpp
    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "handlePollAck: failed to acquire peersMutex within 1s");
        return false;
    }
    bool known = this->peers.markPollAckReceived(padawanMac);
    if (known && !peerVersion.empty())
    {
        // Record the reported firmware version for OtaForwarder to consult
        // during AWAITING_VERSION_CONFIRMED. Overwrite is intentional —
        // newer ACKs win.
        this->peerVersions_[padawanMac] = peerVersion;
    }
    xSemaphoreGive(this->peersMutex);
```

`peerVersion` and `padawanMac` are already in scope from earlier in the function (parsed at lines 980-981).

- [x] **Step 4: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed.

- [x] **Step 5: Run native tests (sanity)**

Run: `pio test -e test`

Expected: existing native suite passes.

- [x] **Step 6: Commit**

```bash
git add lib/AstrOsEspNow/include/AstrOsEspNowService.hpp \
        lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
git commit -m "$(cat <<'EOF'
feat(ota): record last-known peer firmware version + add getPeerVersion accessor

Phase A: OtaForwarder will read this during AWAITING_VERSION_CONFIRMED to
detect when a freshly-rebooted padawan reports its new version. Storage is
a parallel std::unordered_map<MacString, Version> guarded by the existing
peersMutex — espnow_peer_t can't grow (NVS-persisted byte-for-byte), and
version is runtime-only anyway. handlePollAck writes; getPeerVersion reads.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — Master: add AWAITING_VERSION_CONFIRMED phase enum + members

**Files:**
- Modify: `lib/OtaForwarder/include/OtaForwarder.hpp`

This task adds only the type-level pieces (enum value, members, method declarations). The transition logic and tick handler come in Tasks 6 and 7 to keep diff sizes small and reviewable.

- [ ] **Step 1: Extend the Phase enum**

Locate the `enum class Phase` block at `lib/OtaForwarder/include/OtaForwarder.hpp:53-63`. Currently:

```cpp
    enum class Phase : uint8_t
    {
        IDLE = 0,
        AWAITING_BEGIN_ACK = 1,
        STREAMING = 2,
        AWAITING_END_ACK = 3,
        AWAITING_FLASH_RESULT = 4,
        BETWEEN_PADAWANS = 5,
    };
```

Add `AWAITING_VERSION_CONFIRMED` after `AWAITING_FLASH_RESULT`:

```cpp
    enum class Phase : uint8_t
    {
        IDLE = 0,
        AWAITING_BEGIN_ACK = 1,
        STREAMING = 2,
        AWAITING_END_ACK = 3,
        AWAITING_FLASH_RESULT = 4,
        AWAITING_VERSION_CONFIRMED = 5, // FLASH_RESULT OK received; waiting on heartbeat-version match
        BETWEEN_PADAWANS = 6,
    };
```

Note: `BETWEEN_PADAWANS` shifts from 5 to 6 — the wire format does not include this enum, it's purely internal state, so renumbering is safe.

- [ ] **Step 2: Add expectedNewVersion_ and timer handle**

In the private members section of `class OtaForwarder` (where the other `esp_timer_handle_t` declarations live), add:

```cpp
    // Phase A: parsed once per padawan from the staged .bin's esp_app_desc_t
    // when entering AWAITING_BEGIN_ACK; consumed during AWAITING_VERSION_CONFIRMED
    // to decide when the heartbeat-reported version matches.
    std::string expectedNewVersion_;

    // 15 s safety bound on AWAITING_VERSION_CONFIRMED. Fires
    // versionConfirmTimerCallback if the padawan never reports the expected
    // version (silent brick or wrong-image flash).
    esp_timer_handle_t versionConfirmTimer_ = nullptr;
```

- [ ] **Step 3: Add private method declarations**

In the private methods section (where `abortCurrentPadawan` and friends are declared), add:

```cpp
    // Phase A — AWAITING_VERSION_CONFIRMED machinery.
    void versionConfirmTimerStart();
    void versionConfirmTimerStop();
    void handleVersionConfirmTimeout();
    void checkPeerVersionForCurrentPadawan(); // called from the 1 s tick
```

- [ ] **Step 4: Add the timer init/teardown lifecycle**

The timer needs to be created in OtaForwarder's `init` (matching how `flashResultTimer_` is created) and deleted in the destructor. Find `flashResultTimer_` references in `OtaForwarder.cpp` and add parallel lines for `versionConfirmTimer_` in the same sequence. Stub the init/teardown methods to satisfy the build:

In `lib/OtaForwarder/src/OtaForwarder.cpp`, add:

```cpp
void OtaForwarder::versionConfirmTimerStart()
{
    if (versionConfirmTimer_ == nullptr)
    {
        return;
    }
    esp_timer_start_once(versionConfirmTimer_, 15ULL * 1000ULL * 1000ULL);
}

void OtaForwarder::versionConfirmTimerStop()
{
    if (versionConfirmTimer_ == nullptr)
    {
        return;
    }
    esp_timer_stop(versionConfirmTimer_);
}

void OtaForwarder::handleVersionConfirmTimeout()
{
    // Stub; populated in Task 8.
}

void OtaForwarder::checkPeerVersionForCurrentPadawan()
{
    // Stub; populated in Task 7.
}
```

Locate the constructor or `init` method where `flashResultTimer_` is created via `esp_timer_create`. Add a parallel block for `versionConfirmTimer_`:

```cpp
    {
        esp_timer_create_args_t args = {};
        args.callback = [](void *self) {
            static_cast<OtaForwarder *>(self)->handleVersionConfirmTimeout();
        };
        args.arg = this;
        args.name = "otaVersionConfirm";
        esp_timer_create(&args, &versionConfirmTimer_);
    }
```

In the destructor (or wherever `flashResultTimer_` is deleted), mirror:

```cpp
    if (versionConfirmTimer_ != nullptr)
    {
        esp_timer_stop(versionConfirmTimer_);
        esp_timer_delete(versionConfirmTimer_);
        versionConfirmTimer_ = nullptr;
    }
```

- [ ] **Step 5: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed (the stubs make compilation clean; transition logic is wired in Task 6).

- [ ] **Step 6: Commit**

```bash
git add lib/OtaForwarder/include/OtaForwarder.hpp \
        lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "$(cat <<'EOF'
feat(ota): add Phase::AWAITING_VERSION_CONFIRMED enum + stubs (Phase A scaffold)

Type-level scaffolding for the new master-side phase that waits on
post-reboot heartbeat version match. Adds: Phase enum value (shifting
BETWEEN_PADAWANS from 5 to 6 — internal-only, no wire-format impact),
expectedNewVersion_ member, versionConfirmTimer_ esp_timer handle +
create/delete lifecycle, and stubbed method declarations. Transition
logic + tick handler land in the next two tasks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — Master: parse expected version at deploy start + transition on FLASH_RESULT(OK)

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

Two changes in this task:
1. In `startCurrentPadawan` (or wherever the padawan's BulkSender is begun), parse the .bin's app desc once and cache `expectedNewVersion_`.
2. In the `OTA_FLASH_RESULT` handler, on `OtaFlashStatus::OK`, replace the current immediate result-record-and-advance with: emit `FW_PROGRESS REBOOTING`, start `versionConfirmTimer_`, transition to `Phase::AWAITING_VERSION_CONFIRMED`. The tick handler that polls for the version match lands in Task 7.

- [x] **Step 1: Add the EspAppDescParser include**

At the top of `lib/OtaForwarder/src/OtaForwarder.cpp` with the other `#include`s:

```cpp
#include "AstrOsEspAppDescParser.hpp"
```

- [x] **Step 2: Parse expected version on deploy start**

In `OtaForwarder::startCurrentPadawan` (or wherever `firmwareTotalSize_` is set from `stat()`, around line 609), after `firmwareTotalSize_` is established but before `bulk_.begin`, add the parse:

```cpp
    // Phase A: read the staged .bin's esp_app_desc_t to learn the expected
    // post-reboot version string. The 80-byte prefix is plenty — the
    // parser only reads bytes 0..79. fseek back to 0 afterward so the
    // streaming send loop starts at the beginning.
    expectedNewVersion_.clear();
    {
        uint8_t prefix[80] = {0};
        if (std::fread(prefix, 1, sizeof(prefix), firmwareFile_) != sizeof(prefix))
        {
            ESP_LOGW(TAG, "Could not read 80-byte prefix from staged .bin (%s); "
                          "version-confirm will fall back to timeout",
                     firmwarePath.c_str());
        }
        else
        {
            auto desc = AstrOsEspAppDescParser::parse(prefix, sizeof(prefix));
            if (desc.ok)
            {
                expectedNewVersion_ = desc.version;
                ESP_LOGI(TAG, "Parsed expected new version '%s' from staged .bin",
                         expectedNewVersion_.c_str());
            }
            else
            {
                ESP_LOGW(TAG, "esp_app_desc parse failed (%s); version-confirm will "
                              "fall back to timeout",
                         desc.error.c_str());
            }
        }
        if (std::fseek(firmwareFile_, 0, SEEK_SET) != 0)
        {
            ESP_LOGE(TAG, "fseek(0) failed on firmware file");
            std::fclose(firmwareFile_);
            firmwareFile_ = nullptr;
            results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_seek_failed"});
            nextOrderIdx_++;
            continue; // (only valid if inside the while loop; otherwise adjust to match local control flow)
        }
    }
```

If the wrapping function is not a `while`-style loop, adapt the `continue` to whatever control-flow recovery already exists in `startCurrentPadawan` after a `firmware_*_failed` push (the file already handles `firmware_seek_failed` once at line 660 — match that pattern).

- [x] **Step 3: Transition to AWAITING_VERSION_CONFIRMED on FLASH_RESULT(OK)**

Locate `handleFlashResult` in `OtaForwarder.cpp` (around line 1180). The current OK path looks like:

```cpp
    OtaFlashStatus status = static_cast<OtaFlashStatus>(msg.flash_result.status);
    std::string wireReason(msg.flash_result.reason, msg.flash_result.reasonLen);

    auto mapped = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(status, wireReason);
    std::string finalVersion;

    ESP_LOGI(TAG, "Flash result for %s: status=%d reason='%s'", currentControllerId_.c_str(), (int)status,
             mapped.errorReason.c_str());
    results_.push_back({currentControllerId_, mapped.padawanStatus, finalVersion, mapped.errorReason});

    finishCurrentPadawanAndAdvance();
}
```

Replace with:

```cpp
    OtaFlashStatus status = static_cast<OtaFlashStatus>(msg.flash_result.status);
    std::string wireReason(msg.flash_result.reason, msg.flash_result.reasonLen);

    auto mapped = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(status, wireReason);

    ESP_LOGI(TAG, "Flash result for %s: status=%d reason='%s'", currentControllerId_.c_str(), (int)status,
             mapped.errorReason.c_str());

    if (status == OtaFlashStatus::OK)
    {
        // Padawan reported flash success and is about to reboot. Don't record
        // SUCCESS yet — wait until heartbeat shows the expected new version.
        // The flash row already lit FLASHING when END_ACK OK landed; now
        // signal the reboot transition for the UI.
        AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "REBOOTING",
                                               firmwareTotalSize_, firmwareTotalSize_, "");

        phase_ = Phase::AWAITING_VERSION_CONFIRMED;
        versionConfirmTimerStart();
        tickTimerStart(); // re-arm the per-second tick (it was stopped after END_ACK)
        return;
    }

    // FAILED or FLASH_NOT_IMPLEMENTED (legacy) — record immediately and advance.
    results_.push_back({currentControllerId_, mapped.padawanStatus, "", mapped.errorReason});
    finishCurrentPadawanAndAdvance();
}
```

Note: `tickTimerStart()` is the existing 1-Hz tick. Confirm its declaration is visible at this call site; if it lives in a different access level, expose it (see the existing `statsTimerStart()` pattern for reference).

- [x] **Step 4: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed.

- [x] **Step 5: Native build sanity**

Run: `pio test -e test`

Expected: existing native suite passes. (No new tests yet — those land in Task 9.)

- [x] **Step 6: Commit**

```bash
git add lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "$(cat <<'EOF'
feat(ota): parse expected version on deploy start + transition to AWAITING_VERSION_CONFIRMED on OK

Phase A — master now reads the staged .bin's esp_app_desc_t once per padawan
and caches expectedNewVersion_ for heartbeat comparison. OTA_FLASH_RESULT(OK)
no longer records SUCCESS immediately; instead emits FW_PROGRESS REBOOTING,
starts the 15 s version-confirm timer, and enters AWAITING_VERSION_CONFIRMED.
FAILED / FLASH_NOT_IMPLEMENTED paths remain unchanged. Tick-driven version
matching lands in the next task; timeout handler lands after that.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7 — Master: tick-driven version-match polling

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

The existing `tickTimer` already fires every 1 s during transfer (for stats logging). In `AWAITING_VERSION_CONFIRMED`, each tick polls `AstrOs_EspNow.getPeerVersion(currentControllerId_)` and compares to `expectedNewVersion_`. On match: emit `FW_PROGRESS VERSION_CONFIRMED`, stop the version-confirm timer, push SUCCESS to results with `finalVersion`, advance.

- [x] **Step 1: Implement checkPeerVersionForCurrentPadawan**

Replace the stub introduced in Task 5:

```cpp
void OtaForwarder::checkPeerVersionForCurrentPadawan()
{
    if (phase_ != Phase::AWAITING_VERSION_CONFIRMED)
    {
        return;
    }
    if (expectedNewVersion_.empty())
    {
        // Parse failed at deploy start — no comparison possible. The 15 s
        // versionConfirmTimer will fire and record FAILED("version_unconfirmed").
        return;
    }

    std::string reported = AstrOs_EspNow.getPeerVersion(currentControllerId_);
    if (reported.empty() || reported != expectedNewVersion_)
    {
        // Still on old version (or no version reported yet); keep waiting
        // until either match or timeout.
        return;
    }

    ESP_LOGI(TAG, "Version confirmed for %s: '%s' == expected '%s'",
             currentControllerId_.c_str(), reported.c_str(), expectedNewVersion_.c_str());

    versionConfirmTimerStop();

    AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "VERSION_CONFIRMED",
                                           firmwareTotalSize_, firmwareTotalSize_, expectedNewVersion_);

    results_.push_back({currentControllerId_, PadawanStatus::OK, expectedNewVersion_, ""});
    finishCurrentPadawanAndAdvance();
}
```

- [x] **Step 2: Call checkPeerVersionForCurrentPadawan from handleTick**

The 1-second tick handler is `OtaForwarder::handleTick()` at `lib/OtaForwarder/src/OtaForwarder.cpp:439`. It currently early-returns when not in `Phase::STREAMING` so it can't be used as-is for `AWAITING_VERSION_CONFIRMED` work. Add a phase-specific branch at the very top of the function, before the existing STREAMING gate:

Existing function start:

```cpp
void OtaForwarder::handleTick()
{
    // tick(count=0, abandon=false) is indistinguishable from "not streaming";
    // consult status() separately so we skip ticks while idle.
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
        // ...
```

Replace with:

```cpp
void OtaForwarder::handleTick()
{
    // Phase A: drive AWAITING_VERSION_CONFIRMED polling first. handleTick
    // fires every 1 s regardless of bulk_ state, but the STREAMING-only
    // logic below is the more restrictive caller. Run version-check before
    // the bulk_/STREAMING gates so it actually sees the tick.
    if (phase_ == Phase::AWAITING_VERSION_CONFIRMED)
    {
        checkPeerVersionForCurrentPadawan();
        return;
    }

    // tick(count=0, abandon=false) is indistinguishable from "not streaming";
    // consult status() separately so we skip ticks while idle.
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
        // ...
```

Confirm the rest of `handleTick` is unchanged below the existing STREAMING block.

- [ ] **Step 3: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed.

- [ ] **Step 4: Native suite passes**

Run: `pio test -e test`

Expected: existing suite passes; new tests land in Task 9.

- [ ] **Step 5: Commit**

```bash
git add lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "$(cat <<'EOF'
feat(ota): tick-driven version match polling for AWAITING_VERSION_CONFIRMED

Phase A — once per second while in AWAITING_VERSION_CONFIRMED, master reads
AstrOs_EspNow.getPeerVersion(currentControllerId_) and compares against the
expectedNewVersion_ parsed at deploy start. On match: emit FW_PROGRESS
VERSION_CONFIRMED with the version in the detail field, stop the 15 s
version-confirm timer, push {OK, finalVersion=expectedNewVersion_, ""} to
results, advance to next padawan. Mismatch or empty reported version =
keep waiting until match or timeout.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8 — Master: version-confirm timeout handler

**Files:**
- Modify: `lib/OtaForwarder/src/OtaForwarder.cpp`

When the 15 s `versionConfirmTimer_` fires without a successful match, record FAILED for the current padawan and advance. Mirror the existing `handleFlashResultTimeout` pattern.

- [x] **Step 1: Implement handleVersionConfirmTimeout**

Replace the stub introduced in Task 5:

```cpp
void OtaForwarder::handleVersionConfirmTimeout()
{
    if (phase_ != Phase::AWAITING_VERSION_CONFIRMED)
    {
        // Stale fire after we already completed via tick-driven match.
        return;
    }
    ESP_LOGW(TAG, "Version-confirm timeout for %s; recording FAILED(version_unconfirmed)",
             currentControllerId_.c_str());
    results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "version_unconfirmed"});
    finishCurrentPadawanAndAdvance();
}
```

- [x] **Step 2: Ensure abortCurrentPadawan and finishCurrentPadawanAndAdvance stop the timer**

The version-confirm timer must not survive past current-padawan completion or it would mis-fire against the next padawan. Locate `abortCurrentPadawan` and `finishCurrentPadawanAndAdvance` in `OtaForwarder.cpp` and add `versionConfirmTimerStop();` near the other `*TimerStop()` calls (alongside `flashResultTimerStop()` and `statsTimerStop()`).

- [x] **Step 3: Build for both boards**

Run: `pio run -e lolin_d32_pro && pio run -e metro_s3`

Expected: both builds succeed.

- [x] **Step 4: Commit**

```bash
git add lib/OtaForwarder/src/OtaForwarder.cpp
git commit -m "$(cat <<'EOF'
feat(ota): AWAITING_VERSION_CONFIRMED 15s timeout handler

Phase A — if the heartbeat-reported version never matches the expected
version within 15 seconds of entering AWAITING_VERSION_CONFIRMED, record
FAILED("version_unconfirmed") and advance to the next padawan in the order
list. Also wires versionConfirmTimerStop() into the abort / finish paths
so a stale fire never crosses padawan boundaries.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9 — OtaForwarder Phase A state-machine native tests

**Files:**
- Modify: `test/test_native/astros_ota_forwarder_tests.cpp`

The existing test file has fixtures that drive OtaForwarder through the wire-protocol state machine. Phase A adds three cases: happy path through AWAITING_VERSION_CONFIRMED, wrong-version timeout, and parse-failure fallback to timeout.

Note: these tests require the test fixture to be able to inject a peer-version into a fake `AstrOs_EspNow` (the real one isn't built in native mode). Check what mocking pattern the existing tests use:

Run: `grep -n "AstrOs_EspNow\|MockEspNow\|fakeEspNow" test/test_native/astros_ota_forwarder_tests.cpp | head -20`

If a mock/fake already exists, extend it with a settable `peerVersion_` map. If not, the simplest approach is to inject `expectedNewVersion_` directly via a test-only friend accessor, and stub `getPeerVersion` via a test-mode global. The exact technique depends on what's already there — adapt to fit.

- [ ] **Step 1: Add happy-path test (version-match within timer)**

In `test/test_native/astros_ota_forwarder_tests.cpp`, append (adapting the fixture setup to match the existing test style):

```cpp
TEST_F(OtaForwarderTest, AwaitingVersionConfirmed_MatchAdvancesToSuccess)
{
    // Set up: drive forwarder to AWAITING_VERSION_CONFIRMED for padawan A.
    deployTo({"AA:BB:CC:DD:EE:01"});
    driveToFlashResultOK("AA:BB:CC:DD:EE:01");

    // Forwarder should now be in AWAITING_VERSION_CONFIRMED, REBOOTING
    // FW_PROGRESS should have been emitted.
    EXPECT_EQ(getPhase(), OtaForwarder::Phase::AWAITING_VERSION_CONFIRMED);
    EXPECT_TRUE(seenFwProgressStage("REBOOTING"));

    // Inject expected version + peer version match.
    setExpectedNewVersion("1.2.3");
    setMockedPeerVersion("AA:BB:CC:DD:EE:01", "1.2.3");

    // Advance the tick (1 s) — version match should fire.
    fireTickN(1);

    EXPECT_TRUE(seenFwProgressStage("VERSION_CONFIRMED"));
    EXPECT_EQ(getResults().size(), 1u);
    EXPECT_EQ(getResults()[0].status, PadawanStatus::OK);
    EXPECT_EQ(getResults()[0].finalVersion, "1.2.3");
    EXPECT_EQ(getResults()[0].errorReason, "");
}
```

- [ ] **Step 2: Add timeout test (no match in 15 s)**

Append:

```cpp
TEST_F(OtaForwarderTest, AwaitingVersionConfirmed_TimeoutRecordsFailed)
{
    deployTo({"AA:BB:CC:DD:EE:02"});
    driveToFlashResultOK("AA:BB:CC:DD:EE:02");
    setExpectedNewVersion("1.2.3");
    setMockedPeerVersion("AA:BB:CC:DD:EE:02", "1.1.0"); // stuck on old version

    // Advance the tick 15 times (15 s) — no match.
    fireTickN(15);
    EXPECT_EQ(getPhase(), OtaForwarder::Phase::AWAITING_VERSION_CONFIRMED);
    // Then fire the 15 s version-confirm timer expiry.
    fireVersionConfirmTimerExpiry();

    EXPECT_EQ(getResults().size(), 1u);
    EXPECT_EQ(getResults()[0].status, PadawanStatus::FAILED);
    EXPECT_EQ(getResults()[0].errorReason, "version_unconfirmed");
    EXPECT_EQ(getResults()[0].finalVersion, "");
}
```

- [ ] **Step 3: Add parse-failure fallback test**

Append:

```cpp
TEST_F(OtaForwarderTest, AwaitingVersionConfirmed_EmptyExpectedFallsThroughToTimeout)
{
    deployTo({"AA:BB:CC:DD:EE:03"});
    driveToFlashResultOK("AA:BB:CC:DD:EE:03");
    setExpectedNewVersion(""); // simulate parse failure at deploy start

    // Tick handler should be a no-op (no comparison possible).
    setMockedPeerVersion("AA:BB:CC:DD:EE:03", "1.2.3");
    fireTickN(15);
    EXPECT_EQ(getPhase(), OtaForwarder::Phase::AWAITING_VERSION_CONFIRMED);

    // Timer fires and records FAILED.
    fireVersionConfirmTimerExpiry();
    EXPECT_EQ(getResults().size(), 1u);
    EXPECT_EQ(getResults()[0].status, PadawanStatus::FAILED);
    EXPECT_EQ(getResults()[0].errorReason, "version_unconfirmed");
}
```

- [ ] **Step 4: Add the test-only helpers needed by these cases**

If the fixture doesn't already expose `setExpectedNewVersion`, `setMockedPeerVersion`, `fireTickN`, `fireVersionConfirmTimerExpiry`, `getPhase`, `seenFwProgressStage`, or `getResults`, add them as `friend` accessors on `OtaForwarderTest` or as test-only methods on `OtaForwarder` guarded by `#ifdef ASTROS_TEST_BUILD`. Match the existing pattern in the file — do not invent a new mocking framework.

- [ ] **Step 5: Run the new tests**

Run: `pio test -e test -f test_native --filter "*ota_forwarder*"`

Expected: all three new cases PASS along with the existing forwarder tests.

- [ ] **Step 6: Commit**

```bash
git add test/test_native/astros_ota_forwarder_tests.cpp
git commit -m "$(cat <<'EOF'
test(ota): native coverage for Phase A AWAITING_VERSION_CONFIRMED

Three new GoogleTest cases driving OtaForwarder through the new phase:
(1) happy path — peer reports expected version, row → OK with finalVersion,
(2) timeout — peer never reports expected version, row → FAILED
    ("version_unconfirmed"),
(3) parse-failure fallback — expectedNewVersion_ empty (simulating a bad
    .bin header), tick is a no-op, timer eventually records FAILED.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10 — Bench QA plan + USB recovery doc

**Files:**
- Create: `.docs/qa/ota-upgrade-pr-set-2.md` (Phase A section)
- Create: `.docs/qa/ota-upgrade-recovery-via-usb.md`

Document the hardware test cases that exercise behavior native tests can't reach: real partition writes, real reboots, real auto-rollback. Operator-facing recovery procedure for a bricked board.

- [ ] **Step 1: Write the QA test plan**

Create `.docs/qa/ota-upgrade-pr-set-2.md`:

```markdown
# OTA Upgrade — PR set 2 QA test plan

Bench validation for the real flash-commit work shipped in PR set 2. Each
phase ships independently; this doc grows with the PR for each phase.

## Phase A — Padawan flash + master version-confirm gate

### Preconditions

- One master board flashed with the Phase A firmware (post-merge).
- At least one padawan board flashed with the *previous* firmware (so we
  can observe a version change).
- Pi connected to master via USB serial at 115200 baud.
- AstrOs.Server running and able to upload a firmware .bin to the master.
- Build a "next" firmware on the Phase A branch with a bumped `VERSION`
  file so the .bin's `esp_app_desc_t.version` differs from what the
  padawan is currently running.

### Test case A.1 — Padawan happy path

1. From the server UI, upload the next .bin to the master.
2. Wait for the server's "staged" indicator.
3. Initiate a deploy targeting the single padawan.
4. **Expected serial-log progression on the padawan** (visible via
   `pio device monitor -e <env>`):
   - `OTA_BEGIN` received, partition opened, `OtaWriter: active`.
   - `OtaWriter: streaming` — chunks arriving, periodic stats.
   - `handleEnd: sleeping 2s before esp_ota_set_boot_partition`.
   - `handleEnd: boot partition flipped; reporting OK and rebooting`.
   - Padawan reboots.
   - On reboot: `AstrOs.ESP version <NEW> (sha: …)`.
   - First poll cycle (within ~2 s): `OTA rollback cancelled — running
     image is now valid`.
5. **Expected master-side FW_PROGRESS sequence** (visible in server UI
   for the padawan row):
   - `SENDING` (multiple, growing %) → `VERIFYING` → `FLASHING` →
     `REBOOTING` → `VERSION_CONFIRMED`.
6. **Expected DEPLOY_DONE result** for the padawan row:
   `status=OK, finalVersion=<NEW>, errorReason=""`.

### Test case A.2 — Padawan flash failure injection

1. Apply the temporary debug patch:
   ```
   // In OtaWriter::handleEnd, immediately before esp_ota_set_boot_partition:
   bootErr = ESP_FAIL; // FORCED FAILURE
   goto report_failed; // adapt to local control flow — skip the real call
   ```
2. Build and flash the padawan with the patched firmware.
3. From the server, trigger a deploy with a *different* .bin (so the
   padawan would otherwise want to flash).
4. **Expected**: padawan never calls `esp_ota_set_boot_partition`,
   reports `OtaFlashStatus::FAILED` with reason `"ESP_FAIL"`, does not
   reboot. Master records FAILED, deploy advances.
5. Revert the debug patch before continuing.

### Test case A.3 — Padawan boot crash + auto-rollback

1. Apply the temporary debug patch to the `app_main` entry point:
   ```
   void app_main() {
       abort(); // FORCED CRASH
       // ... rest unchanged
   }
   ```
2. Build the .bin with the patch and a bumped `VERSION` so it's
   identifiable.
3. Stage it on the master via the server.
4. Trigger a deploy to a clean padawan (running the previous good
   firmware).
5. **Expected**:
   - Padawan flashes, reports OK, reboots.
   - On reboot, new image immediately aborts.
   - Bootloader sees PENDING_VERIFY image crashed → reverts to old
     partition on next boot.
   - Padawan comes up running the *old* firmware.
   - Master's `versionConfirmTimer` (15 s) fires because the heartbeat
     keeps reporting the old version. Master records
     FAILED("version_unconfirmed").
   - Padawan is still functional, just on the old image.
6. Revert the debug patch.

### Test case A.4 — Version-confirm timeout (slow padawan)

1. Apply a temporary debug patch to the padawan to delay its first
   POLL_ACK send post-reboot:
   ```
   // In AstrOsEspNow::sendPollAck, at the top:
   static bool delayedOnce = false;
   if (!delayedOnce) {
       delayedOnce = true;
       vTaskDelay(pdMS_TO_TICKS(20000)); // 20 s — beyond master's 15 s timer
   }
   ```
2. Build and flash. Trigger a deploy.
3. **Expected**:
   - Padawan flashes, reboots, comes up on new image.
   - First POLL_ACK doesn't go out for 20 s.
   - Master's 15 s `versionConfirmTimer` fires first → records
     FAILED("version_unconfirmed").
   - Later, padawan eventually sends its first POLL_ACK; new firmware
     calls `mark_app_valid_cancel_rollback` (verifies the late-arrival
     path is harmless).
4. Operator-facing UI shows the padawan as FAILED, but the padawan is
   running new firmware. This is the documented "operator-visible
   mismatch" failure mode — record it; not a bug.
5. Revert the debug patch.

### Edge case bench notes

- If a padawan's `esp_app_desc_t` parse fails at deploy start (unusual,
  would mean the staged .bin is corrupted), the master logs the parse
  error and falls through to the 15 s timeout. The padawan row appears
  as FAILED("version_unconfirmed"). The padawan never actually flashes
  in this case because the streaming SHA verify would catch the
  corruption first — but the parser is defense in depth.

### Recovery

If any test case bricks a padawan (image won't boot AND auto-rollback
also fails), follow `.docs/qa/ota-upgrade-recovery-via-usb.md`.
```

- [ ] **Step 2: Write the USB recovery doc**

Create `.docs/qa/ota-upgrade-recovery-via-usb.md`:

```markdown
# OTA bricked board — USB recovery procedure

If a Phase A bench case (or worse, a production OTA) leaves a board
unable to boot AND the auto-rollback safety net also fails, this is the
recovery procedure. Both supported boards expose USB; the procedure is
identical except for the chip flag.

## Preconditions

- Physical access to the bricked board.
- USB cable.
- A workstation with `esptool.py` installed (`pip install esptool` or via
  PlatformIO's bundled toolchain).
- A known-good firmware .bin for the board's environment. Easiest source:
  download the latest RC artifact from the AstrOs.ESP GitHub Releases
  page (`https://github.com/<owner>/AstrOs.ESP/releases`). Pick the
  matching environment (`firmware-lolin_d32_pro-*.bin` or
  `firmware-metro_s3-*.bin`).

## Procedure

### Step 1 — Identify the USB serial port

Plug the board in. On Linux:

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

On macOS:

```bash
ls /dev/cu.usbserial-* /dev/cu.usbmodem-*
```

Note the device path (e.g. `/dev/ttyUSB0`).

### Step 2 — Hold the board in download mode

- `lolin_d32_pro`: press and hold the `BOOT` (or `IO0`) button while
  pressing/releasing `RESET`, then release `BOOT`. The board is now in
  bootloader mode.
- `metro_s3`: double-tap the `RESET` button — the board enters native USB
  bootloader mode (it appears as a USB mass-storage device).

### Step 3 — Erase flash

```bash
esptool.py --port /dev/ttyUSB0 --chip esp32 erase_flash         # lolin_d32_pro
esptool.py --port /dev/ttyUSB0 --chip esp32s3 erase_flash       # metro_s3
```

**Expected output**: `Chip erase completed successfully in <N>s. Hard
resetting via RTS pin...`

### Step 4 — Write the known-good firmware

```bash
esptool.py --port /dev/ttyUSB0 --chip esp32 write_flash \
    --flash_mode dio --flash_size detect 0x1000 firmware-lolin_d32_pro-<version>.bin
```

For `metro_s3` adjust `--chip` to `esp32s3`. The base address `0x1000` is
where the second-stage bootloader sits — this is the standard PlatformIO
partition layout for both boards.

**Expected output**: `Hash of data verified. Leaving... Hard resetting
via RTS pin...`

### Step 5 — Confirm recovery

Open a serial monitor:

```bash
pio device monitor -p /dev/ttyUSB0 -b 115200
```

**Expected first lines**:

```
I (nnn) AstrOs.ESP: AstrOs.ESP version <V> (sha: <SHA>)
```

If you see the version banner, the board is recovered. Re-pair it with
the master if applicable.

## Common failures

- **`esptool.py: command not found`** — install esptool: `pip install
  esptool`.
- **`A fatal error occurred: Failed to connect to ESP32: Timed out
  waiting for packet header`** — the board is not in download mode.
  Repeat Step 2 carefully.
- **Erase succeeds but write fails partway** — the .bin may be for the
  wrong environment (variant mismatch between `lolin_d32_pro` and
  `metro_s3`). Double-check the .bin filename against the board.
```

- [ ] **Step 3: Commit**

```bash
git add .docs/qa/ota-upgrade-pr-set-2.md .docs/qa/ota-upgrade-recovery-via-usb.md
git commit -m "$(cat <<'EOF'
docs(ota): Phase A bench QA plan + USB recovery procedure

Bench test plan for Phase A's hardware-only behavior (real flash commit,
auto-rollback safety net, version-confirm timeout). Four test cases with
explicit debug-patch snippets for failure injection. Companion USB
recovery doc covering both lolin_d32_pro and metro_s3 in case bench
testing bricks a board.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review checklist (for the writer, not the executor)

This section is a checklist for the plan-writer to scan after the plan is
saved. Implementer can ignore.

- [x] **Spec coverage**:
  - Padawan flash commit (design §"Padawan side") → Task 2.
  - Padawan rollback (design §"Padawan side (rollback safety net)") → Task 3.
  - EspAppDescParser (design §C.1 of master side) → Task 1.
  - AWAITING_VERSION_CONFIRMED phase (design §"Master side") → Tasks 5-8.
  - getPeerVersion (design §"Master side") → Task 4.
  - Native tests (design §"Native tests" column) → Tasks 1, 9.
  - Bench QA + USB recovery (design §"Bench / QA plan") → Task 10.
- [x] **No placeholders**: every step has either exact code, an exact
  command, or both. Two areas explicitly call out adapt-to-local: Task 6
  Step 2 fseek error recovery (matches existing `firmware_seek_failed`
  pattern at line 660), Task 9 Step 4 test fixture helpers (extend
  existing pattern in the file).
- [x] **Type consistency**: `PadawanStatus::OK` / `PadawanStatus::FAILED`
  used consistently (matches actual enum at
  `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp:138`).
  `Phase::AWAITING_VERSION_CONFIRMED` introduced in Task 5 and consumed in
  Tasks 6, 7, 8 with matching name.
- [x] **Commit cadence**: each task is one commit. Ten commits total —
  consistent with the established `docs(ota): / feat(ota): / test(ota):`
  prefix style visible in `git log --oneline`.
