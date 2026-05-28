# Firmware OTA — PR set 2 (real flash commit, rollback, master self-flash) — design

## Context

PR set 1 of the firmware OTA mesh-forward feature shipped through 2026-05-27:
M1–M5 wire format, BulkSender, OtaForwarder, OtaWriter, and FW_PROGRESS
emission are all in production-like state. The single remaining placeholder is
the actual boot-partition flip — `OtaWriter::handleEnd` currently calls
`sendFlashResult(... FLASH_NOT_IMPLEMENTED ...)` instead of
`esp_ota_set_boot_partition` + `esp_restart`. See the in-source comment at
`lib/OtaWriter/src/OtaWriter.cpp:603-619` for the exact recipe.

This design covers **PR set 2**: replace the placeholder with the real flash
commit, add a rollback safety net so a bad image auto-reverts, wire up
post-reboot version confirmation via the existing heartbeat path, and finally
turn on master self-flash. Cross-milestone context lives in
`.docs/plans/20260523-1023-firmware-ota-mesh-forward-design.md`.

PR set 2 carries the first real bricking risk in the OTA feature, so the work
is split into three sequentially-shippable phases:

> **Phase A** — Padawan flash + rollback handshake + master-side version-confirm
> gate. Bricks padawans only. Rollback safety net + USB recovery doc.
>
> **Phase B** — `REBOOTING` + `VERSION_CONFIRMED` FW_PROGRESS emission triggers
> + native tests. Cosmetic / UI signal only. Zero firmware brick risk.
>
> **Phase C** — Master self-flash via `OtaWriter` loopback, new
> `PadawanStatus::PENDING` wire status, server-side deploy finalization via
> existing self-POLL_ACK stream. Can brick master. Same rollback net.
> Cross-repo coordination with AstrOs.Server.

## Decisions (confirmed with user 2026-05-28)

- **PR shape**: three sequential PRs (Phase A → B → C). Brick risk firewalled
  to Phase A; B and C build on it. Confirmed during brainstorm.
- **Rollback trigger (padawan)**: first successful POLL_ACK send post-reboot
  calls `esp_ota_mark_app_valid_cancel_rollback`. If the new image crashes
  before that, ESP-IDF bootloader auto-reverts on next boot.
- **Rollback trigger (master)**: first successful self-POLL_ACK serial TX
  post-reboot calls `esp_ota_mark_app_valid_cancel_rollback`. Symmetric to
  padawan; uses the existing self-poll already wired at
  `src/main.cpp:541-544`.
- **Master SUCCESS gate**: master waits for `VERSION_CONFIRMED` via the
  existing 2 s heartbeat before recording SUCCESS for a padawan row. Adds
  a new `AWAITING_VERSION_CONFIRMED` phase to OtaForwarder.
- **Expected version source**: master parses `esp_app_desc_t` from the staged
  `.bin` at deploy start. New PURE helper in `lib_native/AstrOsUtility`. No
  server-side changes for version reporting.
- **Version watcher wiring**: `AstrOsEspNow` stores last-known version per
  peer; `OtaForwarder` polls it from its tick in `AWAITING_VERSION_CONFIRMED`.
  Loose coupling — AstrOsEspNow stays OTA-agnostic.
- **Loopback seam (master self-flash)**: new `beginLocalFlash` /
  `handleLocalDataChunk` / `endLocalFlash` entry points on `OtaWriter`. Master
  feeds the staged file directly into these methods — bypasses BulkSender
  entirely while reusing all of OtaWriter's SHA verify, read-back-rehash, and
  esp_ota_* logic.
- **Master DONE timing**: master emits `FW_DEPLOY_DONE` with its own row
  marked `PadawanStatus::PENDING` before reboot. Server resolves PENDING rows
  using the master's existing self-POLL_ACK heartbeat. New status enum value
  in the wire format.
- **Server-side scope**: PR set 2 expands beyond the original "firmware-only"
  framing. AstrOs.Server gains a `Finalizing` deploy state that watches
  self-POLL_ACK for PENDING-row resolution.
- **Bench recovery**: both rollback safety net AND USB recovery doc.
- **Master order position**: master row is always processed last regardless of
  its position in `orderList_`. OtaForwarder defers it via a
  `masterRowDeferred_` flag rather than mutating the order list.

## Architecture

### Phase A — Padawan flash + master version-confirm gate

#### Padawan side (`lib/OtaWriter`)

