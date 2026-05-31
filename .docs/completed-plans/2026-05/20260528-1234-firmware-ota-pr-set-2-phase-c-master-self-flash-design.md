# Firmware OTA — PR set 2 Phase C — Master self-flash + `PadawanStatus::PENDING` — design

## Context

PR set 2 Phase A shipped to develop on 2026-05-28 via PR #40, landing the real
flash commit + rollback safety net + master-side `AWAITING_VERSION_CONFIRMED`
gate. Cross-milestone design is at
`.docs/plans/20260528-0709-firmware-ota-pr-set-2-design.md`.

Phase B (additive FW_PROGRESS emission triggers) was effectively absorbed into
Phase A — the `REBOOTING` and `VERSION_CONFIRMED` stage emissions already
ship in `OtaForwarder::handleFlashResult` (OK path) and
`OtaForwarder::checkPeerVersionForCurrentPadawan` (match path). The original
Phase B scope's remaining work (4 native test cases for stage strings,
1 QA paragraph) is marginal-value and folded into Phase C if convenient.

Phase C is the master self-flash work that closes out PR set 2. It carries
brick risk on the master partition — the rollback safety net is now actually
active thanks to Phase A's `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` fix in
`862503b`. The new `PadawanStatus::PENDING` wire status requires coordinated
AstrOs.Server work, but Phase C firmware can ship first (per the
brainstorm-confirmed scope) provided the server's `FW_DEPLOY_DONE` parser
handles unknown `PadawanStatus` bytes permissively.

## Scope confirmation (2026-05-28)

- **Firmware-first ship**: Phase C firmware lands first. Server PR for
  `Finalizing` deploy state + self-POLL_ACK version watcher + 90 s timeout
  + UI spinner on PENDING rows ships as a separate follow-up.
- **Pre-merge gate**: confirm server `FW_DEPLOY_DONE` deserializer is
  permissive about unknown `PadawanStatus` byte values (or sequence
  server-first if not).
- **Bench validation**: spare board reconfigured as master via existing
  role-config mechanism; USB cable tethered throughout. Production master
  is NOT used as the test target.

## Decisions