Replace the placeholder block at `lib/OtaWriter/src/OtaWriter.cpp:603-628`
with the real flash commit. The 2 s `vTaskDelay` stays in place so the
FLASHING row remains visible at the same cadence the server-side state
machine was tested against under PR set 1.

`handleEnd` success path becomes:

1. `vTaskDelay(pdMS_TO_TICKS(2000))` — preserves visible cadence.
2. `esp_err_t err = esp_ota_set_boot_partition(inactivePartition_)`.
3. Send `OTA_FLASH_RESULT` — status `OK` on success, status `FAILED` with
   `esp_err_to_name(err)` as the reason on failure.
4. On success: `vTaskDelay(pdMS_TO_TICKS(200))` (let the ACK go on the wire)
   → `esp_restart()`.
5. On failure: leave bootloader pointing at running image, no reboot.

#### Padawan side (rollback safety net)

Two changes in `lib/AstrOsEspNow`:

- In `AstrOsEspNow::sendPollAck`, after a successful `esp_now_send` return,
  check a `firstPollAckSent_` static flag. On `false→true` transition, call
  `esp_ota_mark_app_valid_cancel_rollback()`.
- No state-check guard needed — the ESP-IDF call is documented as idempotent
  when the running image is not in `PENDING_VERIFY` state.

Optional informational log in `app_main` if `esp_ota_get_state_partition`
returns `ESP_OTA_IMG_PENDING_VERIFY` at boot. Useful for bench observability;
not load-bearing.

#### Master side (`lib_native/AstrOsUtility`, `lib/OtaForwarder`, `lib/AstrOsEspNow`)

Three changes:

1. **New PURE helper `EspAppDescParser`** in `lib_native/AstrOsUtility`:
   - Reads the .bin's first ~300 bytes (caller supplies a buffer).
   - Validates the image header magic byte `0xE9` at offset 0.
   - Skips the 24 B `esp_image_header_t` + 8 B `esp_image_segment_header_t`
     to land on `esp_app_desc_t`.
   - Validates `esp_app_desc_t.magic_word == 0xABCD5432`.
   - Extracts `version[32]` as a null-terminated `std::string`.
   - Returns a result struct: `{ ok: bool, version: string, error: string }`.
   - Native-testable; hand-define the byte layout to avoid the ESP-IDF
     dependency. The layout has been stable since ESP-IDF 3.x.
   - Tests cover: known-good fixture, bad magic byte, bad app-desc magic,
     truncated input.

2. **New `AWAITING_VERSION_CONFIRMED` phase in `OtaForwarder`**:
   - `startCurrentPadawan` parses `esp_app_desc_t` from the staged .bin
     once and caches `expectedNewVersion_`.
   - On `OTA_FLASH_RESULT(OK)`:
     - Stop `flashResultTimer`.
     - Emit `FW_PROGRESS REBOOTING (firmwareTotalSize_, firmwareTotalSize_, "")`.
     - Start `versionConfirmTimer` (15 s).
     - Enter `AWAITING_VERSION_CONFIRMED` phase.
     - Restart `tickTimer` at 50 ms cadence.
   - Each tick: call `AstrOs_EspNow.getPeerVersion(currentPadawanMac_)`.
     - Match against `expectedNewVersion_`: emit `FW_PROGRESS VERSION_CONFIRMED
       (..., expectedNewVersion_)`, push `{controllerId, PadawanStatus::OK,
       finalVersion=expectedNewVersion_, ""}` to results, advance to next
       padawan.
     - Still on old version: keep waiting.
   - On `versionConfirmTimer` expiry: push `{controllerId, FAILED, "",
     "version_unconfirmed"}` to results, advance.

3. **Per-peer last-known version in `AstrOsEspNow`**:
   - Add a `version` field to the peer record (or parallel map). Bounded by
     the existing 10-peer limit — trivial cost.
   - `handlePollAck` writes the parsed version under `peersMutex`.
   - New `getPeerVersion(mac) → std::string` accessor reads under the same
     mutex and returns a copy.

#### Failure modes (Phase A)

| Mode | Behavior |
|---|---|
| `esp_ota_set_boot_partition` fails | Padawan: FAILED + esp_err_name, no reboot. Master: records FAILED, deploy continues. |
| Padawan reboots but new image crashes before first POLL_ACK | Bootloader auto-reverts on next boot. Master times out version-confirm → FAILED("version_unconfirmed"). Padawan back on old image, retry possible. |
| Padawan reboots but POLL_ACK never reaches master | Master times out version-confirm → FAILED("version_unconfirmed"). Padawan stays on new image (it marked valid locally). Operator-visible mismatch. |
| Padawan reboots with wrong version | Master sees mismatched version → keeps waiting until timeout → FAILED("version_unconfirmed"). |
| Master never receives `OTA_FLASH_RESULT` | Existing FAILED("flash_result_timeout") path. No change. |

### Phase B — FW_PROGRESS emission triggers

Purely additive. Two new stage strings + 4 native test cases + 1 QA paragraph.

#### Stage string additions (`lib/OtaForwarder/src/OtaForwarder.cpp`)

- `"REBOOTING"` — fired in the `OTA_FLASH_RESULT(OK)` handler at the same
  point the new `AWAITING_VERSION_CONFIRMED` phase begins (Phase A change).
- `"VERSION_CONFIRMED"` — fired in the version-confirm tick handler when
  `getPeerVersion()` returns the expected version (Phase A change).

Both stages set `bytesSent = totalBytes = firmwareTotalSize_`, matching the
existing `FLASHING` row convention at `OtaForwarder.cpp:408-409`. UI renders
100% progress bar on these terminal stages.

#### Native tests (`test/test_native/astros_serial_messages_tests.cpp`)

Four cases mirroring the existing `FLASHING` test pattern:

1. `getFwProgress(..., "REBOOTING", N, N, "")` → assert `parts[2] == "REBOOTING"`.
2. `getFwProgress(..., "VERSION_CONFIRMED", N, N, "1.2.3")` → assert
   `parts[2] == "VERSION_CONFIRMED"` and detail field carries the version.
3. Round-trip parse — both stages survive serialization/deserialization.
4. Detail field handles version strings with embedded dots (`"1.2.3"`)
   without splitter confusion.

#### QA documentation

One paragraph in `.docs/qa/ota-upgrade-pr-set-2.md` describing the new stage
sequence the UI will display:

- Success: `SENDING → VERIFYING → FLASHING → REBOOTING → VERSION_CONFIRMED`.
- Timeout: `SENDING → VERIFYING → FLASHING → REBOOTING → (no further stages;
  row marked FAILED with reason 'version_unconfirmed')`.

### Phase C — Master self-flash + server-side finalization

Three firmware pieces + one server-repo PR coordinated with the firmware.

#### C.1 — OtaWriter local-flash entry points (`lib/OtaWriter`)

New public methods:

```cpp
bool beginLocalFlash(uint32_t totalBytes, const uint8_t sha256[32]);
bool handleLocalDataChunk(uint16_t seq, const uint8_t *bytes, size_t len);
OtaFlashStatus endLocalFlash(const uint8_t streamedSha256[32],
                             std::string &outErrorReason);
```

Internals synthesize the same inputs that the wire-driven path produces, then
call the existing private helpers (`bulk_.onBegin`, `bulk_.onData`,
`bulk_.onEnd`) plus the same `esp_ota_begin` / `esp_ota_write` / `esp_ota_end`
/ read-back-rehash / `esp_ota_set_boot_partition` calls. No synthetic
ESP-NOW frames — the methods bypass the wire layer entirely.

The three methods are blocking and single-caller. `OtaForwarder` calls them
sequentially from its own task. Native tests cover the entry-point contract
using a stubbed flash partition.

#### C.2 — Master self-flash sequence in OtaForwarder

Replace the placeholder skip at `OtaForwarder.cpp:552-557`:

- When walking the order list and encountering the all-zero MAC, set
  `masterRowDeferred_ = true`, remember the row's original index, and
  `nextOrderIdx_++`. Don't record a result yet.
- After the padawan loop reaches `nextOrderIdx_ >= orderList_.size()`, if
  `masterRowDeferred_`, enter a new `MASTER_SELF_FLASH` phase before
  emitting `FW_DEPLOY_DONE`.

`MASTER_SELF_FLASH` phase sequence:

1. Emit `FW_PROGRESS SENDING (totalSize, totalSize, "master")`.
2. Emit `FW_PROGRESS VERIFYING` — parallel cadence to padawan path for UI
   consistency.