- **Loopback seam**: sentinel-message-driven across the OtaForwarder ↔
  OtaWriter task boundary. New `OTA_WR_LOCAL_FLASH_REQ` posted to
  `otaWriterQueue`; new `OTA_FWD_LOCAL_FLASH_RESULT` posted back to
  `otaForwarderQueue`. Both tasks retain their single-task-state invariants
  (the discipline learned from Phase A's `versionConfirmTimerCb`).
- **New `Phase::MASTER_SELF_FLASHING`**: one new phase, slotted between
  `AWAITING_VERSION_CONFIRMED` and `BETWEEN_PADAWANS`. No separate
  `MASTER_REBOOTING` phase — the state machine doesn't survive the reboot
  anyway.
- **Master row deferral**: bool flag `masterRowDeferred_` flipped true when
  the all-zero MAC is encountered in the order list. Master always
  self-flashes last regardless of orderList_ position (cross-milestone
  design commitment).
- **Master `mark_app_valid` trigger**: inline in `main.cpp` polling code at
  the existing self-POLL_ACK call site. `sendPollAckNak` gains a `bool`
  return so the check fires on first successful serial-queue post.
- **Reboot orchestration**: `OtaForwarder` (not `OtaWriter`) calls
  `esp_restart` after emitting `FW_DEPLOY_DONE` with the PENDING master
  row. OtaWriter just reports success/failure of the local flash.
- **Bench plan**: spare master-role board + USB tether. Documented as a
  precondition in the QA plan; production master not used.

## Architecture

### Section 1 — Loopback wire architecture

Two new sentinel kinds carry the cross-task loopback.

#### `OTA_WR_LOCAL_FLASH_REQ` (OtaForwarder → otaWriterQueue)

New union arm in `queue_ota_writer_msg_t`:

```c
struct {
    char     firmwarePath[64];     // null-terminated; staged path on master
    uint32_t expectedSize;         // bytes (from stat())
    uint8_t  expectedSha256[32];   // for streaming-SHA comparison
} local_flash_req;
```

64-byte path is sufficient for the established staging convention
(`/sdcard/firmware/<sha-prefix>.bin`). SHA is inline (no malloc).

#### `OTA_FWD_LOCAL_FLASH_RESULT` (OtaWriter → otaForwarderQueue)

New union arm in `queue_ota_forwarder_msg_t`:

```c
struct {
    uint8_t  status;                // OtaFlashStatus value (OK or FAILED)
    uint8_t  errorReasonLen;        // 0..62
    char     errorReason[63];       // populated on FAILED; not NUL-terminated
} local_flash_result;
```

Mirrors the existing `OTA_FLASH_RESULT` wire payload shape used between
padawan and master — fixed-size inline strings, no malloc/free pair.

#### `OtaWriter::handleLocalFlashReq` (otaWriterTask)

Runs the full self-flash sequence inline on otaWriterTask:

1. `active_.store(true)` — pre-existing polling-pause gate at `main.cpp:520`
   already includes `AstrOs_OtaWriter.isActive()`, so master polling
   auto-pauses for the duration.
2. `fopen` firmware path; `fseek` to 0; verify file size matches
   `expectedSize`.
3. `esp_ota_get_next_update_partition(NULL)` → `inactivePartition_`.
4. `esp_ota_begin(inactivePartition_, OTA_SIZE_UNKNOWN, &otaHandle_)`.
5. `AstrOsSha256_init(&shaCtx_)`.
6. Loop: `fread` 4 KB chunks → `esp_ota_write(otaHandle_, buf, n)` →
   `AstrOsSha256_update`.
7. `AstrOsSha256_final(&shaCtx_, streamedDigest)` → compare to
   `expectedSha256`.
8. `esp_ota_end(otaHandle_)`.
9. Read-back-rehash loop (`esp_partition_read` + `AstrOsSha256`) → compare
   again.
10. `esp_ota_set_boot_partition(inactivePartition_)`.
11. `resetOtaHandleAndSha()`; `active_.store(false)`.
12. Post `OTA_FWD_LOCAL_FLASH_RESULT` to `otaForwarderQueue` with OK or
    FAILED + reason.
13. **OtaWriter does NOT call `esp_restart`** — orchestration belongs to
    OtaForwarder.

No code shared with `handleBegin`/`handleData`/`handleEnd` beyond the
underlying ESP-IDF calls and SHA helpers. The wire-protocol path (xferId
validation, BulkReceiver, OTA_*_ACK sends) is bypassed entirely.

#### Failure modes

| Failure | Reason string |
|---|---|
| `fopen` fails | `firmware_open_failed` |
| File size mismatch | `firmware_size_mismatch` |
| `esp_ota_begin` fails | `esp_err_to_name(err)` |
| `esp_ota_write` fails | `esp_err_to_name(err)` (+ `esp_ota_abort`) |
| Streaming SHA mismatch | `sha_mismatch` |
| `esp_ota_end` fails | `esp_err_to_name(err)` |
| `esp_partition_read` fails (read-back) | `esp_err_to_name(err)` |
| Read-back SHA mismatch | `readback_mismatch` |
| `esp_ota_set_boot_partition` fails | `esp_err_to_name(err)` |

All failures: post FAILED result + reason, return to IDLE, no reboot.

### Section 2 — OtaForwarder state machine + master row deferral

#### `Phase` enum extension

```cpp
enum class Phase : uint8_t
{
    IDLE = 0,
    AWAITING_BEGIN_ACK = 1,
    STREAMING = 2,
    AWAITING_END_ACK = 3,
    AWAITING_FLASH_RESULT = 4,
    AWAITING_VERSION_CONFIRMED = 5,
    MASTER_SELF_FLASHING = 6,      // new
    BETWEEN_PADAWANS = 7,           // shifted from 6
};
```

Internal-only enum; renumbering safe (no wire exposure). `handleStatsFire`
gains a `case MASTER_SELF_FLASHING:` to silence `-Wswitch`.

#### `masterRowDeferred_` flag + `masterRowOriginalIndex_`

Two new private members:
- `bool masterRowDeferred_ = false;`
- `size_t masterRowOriginalIndex_ = 0;` — captures the position of the
  master row in `orderList_` so the result can be inserted at that index
  when pushed (preserves the operator-submitted ordering in
  `FW_DEPLOY_DONE`, matching the PR-set-1 behavior where the
  inline-recorded `master_self_flash_pending` row appeared at its
  order-list position).

Both reset in `emitDeployDoneAndReset` (deploy lifecycle end).
Default-initialized in the class definition.

#### Order-list walk change

In `startNextPadawan`, the existing master-row-skip block at
`OtaForwarder.cpp:552-556` becomes:

```cpp
if (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "00:00:00:00:00:00")
{
    // Defer master row until after all padawan rows complete. Master
    // always self-flashes last per the cross-milestone design. Record
    // nothing now — the deferred row's result gets inserted at this
    // original index in handleLocalFlashResult so the FW_DEPLOY_DONE
    // row order matches the operator-submitted order list.
    masterRowDeferred_ = true;
    masterRowOriginalIndex_ = nextOrderIdx_;
    nextOrderIdx_++;
    continue;
}
```

#### Padawan-loop-exhausted handler

The existing block at `OtaForwarder.cpp:559-562` becomes:

```cpp
if (nextOrderIdx_ >= orderList_.size())
{
    if (masterRowDeferred_)
    {
        startMasterSelfFlash();
        return;
    }
    emitDeployDoneAndReset();
    return;
}
```

#### `startMasterSelfFlash()` (otaForwarderTask)

1. Look up staged firmware path via `AstrOs_OtaReceiver.getLastFirmwarePath()`.
2. If absent → push `{master, FAILED, "", "no_firmware"}`, emit DEPLOY_DONE,
   return.
3. `stat()` file for size; on failure push `firmware_stat_failed` and emit
   DEPLOY_DONE.
4. Compute SHA-256 of the file (reuse existing one-shot SHA helper from
   `startCurrentPadawan`).
5. Build `queue_ota_writer_msg_t` with kind `OTA_WR_LOCAL_FLASH_REQ`; fill
   `firmwarePath`, `expectedSize`, `expectedSha256`.
6. Set `phase_ = Phase::MASTER_SELF_FLASHING`. Set
   `currentControllerId_ = "00:00:00:00:00:00"` so any downstream logging
   includes the master-row identity.
7. `xQueueSend(otaWriterQueue_, &msg, 0)`. On queue-full (shouldn't happen
   — OtaWriter is idle by this point), log ESP_LOGE, push
   `{master, FAILED, "", "writer_queue_full"}`, emit DEPLOY_DONE.

#### `handleLocalFlashResult(msg)` (otaForwarderTask)

New dispatch case in `process()`:

```cpp
case OTA_FWD_LOCAL_FLASH_RESULT:
    handleLocalFlashResult(msg);
    break;
```

Body:

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

    // insertMasterRow inserts at masterRowOriginalIndex_ (clamped to
    // results_.size() if padawan rows produced fewer results than the
    // index would imply — e.g., aborted-mid-loop deploys). Keeps
    // FW_DEPLOY_DONE row order matching the operator-submitted order
    // list, preserving PR-set-1 behavior.
    auto insertMasterRow = [this](PadawanStatus status, const std::string &finalVersion,
                                  const std::string &errorReason) {
        size_t idx = std::min(masterRowOriginalIndex_, results_.size());
        results_.insert(results_.begin() + idx,
                        {"00:00:00:00:00:00", status, finalVersion, errorReason});
    };

    if (status == OtaFlashStatus::OK)
    {
        // Optimistic PENDING — server resolves to OK on post-reboot
        // self-POLL_ACK version match (or to FAILED on 90s timeout).
        // Master reboots before the server can do that resolution; the
        // post-reboot heartbeat is the signal.
        insertMasterRow(PadawanStatus::PENDING, "", "awaiting_post_reboot_version");
        emitDeployDoneAndReset();  // emits DEPLOY_DONE with full results vector

        ESP_LOGI(TAG, "Master self-flash complete; rebooting in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        // not reached
    }

    // FAILED: master partition was NOT switched; bootloader still points at
    // the running image. Record + emit DEPLOY_DONE; no reboot.
    ESP_LOGE(TAG, "Master self-flash failed: %s", reason.c_str());
    insertMasterRow(PadawanStatus::FAILED, "", reason);
    emitDeployDoneAndReset();
}
```

### Section 3 — Wire format: `PadawanStatus::PENDING`

#### Enum addition

`lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`:

```cpp
enum class PadawanStatus : uint8_t
{
    OK,
    FAILED,
    PENDING,   // new — row not yet finalized; server resolves via heartbeat
};
```

Implicit values: `OK=0`, `FAILED=1`, `PENDING=2`. Stable across the wire.

#### `mapOtaFlashStatusToResult` unchanged

`OtaFlashStatus` has no PENDING input (wire `OTA_FLASH_RESULT` is always
terminal). PENDING is produced exclusively by
`OtaForwarder::handleLocalFlashResult` for the master self-flash row.

#### Serialization

The `FW_DEPLOY_DONE` serializer in `lib_native/AstrOsMessaging/` builds the
wire message from `PadawanResult { controllerId, status, finalVersion,
errorReason }`. Implementation needs to verify the existing serializer
handles the new byte value cleanly (likely just an implicit conversion of
the enum to its underlying type — but worth a direct check during
implementation).

#### Server-side pre-merge gate

Phase C firmware-first ship requires the server's `FW_DEPLOY_DONE` parser
to handle unknown `PadawanStatus` bytes gracefully. Three possible
behaviors:

| Server behavior on unknown PadawanStatus byte | Firmware ship status |
|---|---|
| Permissive: renders as "unknown row" | OK to ship firmware first |
| Strict but defaults to FAILED | OK to ship; master row shows "Failed" until server PR lands |
| Crashes / rejects the whole DEPLOY_DONE | **BLOCKING** — sequence server PR first |

Pre-merge action: check server's DEPLOY_DONE parser. If case 3, switch the
PR ordering.

#### Native tests

Add to `test/test_native/astros_serial_messages_tests.cpp` (or wherever
`FW_DEPLOY_DONE` round-trips are tested today):

1. `FwDeployDone_PendingRow_RoundTripsCleanly` — build DEPLOY_DONE with
   OK + FAILED + PENDING rows; serialize; parse; assert each status
   matches.
2. `FwDeployDone_PendingRow_StatusByteIsTwo` — assert the wire byte for
   PENDING is `2` (pins the contract against enum reordering).

### Section 4 — Master `mark_app_valid` trigger

Mirror the padawan-side Phase A pattern, adapted to the master's
self-POLL_ACK serial-TX call site.

#### `sendPollAckNak` returns bool

`lib/AstrOsSerialMsgHandler/{include,src}/AstrOsSerialMsgHandler.{hpp,cpp}`:
change return type from `void` to `bool`. Returns `true` on successful
`xQueueSend` to `serialQueue`; `false` on queue-full. Existing call sites
that don't care about the return remain compatible (compiler will accept
the discarded return).

#### `src/main.cpp` polling-code addition

At the master self-POLL_ACK call site (currently lines 528-545), wrap the
`sendPollAckNak` call:

```cpp
static std::atomic<bool> firstSelfPollAckSent_{false};

bool serialQueued = AstrOs_SerialMsgHandler.sendPollAckNak(
    "00:00:00:00:00:00", "master", std::string(fingerprint),
    AstrOsConstants::Version, AstrOsConstants::Variant, true);

if (serialQueued)
{
    // First successful self-POLL_ACK queue post post-reboot proves: NVS
    // read worked (fingerprint), serial queue alive, message-service
    // serialization alive, FreeRTOS scheduler healthy. Enough evidence
    // to cancel auto-rollback for a freshly-flashed master image.
    // mark_valid is documented as safe to call unconditionally; returns
    // ESP_ERR_NOT_FOUND if no rollback was pending (the normal
    // subsequent-boot case).
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

Log-level discrimination: `ESP_LOGI` on genuine cancel, suppressed for the
expected `ESP_ERR_NOT_FOUND` (no rollback pending), `ESP_LOGW` for
unexpected returns. Addresses one of the deferred follow-up items from
Phase A's PR-toolkit review.

#### Why master-specific (vs. shared with padawan)

Padawan-side `mark_app_valid` is in `AstrOsEspNow::sendPollAck` (gating on
`esp_now_send` success). Master self-POLL_ACK goes through serial, not
ESP-NOW — different code path. Sharing infrastructure would entangle two
unrelated message handlers with OTA semantics or require a new
abstraction. Duplication is small (~15 lines per side) and each lives at
the call site most relevant to its concern.

### Section 5 — Master OtaWriter initialization

OtaWriter is currently padawan-only at `main.cpp:337-364` (queue creation,
task creation, `OtaWriter::Init`, `AstrOs_EspNow.setOtaWriterQueue` are
all guarded by `!isMasterNode.load()`). Phase C lifts those guards so
OtaWriter is initialized on master too.

Specific changes in `src/main.cpp`:

- Remove the `if (!isMasterNode.load())` guards around:
  - `otaWriterQueue` creation
  - `otaWriterTask` creation
  - `AstrOs_OtaWriter.Init(otaWriterQueue)`
- KEEP the guard around `AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue)`
  — master never receives wire OTA_BEGIN / OTA_DATA / OTA_END messages
  from a peer, so the ESP-NOW dispatch path stays padawan-only. The new
  loopback path posts to `otaWriterQueue` directly from OtaForwarder, not
  via `AstrOs_EspNow`.

After this change, both master and padawan have a live OtaWriter +
otaWriterTask. On padawans, it processes wire-driven messages; on
masters, it processes only the new `OTA_WR_LOCAL_FLASH_REQ` sentinel.
`active_` is false until a local-flash request lands, so the polling-pause
gate is correctly inert until master self-flash actually runs.

### Section 6 — Bench plan + brick recovery

#### Setup precondition

Adds one paragraph to `.docs/qa/ota-upgrade-pr-set-2.md` Phase C section:

> **Phase C bench requires a master-role spare board with USB tether.**
> Do not test on the production master — use a reconfigured spare so a
> brick doesn't take down the fleet. Either lolin_d32_pro or metro_s3 can
> be reconfigured as a master via the existing role-config mechanism
> (see `main.cpp:1711-1717` for the serial-driven `isMasterNode` toggle).

#### Test cases (extending `.docs/qa/ota-upgrade-pr-set-2.md`)

**C.1 — Master self-flash happy path**
- Deploy targeting `["00:00:00:00:00:00", "<padawan_mac>"]`.
- Expected: padawan loop runs first; after padawan completes, master
  serial log shows `Master self-flash complete; rebooting in 500ms`;
  DEPLOY_DONE emitted with master=PENDING + padawan=OK; master reboots
  and comes up on new firmware; first self-POLL_ACK (~2 s) logs
  `Master OTA rollback cancelled — running image is now valid`.
- Server-side verification: deploy record shows master as PENDING
  immediately; resolves to OK after the post-reboot self-POLL_ACK lands
  (requires server-side Phase C PR; without it, master stays PENDING in
  the UI — known operational state).

**C.2 — Master flash failure injection**
- Temporary debug patch in `OtaWriter::handleLocalFlashReq`: force
  `bootErr = ESP_FAIL;` before `esp_ota_set_boot_partition`.
- Deploy with master in order list.
- Expected: master row → FAILED with reason `"ESP_FAIL"`, no reboot,
  DEPLOY_DONE emitted normally; master still running old firmware.
- Revert patch.

**C.3 — Master boot-crash + auto-rollback** (highest stakes)
- Temporary debug patch in `app_main`: `abort()` at the top.
- Build with bumped VERSION for identifiability.
- Stage on master via server; deploy targeting `["00:00:00:00:00:00"]`
  (master-only — minimizes test surface).
- Expected: master self-flash succeeds locally; DEPLOY_DONE emitted with
  master=PENDING; master reboots; new image immediately aborts;
  bootloader detects PENDING_VERIFY image crashed → reverts to old
  partition on next boot; master comes up running OLD firmware;
  self-POLL_ACK starts firing again, reporting OLD version.
- Server-side: PENDING row resolves to FAILED on 90 s
  `post_reboot_timeout` (requires server-side Phase C PR).
- Master is still functional, just on the old image.
- Revert patch.

**C.4 — Master `firstSelfPollAckSent_` triggers exactly once**
- Normal boot (no flash). Observe serial log.
- Expected: exactly one `Master OTA rollback cancelled` log message (or
  silent if the boot wasn't from a PENDING_VERIFY state). Subsequent
  poll cycles: no additional rollback-path log messages.

#### Recovery procedure

If C.3 (or any unforeseen issue) leaves the spare master bricked AND
auto-rollback also fails, USB-recover via
`.docs/qa/ota-upgrade-recovery-via-usb.md` (already covers both boards —
no Phase C-specific changes to the recovery doc).

## Cross-phase file summary

| File | Change |
|---|---|
| `lib/OtaWriter/include/OtaWriterQueueMessage.h` | Add `OTA_WR_LOCAL_FLASH_REQ` kind + `local_flash_req` union arm |
| `lib/OtaWriter/include/OtaWriter.hpp` | Declare `handleLocalFlashReq(msg)` private method |
| `lib/OtaWriter/src/OtaWriter.cpp` | Implement `handleLocalFlashReq`; add dispatch case in `process()` |
| `lib/OtaForwarder/include/OtaForwarderQueueMessage.h` | Add `OTA_FWD_LOCAL_FLASH_RESULT` kind + `local_flash_result` union arm |
| `lib/OtaForwarder/include/OtaForwarder.hpp` | Add `MASTER_SELF_FLASHING` enum + `masterRowDeferred_` + method declarations |
| `lib/OtaForwarder/src/OtaForwarder.cpp` | Master-row deferral + `startMasterSelfFlash` + `handleLocalFlashResult` + dispatch case + `handleStatsFire` switch case |
| `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` | Add `PadawanStatus::PENDING` |
| `lib_native/AstrOsMessaging/` | Verify (or extend) `FW_DEPLOY_DONE` serializer handles PENDING byte |
| `lib/AstrOsSerialMsgHandler/include/AstrOsSerialMsgHandler.hpp` | Change `sendPollAckNak` return type from `void` to `bool` |
| `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp` | Return `xQueueSend` success status |
| `src/main.cpp` | Lift OtaWriter init guards for master; add `firstSelfPollAckSent_` + `mark_app_valid` call at self-POLL_ACK site |
| `test/test_native/astros_serial_messages_tests.cpp` | 2 new tests for PENDING round-trip + wire-byte pin |
| `test/test_native/astros_ota_writer_tests.cpp` (if it exists) or stand-in | Coverage for `handleLocalFlashReq` success / failure paths |
| `test/test_native/astros_ota_forwarder_tests.cpp` | Stand-in coverage for master-row-deferral + handleLocalFlashResult OK/FAILED paths |
| `.docs/qa/ota-upgrade-pr-set-2.md` | Phase C section with C.1–C.4 + spare-board precondition |

## Bench risk

Master self-flash is the second bricking-risk surface in PR set 2 (after
Phase A's padawan flash). Mitigations:

- **Rollback safety net** now actually active (Phase A's `862503b`
  enabled `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). New firmware that
  crashes before first self-POLL_ACK is auto-reverted by the bootloader.
- **Spare-board bench discipline** — production master is never the test
  target. Brick-on-bench affects only the spare.
- **USB recovery doc** already covers both boards.
- **Conservative failure paths** — every error in `handleLocalFlashReq`
  routes to "report FAILED, do not reboot." The partition flip is the
  last step; everything before it is reversible.

## Server-side coordination (separate AstrOs.Server PR)

Not in scope for this firmware PR, but documented for the follow-up:

- New `Finalizing` deploy state in the server orchestrator.
- Self-POLL_ACK version watcher: when a PENDING row exists in an open
  deploy, subscribe to the master's self-POLL_ACK stream; on version match
  → mutate PENDING → OK with finalVersion.
- 90 s timeout: covers worst-case master reboot (~5 s) + SD remount
  (~3 s) + first poll cycle (~2 s) + Pi UART latency (negligible) + 10×
  margin. On timeout: mutate PENDING → FAILED("post_reboot_timeout"),
  close the deploy.
- UI: render Finalizing state with a spinner on PENDING rows. Existing
  SUCCESS/FAILED rendering needs no changes once rows mutate.

## Open items (not in scope for Phase C)

- **Phase B remainder**: 4 native test cases for the `REBOOTING` and
  `VERSION_CONFIRMED` stage strings, plus a QA paragraph. Marginal value;
  fold into Phase C implementation if convenient, otherwise drop entirely.
- **Resume-after-error for a single padawan**: wire format already
  supports `next-expected-seq`; current v1 policy is abort-and-skip per
  the cross-milestone design. Future extension.
- **Parallel / broadcast OTA delivery**: explicitly out of scope per the
  cross-milestone design.
- **Improved server-side resolution latency**: the 90 s timeout is
  conservative. Could be tuned downward after bench validation produces
  measured tail-latency data.