3. Open the staged .bin, compute SHA-256 (reuse existing one-shot SHA helper).
4. Call `AstrOs_OtaWriter.beginLocalFlash(totalSize, sha)`.
5. Loop `fread` chunks → `handleLocalDataChunk(seq, bytes, len)`.
6. Call `endLocalFlash(sha, outReason)`.
7. On `OtaFlashStatus::OK`:
   - Emit `FW_PROGRESS FLASHING`.
   - Emit `FW_PROGRESS REBOOTING`.
   - Push `{master, PadawanStatus::PENDING, "", "awaiting_post_reboot_version"}` to results.
   - Emit `FW_DEPLOY_DONE` with the full results vector.
   - `vTaskDelay(pdMS_TO_TICKS(500))` to flush UART.
   - `esp_restart()`.
8. On failure:
   - Emit `FW_PROGRESS` with the failure reason as detail.
   - Push `{master, PadawanStatus::FAILED, "", outReason}` to results.
   - Emit `FW_DEPLOY_DONE`.
   - No reboot.

`AstrOs_OtaWriter` is currently initialized only on padawan. Phase C
initializes it on master too in `main.cpp`. No side effects when not actively
flashing — `active_` stays false until `beginLocalFlash` is called.

#### C.3 — Master `mark_app_valid` wiring

In `lib/AstrOsSerialMsgHandler::sendPollAckNak` (or directly at the master
self-POLL_ACK call site at `src/main.cpp:543-544`, whichever yields the
cleaner first-success detection), check a `firstSelfPollAckSent_` static
flag. On `false→true` transition after a successful serial write, call
`esp_ota_mark_app_valid_cancel_rollback()`. Same idempotency note as
padawan — no state-check guard needed.

#### C.4 — Wire format: `PadawanStatus::PENDING`

The enum lives in `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp`
(it's reused by both the PURE mapping function and the MIXED OtaForwarder).
Current values use implicit ordering: `OK = 0, FAILED = 1`. Phase C extends:

```cpp
enum class PadawanStatus : uint8_t
{
    OK,
    FAILED,
    PENDING,  // added — row not yet finalized; server resolves via heartbeat
};
```

`FlashResultMapped` and the wire-format serialization currently treat the
enum as two-state. Phase C broadens that to three-state. Native tests cover:

- Serialization round-trip for PENDING.
- String mapping is explicit (`"PENDING"` rather than numeric).
- Mixed-status `FW_DEPLOY_DONE` with OK + FAILED + PENDING rows parses
  correctly.

Note: `mapOtaFlashStatusToResult` (PURE helper in the same file) maps from
`OtaFlashStatus` to `PadawanStatus`. Since `OtaFlashStatus` has no "pending"
input case (the wire flash result is always terminal), the existing mapping
function does not need to produce PENDING. PENDING is produced exclusively
by `OtaForwarder` for the master self-flash row.

#### C.5 — Server-side finalization (AstrOs.Server repo, coordinated PR)

When `FW_DEPLOY_DONE` arrives with any rows in `PENDING` status:

1. Hold the deploy record in a new `Finalizing` state instead of closing it.
2. Subscribe to incoming self-POLL_ACK messages (already streamed from
   master).
3. For each PENDING row, watch for a self-POLL_ACK whose `version` field
   matches the deploy's expected version. When matched, mutate that row to
   `SUCCESS` with `finalVersion` populated.
4. Per-deploy finalization timeout — 90 s. Covers worst-case ESP boot
   (~5 s) + SD remount (~3 s) + first poll cycle (~2 s) + Pi UART latency
   (negligible) + ~10× margin. On timeout, mutate remaining PENDING rows to
   `FAILED("post_reboot_timeout")` and close the deploy.
5. UI: render `Finalizing` state with a spinner on PENDING rows. Existing
   SUCCESS/FAILED row rendering needs no changes once rows mutate.

The server-side PR is a separate ship cadence. Firmware Phase C can ship
first if the server team prefers to absorb PENDING handling under a feature
flag, but the firmware-first ship has a real risk: pre-PENDING server
versions parsing a PENDING wire value depends on the existing deserialization
fallback (default-to-FAILED vs raise-on-unknown — verify before shipping).
Recommend coordinating Phase C firmware + server PRs to ship together.

## Cross-phase summary

| | Phase A | Phase B | Phase C |
|---|---|---|---|
| **Firmware files touched** | `lib/OtaWriter/{src,include}/*`, `lib/OtaForwarder/{src,include}/*`, `lib/AstrOsEspNow/{src,include}/*`, new `lib_native/AstrOsUtility/EspAppDescParser.*`, `src/main.cpp` | `lib/OtaForwarder/src/OtaForwarder.cpp` (2 stage strings) | `lib/OtaWriter/{src,include}/*` (new local-flash entry points), `lib/OtaForwarder/src/OtaForwarder.cpp`, `lib_native/AstrOsEspNowProtocol/include/AstrOsEspNowProtocol.hpp` (PENDING enum) + matching serialization in `lib_native/AstrOsMessaging/*`, `src/main.cpp` (master mark_valid wiring, OtaWriter init on master) |
| **Native tests** | EspAppDescParser parses/rejects malformed; OtaForwarder phase-transition fixtures; getPeerVersion accessor | 4 cases in astros_serial_messages_tests | PadawanStatus::PENDING round-trip; OtaWriter local-flash entry-point fixtures |
| **Server repo** | None | None | New `Finalizing` deploy state; PENDING row handling; self-POLL_ACK version watcher; 90 s timeout; UI spinner on PENDING rows |
| **Brick risk** | Padawan only; rollback net + USB recovery doc | None | Master + padawan; same rollback + recovery; USB recovery doc extends to cover master |
| **QA artifact** | `.docs/qa/ota-upgrade-pr-set-2.md` (Phase A section) + `.docs/qa/ota-upgrade-recovery-via-usb.md` | Folded into Phase A doc | Extended `.docs/qa/ota-upgrade-pr-set-2.md` Phase C section |

## Bench / QA plan

A new `.docs/qa/ota-upgrade-pr-set-2.md` covering all three phases:

- **Phase A bench cases**
  - Padawan happy path: trigger deploy, observe `FLASHING → REBOOTING →
    VERSION_CONFIRMED`, confirm the next POLL_ACK from that padawan reports
    the new version (visible in master log + UI version column).
  - Padawan flash failure injection: temporary debug build that returns
    `ESP_FAIL` from `esp_ota_set_boot_partition`. Confirm row → FAILED with
    `esp_err_to_name` in reason, no reboot, no rollback.
  - Padawan boot crash + auto-rollback: temporary debug build with a
    forced `abort()` (or `assert(0)`) in `app_main`. Flash, reboot, confirm
    bootloader reverts to old image on next boot, padawan comes up on old
    version, master records FAILED("version_unconfirmed").
  - Version-confirm timeout: temporary debug build that inserts a 20 s
    `vTaskDelay` *before* the first POLL_ACK send post-reboot. Master's 15 s
    timer fires → row → FAILED("version_unconfirmed"); padawan eventually
    sends POLL_ACK afterward (verifies the late-arrival is harmless).
- **Phase C bench cases**
  - Master happy path: trigger deploy with master in order list, observe
    padawans complete first, then master `SENDING/VERIFYING/FLASHING/
    REBOOTING`, DEPLOY_DONE with master=PENDING, master reboots, server
    resolves PENDING → SUCCESS via self-POLL_ACK.
  - Master flash failure: simulate via deliberately bad partition handle in
    debug build. Confirm master row → FAILED, no reboot, deploy closes.
  - Master boot crash + auto-rollback: same panic injection as padawan
    case, on master. Verify bootloader reverts, master comes up on old
    firmware, server times out PENDING → FAILED("post_reboot_timeout").
- **USB recovery doc** (`.docs/qa/ota-upgrade-recovery-via-usb.md`)
  - Pull the bricked board.
  - USB connect via boot button.
  - `esptool.py erase_flash`.
  - `esptool.py write_flash` of a known-good RC artifact from the
    GitHub releases page.
  - Expected serial output at each step. Both boards in the matrix
    (`lolin_d32_pro` and `metro_s3`) expose USB; no hardware difference
    needed in the procedure.

## Open items (not in scope for PR set 2)

- **Resume-after-error for a single padawan**: the wire format already
  supports `next-expected-seq` for resume; current v1 policy is abort-and-
  skip-padawan-on-failure. Future extension.
- **Parallel / broadcast OTA delivery**: explicitly out of scope per PR set 1
  design. ESP-NOW topology and per-padawan ACK demux make this a major
  separate effort.
- **`OTA_COMMIT` / `OTA_COMMIT_ACK` wire types**: the PR-set-1 design mention
  of these is superseded by the heartbeat-based VERSION_CONFIRMED approach.
  Not adding them; the heartbeat path is simpler and reuses existing
  infrastructure.
